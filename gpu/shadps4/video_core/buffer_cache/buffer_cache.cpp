// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <array>
#include <map>
#include <optional>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <magic_enum/magic_enum.hpp>
#include "sotb_copy_pool.h"
#include "sotb_toggles.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_device_lost.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

namespace DeviceLost = Vulkan::DeviceLost;

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t NULL_INDEX_BUFFER_SIZE = 16;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;
constexpr std::optional<u32> CmaskColorExpandedValue(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return 0xFFFFFFFF;
    case 2:
        return 0xDDDDDDDD;
    case 4:
        return 0xEEEEEEEE;
    case 8:
        return 0xFFFFFFFF;
    default:
        return std::nullopt;
    }
}
static_assert(CmaskColorExpandedValue(1) == 0xFFFFFFFF);
static_assert(CmaskColorExpandedValue(2) == 0xDDDDDDDD);
static_assert(CmaskColorExpandedValue(4) == 0xEEEEEEEE);
static_assert(CmaskColorExpandedValue(8) == 0xFFFFFFFF);

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance, DeviceLost::Timeline::Memory} {
    const auto decision = ArenaLayout::DecideArenaMode(instance.GetSparseCapabilities());
    sparse_supported = decision.mode == ArenaLayout::ArenaMode::Sparse;
    arena_page_bits = ArenaLayout::PageBits(sparse_supported, instance.GetMaxBufferSize(),
                                            instance.MaxMemoryAllocationSize());
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = ArenaLayout::NumPages(arena_page_bits);
    address_space.resize(num_arena_pages);
    LOG_INFO(Render_Vulkan, "GPU memory arena: {} ({}), {} MiB pages", ArenaLayout::ModeName(decision.mode),
             decision.reason, arena_page_size >> 20);
    const vk::BufferCreateInfo probe_ci = {
        .flags = sparse_supported ? vk::BufferCreateFlagBits::eSparseBinding |
                                        vk::BufferCreateFlagBits::eSparseResidency
                                  : vk::BufferCreateFlags{},
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = static_cast<u32>(ArenaLayout::BlockSize(sparse_supported, reqs.alignment));
    ASSERT_MSG(ArenaLayout::IsValidBlockSize(block_size, arena_page_size),
               "Arena block size {} is not a power of 2 within an arena page", block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    {
        const auto& props = instance.GetMemoryProperties();
        constexpr auto unusable = vk::MemoryPropertyFlagBits::eLazilyAllocated |
                                  vk::MemoryPropertyFlagBits::eProtected |
                                  vk::MemoryPropertyFlagBits::eDeviceCoherentAMD |
                                  vk::MemoryPropertyFlagBits::eDeviceUncachedAMD;
        for (u32 i = 0; i < props.memoryTypeCount; ++i) {
            const auto flags = props.memoryTypes[i].propertyFlags;
            memory_type_traits.push_back(
                {static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eDeviceLocal),
                 static_cast<bool>(flags & unusable)});
        }
        backing_memory_types = BackingMemory::CandidateTypes(memory_type_traits, reqs.memoryTypeBits,
                                                             arena_memory_type_index);
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    null_index_buffer = std::make_unique<Buffer>(instance, 0, NULL_INDEX_BUFFER_SIZE,
                                                 MemoryType::DeviceLocal, "Null Index Buffer");
    runtime.FillBuffer(null_index_buffer.get(), 0u, NULL_INDEX_BUFFER_SIZE, 0u);

    // A zeroed block for the neighbours of resident ranges. AMD faults a shader that reads an
    // unbound sparse page, other drivers answer zero. Needs aliased binding.
    if (sparse_supported && sparse_aliased_enabled) {
        const auto guard_type = FindMemoryType(
            instance.GetMemoryProperties(),
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            reqs.memoryTypeBits);
        if (guard_type) {
            const vk::MemoryAllocateFlagsInfo alloc_flags = {
                .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
            };
            const vk::MemoryAllocateInfo guard_info = {
                .pNext = &alloc_flags,
                .allocationSize = block_size,
                .memoryTypeIndex = *guard_type,
            };
            const auto alloc = device.allocateMemory(guard_info);
            if (alloc.result == vk::Result::eSuccess) {
                guard_memory = alloc.value;
                const auto map = device.mapMemory(guard_memory, 0, block_size);
                if (map.result == vk::Result::eSuccess) {
                    std::memset(map.value, 0, block_size);
                    device.unmapMemory(guard_memory);
                } else {
                    device.freeMemory(guard_memory);
                    guard_memory = VK_NULL_HANDLE;
                }
            }
        }
    }
}

BufferCache::~BufferCache() {
    if (guard_memory) {
        instance.GetDevice().freeMemory(guard_memory);
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// sotb: games fill their per-frame buffers (constants, skinning output) sequentially, and a
// protection fault per 4 KiB page is expensive. A fault unprotects the aligned window around it
// instead; pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const SotbCopyPool::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const SotbCopyPool::Item& item) {
    // Only the recording thread may Record() or use its copy batch. A draw preparation worker
    // obtaining a buffer copies at once, before the buffer is returned: recording from a worker
    // while the GPU thread handed its chunk to the recorder writes into a null chunk, and a
    // worker's thread-local copy batch could stay unflushed.
    if (!scheduler.OwnsRecording() && scheduler.IsRecordingDeferred()) {
        item.run(item);
        return;
    }
    if (scheduler.IsRecordingDeferred() && !SotbToggle::Disabled(SotbToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    SotbCopyPool::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static constexpr u64 window = 256 * 1024;
    if (window <= TRACKER_BYTES_PER_PAGE || SotbToggle::Disabled(SotbToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        if (!sparse_supported) {
            const VAddr window_start = Common::AlignDown(device_addr, WindowSize);
            DownloadOwnedRange(window_start,
                               std::max<VAddr>(window_start + WindowSize, device_addr + size));
            if (is_write) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
            }
            return;
        }
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

// GPU-modified data only lives in pages that already have an arena.
void BufferCache::DownloadOwnedRange(VAddr start, VAddr end) {
    while (start < end) {
        const u64 page = start >> arena_page_bits;
        const Buffer* arena = address_space[page];
        const VAddr run_end = arena ? std::min<VAddr>(end, arena->cpu_addr + arena->size_bytes)
                                    : std::min<VAddr>(end, (page + 1) << arena_page_bits);
        if (arena) {
            DownloadMemory(arena, start, run_end - start);
        }
        start = run_end;
    }
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferObtain);
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        // sotb: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !SotbToggle::Disabled(SotbToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    const u64 migrations = arena_migrations;
    EnsureResident(arena, first_block, last_block);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (arena_migrations != migrations) {
        arena = FollowMigration(arena, device_addr, size, first_block, last_block);
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferForImage);
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!SotbToggle::Disabled(SotbToggle::DeferredUploads)) {
        // sotb: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const SotbCopyPool::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                SotbCopyPool::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    if (!sparse_supported) {
        return GetResidentArena(first_block, last_block);
    }
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

const Buffer* BufferCache::GetResidentArena(u64 first_block, u64 last_block) {
    struct Ops {
        BufferCache& cache;
        const Buffer* source = nullptr;
        boost::container::small_vector<vk::BufferCopy, 16> copies;
        boost::container::small_vector<Interval, 16> published;

        const Buffer* Create(VAddr cpu_addr, u64 size) {
            std::scoped_lock lock{cache.arenas_mutex};
            return &cache.arenas.emplace_back(cache.instance, cpu_addr, size,
                                              MemoryType::DeviceLocal);
        }
        void Copy(const Buffer& from, const Buffer& to, u64 start, u64 end) {
            if (&from != source) {
                FlushCopies(to);
                source = &from;
                // Shaders also write arenas through the BDA page table, unseen by the tracker.
                cache.runtime.AccessBuffer(source, 0, source->SizeBytes(),
                                           vk::PipelineStageFlagBits2::eAllCommands,
                                           vk::AccessFlagBits2::eMemoryWrite);
            }
            const VAddr addr = start << cache.block_shift;
            const u64 size = (end - start) << cache.block_shift;
            auto* last = copies.empty() ? nullptr : &copies.back();
            if (last && last->srcOffset + last->size == from.Offset(addr)) {
                last->size += size;
                return;
            }
            copies.push_back({from.Offset(addr), to.Offset(addr), size});
        }
        void FlushCopies(const Buffer& to) {
            if (!copies.empty()) {
                cache.runtime.CopyBuffer(source, &to, copies);
                copies.clear();
            }
        }
        void Publish(const Buffer&, u64 start, u64 end) {
            published.push_back({start, end});
        }
        void Retire(const Buffer& arena) {
            cache.retired_arenas.push_back(&arena);
        }
    } ops{*this};

    const size_t retired = retired_arenas.size();
    const u64 max_arena_bytes =
        std::min<u64>(instance.GetMaxBufferSize(), instance.MaxMemoryAllocationSize());
    const Buffer* arena = ArenaLayout::ResidentArenaFor<Buffer>(
        address_space, resident_ranges, arena_page_bits, block_shift, first_block, last_block,
        max_arena_bytes, ops);
    ASSERT_MSG(arena, "No buffer arena for blocks {:#x}-{:#x} (driver limit {} MiB)", first_block,
               last_block, max_arena_bytes >> 20);
    if (retired_arenas.size() != retired) {
        LOG_WARNING(Render, "Migrating arena");
        ops.FlushCopies(*arena);
        PublishArenaBda(arena, {ops.published.data(), ops.published.size()});
        runtime.FlushBarriers();
        ++arena_migrations;
    }
    return arena;
}

// A nested migration (a fault processed while waiting) replaced `stale` after the caller got it.
const Buffer* BufferCache::FollowMigration(const Buffer* stale, VAddr device_addr, u32 size,
                                           u64 first_block, u64 last_block) {
    const Buffer* current = GetArena(first_block, last_block);
    if (current == stale) {
        return current;
    }
    EnsureResident(current, first_block, last_block);
    const vk::BufferCopy copy{stale->Offset(device_addr), current->Offset(device_addr), size};
    runtime.CopyBuffer(stale, current, {&copy, 1});
    const Interval blocks{first_block, last_block + 1};
    PublishArenaBda(current, {&blocks, 1});
    runtime.FlushBarriers();
    return current;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    SotbStats::Timer timer{SotbStats::t_resident};
    const std::span<const Interval> new_ranges{bind_ranges.begin(), bind_ranges.end()};

    if (!sparse_supported) {
        for (const auto& range : new_ranges) {
            resident_ranges.Add(Backing{{range.start, range.end}, {}, 0});
        }
        PublishArenaBda(arena, new_ranges);
        return;
    }

    const auto backing_bytes = BackingMemory::BackingBytes(resident_blocks, block_shift);
    if (!backing_bytes) {
        LOG_ERROR(Render_Vulkan, "{} buffer blocks of {} bytes overflow the allocation size",
                  resident_blocks, block_size);
        return;
    }
    // The arena buffer has a device address, so the memory bound to it needs one too.
    const vk::MemoryAllocateFlagsInfo alloc_flags = {
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    vk::MemoryAllocateInfo alloc_info = {
        .pNext = &alloc_flags,
        .allocationSize = *backing_bytes,
    };
    vk::DeviceMemory device_memory{};
    const auto allocation = BackingMemory::AllocateBacking(backing_memory_types, [&](u32 type) {
        alloc_info.memoryTypeIndex = type;
        return DeviceLost::CallWhileHealthy(
            DeviceLost::Global(), DeviceLost::Operation::AllocateMemory, "arena backing",
            0, [&] {
                const auto [result, memory] = instance.GetDevice().allocateMemory(alloc_info);
                device_memory = memory;
                return result;
            });
    });
    if (allocation.result != BackingMemory::ResultSuccess) {
        if (allocation.result != BackingMemory::ResultDeviceLost) {
            LOG_ERROR(Render_Vulkan, "No memory for {} bytes of buffer pages after {} memory types: {}",
                      alloc_info.allocationSize, allocation.attempts,
                      vk::to_string(vk::Result(allocation.result)));
        }
        return;
    }
    if (allocation.type != arena_memory_type_index) {
        const auto placement = BackingMemory::Classify(memory_type_traits, allocation.type,
                                                       arena_memory_type_index);
        if (backing_telemetry.Record(alloc_info.allocationSize, placement)) {
            LOG_WARNING(Render_Vulkan,
                        "Buffer arena backing of {} KiB placed in memory type {} ({}): the preferred type is "
                        "full; {} fallbacks so far, {} MiB of them in system memory",
                        alloc_info.allocationSize >> 10, allocation.type,
                        placement == BackingMemory::Placement::SystemMemory ? "system memory"
                                                                            : "device local",
                        backing_telemetry.Events(), backing_telemetry.SystemBytes() >> 20);
        }
    }

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset >> block_shift;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;
    }

    // The blocks just outside the new range keep reading zero when a shader steps over the end.
    if (guard_memory) {
        const u64 arena_first = arena->cpu_addr >> block_shift;
        const u64 arena_last = (arena->cpu_addr + arena->size_bytes - 1) >> block_shift;
        const auto guard = [&](u64 block) {
            if (block < arena_first || block > arena_last) {
                return;
            }
            bool resident = false;
            resident_ranges.ForEachInRange(block, block + 1,
                                           [&](const Backing&) { resident = true; });
            if (resident) {
                return;
            }
            binds->binds.push_back(vk::SparseMemoryBind{
                .resourceOffset = (block << block_shift) - arena->cpu_addr,
                .size = block_size,
                .memory = guard_memory,
                .memoryOffset = 0,
            });
        };
        if (first_block > 0) {
            guard(first_block - 1);
        }
        guard(last_block + 1);
    }

    PublishArenaBda(arena, new_ranges);
}

void BufferCache::PublishArenaBda(const Buffer* arena, std::span<const Interval> ranges) {
    u64 num_blocks = 0;
    for (const auto& range : ranges) {
        num_blocks += range.end - range.start;
    }
    if (num_blocks == 0) {
        return;
    }
    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(num_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    const vk::DeviceAddress arena_address = arena->BufferDeviceAddress();
    u64 offset = staging.offset;
    for (const auto& range : ranges) {
        ASSERT_MSG(ArenaLayout::BlocksInsideArena(arena->cpu_addr, arena->size_bytes, range.start,
                                                  range.end, block_shift),
                   "Blocks {:#x}-{:#x} are outside their arena", range.start, range.end);
        for (u64 block = range.start; block < range.end; ++block) {
            *(bda_addrs++) = ArenaLayout::BlockDeviceAddress(arena_address, arena->cpu_addr, block,
                                                             block_shift);
        }
        const u64 copy_size = (range.end - range.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, range.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }
    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    boost::container::small_vector<std::pair<VAddr, u64>, 4> cpu_uploads;
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            cpu_uploads.emplace_back(addr, size);
            total_size_bytes += size;
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    for (const auto& [upload_address, upload_size] : cpu_uploads) {
        texture_cache.RevokeCmaskForCpuUpload(upload_address, upload_size);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    // Resolve before launching host copies: a copy-thread fault could deadlock submission.
    for (const auto& copy : copies) {
        texture_cache.ResolveReadbacks(copy.dstOffset, copy.size, true, ReadbackSource::BufferUpload);
    }
    SotbStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // sotb: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!SotbToggle::Disabled(SotbToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!SotbToggle::Disabled(SotbToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            SotbCopyPool::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        SotbCopyPool::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (const auto meta = texture_cache.GetMetaInfo(device_addr)) {
        if (meta->type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else if (meta->type == TextureCache::MetaType::CMask) {
            // Color images are always fully expanded on the host: every 4-bit CMask tile code
            // reads as the uncompressed value of the target's sample count.
            const auto expanded_value = CmaskColorExpandedValue(meta->num_samples);
            if (!expanded_value) {
                LOG_WARNING(Render_Vulkan,
                            "Unsupported CMask sample count: address={:#x}, size={:#x}, "
                            "samples={}, owner={}:{}",
                            device_addr, size, meta->num_samples, meta->owner_id.index,
                            meta->owner_uid);
                return false;
            }
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, *expanded_value);
            return true;
        } else {
            // Once per type: a game can read metadata through buffers every frame.
            static std::atomic<u32> warned{0};
            const u32 bit = 1u << static_cast<u32>(meta->type);
            if (!(warned.fetch_or(bit) & bit)) {
                LOG_WARNING(Render_Vulkan, "Unhandled metadata type {} (reported once)",
                            magic_enum::enum_name(meta->type));
            }
        }
    }
    // sotb: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !SotbToggle::Disabled(SotbToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // sotb: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::RefreshBinding(const Buffer*& buffer, u64& offset) const {
    std::tie(buffer, offset) = ArenaLayout::CurrentBinding<Buffer>(
        address_space, retired_arenas, arena_page_bits, buffer, offset);
}

void BufferCache::RefreshBinding(vk::Buffer& handle, vk::DeviceSize& offset) const {
    if (!handle) {
        return;
    }
    for (const Buffer* arena : retired_arenas) {
        if (arena->Handle() == handle) {
            const Buffer* current = arena;
            RefreshBinding(current, offset);
            handle = current->Handle();
            return;
        }
    }
}

void BufferCache::RetireMigratedArenas() {
    if (retired_arenas.empty()) {
        return;
    }
    scheduler.DeferOperation([this, retired = std::move(retired_arenas)] {
        std::scoped_lock lock{arenas_mutex};
        for (const Buffer* arena : retired) {
            arenas.remove_if([arena](const Buffer& buffer) { return &buffer == arena; });
        }
    });
    retired_arenas.clear();
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }
    if (DeviceLost::IsLost()) {
        DeviceLost::Global().NoteRefused();
        pending_binds.clear();
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    const s32 submit_result = DeviceLost::CallWhileHealthy(
        DeviceLost::Global(), DeviceLost::Operation::QueueBindSparse, "sparse", signal_tick,
        [&] { return instance.GetGraphicsQueue().bindSparse(sparse_info); }, DeviceLost::Timeline::Memory);
    if (submit_result != DeviceLost::DeviceLostResult) {
        ASSERT_MSG(submit_result == DeviceLost::Success, "Sparse bind failed: {}",
                   vk::to_string(vk::Result(submit_result)));
    }

    pending_binds.clear();
}

} // namespace VideoCore

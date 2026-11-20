// Vulkan recording and synchronization hardening tests: the dynamic rendering scope of the replayed command buffer,
// staging and recording-chunk size, alignment and reuse rules, the bounded device fault report and the places the
// scheduler, semaphore and pools use them. No Vulkan device, no window.
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "video_core/renderer_vulkan/vk_fault_report.h"
#include "video_core/renderer_vulkan/vk_recording_state.h"
#include "video_core/renderer_vulkan/vk_staging_policy.h"

namespace {

using namespace Vulkan;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

std::string ReadSource(const std::string& relative) {
    std::ifstream file(std::string(SOTB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

constexpr std::uint64_t KiB = 1024;
constexpr std::uint64_t MiB = 1024 * KiB;

void TestRenderingScope() {
    RenderingScope scope;
    CHECK(!scope.IsOpen());
    CHECK(!scope.End());
    CHECK(!scope.Begin());
    CHECK(scope.IsOpen());
    CHECK(scope.Begin());
    CHECK(scope.IsOpen());
    CHECK(scope.End());
    CHECK(!scope.IsOpen());
    CHECK(!scope.End());

    scope.Begin();
    scope.Reset();
    CHECK(!scope.IsOpen());
    CHECK(!scope.Begin());

    // A replayed stream that is unbalanced still ends with every end matched to an open instance.
    RenderingScope replay;
    int begins = 0, ends = 0, forced_ends = 0;
    const char stream[] = {'B', 'B', 'E', 'E', 'E', 'B'};
    for (const char op : stream) {
        if (op == 'B') {
            if (replay.Begin()) {
                ++forced_ends;
                ++ends;
            }
            ++begins;
        } else if (replay.End()) {
            ++ends;
        }
    }
    CHECK(forced_ends == 1);
    CHECK(replay.IsOpen());
    CHECK(begins - ends == 1);
}

void TestStagingRequestValidation() {
    using namespace StagingPolicy;
    CHECK(IsValidRequest(0, 0));
    CHECK(IsValidRequest(1, 1));
    CHECK(IsValidRequest(MaxRequestBytes, 256));
    CHECK(!IsValidRequest(MaxRequestBytes + 1, 0));
    CHECK(!IsValidRequest(std::numeric_limits<std::uint64_t>::max(), 0));
    CHECK(!IsValidRequest(64, MaxAlignment + 1));
    CHECK(IsValidRequest(64, MaxAlignment));
    CHECK(NormalizeAlignment(0) == 1u);
    CHECK(NormalizeAlignment(1) == 1u);
    CHECK(NormalizeAlignment(12) == 12u);
    CHECK(NormalizeAlignment(256) == 256u);
    CHECK(!NormalizeAlignment(MaxAlignment + 1).has_value());
}

void TestRingBlockFit() {
    using namespace StagingPolicy;
    const std::uint64_t block = 16 * MiB;
    CHECK(FitsRingBlock(0, 0, block));
    CHECK(FitsRingBlock(1 * MiB, 64, block));
    CHECK(FitsRingBlock(block, 0, block));
    CHECK(FitsRingBlock(block, 1, block));
    CHECK(FitsRingBlock(block, 64, block));
    CHECK(FitsRingBlock(block - 63, 64, block));
    CHECK(!FitsRingBlock(block, 48, block));
    CHECK(!FitsRingBlock(block - 1, 48, block));
    CHECK(FitsRingBlock(block - 48, 48, block));
    CHECK(!FitsRingBlock(block + 1, 0, block));
    CHECK(!FitsRingBlock(std::numeric_limits<std::uint64_t>::max(), 64, block));
    CHECK(!FitsRingBlock(std::numeric_limits<std::uint64_t>::max() - 10, 4096, block));
}

void TestHeldBucketSize() {
    using namespace StagingPolicy;
    CHECK(HeldBucketSize(0, 256 * KiB) == 256 * KiB);
    CHECK(HeldBucketSize(1, 256 * KiB) == 256 * KiB);
    CHECK(HeldBucketSize(256 * KiB, 256 * KiB) == 256 * KiB);
    CHECK(HeldBucketSize(256 * KiB + 1, 256 * KiB) == 512 * KiB);
    CHECK(HeldBucketSize(3 * MiB, 256 * KiB) == 4 * MiB);
    CHECK(HeldBucketSize(MaxRequestBytes, 256 * KiB) == MaxRequestBytes);
    CHECK(!HeldBucketSize(MaxRequestBytes + 1, 256 * KiB).has_value());
    CHECK(!HeldBucketSize(std::numeric_limits<std::uint64_t>::max(), 256 * KiB).has_value());
    CHECK(!HeldBucketSize((std::uint64_t{1} << 63) + 1, 256 * KiB).has_value());
}

void TestNoReuseBeforeCompletion() {
    using namespace StagingPolicy;
    const std::uint64_t want = 1 * MiB, max = 2 * MiB - 1;
    CHECK(CanReuseLarge({1 * MiB, false, true}, want, max));
    CHECK(!CanReuseLarge({1 * MiB, true, true}, want, max));
    CHECK(!CanReuseLarge({1 * MiB, false, false}, want, max));
    CHECK(!CanReuseLarge({1 * MiB, true, false}, want, max));
    CHECK(!CanReuseLarge({want - 1, false, true}, want, max));
    CHECK(!CanReuseLarge({max + 1, false, true}, want, max));
    CHECK(CanReuseLarge({max, false, true}, want, max));
}

void TestChunkPlacement() {
    using namespace StagingPolicy;
    constexpr std::size_t capacity = 128 * 1024;
    CHECK(ChunkOffset(0, 16, 8, capacity) == 0u);
    CHECK(ChunkOffset(1, 16, 8, capacity) == 8u);
    CHECK(ChunkOffset(8, 16, 8, capacity) == 8u);
    CHECK(ChunkOffset(capacity - 16, 16, 8, capacity) == capacity - 16);
    CHECK(!ChunkOffset(capacity - 15, 16, 8, capacity).has_value());
    CHECK(!ChunkOffset(capacity, 1, 1, capacity).has_value());
    CHECK(ChunkOffset(capacity, 0, 1, capacity) == capacity);
    CHECK(!ChunkOffset(0, 16, 0, capacity).has_value());
    CHECK(!ChunkOffset(0, 16, 3, capacity).has_value());
    CHECK(!ChunkOffset(capacity + 1, 0, 1, capacity).has_value());

    const std::size_t max = std::numeric_limits<std::size_t>::max();
    CHECK(!ChunkOffset(0, max, 8, capacity).has_value());
    CHECK(!ChunkOffset(16, max - 8, 8, capacity).has_value());
    CHECK(!ChunkOffset(max, 8, 8, capacity).has_value());
    CHECK(!ChunkOffset(capacity - 4, 8, 4096, capacity).has_value());

    std::size_t used = 0;
    int placed = 0;
    while (const auto offset = ChunkOffset(used, 24, 8, capacity)) {
        CHECK(*offset % 8 == 0 && *offset >= used);
        used = *offset + 24;
        ++placed;
    }
    CHECK(used <= capacity && placed == int(capacity / 24));
}

FaultReport CapturedReport() {
    FaultReport report;
    report.status = FaultStatus::Captured;
    report.result = 0;
    report.description = "GPU hang";
    report.address_count = 2;
    report.vendor_count = 1;
    report.addresses = {{1, 0x1000, 0x10}, {2, 0xdeadbeefull, 0}};
    report.vendors = {{0x42, 0x7, "shader stall"}};
    return report;
}

void TestFaultReportOptional() {
    const FaultReport none;
    CHECK(none.status == FaultStatus::Unsupported);
    const auto lines = FormatFaultReport(none);
    CHECK(lines.size() == 1);
    CHECK(lines[0] == "Device fault report unavailable (no VK_EXT_device_fault)");

    FaultReport failed;
    failed.status = FaultStatus::Failed;
    failed.result = -4;
    const auto failed_lines = FormatFaultReport(failed);
    CHECK(failed_lines.size() == 1);
    CHECK(failed_lines[0] == "Device fault report failed (result -4)");
}

void TestFaultReportFormatting() {
    const auto lines = FormatFaultReport(CapturedReport());
    CHECK(lines.size() == 4);
    CHECK(lines[0] == "Device fault: GPU hang");
    CHECK(lines[1] == "Fault address type 1 at 0x1000 (precision 0x10)");
    CHECK(lines[2] == "Fault address type 2 at 0xdeadbeef (precision 0)");
    CHECK(lines[3] == "Vendor fault 0x42 data 0x7: shader stall");
    CHECK(FormatFaultReport(CapturedReport()) == lines);

    auto truncated = CapturedReport();
    truncated.address_count = 100;
    truncated.vendor_count = 50;
    const auto truncated_lines = FormatFaultReport(truncated);
    CHECK(truncated_lines.size() == 5);
    CHECK(truncated_lines.back() == "Device fault: 100 address and 50 vendor entries, first 2 and 1 shown");
}

void TestFaultReportBounds() {
    CHECK(BoundedFaultCount(0, MaxFaultAddresses) == 0);
    CHECK(BoundedFaultCount(3, MaxFaultAddresses) == 3);
    CHECK(BoundedFaultCount(MaxFaultAddresses, MaxFaultAddresses) == MaxFaultAddresses);
    CHECK(BoundedFaultCount(0xffffffffu, MaxFaultAddresses) == MaxFaultAddresses);
    CHECK(BoundedFaultCount(0xffffffffu, MaxVendorFaults) == MaxVendorFaults);

    const char plain[] = "out of memory";
    CHECK(SanitizeDriverText(plain, sizeof(plain)) == "out of memory");
    char unterminated[8];
    for (char& c : unterminated) {
        c = 'x';
    }
    CHECK(SanitizeDriverText(unterminated, sizeof(unterminated)) == "xxxxxxxx");
    const char binary[] = {'a', char(0x01), char(0xff), 'b', '\0', 'c'};
    CHECK(SanitizeDriverText(binary, sizeof(binary)) == "a??b");
    std::vector<char> huge(MaxFaultTextBytes * 4, 'z');
    CHECK(SanitizeDriverText(huge.data(), huge.size()).size() == MaxFaultTextBytes);
    CHECK(SanitizeDriverText("", 1).empty());

    FaultReport report;
    report.status = FaultStatus::Captured;
    report.description = SanitizeDriverText(huge.data(), huge.size());
    for (std::uint32_t i = 0; i < MaxFaultAddresses; ++i) {
        report.addresses.push_back({i, i, i});
    }
    for (std::uint32_t i = 0; i < MaxVendorFaults; ++i) {
        report.vendors.push_back({i, i, SanitizeDriverText(huge.data(), huge.size())});
    }
    report.address_count = MaxFaultAddresses;
    report.vendor_count = MaxVendorFaults;
    const auto lines = FormatFaultReport(report);
    CHECK(lines.size() == 1 + MaxFaultAddresses + MaxVendorFaults);
    std::size_t total = 0;
    for (const auto& line : lines) {
        total += line.size();
    }
    CHECK(total < 8 * KiB);
}

void TestDeviceLostCleanupPaths() {
    const auto instance = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    const auto query = instance.find("FaultReport Instance::QueryDeviceFault() const");
    CHECK(query != std::string::npos);
    const auto available = instance.find("device_fault && VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT", query);
    const auto collect = instance.find("CollectFaultReport(", query);
    const auto first_call = instance.find("getFaultInfoEXT", query);
    CHECK(available != std::string::npos && available < collect && collect < first_call);
    CHECK(Contains(instance, "std::array<vk::DeviceFaultAddressInfoEXT, MaxFaultAddresses>"));
    CHECK(Contains(instance, "device_fault = feature_chain.get<vk::PhysicalDeviceFaultFeaturesEXT>().deviceFault"));
    CHECK(Contains(instance, "add_extension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)"));
    const auto fault_header = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_fault_report.h");
    CHECK(Contains(fault_header, "BoundedFaultCount(counts.addresses, MaxFaultAddresses)"));
    CHECK(Contains(fault_header, "BoundedFaultCount(counts.vendors, MaxVendorFaults)"));

    const auto semaphore = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp");
    const auto refresh = semaphore.find("void Semaphore::Refresh()");
    const auto read = semaphore.find("DeviceLost::Operation::GetSemaphoreCounterValue", refresh);
    const auto lost = semaphore.find("result == DeviceLost::DeviceLostResult", refresh);
    const auto assert_at = semaphore.find("Failed to get master semaphore value", refresh);
    CHECK(read != std::string::npos && read < lost && lost < assert_at);
    CHECK(Contains(semaphore, "DeviceLost::Operation::WaitSemaphores"));

    const auto buffer_cache = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    const auto bind = buffer_cache.find("bindSparse(sparse_info)");
    CHECK(bind != std::string::npos);
    CHECK(buffer_cache.rfind("CallWhileHealthy", bind) != std::string::npos);
    CHECK(buffer_cache.find("DeviceLost::DeviceLostResult", bind) < buffer_cache.find("Sparse bind failed", bind));
}

void TestSubmissionAndRecordingWiring() {
    const auto scheduler = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp");
    const auto header = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h");

    const auto submit = scheduler.find("GetGraphicsQueue().submit(submit_info, info.fence)");
    CHECK(submit != std::string::npos);
    const auto lost = scheduler.find("submit_result == DeviceLost::DeviceLostResult", submit);
    const auto failed_assert = scheduler.find("Queue submit failed", submit);
    const auto refresh = scheduler.find("work_semaphore.Refresh()", submit);
    CHECK(lost != std::string::npos && lost < failed_assert && failed_assert < refresh);

    CHECK(Contains(scheduler, "replay_rendering.Begin()"));
    CHECK(Contains(scheduler, "replay_rendering.End()"));
    CHECK(Contains(scheduler, "replay_rendering.IsOpen()"));
    CHECK(Contains(header, "RenderingScope replay_rendering"));

    const auto submit_exec = scheduler.find("void Scheduler::SubmitExecution");
    const auto sync = scheduler.find("SyncRecording()", submit_exec);
    const auto host_copies = scheduler.find("WaitHostCopies()", submit_exec);
    const auto end_cmd = scheduler.find("current_cmdbuf.end()", submit_exec);
    CHECK(sync < host_copies && host_copies < end_cmd);

    const auto hand_off = scheduler.find("void Scheduler::KickRecording");
    const auto exchange = scheduler.find("std::exchange(record_chunk, AcquireChunk())", hand_off);
    const auto push = scheduler.find("recorder_queue.push_back(std::move(filled))", hand_off);
    CHECK(exchange != std::string::npos && push != std::string::npos && exchange < push);

    CHECK(Contains(header, "StagingPolicy::ChunkOffset(used, sizeof(Command), alignof(Command), Capacity)"));
    CHECK(Contains(header, "StagingPolicy::ChunkOffset(used, bytes, align, Capacity)"));
    CHECK(Contains(header, "bytes > RecordChunk::Capacity - 1024"));
    CHECK(Contains(header, "[this, info = rendering_info") || Contains(scheduler, "[this, info = rendering_info"));
}

void TestStagingPoolWiring() {
    const auto pool = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp");
    CHECK(Contains(pool, "StagingPolicy::IsValidRequest(size, alignment)"));
    CHECK(Contains(pool, "StagingPolicy::FitsRingBlock(size, instance.NonCoherentAtomSize(), BLOCK_SIZE)"));
    CHECK(Contains(pool, "StagingPolicy::HeldBucketSize(size, 256_KB)"));
    CHECK(Contains(pool, "StagingPolicy::CanReuseLarge("));
    CHECK(!Contains(pool, "*best->buffer->Reserve("));
    CHECK(Contains(pool, "return RequestLarge(size, type, false)"));
    const auto free_deferred = pool.find("void StagingBufferPool::FreeDeferred");
    const auto lock = pool.find("std::scoped_lock lock{large_mutex}", free_deferred);
    CHECK(free_deferred != std::string::npos && lock != std::string::npos && lock - free_deferred < 120);
    CHECK(Contains(pool, "FreeDeferred on an allocation that is not held"));
}

} // namespace

int main() {
    TestRenderingScope();
    TestStagingRequestValidation();
    TestRingBlockFit();
    TestHeldBucketSize();
    TestNoReuseBeforeCompletion();
    TestChunkPlacement();
    TestFaultReportOptional();
    TestFaultReportFormatting();
    TestFaultReportBounds();
    TestDeviceLostCleanupPaths();
    TestSubmissionAndRecordingWiring();
    TestStagingPoolWiring();
    std::printf("gpu recording tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

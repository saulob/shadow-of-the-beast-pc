// SPDX-License-Identifier: GPL-2.0-or-later
// A fixed-size, lock-free history of the latest GPU operations, for the incident that follows a device loss.
// Vulkan-independent so it can be tested headless.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Vulkan::DeviceLost {

// Each timeline semaphore counts its own ticks: a tick means something only together with its timeline.
enum class Timeline : std::uint8_t { None, Work, Memory, Count };

inline const char* TimelineName(Timeline timeline) noexcept {
    switch (timeline) {
    case Timeline::Work:
        return "work";
    case Timeline::Memory:
        return "memory";
    case Timeline::None:
    case Timeline::Count:
        break;
    }
    return "none";
}

inline constexpr std::size_t TimelineCapacity = 128;
inline constexpr std::size_t StageBytes = 8;

struct TimelineEntry {
    std::uint64_t sequence{};
    std::uint32_t kind{};
    std::int32_t result{};
    std::uint64_t tick{};
    std::uint32_t thread{};
    Timeline timeline{};
    char stage[StageBytes + 1]{};
};

class EventRing {
public:
    // Oldest entries are overwritten. Returns the event's sequence number, starting at 1.
    std::uint64_t Record(std::uint32_t kind, const char* stage, std::int32_t result, std::uint64_t tick,
                         Timeline timeline = Timeline::None) noexcept {
        const std::uint64_t sequence = next.fetch_add(1, std::memory_order_relaxed) + 1;
        Slot& slot = slots[(sequence - 1) % TimelineCapacity];
        slot.sequence.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.kind_result.store(std::uint64_t(kind) | (std::uint64_t(std::uint32_t(result)) << 32),
                               std::memory_order_relaxed);
        slot.tick.store(tick, std::memory_order_relaxed);
        slot.thread_timeline.store(std::uint64_t(CurrentThread()) | (std::uint64_t(timeline) << 32),
                                   std::memory_order_relaxed);
        slot.stage.store(PackStage(stage), std::memory_order_relaxed);
        slot.sequence.store(sequence, std::memory_order_release);
        return sequence;
    }

    // Copies the retained entries, oldest first, into `out` (never allocates) and returns how many. An entry being
    // overwritten at that moment is left out.
    std::size_t Snapshot(TimelineEntry* out, std::size_t capacity) const noexcept {
        const std::uint64_t last = next.load(std::memory_order_acquire);
        const std::uint64_t first = last > TimelineCapacity ? last - TimelineCapacity + 1 : 1;
        std::size_t count = 0;
        for (std::uint64_t sequence = first; sequence <= last && count < capacity; ++sequence) {
            if (Read(sequence, out[count])) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::uint64_t Recorded() const noexcept {
        return next.load(std::memory_order_relaxed);
    }

private:
    struct Slot {
        std::atomic<std::uint64_t> sequence{};
        std::atomic<std::uint64_t> kind_result{};
        std::atomic<std::uint64_t> tick{};
        std::atomic<std::uint64_t> thread_timeline{};
        std::atomic<std::uint64_t> stage{};
    };

    static std::uint32_t CurrentThread() noexcept {
        static std::atomic<std::uint32_t> counter{};
        thread_local const std::uint32_t id = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        return id;
    }

    static std::uint64_t PackStage(const char* text) noexcept {
        char bytes[StageBytes]{};
        for (std::size_t i = 0; text && i < StageBytes && text[i]; ++i) {
            bytes[i] = text[i];
        }
        std::uint64_t packed;
        std::memcpy(&packed, bytes, sizeof(packed));
        return packed;
    }

    bool Read(std::uint64_t sequence, TimelineEntry& out) const noexcept {
        const Slot& slot = slots[(sequence - 1) % TimelineCapacity];
        if (slot.sequence.load(std::memory_order_acquire) != sequence) {
            return false;
        }
        const std::uint64_t kind_result = slot.kind_result.load(std::memory_order_relaxed);
        const std::uint64_t tick = slot.tick.load(std::memory_order_relaxed);
        const std::uint64_t thread_timeline = slot.thread_timeline.load(std::memory_order_relaxed);
        const std::uint64_t stage = slot.stage.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_relaxed) != sequence) {
            return false;
        }
        out.sequence = sequence;
        out.kind = std::uint32_t(kind_result);
        out.result = std::int32_t(std::uint32_t(kind_result >> 32));
        out.tick = tick;
        out.thread = std::uint32_t(thread_timeline);
        out.timeline = Timeline(std::uint8_t(thread_timeline >> 32));
        std::memcpy(out.stage, &stage, StageBytes);
        out.stage[StageBytes] = 0;
        return true;
    }

    std::atomic<std::uint64_t> next{};
    std::array<Slot, TimelineCapacity> slots{};
};

} // namespace Vulkan::DeviceLost

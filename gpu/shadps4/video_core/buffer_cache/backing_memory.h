// SPDX-License-Identifier: GPL-2.0-or-later
// Which memory types an arena's sparse backing may be allocated from when the preferred one is full, and what the
// fallback is worth logging; free of Vulkan so it can be tested headless.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace VideoCore::BackingMemory {

inline constexpr std::int32_t ResultSuccess = 0;
inline constexpr std::int32_t ResultOutOfHostMemory = -1;
inline constexpr std::int32_t ResultOutOfDeviceMemory = -2;
inline constexpr std::int32_t ResultDeviceLost = -4;
inline constexpr std::uint32_t MaxMemoryTypes = 32;

struct TypeTraits {
    bool device_local{};
    // Lazily allocated, protected and device-coherent types never back a general buffer.
    bool excluded{};
};

enum class Placement { PreferredDeviceLocal, OtherDeviceLocal, SystemMemory };

/// The preferred type, then the other device-local types, then the non-device-local ones; each valid type once, in
/// index order inside a group.
inline std::vector<std::uint32_t> CandidateTypes(std::span<const TypeTraits> types, std::uint32_t allowed_bits,
                                                 std::optional<std::uint32_t> preferred) {
    const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(types.size(), MaxMemoryTypes));
    const auto usable = [&](std::uint32_t type) {
        return type < count && ((allowed_bits >> type) & 1) != 0 && !types[type].excluded;
    };
    std::vector<std::uint32_t> candidates;
    if (preferred && usable(*preferred)) {
        candidates.push_back(*preferred);
    }
    for (const bool device_local : {true, false}) {
        for (std::uint32_t type = 0; type < count; ++type) {
            if (usable(type) && types[type].device_local == device_local &&
                !(preferred && *preferred == type)) {
                candidates.push_back(type);
            }
        }
    }
    return candidates;
}

inline Placement Classify(std::span<const TypeTraits> types, std::uint32_t selected,
                          std::optional<std::uint32_t> preferred) {
    if (preferred && selected == *preferred) {
        return Placement::PreferredDeviceLocal;
    }
    return selected < types.size() && types[selected].device_local ? Placement::OtherDeviceLocal
                                                                   : Placement::SystemMemory;
}

struct Allocation {
    std::int32_t result = ResultOutOfDeviceMemory;
    std::uint32_t type = 0;
    std::uint32_t attempts = 0;
};

/// Each candidate is tried once, in order. Only a full device memory moves on to the next type: a lost device, a full
/// host or anything else ends the search with that result. `allocate(type)` returns a Vulkan result.
template <typename Allocate>
Allocation AllocateBacking(std::span<const std::uint32_t> candidates, Allocate&& allocate) {
    Allocation allocation;
    for (const std::uint32_t type : candidates) {
        ++allocation.attempts;
        allocation.type = type;
        allocation.result = allocate(type);
        if (allocation.result != ResultOutOfDeviceMemory) {
            break;
        }
    }
    return allocation;
}

/// Bytes of `blocks` blocks of 1 << `block_shift` bytes, or nothing when they do not fit 64 bits.
constexpr std::optional<std::uint64_t> BackingBytes(std::uint64_t blocks, std::uint32_t block_shift) {
    if (block_shift >= 64 || blocks > (UINT64_MAX >> block_shift)) {
        return std::nullopt;
    }
    return blocks << block_shift;
}

/// Counts every fallback; asks for a log line for the first few and then for every 64th, so a game that keeps running
/// with a full card does not flood the log.
class FallbackTelemetry {
public:
    static constexpr std::uint64_t LoggedFirst = 4;
    static constexpr std::uint64_t LogEvery = 64;

    bool Record(std::uint64_t bytes, Placement placement) noexcept {
        const std::uint64_t index = events.fetch_add(1, std::memory_order_relaxed);
        total_bytes.fetch_add(bytes, std::memory_order_relaxed);
        if (placement == Placement::SystemMemory) {
            system_events.fetch_add(1, std::memory_order_relaxed);
            system_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        return index < LoggedFirst || index % LogEvery == 0;
    }

    [[nodiscard]] std::uint64_t Events() const noexcept {
        return events.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t TotalBytes() const noexcept {
        return total_bytes.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t SystemEvents() const noexcept {
        return system_events.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t SystemBytes() const noexcept {
        return system_bytes.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> events{};
    std::atomic<std::uint64_t> total_bytes{};
    std::atomic<std::uint64_t> system_events{};
    std::atomic<std::uint64_t> system_bytes{};
};

} // namespace VideoCore::BackingMemory

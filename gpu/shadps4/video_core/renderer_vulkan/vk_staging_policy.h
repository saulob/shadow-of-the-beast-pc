// SPDX-License-Identifier: GPL-2.0-or-later
// Vulkan-independent size, alignment and reuse rules of staging memory and recording chunks.

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Vulkan::StagingPolicy {

/// No real staging need approaches this; larger is a corrupt size.
constexpr std::uint64_t MaxRequestBytes = std::uint64_t{4} << 30;
constexpr std::uint64_t MaxAlignment = std::uint64_t{1} << 20;

/// 0 and 1 mean no alignment; texel sizes such as 12 bytes are not powers of two. Null when
/// the alignment is absurd.
inline std::optional<std::uint64_t> NormalizeAlignment(std::uint64_t alignment) {
    if (alignment > MaxAlignment) {
        return std::nullopt;
    }
    return std::max<std::uint64_t>(alignment, 1);
}

inline bool IsValidRequest(std::uint64_t size, std::uint64_t alignment) {
    return size <= MaxRequestBytes && NormalizeAlignment(alignment).has_value();
}

/// Whether a request fits a fresh, empty ring block of `block_size` (offset 0 satisfies any
/// alignment) once non-coherent memory rounds it up to `atom_size`. A request that does not fit
/// must take the dedicated path: a fresh block would still refuse it.
inline bool FitsRingBlock(std::uint64_t size, std::uint64_t atom_size, std::uint64_t block_size) {
    if (size > block_size) {
        return false;
    }
    if (atom_size <= 1) {
        return true;
    }
    const std::uint64_t rounded = (size + atom_size - 1) / atom_size * atom_size;
    return size <= UINT64_MAX - atom_size && rounded <= block_size;
}

/// Size of a held (readback) buffer: the next power of two, at least `minimum`. Null when the
/// request is invalid, so the rounding can never overflow.
inline std::optional<std::uint64_t> HeldBucketSize(std::uint64_t size, std::uint64_t minimum) {
    if (size > MaxRequestBytes) {
        return std::nullopt;
    }
    return std::bit_ceil(std::max(size, minimum));
}

/// A cached large buffer may serve a request only when no recorded work still uses it and its
/// size is within the reuse window.
struct LargeEntry {
    std::uint64_t size;
    bool held;
    bool tick_complete;
};

inline bool CanReuseLarge(const LargeEntry& entry, std::uint64_t min_size, std::uint64_t max_size) {
    return !entry.held && entry.tick_complete && entry.size >= min_size && entry.size <= max_size;
}

/// Where `bytes` at `align` would start in a chunk of `capacity` with `used` bytes taken; null
/// when it does not fit. Written so no sum can wrap.
inline std::optional<std::size_t> ChunkOffset(std::size_t used, std::size_t bytes, std::size_t align,
                                              std::size_t capacity) {
    if (align == 0 || !std::has_single_bit(align) || used > capacity) {
        return std::nullopt;
    }
    const std::size_t pad = (align - (used & (align - 1))) & (align - 1);
    if (pad > capacity - used) {
        return std::nullopt;
    }
    const std::size_t offset = used + pad;
    if (bytes > capacity - offset) {
        return std::nullopt;
    }
    return offset;
}

} // namespace Vulkan::StagingPolicy

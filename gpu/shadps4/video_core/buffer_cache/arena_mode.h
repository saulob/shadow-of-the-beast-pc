// SPDX-License-Identifier: GPL-2.0-or-later
// Whether the buffer cache can use sparse arenas; free of Vulkan so it can be tested headless.
#pragma once

#include <string_view>

namespace VideoCore::ArenaLayout {

struct SparseCapabilities {
    bool sparse_binding = false;
    bool sparse_residency_buffer = false;
    bool queue_sparse_binding = false;
};

enum class ArenaMode { Sparse, FullyResident };

struct ArenaDecision {
    ArenaMode mode;
    std::string_view reason;
};

// Anything short of the whole sparse path selects the fully resident arenas.
constexpr ArenaDecision DecideArenaMode(const SparseCapabilities& caps) {
    if (!caps.sparse_binding) {
        return {ArenaMode::FullyResident, "sparseBinding is unavailable"};
    }
    if (!caps.sparse_residency_buffer) {
        return {ArenaMode::FullyResident, "sparseResidencyBuffer is unavailable"};
    }
    if (!caps.queue_sparse_binding) {
        return {ArenaMode::FullyResident, "the graphics queue cannot bind sparse memory"};
    }
    return {ArenaMode::Sparse, "sparse binding and buffer residency are available"};
}

constexpr const char* ModeName(ArenaMode mode) {
    return mode == ArenaMode::Sparse ? "sparse" : "fully resident fallback";
}

} // namespace VideoCore::ArenaLayout

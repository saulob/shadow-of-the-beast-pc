// SPDX-License-Identifier: GPL-2.0-or-later
// Replay-side state of the deferred command recording (vk_scheduler.h).

#pragma once

#include <utility>

namespace Vulkan {

/// Whether the command buffer being replayed has an open dynamic rendering instance. Only the
/// thread replaying into the command buffer uses it.
class RenderingScope {
public:
    /// True when an instance is still open and must be ended before this begin.
    bool Begin() noexcept {
        return std::exchange(open, true);
    }

    /// True when an instance is open, i.e. the end is valid.
    bool End() noexcept {
        return std::exchange(open, false);
    }

    [[nodiscard]] bool IsOpen() const noexcept {
        return open;
    }

    void Reset() noexcept {
        open = false;
    }

private:
    bool open = false;
};

} // namespace Vulkan

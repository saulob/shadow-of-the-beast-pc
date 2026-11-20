// SPDX-License-Identifier: GPL-2.0-or-later
// What a swapchain acquire or present result asks of the presenter, as a small state machine with no Vulkan in it
// (tests/swapchain_state_tests.cpp). VK_ERROR_DEVICE_LOST never reaches it: that result belongs to the global
// device-lost owner, and nothing is recreated after it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "sotb_log_gate.h"

namespace SotbSwapchain {

using SotbLog::LogGate;

enum class Result : std::uint8_t { Success, Suboptimal, OutOfDate, SurfaceLost, Failure };

// Values of VkResult; vk_swapchain.cpp asserts them against the Vulkan headers.
inline constexpr int kVkSuccess = 0;
inline constexpr int kVkErrorDeviceLost = -4;
inline constexpr int kVkErrorUnknown = -13;
inline constexpr int kVkErrorSurfaceLost = -1000000000;
inline constexpr int kVkSuboptimal = 1000001003;
inline constexpr int kVkErrorOutOfDate = -1000001004;

// Some drivers answer a vanished window with VK_ERROR_UNKNOWN on acquire only.
constexpr Result ClassifyAcquire(int vk_result) {
    switch (vk_result) {
    case kVkSuccess:
        return Result::Success;
    case kVkSuboptimal:
        return Result::Suboptimal;
    case kVkErrorOutOfDate:
    case kVkErrorUnknown:
        return Result::OutOfDate;
    case kVkErrorSurfaceLost:
        return Result::SurfaceLost;
    default:
        return Result::Failure;
    }
}

constexpr Result ClassifyPresent(int vk_result) {
    return vk_result == kVkErrorUnknown ? Result::Failure : ClassifyAcquire(vk_result);
}

// Why a swapchain was created again. Suboptimal, OutOfDate and SurfaceLost come from results; the rest are changes
// the presenter made itself.
enum class Reason : std::uint8_t { None, Suboptimal, OutOfDate, SurfaceLost, Resize, PresentMode, Hdr, Count };

inline const char* ReasonName(Reason reason) {
    switch (reason) {
    case Reason::Suboptimal:
        return "suboptimal";
    case Reason::OutOfDate:
        return "out of date";
    case Reason::SurfaceLost:
        return "surface lost";
    case Reason::Resize:
        return "resize";
    case Reason::PresentMode:
        return "present mode";
    case Reason::Hdr:
        return "HDR";
    case Reason::None:
    case Reason::Count:
        break;
    }
    return "requested";
}

struct Counters {
    std::uint32_t suboptimal = 0;      // suboptimal results seen
    std::uint32_t suboptimal_kept = 0; // ... that were not acted on: the swapchain already replaced a suboptimal one
    std::uint32_t out_of_date = 0;
    std::uint32_t surface_lost = 0;
    std::uint32_t display_changes = 0;
    std::uint32_t recreations = 0; // swapchains created after the first
    std::array<std::uint32_t, std::size_t(Reason::Count)> by_reason{};
};

// Suboptimal is a success code: the image is usable and the frame goes on. It asks for one recreation. A replacement
// that is suboptimal again is kept until a clean frame, or a surface that always reports it would get a new swapchain
// every frame. Out of date and a lost surface are never held back, and a recreation the presenter forces (resize,
// present mode, HDR) or a display change counts as evidence that the next suboptimal result may be acted on again.
class Health {
public:
    // Whether the acquired image can be used.
    bool Acquire(Result result) {
        clean_acquire_ = result == Result::Success;
        return Observe(result);
    }

    // Whether the image was queued for display.
    bool Present(Result result) {
        const bool queued = Observe(result);
        if (queued && result == Result::Success && clean_acquire_) {
            suppress_ = false;
        }
        return queued;
    }

    // A new swapchain is in place.
    void Recreated(Reason forced = Reason::None) {
        if (stopped_) {
            return;
        }
        Reason effective = pending_;
        if (forced != Reason::None && (pending_ == Reason::None || pending_ == Reason::Suboptimal)) {
            effective = forced;
        }
        suppress_ = pending_ == Reason::Suboptimal && forced == Reason::None;
        pending_ = Reason::None;
        clean_acquire_ = false;
        if (created_) {
            ++counters_.recreations;
            ++counters_.by_reason[std::size_t(effective)];
        }
        created_ = true;
        last_reason_ = effective;
    }

    // No swapchain could be created (a window with no pixels): the next frame tries again.
    void CreateFailed() {
        if (!stopped_) {
            Raise(Reason::OutOfDate);
            suppress_ = false;
        }
    }

    void DisplayChanged() {
        if (!stopped_) {
            ++counters_.display_changes;
            suppress_ = false;
        }
    }

    void Stop() {
        stopped_ = true;
        pending_ = Reason::None;
        suppress_ = false;
    }

    [[nodiscard]] bool NeedsRecreation() const { return pending_ != Reason::None; }
    [[nodiscard]] bool NeedsNewSurface() const { return pending_ == Reason::SurfaceLost; }
    [[nodiscard]] Reason Pending() const { return pending_; }
    [[nodiscard]] Reason LastReason() const { return last_reason_; }
    [[nodiscard]] bool Suppressing() const { return suppress_; }
    [[nodiscard]] bool Stopped() const { return stopped_; }
    [[nodiscard]] const Counters& Stats() const { return counters_; }

    // States that cannot occur: a suppressed suboptimal state with a suboptimal recreation pending, a stopped state
    // that still asks for work.
    [[nodiscard]] bool Consistent() const {
        return !(suppress_ && pending_ == Reason::Suboptimal) && !(stopped_ && pending_ != Reason::None);
    }

private:
    bool Observe(Result result) {
        if (stopped_) {
            return false;
        }
        switch (result) {
        case Result::Success:
            return true;
        case Result::Suboptimal:
            ++counters_.suboptimal;
            if (suppress_) {
                ++counters_.suboptimal_kept;
            } else {
                Raise(Reason::Suboptimal);
            }
            return true;
        case Result::OutOfDate:
            ++counters_.out_of_date;
            Raise(Reason::OutOfDate);
            return false;
        case Result::SurfaceLost:
            ++counters_.surface_lost;
            Raise(Reason::SurfaceLost);
            return false;
        case Result::Failure:
            break;
        }
        return false;
    }

    // A pending recreation only gets more severe.
    void Raise(Reason reason) {
        if (std::uint8_t(reason) > std::uint8_t(pending_)) {
            pending_ = reason;
        }
    }

    Reason pending_ = Reason::None;
    Reason last_reason_ = Reason::None;
    bool suppress_ = false; // this swapchain replaced a suboptimal one and stays even if suboptimal
    bool clean_acquire_ = false;
    bool created_ = false;
    bool stopped_ = false;
    Counters counters_;
};

enum class FrameAction : std::uint8_t { Skip, Keep, Recreate };

// What the presenter does before a frame. A window with no pixels (minimized) presents nothing and recreates
// nothing, so it cannot loop; a restored window with the same size keeps its swapchain.
constexpr FrameAction ChooseFrameAction(int window_width, int window_height, std::uint32_t chain_width,
                                        std::uint32_t chain_height, bool recreation_pending) {
    if (window_width <= 0 || window_height <= 0) {
        return FrameAction::Skip;
    }
    if (recreation_pending || std::uint32_t(window_width) != chain_width ||
        std::uint32_t(window_height) != chain_height) {
        return FrameAction::Recreate;
    }
    return FrameAction::Keep;
}

// A size change is concrete evidence that a new swapchain differs from the old one.
constexpr Reason ForcedReason(std::uint32_t old_width, std::uint32_t old_height, std::uint32_t new_width,
                              std::uint32_t new_height) {
    return old_width != new_width || old_height != new_height ? Reason::Resize : Reason::None;
}

inline int FormatRecreation(char* out, std::size_t size, Reason reason, const Counters& counters,
                            std::uint32_t width, std::uint32_t height, std::uint32_t images, const char* mode) {
    return std::snprintf(out, size,
                         "Swapchain: recreated #%u (%s), %ux%u, %u images, %s; suboptimal %u (%u kept), "
                         "out of date %u, surface lost %u",
                         counters.recreations, ReasonName(reason), width, height, images, mode ? mode : "?",
                         counters.suboptimal, counters.suboptimal_kept, counters.out_of_date,
                         counters.surface_lost);
}

} // namespace SotbSwapchain

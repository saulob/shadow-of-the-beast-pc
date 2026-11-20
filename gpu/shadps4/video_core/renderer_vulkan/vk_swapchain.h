// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <string>
#include <vector>
#include "common/types.h"
#include "sotb_swapchain_health.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
class WindowSDL;
}

namespace Vulkan {

class Instance;
class Scheduler;

class Swapchain {
public:
    explicit Swapchain(const Instance& instance, const Frontend::WindowSDL& window);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size. False when none exists afterwards: the device
    /// is lost, or the surface has no pixels yet.
    bool Create(u32 width, u32 height, SotbSwapchain::Reason forced = SotbSwapchain::Reason::None);

    /// Recreates the swapchain with a given size and current surface.
    bool Recreate(u32 width, u32 height, SotbSwapchain::Reason forced = SotbSwapchain::Reason::None);

    /// Present thread, before a frame: applies a VSync or display change and recreates for a new window size or a
    /// pending recreation. False while the window has no pixels or the device is lost: nothing is presented.
    bool PrepareFrame();

    /// Recreates for the window's current size unless it has no pixels or the device is lost.
    bool RecreateForWindow();

    /// A recreation, then one more acquire after an acquire that needed one.
    bool RecreateAndAcquire();

    /// Acquires the next image in the swapchain.
    bool AcquireNextImage();

    /// Presents the current image and move to the next one
    bool Present();

    vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    vk::Image Image() const {
        return images[image_index];
    }

    vk::ImageView ImageView() const {
        return images_view[image_index];
    }

    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    u32 GetImageCount() const {
        return image_count;
    }

    bool NeedsRecreation() const {
        return health.NeedsRecreation();
    }

    const SotbSwapchain::Health& GetHealth() const {
        return health;
    }

    u32 GetFrameIndex() const {
        return frame_index;
    }

    vk::Extent2D GetExtent() const {
        return extent;
    }

    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

    bool HasHDR() const {
        return supports_hdr;
    }

    void SetHDR(bool hdr);

    /// VSync from the settings (SotbSettings::vsync); recreates the swapchain when it changed.
    /// Present thread only.
    void UpdateVsync();

    bool GetHDR() const {
        return needs_hdr;
    }

private:
    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Selects the best available present mode
    void FindPresentMode();

    /// Sets the surface properties according to device capabilities; false when the surface has no pixels
    bool SetSurfaceProperties();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

    /// Creates the image acquired and present ready semaphores
    void RefreshSemaphores();

    void LogImages();

    /// Rate-limited line for a recreation or a suboptimal result held back.
    void LogRecreation(SotbSwapchain::Reason reason);
    void LogSuboptimal(bool during_present);
    void NoteDisplayChange();

private:
    const Instance& instance;
    const Frontend::WindowSDL& window;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceKHR surface{};
    const bool gamescope; ///< Gamescope session: low-latency image count
    vk::SurfaceFormatKHR surface_format;
    vk::Format view_format;
    vk::PresentModeKHR present_mode;
    bool vsync = true; ///< the setting present_mode was chosen for
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> images_view;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0; ///< what the swapchain returned, not what was requested
    u32 requested_images = 0;
    u32 surface_min_images = 0;
    u32 surface_max_images = 0;
    bool logged_modes = false;
    std::string logged_images;
    u32 image_index = 0;
    u32 frame_index = 0;
    SotbSwapchain::Health health;
    SotbSwapchain::LogGate recreation_log{4, 10'000'000'000ull};
    SotbSwapchain::LogGate suboptimal_log{2, 30'000'000'000ull};
    u32 seen_display_generation = 0;
    bool needs_hdr = false;    // The game requested HDR swapchain
    bool supports_hdr = false; // SC supports HDR output
};

} // namespace Vulkan

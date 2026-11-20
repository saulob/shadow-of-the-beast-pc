// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "sotb_present.h"
#include "sotb_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_device_lost.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

namespace Vulkan {

// sotb: vkDeviceWaitIdle needs every queue of the device externally synchronized, and the GPU thread keeps
// submitting while the window changes size (F11, display mode, VSync).
static void WaitDeviceIdle(const Instance& instance, const char* why) {
    std::scoped_lock lock{Scheduler::submit_mutex};
    const s32 result = DeviceLost::CallWhileHealthy(
        DeviceLost::Global(), DeviceLost::Operation::DeviceWaitIdle, "idle", 0,
        [&] { return instance.GetDevice().waitIdle(); });
    if (result != DeviceLost::Success && result != DeviceLost::DeviceLostResult) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for the device to become idle {}: {}", why,
                    vk::to_string(vk::Result(result)));
    }
}

static constexpr vk::SurfaceFormatKHR SURFACE_FORMAT_HDR = {
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

static_assert(SotbSwapchain::kVkSuccess == int(vk::Result::eSuccess) &&
              SotbSwapchain::kVkSuboptimal == int(vk::Result::eSuboptimalKHR) &&
              SotbSwapchain::kVkErrorOutOfDate == int(vk::Result::eErrorOutOfDateKHR) &&
              SotbSwapchain::kVkErrorSurfaceLost == int(vk::Result::eErrorSurfaceLostKHR) &&
              SotbSwapchain::kVkErrorUnknown == int(vk::Result::eErrorUnknown) &&
              SotbSwapchain::kVkErrorDeviceLost == DeviceLost::DeviceLostResult);

static_assert(vk::PresentModeKHR(SotbPresent::Mode::Immediate) == vk::PresentModeKHR::eImmediate &&
              vk::PresentModeKHR(SotbPresent::Mode::Mailbox) == vk::PresentModeKHR::eMailbox &&
              vk::PresentModeKHR(SotbPresent::Mode::Fifo) == vk::PresentModeKHR::eFifo);

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_)
    : instance{instance_}, window{window_}, surface{CreateSurface(instance.GetInstance(), window)},
      gamescope{SotbPresent::GamescopeSession()} {
    FindPresentFormat();
    FindPresentMode();

    Create(window.GetWidth(), window.GetHeight());
    std::printf("Swapchain: %s %s, %u images, %ux%u\n", vk::to_string(surface_format.format).c_str(),
                vk::to_string(surface_format.colorSpace).c_str(), image_count, extent.width, extent.height);
    ImGui::Core::Initialize(instance, window, image_count, surface_format.format);
}

static u64 NowNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

Swapchain::~Swapchain() {
    health.Stop();
    Destroy();
    instance.GetInstance().destroySurfaceKHR(surface);
}

bool Swapchain::Create(u32 width_, u32 height_, SotbSwapchain::Reason forced) {
    if (DeviceLost::IsLost()) {
        return false;
    }
    if (forced == SotbSwapchain::Reason::None) {
        forced = SotbSwapchain::ForcedReason(width, height, width_, height_);
    }
    width = width_;
    height = height_;

    Destroy();
    if (health.NeedsNewSurface()) {
        instance.GetInstance().destroySurfaceKHR(surface);
        surface = CreateSurface(instance.GetInstance(), window);
    }

    if (!SetSurfaceProperties()) {
        health.CreateFailed();
        return false;
    }

    const std::array queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };

    const bool exclusive = queue_family_indices[0] == queue_family_indices[1];
    const u32 queue_family_indices_count = exclusive ? 1u : 2u;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    const auto format = needs_hdr ? SURFACE_FORMAT_HDR : surface_format;
    const vk::SwapchainCreateInfoKHR swapchain_info = {
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = nullptr,
    };

    vk::SwapchainKHR chain{};
    const s32 created = DeviceLost::CallWhileHealthy(
        DeviceLost::Global(), DeviceLost::Operation::CreateSwapchain, "swapchn", 0, [&] {
            const auto [result, handle] = instance.GetDevice().createSwapchainKHR(swapchain_info);
            chain = handle;
            return result;
        });
    if (created == DeviceLost::DeviceLostResult) {
        return false;
    }
    ASSERT_MSG(vk::Result(created) == vk::Result::eSuccess, "Failed to create swapchain: {}",
               vk::to_string(vk::Result(created)));
    swapchain = chain;
    health.Recreated(forced);

    SetupImages();
    RefreshSemaphores();
    frame_index = 0; // the semaphores are new and sized for the returned image count
    LogImages();
    return true;
}

// Once per change of the image counts or the present mode: a window resize alone is not logged again.
void Swapchain::LogImages() {
    char counts[192];
    std::snprintf(counts, sizeof(counts), "requested %u, got %u (surface min %u, max %u), %s, %s", requested_images,
                  image_count, surface_min_images, surface_max_images, vk::to_string(present_mode).c_str(),
                  gamescope ? "Gamescope low-latency count" : "default count");
    if (logged_images == counts) {
        return;
    }
    logged_images = counts;
    std::printf("Swapchain: images %s, extent %ux%u\n", counts, extent.width, extent.height);
}

bool Swapchain::Recreate(u32 width_, u32 height_, SotbSwapchain::Reason forced) {
    LOG_DEBUG(Render_Vulkan, "Recreate the swapchain: width={} height={} HDR={}", width_, height_,
              needs_hdr);
    SotbSwapchain::Reason reason = health.NeedsRecreation() ? health.Pending() : forced;
    if (reason == SotbSwapchain::Reason::None) {
        reason = SotbSwapchain::ForcedReason(width, height, width_, height_);
    }
    const bool created = Create(width_, height_, forced);
    if (created) {
        LogRecreation(reason);
    }
    return created;
}

bool Swapchain::PrepareFrame() {
    const s32 window_width = window.GetWidth();
    const s32 window_height = window.GetHeight();
    if (window_width <= 0 || window_height <= 0 || DeviceLost::IsLost()) {
        return false;
    }
    UpdateVsync();
    NoteDisplayChange();
    if (SotbSwapchain::ChooseFrameAction(window_width, window_height, width, height,
                                         health.NeedsRecreation()) == SotbSwapchain::FrameAction::Recreate) {
        Recreate(u32(window_width), u32(window_height));
    }
    return true;
}

bool Swapchain::RecreateForWindow() {
    const s32 window_width = window.GetWidth();
    const s32 window_height = window.GetHeight();
    if (window_width <= 0 || window_height <= 0 || DeviceLost::IsLost()) {
        return false;
    }
    return Recreate(u32(window_width), u32(window_height));
}

bool Swapchain::RecreateAndAcquire() {
    return RecreateForWindow() && AcquireNextImage();
}

void Swapchain::NoteDisplayChange() {
    const u32 generation = window.DisplayGeneration();
    if (generation != seen_display_generation) {
        seen_display_generation = generation;
        health.DisplayChanged();
    }
}

// Present thread. One line per recreation while they are rare, then at most one every ten seconds.
void Swapchain::LogRecreation(SotbSwapchain::Reason reason) {
    if (!recreation_log.Allow(NowNs())) {
        return;
    }
    char line[256];
    SotbSwapchain::FormatRecreation(line, sizeof(line), reason, health.Stats(), extent.width, extent.height,
                                    image_count, vk::to_string(present_mode).c_str());
    const unsigned held_back = recreation_log.TakeSuppressed();
    if (held_back != 0) {
        std::printf("%s; %u recreation lines held back\n", line, held_back);
    } else {
        std::printf("%s\n", line);
    }
}

void Swapchain::LogSuboptimal(bool during_present) {
    if (!suboptimal_log.Allow(NowNs())) {
        return;
    }
    const auto& stats = health.Stats();
    std::printf("Swapchain: suboptimal %s (%u seen, %u kept without recreation), %ux%u, %s\n",
                during_present ? "present" : "acquire", stats.suboptimal, stats.suboptimal_kept, extent.width,
                extent.height, health.Suppressing() ? "the surface stays suboptimal" : "recreation scheduled");
}

void Swapchain::SetHDR(bool hdr) {
    if (needs_hdr == hdr) {
        return;
    }

    WaitDeviceIdle(instance, "on an HDR change");
    needs_hdr = hdr;
    Recreate(width, height, SotbSwapchain::Reason::Hdr);
    ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                 : surface_format.format);
}

bool Swapchain::AcquireNextImage() {
    if (!swapchain) {
        return false;
    }
    vk::Device device = instance.GetDevice();
    const s32 acquired = DeviceLost::CallWhileHealthy(
        DeviceLost::Global(), DeviceLost::Operation::AcquireNextImage, "acquire", 0, [&] {
            return device.acquireNextImageKHR(swapchain, std::numeric_limits<u64>::max(),
                                              image_acquired[frame_index], VK_NULL_HANDLE,
                                              &image_index);
        });
    if (acquired == DeviceLost::DeviceLostResult) {
        return false;
    }

    const SotbSwapchain::Result kind = SotbSwapchain::ClassifyAcquire(acquired);
    if (kind == SotbSwapchain::Result::Failure) {
        LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned unknown result {}",
                     vk::to_string(vk::Result(acquired)));
        UNREACHABLE();
    }

    const bool usable = health.Acquire(kind);
    if (kind == SotbSwapchain::Result::Suboptimal) {
        LogSuboptimal(false);
    }
    return usable;
}

bool Swapchain::Present() {
    if (!swapchain) {
        return false;
    }
    const vk::PresentInfoKHR present_info = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };

    const s32 presented = DeviceLost::CallWhileHealthy(
        DeviceLost::Global(), DeviceLost::Operation::QueuePresent, "present", 0,
        [&] { return instance.GetPresentQueue().presentKHR(present_info); });
    if (presented == DeviceLost::DeviceLostResult) {
        return true;
    }

    const SotbSwapchain::Result kind = SotbSwapchain::ClassifyPresent(presented);
    ASSERT_MSG(kind != SotbSwapchain::Result::Failure, "Swapchain presentation failed: {}",
               vk::to_string(vk::Result(presented)));
    health.Present(kind);
    if (kind == SotbSwapchain::Result::Suboptimal) {
        LogSuboptimal(true);
    }

    frame_index = (frame_index + 1) % image_count;

    return !health.NeedsRecreation();
}

void Swapchain::FindPresentFormat() {
    const auto [formats_result, formats] =
        instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);
    ASSERT_MSG(formats_result == vk::Result::eSuccess, "Failed to query surface formats: {}",
               vk::to_string(formats_result));

    // Check if the device supports HDR formats. Here we care of Rec.2020 PQ only as it is expected
    // game output. Other variants as e.g. linear Rec.2020 will require additional color space
    // rotation
    supports_hdr =
        std::find_if(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR& format) {
            return format == SURFACE_FORMAT_HDR;
        }) != formats.end();
    // Also make sure that user allowed us to use HDR
    supports_hdr &= EmulatorSettings.IsHdrAllowed();

    // If there is a single undefined surface format, the device doesn't care, so we'll just use
    // RGBA sRGB.
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return;
    }

    // Try to find a suitable format.
    for (const vk::SurfaceFormatKHR& sformat : formats) {
        vk::Format format = sformat.format;
        if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eB8G8R8A8Unorm) {
            continue;
        }

        surface_format.format = format;
        surface_format.colorSpace = sformat.colorSpace;
        return;
    }

    UNREACHABLE_MSG("Unable to find required swapchain format!");
}

void Swapchain::FindPresentMode() {
    const auto [modes_result, modes] =
        instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
    if (modes_result != vk::Result::eSuccess) {
        LOG_ERROR(Render, "Failed to query available present modes, falling back to Fifo as "
                          "guaranteed supported option.");
        present_mode = vk::PresentModeKHR::eFifo;
        return;
    }

    if (!logged_modes) {
        logged_modes = true;
        std::string names;
        for (const vk::PresentModeKHR mode : modes) {
            names += (names.empty() ? "" : ", ") + vk::to_string(mode);
        }
        std::printf("Swapchain: surface present modes %s\n", names.c_str());
    }
    vsync = SotbSettings::Get().vsync;
    const auto has = [&](vk::PresentModeKHR mode) { return std::ranges::find(modes, mode) != modes.cend(); };
    present_mode = vk::PresentModeKHR(
        SotbPresent::ChooseMode(vsync, has(vk::PresentModeKHR::eMailbox), has(vk::PresentModeKHR::eImmediate)));
    std::printf("Swapchain: VSync %s (%s)\n", vsync ? "on" : "off", vk::to_string(present_mode).c_str());
}

void Swapchain::UpdateVsync() {
    if (SotbSettings::Get().vsync == vsync) {
        return;
    }
    WaitDeviceIdle(instance, "before a VSync change");
    FindPresentMode();
    Recreate(width, height, SotbSwapchain::Reason::PresentMode);
}

bool Swapchain::SetSurfaceProperties() {
    const auto [capabilities_result, capabilities] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    ASSERT_MSG(capabilities_result == vk::Result::eSuccess,
               "Failed to query surface capabilities: {}", vk::to_string(capabilities_result));

    vk::Extent2D surface_extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        surface_extent.width = std::max(capabilities.minImageExtent.width,
                                        std::min(capabilities.maxImageExtent.width, width));
        surface_extent.height = std::max(capabilities.minImageExtent.height,
                                         std::min(capabilities.maxImageExtent.height, height));
    }
    if (surface_extent.width == 0 || surface_extent.height == 0) {
        return false; // a minimized window can report 0x0 between the presenter's check and this query
    }
    extent = surface_extent;

    surface_min_images = capabilities.minImageCount;
    surface_max_images = capabilities.maxImageCount;
    requested_images = SotbPresent::ImageCount(surface_min_images, surface_max_images, gamescope);
    image_count = requested_images;

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }

    // Opaque is not supported everywhere.
    composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (!(capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)) {
        composite_alpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
    }
    return true;
}

void Swapchain::Destroy() {
    if (!swapchain && images_view.empty() && image_acquired.empty() && present_ready.empty()) {
        return;
    }
    vk::Device device = instance.GetDevice();
    WaitDeviceIdle(instance, "before destroying the swapchain");

    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();

    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
        swapchain = nullptr;
    }

    for (const auto& sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto& sem : present_ready) {
        device.destroySemaphore(sem);
    }

    image_acquired.clear();
    present_ready.clear();
}

void Swapchain::RefreshSemaphores() {
    const vk::Device device = instance.GetDevice();
    image_acquired.resize(image_count);
    present_ready.resize(image_count);

    for (vk::Semaphore& semaphore : image_acquired) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create image acquired semaphore: {}",
                   vk::to_string(semaphore_result));
        semaphore = sem;
    }
    for (vk::Semaphore& semaphore : present_ready) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create present ready semaphore: {}", vk::to_string(semaphore_result));
        semaphore = sem;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, image_acquired[i], "Swapchain Semaphore: image_acquired {}", i);
        SetObjectName(device, present_ready[i], "Swapchain Semaphore: present_ready {}", i);
    }
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    auto [images_result, imgs] = device.getSwapchainImagesKHR(swapchain);
    ASSERT_MSG(images_result == vk::Result::eSuccess, "Failed to create swapchain images: {}",
               vk::to_string(images_result));
    images = std::move(imgs);
    image_count = static_cast<u32>(images.size());
    images_view.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        if (images_view[i]) {
            device.destroyImageView(images_view[i]);
        }
        auto [im_view_result, im_view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        });
        ASSERT_MSG(im_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(im_view_result));
        images_view[i] = im_view;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, images[i], "Swapchain Image {}", i);
        SetObjectName(device, images_view[i], "Swapchain ImageView {}", i);
    }
}

} // namespace Vulkan

// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher's list of Vulkan devices the renderer can use (the Graphics card row), and the check that Vulkan can run
// the renderer at all. Neither may end the process on a system without usable Vulkan.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "common/types.h"
#include "sotb_renderer.h"
#include "sotb_settings.h"
#include "sotb_vulkan_support.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace {

SotbVulkan::Preflight RunSystemPreflight() {
    using namespace Vulkan;
    static std::unique_ptr<vk::detail::DynamicLoader> loader;
    const SotbVulkan::Probe probe{
        [] {
            if (!loader || !loader->success()) {
                loader = std::make_unique<vk::detail::DynamicLoader>();
            }
            if (loader->success()) {
                VULKAN_HPP_DEFAULT_DISPATCHER.init(
                    loader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
            }
            return loader->success();
        },
        [] {
            if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion) {
                return u32{VK_API_VERSION_1_0};
            }
            const auto [result, value] = vk::enumerateInstanceVersion();
            return result == vk::Result::eSuccess ? value : u32{VK_API_VERSION_1_0};
        },
        [] {
            SotbVulkan::DeviceProbe probed;
            auto [result, instance] = CreateProbeInstance();
            if (result == vk::Result::eErrorIncompatibleDriver) {
                probed.status = SotbVulkan::InstanceStatus::IncompatibleDriver;
            } else if (result != vk::Result::eSuccess) {
                probed.status = SotbVulkan::InstanceStatus::Failed;
                probed.failure = vk::to_string(result);
            } else {
                const auto [devices_result, devices] = instance->enumeratePhysicalDevices();
                if (devices_result == vk::Result::eSuccess) {
                    for (const auto& device : devices) {
                        probed.devices.push_back(QueryDeviceCapabilities(device));
                    }
                }
            }
            return probed;
        }};
    return SotbVulkan::RunPreflight(probe, TargetVulkanApiVersion);
}

// A success is remembered; a failure is asked again, as the player may install a driver and press Play again.
SotbVulkan::Preflight CachedPreflight() {
    static std::optional<SotbVulkan::Preflight> passed;
    if (passed) {
        return *passed;
    }
    auto result = RunSystemPreflight();
    if (result.Passed()) {
        passed = result;
    }
    return result;
}

} // namespace

namespace SotbVulkan {
Preflight CheckSystemVulkan() {
    return CachedPreflight();
}
} // namespace SotbVulkan

extern "C" int sotb_check_vulkan(char* cause, size_t cause_size, char* advice, size_t advice_size) {
    const auto result = CachedPreflight();
    std::printf("Graphics: %s\n", result.Summary().c_str());
    if (result.Passed()) {
        return 1;
    }
    std::snprintf(cause, cause_size, "%s", result.problem->cause.c_str());
    std::snprintf(advice, advice_size, "%s", result.problem->advice.c_str());
    return 0;
}

// Vulkan stops the start only when the player's Renderer setting names it.
extern "C" int sotb_check_renderer(const char* settings_file, char* cause, size_t cause_size,
                                  char* advice, size_t advice_size) {
    SotbSettings::Values values;
    SotbSettings::Read(settings_file ? settings_file : "", values);
    const auto choice = SotbRenderer::Choice(values.renderer.load());
    if (choice == SotbRenderer::OpenGL || choice == SotbRenderer::Direct3D11) {
        return 1;
    }
    const bool vulkan_usable = sotb_check_vulkan(cause, cause_size, advice, advice_size) != 0;
    return SotbRenderer::PlayAllowed(choice, vulkan_usable);
}

extern "C" int sotb_gpu_names(char (*names)[256], int max) {
    using namespace Vulkan;
    if (!CachedPreflight().Passed()) {
        return 0;
    }
    int count = 0;
    {
        auto [created, instance] = CreateProbeInstance();
        if (created != vk::Result::eSuccess) {
            return 0;
        }
        const auto [result, devices] = instance->enumeratePhysicalDevices();
        if (result != vk::Result::eSuccess) {
            return 0;
        }
        for (const auto& device : devices) {
            if (count == max || !IsDeviceUsable(device)) {
                continue;
            }
            const auto properties = device.getProperties();
            const char* name = properties.deviceName.data();
            bool known = false;
            for (int i = 0; i < count; ++i) {
                known |= !std::strcmp(names[i], name);
            }
            if (!known) {
                std::snprintf(names[count++], 256, "%s", name);
            }
        }
    }
    return count;
}

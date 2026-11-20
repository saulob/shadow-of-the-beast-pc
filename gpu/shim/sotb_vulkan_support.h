// SPDX-License-Identifier: GPL-2.0-or-later
// Whether Vulkan can run the renderer, decided from facts a probe gathers step by step before any renderer or window
// exists. A step runs only once the earlier ones passed, so an unsupported system gets an explanation instead of a
// fatal assertion. Free of Vulkan headers: tests feed it synthetic capabilities.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "video_core/buffer_cache/arena_mode.h"

namespace SotbVulkan {

constexpr std::uint32_t Version(std::uint32_t major, std::uint32_t minor) {
    return (major << 22) | (minor << 12);
}
constexpr std::uint32_t Major(std::uint32_t version) {
    return (version >> 22) & 0x7f;
}
constexpr std::uint32_t Minor(std::uint32_t version) {
    return (version >> 12) & 0x3ff;
}

inline std::string VersionText(std::uint32_t version) {
    return std::to_string(Major(version)) + "." + std::to_string(Minor(version));
}

// Mandatory: the renderer asserts without them. Every other extension the renderer asks for is optional and only
// switches an optimisation or a feature on.
inline constexpr std::array<std::string_view, 4> kMandatoryExtensions = {
    "VK_KHR_swapchain", "VK_KHR_push_descriptor", "VK_EXT_vertex_attribute_divisor", "VK_EXT_robustness2"};

struct DeviceCapabilities {
    std::string name;
    std::uint32_t vendor_id = 0;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
    std::string driver_name;
    std::string driver_info;
    std::vector<std::string> extensions;
    bool robust_buffer_access2 = false;
    bool robust_image_access2 = false;
    bool null_descriptor = false;
    bool graphics_queue = false;
    bool device_local_memory = false;
    // Optional, with a fallback when absent: the buffer cache's sparse arenas.
    VideoCore::ArenaLayout::SparseCapabilities sparse;
};

struct DeviceVerdict {
    bool usable = false;
    std::vector<std::string> missing; // mandatory capabilities the device lacks, in check order
    VideoCore::ArenaLayout::ArenaDecision arena{};

    std::string FirstMissing() const { return missing.empty() ? std::string{} : missing.front(); }
};

inline bool HasExtension(const DeviceCapabilities& caps, std::string_view name) {
    for (const std::string& extension : caps.extensions) {
        if (extension == name) {
            return true;
        }
    }
    return false;
}

inline DeviceVerdict EvaluateDevice(const DeviceCapabilities& caps, std::uint32_t required_version) {
    DeviceVerdict verdict;
    if (caps.api_version < required_version) {
        verdict.missing.push_back("Vulkan " + VersionText(required_version) + " (device supports " +
                                  VersionText(caps.api_version) + ")");
    }
    for (const std::string_view extension : kMandatoryExtensions) {
        if (!HasExtension(caps, extension)) {
            verdict.missing.push_back(std::string{extension});
        }
    }
    if (!caps.robust_buffer_access2) {
        verdict.missing.push_back("feature robustBufferAccess2");
    }
    if (!caps.robust_image_access2) {
        verdict.missing.push_back("feature robustImageAccess2");
    }
    if (!caps.null_descriptor) {
        verdict.missing.push_back("feature nullDescriptor");
    }
    if (!caps.graphics_queue) {
        verdict.missing.push_back("graphics queue");
    }
    if (!caps.device_local_memory) {
        verdict.missing.push_back("device-local memory");
    }
    verdict.usable = verdict.missing.empty();
    verdict.arena = VideoCore::ArenaLayout::DecideArenaMode(caps.sparse);
    return verdict;
}

inline std::string DescribeDriver(const DeviceCapabilities& caps) {
    std::string text = caps.driver_name.empty() ? "driver" : caps.driver_name;
    if (!caps.driver_info.empty()) {
        text += " " + caps.driver_info;
    }
    char raw[32];
    std::snprintf(raw, sizeof(raw), " (raw %08x)", static_cast<unsigned>(caps.driver_version));
    return text + raw;
}

struct Problem {
    std::string cause;
    std::string advice;
};

enum class InstanceStatus { Created, IncompatibleDriver, Failed };

struct DeviceProbe {
    InstanceStatus status = InstanceStatus::Created;
    std::string failure; ///< what the instance reported when status is Failed
    std::vector<DeviceCapabilities> devices;
};

struct Probe {
    std::function<bool()> loader_found;
    std::function<std::uint32_t()> instance_version; ///< 1.0 when the loader cannot report a version
    std::function<DeviceProbe()> probe_devices;      ///< makes a bare instance and looks at its devices
};

enum class Stage { Passed, NoLoader, RuntimeTooOld, InstanceFailed, NoDevice, NoUsableDevice };

inline const char* StageName(Stage stage) {
    switch (stage) {
    case Stage::Passed:
        return "passed";
    case Stage::NoLoader:
        return "no loader";
    case Stage::RuntimeTooOld:
        return "runtime too old";
    case Stage::InstanceFailed:
        return "instance failed";
    case Stage::NoDevice:
        return "no device";
    default:
        return "no usable device";
    }
}

struct Preflight {
    Stage stage = Stage::Passed;
    std::optional<Problem> problem;
    std::string failed_capability; ///< the first mandatory capability the best candidate lacks
    std::string gpu;               ///< the first usable device, else the closest candidate
    std::string driver;
    std::size_t devices = 0;
    std::size_t usable = 0;
    std::string arena_mode; ///< the buffer cache's mode on the reported GPU

    bool Passed() const { return stage == Stage::Passed; }

    // One log line: the outcome, the GPU and driver, and the mandatory capability that failed.
    std::string Summary() const {
        std::string text = "Vulkan preflight: ";
        text += Passed() ? std::string("passed") : std::string("failed (") + StageName(stage) + ")";
        if (!gpu.empty()) {
            text += "; GPU " + gpu + ", " + driver;
        }
        if (!failed_capability.empty()) {
            text += "; missing " + failed_capability;
        }
        if (Passed() && !arena_mode.empty()) {
            text += "; memory arena " + arena_mode;
        }
        return text;
    }
};

inline Preflight RunPreflight(const Probe& probe, std::uint32_t required) {
    Preflight result;
    const std::string required_text = VersionText(required);
    const std::string advice = "Update your graphics driver, or choose another renderer.";
    const auto fail = [&](Stage stage, Problem problem) {
        result.stage = stage;
        result.problem = std::move(problem);
        return result;
    };
    if (!probe.loader_found()) {
        return fail(Stage::NoLoader,
                    {"Vulkan is not installed on this system.",
                     "Install your graphics driver's Vulkan support, or choose another renderer."});
    }
    const std::uint32_t version = probe.instance_version();
    if (version < required) {
        result.failed_capability = "Vulkan " + required_text;
        return fail(Stage::RuntimeTooOld,
                    {"This system's Vulkan runtime supports only " + VersionText(version) + ", but Vulkan " +
                         required_text + " or newer is required.",
                     advice});
    }
    const Problem no_driver{"No Vulkan graphics driver was found.", advice};
    DeviceProbe probed = probe.probe_devices();
    if (probed.status == InstanceStatus::IncompatibleDriver) {
        return fail(Stage::NoDevice, no_driver);
    }
    if (probed.status == InstanceStatus::Failed) {
        return fail(Stage::InstanceFailed, {"Vulkan could not be started (" + probed.failure + ").", advice});
    }
    result.devices = probed.devices.size();
    if (probed.devices.empty()) {
        return fail(Stage::NoDevice, no_driver);
    }

    const DeviceCapabilities* chosen = nullptr;
    DeviceVerdict chosen_verdict;
    for (const DeviceCapabilities& device : probed.devices) {
        DeviceVerdict verdict = EvaluateDevice(device, required);
        if (verdict.usable) {
            ++result.usable;
        }
        const bool better = !chosen || (verdict.usable && !chosen_verdict.usable) ||
                            (!verdict.usable && !chosen_verdict.usable &&
                             verdict.missing.size() < chosen_verdict.missing.size());
        if (better) {
            chosen = &device;
            chosen_verdict = std::move(verdict);
        }
    }
    result.gpu = chosen->name;
    result.driver = DescribeDriver(*chosen);
    if (result.usable != 0) {
        result.arena_mode = VideoCore::ArenaLayout::ModeName(chosen_verdict.arena.mode);
        return result;
    }
    result.failed_capability = chosen_verdict.FirstMissing();
    return fail(Stage::NoUsableDevice,
                {"No graphics card here supports Vulkan " + required_text + " with the features the game needs (" +
                     chosen->name + " lacks " + result.failed_capability + ").",
                 advice});
}

inline std::optional<Problem> FindProblem(const Probe& probe, std::uint32_t required) {
    return RunPreflight(probe, required).problem;
}

// The system's Vulkan (defined by the video core): asks the loader and the drivers, creates no device and no window.
Preflight CheckSystemVulkan();

} // namespace SotbVulkan

// Vulkan preflight tests: the step-by-step decision on synthetic loader, runtime, instance and device facts, how the
// renderer policy uses it, and the sources that must agree. No Vulkan loader, device, window or game files.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sotb_renderer.h"
#include "sotb_vulkan_support.h"

namespace {

using namespace SotbVulkan;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

constexpr std::uint32_t kRequired = Version(1, 3);

std::string ReadAll(const std::string& relative) {
    std::ifstream file(std::string(SOTB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

DeviceCapabilities GoodDevice(const std::string& name = "Synthetic GPU") {
    DeviceCapabilities caps;
    caps.name = name;
    caps.vendor_id = 0x10de;
    caps.api_version = Version(1, 3);
    caps.driver_version = 0x1234;
    caps.driver_name = "Synthetic driver";
    caps.driver_info = "1.2.3";
    for (const std::string_view extension : kMandatoryExtensions) {
        caps.extensions.emplace_back(extension);
    }
    caps.robust_buffer_access2 = caps.robust_image_access2 = caps.null_descriptor = true;
    caps.graphics_queue = caps.device_local_memory = true;
    caps.sparse = {true, true, true};
    return caps;
}

struct Counters {
    int loader = 0, version = 0, devices = 0;
};

struct Scenario {
    bool loader = true;
    std::uint32_t runtime = Version(1, 3);
    DeviceProbe probe;
};

Preflight Run(const Scenario& scenario, Counters* counters = nullptr) {
    Counters local;
    Counters& seen = counters ? *counters : local;
    const Probe probe{[&] { ++seen.loader; return scenario.loader; },
                      [&] { ++seen.version; return scenario.runtime; },
                      [&] { ++seen.devices; return scenario.probe; }};
    return RunPreflight(probe, kRequired);
}

Scenario OneDevice(DeviceCapabilities caps) {
    Scenario scenario;
    scenario.probe.devices.push_back(std::move(caps));
    return scenario;
}

void TestValidProfile() {
    const Preflight result = Run(OneDevice(GoodDevice()));
    CHECK(result.Passed());
    CHECK(!result.problem);
    CHECK(result.failed_capability.empty());
    CHECK(result.devices == 1 && result.usable == 1);
    CHECK(result.gpu == "Synthetic GPU");
    CHECK(result.driver == "Synthetic driver 1.2.3 (raw 00001234)");
    CHECK(result.arena_mode == "sparse");
    CHECK(result.Summary() ==
          "Vulkan preflight: passed; GPU Synthetic GPU, Synthetic driver 1.2.3 (raw 00001234); memory arena sparse");
    CHECK(!FindProblem(Probe{[] { return true; }, [] { return Version(1, 4); },
                             [] { return OneDevice(GoodDevice()).probe; }},
                       kRequired));
}

void TestLoaderUnavailable() {
    Scenario scenario = OneDevice(GoodDevice());
    scenario.loader = false;
    Counters counters;
    const Preflight result = Run(scenario, &counters);
    CHECK(!result.Passed() && result.stage == Stage::NoLoader);
    CHECK(result.problem && result.problem->cause == "Vulkan is not installed on this system.");
    CHECK(result.problem->advice.find("choose another renderer") != std::string::npos);
    CHECK(counters.loader == 1 && counters.version == 0 && counters.devices == 0);
}

void TestRuntimeTooOld() {
    for (const std::uint32_t runtime : {Version(1, 0), Version(1, 1), Version(1, 2)}) {
        Scenario scenario = OneDevice(GoodDevice());
        scenario.runtime = runtime;
        Counters counters;
        const Preflight result = Run(scenario, &counters);
        CHECK(result.stage == Stage::RuntimeTooOld);
        CHECK(result.failed_capability == "Vulkan 1.3");
        CHECK(result.problem && result.problem->cause == "This system's Vulkan runtime supports only " +
                                                             VersionText(runtime) +
                                                             ", but Vulkan 1.3 or newer is required.");
        CHECK(counters.devices == 0);
    }
    Scenario exact = OneDevice(GoodDevice());
    exact.runtime = Version(1, 3);
    CHECK(Run(exact).Passed());
}

void TestNoPhysicalDevice() {
    Scenario none;
    const Preflight empty = Run(none);
    CHECK(empty.stage == Stage::NoDevice && empty.devices == 0);
    CHECK(empty.problem && empty.problem->cause == "No Vulkan graphics driver was found.");

    Scenario incompatible;
    incompatible.probe.status = InstanceStatus::IncompatibleDriver;
    incompatible.probe.devices.push_back(GoodDevice());
    const Preflight driver = Run(incompatible);
    CHECK(driver.stage == Stage::NoDevice && driver.problem->cause == "No Vulkan graphics driver was found.");

    Scenario failed;
    failed.probe.status = InstanceStatus::Failed;
    failed.probe.failure = "ErrorInitializationFailed";
    const Preflight start = Run(failed);
    CHECK(start.stage == Stage::InstanceFailed);
    CHECK(start.problem->cause == "Vulkan could not be started (ErrorInitializationFailed).");
}

void TestMissingMandatoryExtension() {
    for (const std::string_view missing : kMandatoryExtensions) {
        DeviceCapabilities caps = GoodDevice();
        std::erase(caps.extensions, std::string(missing));
        const Preflight result = Run(OneDevice(caps));
        CHECK(result.stage == Stage::NoUsableDevice);
        CHECK(result.failed_capability == missing);
        CHECK(result.problem->cause.find(std::string(missing)) != std::string::npos);
        CHECK(result.problem->cause.find("Synthetic GPU") != std::string::npos);
        CHECK(result.Summary().find("missing " + std::string(missing)) != std::string::npos);
        CHECK(result.usable == 0 && result.devices == 1);
    }
}

void TestMissingMandatoryFeature() {
    struct Case {
        bool DeviceCapabilities::*feature;
        const char* name;
    };
    for (const Case& test : {Case{&DeviceCapabilities::robust_buffer_access2, "feature robustBufferAccess2"},
                             Case{&DeviceCapabilities::robust_image_access2, "feature robustImageAccess2"},
                             Case{&DeviceCapabilities::null_descriptor, "feature nullDescriptor"},
                             Case{&DeviceCapabilities::graphics_queue, "graphics queue"},
                             Case{&DeviceCapabilities::device_local_memory, "device-local memory"}}) {
        DeviceCapabilities caps = GoodDevice();
        caps.*(test.feature) = false;
        const Preflight result = Run(OneDevice(caps));
        CHECK(result.stage == Stage::NoUsableDevice);
        CHECK(result.failed_capability == test.name);
        CHECK(result.problem->cause.find(test.name) != std::string::npos);
    }
    DeviceCapabilities old = GoodDevice();
    old.api_version = Version(1, 2);
    const Preflight result = Run(OneDevice(old));
    CHECK(result.stage == Stage::NoUsableDevice);
    CHECK(result.failed_capability == "Vulkan 1.3 (device supports 1.2)");
}

void TestOptionalCapabilitiesDoNotReject() {
    // The profile carries only the mandatory extensions: every optional one the renderer can use is absent.
    DeviceCapabilities bare = GoodDevice();
    CHECK(bare.extensions.size() == kMandatoryExtensions.size());
    const Preflight result = Run(OneDevice(bare));
    CHECK(result.Passed() && result.usable == 1);

    // Sparse buffers are handled by the fully resident fallback and never reject the card.
    for (int bits = 0; bits < 8; ++bits) {
        DeviceCapabilities caps = GoodDevice();
        caps.sparse = {bool(bits & 1), bool(bits & 2), bool(bits & 4)};
        const Preflight sparse = Run(OneDevice(caps));
        CHECK(sparse.Passed());
        CHECK(sparse.arena_mode == (bits == 7 ? "sparse" : "fully resident fallback"));
    }
    DeviceCapabilities extra = GoodDevice();
    extra.extensions.push_back("VK_EXT_something_unknown");
    CHECK(Run(OneDevice(extra)).Passed());
}

void TestSeveralDevices() {
    DeviceCapabilities weak = GoodDevice("Integrated");
    std::erase(weak.extensions, std::string("VK_EXT_robustness2"));
    weak.robust_buffer_access2 = weak.robust_image_access2 = weak.null_descriptor = false;
    Scenario scenario;
    scenario.probe.devices = {weak, GoodDevice("Discrete")};
    const Preflight result = Run(scenario);
    CHECK(result.Passed() && result.devices == 2 && result.usable == 1);
    CHECK(result.gpu == "Discrete");

    // When none can run, the closest candidate is the one named.
    DeviceCapabilities close = GoodDevice("Almost");
    close.null_descriptor = false;
    Scenario all_bad;
    all_bad.probe.devices = {weak, close};
    const Preflight failed = Run(all_bad);
    CHECK(failed.stage == Stage::NoUsableDevice && failed.usable == 0);
    CHECK(failed.gpu == "Almost" && failed.failed_capability == "feature nullDescriptor");
}

void TestRendererPolicy() {
    using namespace SotbRenderer;
    Scenario broken = OneDevice(GoodDevice());
    broken.loader = false;
    const Preflight preflight = Run(broken);
    const bool usable = preflight.Passed();
    const std::string cause = preflight.problem->cause;

    const auto automatic = Resolve(Auto, usable, cause);
    CHECK(automatic.backend == Backend::OpenGL);
    CHECK(automatic.fallback_reason == cause);
    CHECK(std::string(BackendName(automatic.backend)) == "OpenGL");

    const auto named = Resolve(Vulkan, usable, cause);
    CHECK(named.backend == Backend::Vulkan && named.fallback_reason.empty());
    CHECK(!PlayAllowed(Vulkan, usable));
    CHECK(PlayAllowed(Auto, usable) && PlayAllowed(OpenGL, usable) && PlayAllowed(Direct3D11, usable));
    CHECK(PlayAllowed(Vulkan, true));
    CHECK(Resolve(OpenGL, usable, cause).backend == Backend::OpenGL);

    const auto working = Resolve(Auto, true, "");
    CHECK(working.backend == Backend::Vulkan && working.fallback_reason.empty());

    // A device that lacks one extension: Auto still reaches the existing fallback with that extension named.
    DeviceCapabilities caps = GoodDevice();
    std::erase(caps.extensions, std::string("VK_KHR_push_descriptor"));
    const Preflight missing = Run(OneDevice(caps));
    const auto fallback = Resolve(Auto, missing.Passed(), missing.problem->cause);
    CHECK(fallback.backend == Backend::OpenGL);
    CHECK(fallback.fallback_reason.find("VK_KHR_push_descriptor") != std::string::npos);
}

void TestSources() {
    const std::string instance = ReadAll("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    // The renderer's own mandatory extensions are the preflight's.
    for (const char* macro : {"VK_KHR_SWAPCHAIN_EXTENSION_NAME", "VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME",
                              "VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME", "VK_EXT_ROBUSTNESS_2_EXTENSION_NAME"}) {
        CHECK(Has(instance, std::string("ASSERT_MSG(add_extension(") + macro + ")"));
        CHECK(Has(instance, std::string("== ") + macro));
    }
    CHECK(Has(instance, "SotbVulkan::EvaluateDevice(QueryDeviceCapabilities(physical), TargetVulkanApiVersion)"));
    for (const char* feature : {"robustBufferAccess2", "robustImageAccess2", "nullDescriptor"}) {
        CHECK(Has(instance, std::string("ASSERT_MSG(robustness2_features.") + feature));
    }

    const std::string list = ReadAll("gpu/shim/sotb_gpu_list.cpp");
    CHECK(Has(list, "SotbVulkan::RunPreflight(probe, TargetVulkanApiVersion)"));
    CHECK(Has(list, "SotbRenderer::PlayAllowed(choice, vulkan_usable)"));
    CHECK(Has(list, "if (!CachedPreflight().Passed()) {\n        return 0;"));
    CHECK(Has(list, "CreateProbeInstance()"));
    CHECK(!Has(list, "CreateInstance(Frontend::WindowSystemType::Headless"));

    const std::string gpu = ReadAll("gpu/shim/sotbgpu.cpp");
    CHECK(Has(gpu, "static bool SelectRenderer()"));
    CHECK(Has(gpu, "if (choice == SotbRenderer::Vulkan || choice == SotbRenderer::Auto) {"));
    CHECK(Has(gpu, "Renderer: requested %s"));
    CHECK(Has(gpu, "Renderer: Vulkan was requested and cannot run"));
    CHECK(Has(gpu, "if (!SelectRenderer()) {\n        return 1;"));
    CHECK(Has(gpu, "Renderer: selected %s, active %s"));

    const std::string loader = ReadAll("src/loader.c");
    CHECK(Has(loader, "sotb_check_renderer(renderer_settings_file, cause, cause_size, advice, advice_size)"));
    CHECK(Has(loader, "skip_launcher, force_launcher, system_usable,"));
    CHECK(Has(loader, "graphics_needed ? system_usable("));

    // The decision stays testable: no Vulkan, assertion or logging machinery in it.
    const std::string support = ReadAll("gpu/shim/sotb_vulkan_support.h");
    for (const char* forbidden : {"vulkan.h", "vulkan/", "ASSERT", "abort(", "exit(", "LOG_"}) {
        CHECK(!Has(support, forbidden));
    }
    for (const char* other : {"CUSA", "GameBuild", "eboot", "odin", "Odin", "OSL_", "osl_"}) {
        CHECK(!Has(support, other));
    }
}

} // namespace

int main() {
    TestValidProfile();
    TestLoaderUnavailable();
    TestRuntimeTooOld();
    TestNoPhysicalDevice();
    TestMissingMandatoryExtension();
    TestMissingMandatoryFeature();
    TestOptionalCapabilitiesDoNotReject();
    TestSeveralDevices();
    TestRendererPolicy();
    TestSources();
    std::printf("vulkan preflight tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

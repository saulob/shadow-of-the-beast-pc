// Presentation hardening tests: acquire, present and swapchain creation results against the global device-lost owner
// and the GPU crash timeline, the timeline's sampling of ordinary presentations, and the wiring of the swapchain,
// presenter, window and present thread. Fake results, no Vulkan device, no window.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "sotb_swapchain_health.h"
#include "video_core/renderer_vulkan/vk_device_lost.h"

namespace {

using namespace Vulkan::DeviceLost;
using SotbSwapchain::ClassifyAcquire;
using SotbSwapchain::ClassifyPresent;
using SotbSwapchain::Health;
using SotbSwapchain::Result;

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

constexpr std::int32_t kSuboptimal = SotbSwapchain::kVkSuboptimal;
constexpr std::int32_t kOutOfDate = SotbSwapchain::kVkErrorOutOfDate;
constexpr std::int32_t kSurfaceLost = SotbSwapchain::kVkErrorSurfaceLost;

std::string ReadSource(const std::string& relative) {
    std::ifstream file(std::string(SOTB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    std::string source = text.str();
    source.erase(std::remove(source.begin(), source.end(), char(13)), source.end());
    return source;
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// The text from `start` up to the next `end` after it; empty when `start` is missing.
std::string Between(const std::string& text, const std::string& start, const std::string& end) {
    const std::size_t from = text.find(start);
    if (from == std::string::npos) {
        return {};
    }
    const std::size_t to = text.find(end, from + start.size());
    return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

struct Capture {
    std::mutex mutex;
    std::vector<std::string> lines;
    std::string stop_reason;
    int stops{};
};
Capture* g_capture = nullptr;

Hooks MakeHooks(Capture& capture) {
    g_capture = &capture;
    Hooks hooks;
    hooks.context = &capture;
    hooks.print = [](const char* line) {
        std::scoped_lock lock{g_capture->mutex};
        g_capture->lines.emplace_back(line);
    };
    hooks.request_stop = [](const char* reason) {
        g_capture->stop_reason = reason;
        ++g_capture->stops;
    };
    return hooks;
}

int IndexOf(const std::vector<std::string>& lines, const std::string& needle) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (Contains(lines[i], needle)) {
            return int(i);
        }
    }
    return -1;
}

// The frame as Swapchain::AcquireNextImage and Present run it: the Vulkan call goes through the global owner, a lost
// device ends the frame, anything else is classified for the state machine.
struct FakeFrame {
    Monitor& monitor;
    Health& health;
    int acquire_calls = 0;
    int present_calls = 0;
    int create_calls = 0;

    bool Acquire(std::int32_t result) {
        const std::int32_t got =
            CallWhileHealthy(monitor, Operation::AcquireNextImage, "acquire", 0, [&] {
                ++acquire_calls;
                return result;
            });
        if (got == DeviceLostResult) {
            return false;
        }
        return health.Acquire(ClassifyAcquire(got));
    }

    bool Present(std::int32_t result) {
        const std::int32_t got = CallWhileHealthy(monitor, Operation::QueuePresent, "present", 0, [&] {
            ++present_calls;
            return result;
        });
        if (got == DeviceLostResult) {
            return true;
        }
        health.Present(ClassifyPresent(got));
        return !health.NeedsRecreation();
    }

    // Swapchain::Create: refused while the device is lost, a lost device while creating goes to the owner.
    bool Create(std::int32_t result) {
        if (monitor.IsLost()) {
            return false;
        }
        const std::int32_t got = CallWhileHealthy(monitor, Operation::CreateSwapchain, "swapchn", 0, [&] {
            ++create_calls;
            return result;
        });
        if (got != Success) {
            return false;
        }
        health.Recreated();
        return true;
    }
};

void TestAcquireDeviceLostRoutesToTheOwner() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    monitor.SetDevice("Test GPU", "1.2.3");
    Health health;
    health.Recreated();
    FakeFrame frame{monitor, health};
    CHECK(frame.Acquire(Success));
    CHECK(!frame.Acquire(DeviceLostResult));
    CHECK(monitor.IsLost() && monitor.HasPublishedIncident());
    CHECK(monitor.GetIncident().operation == Operation::AcquireNextImage);
    CHECK(std::string(monitor.GetIncident().stage) == "acquire");
    CHECK(!health.NeedsRecreation() && health.Stats().out_of_date == 0 && health.Stats().suboptimal == 0);
    CHECK(capture.stops == 1);
    CHECK(IndexOf(capture.lines, "GPU device lost: first detected in vkAcquireNextImageKHR (acquire)") >= 0);
}

void TestPresentDeviceLostRoutesToTheOwner() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    Health health;
    health.Recreated();
    FakeFrame frame{monitor, health};
    CHECK(frame.Acquire(Success));
    CHECK(frame.Present(DeviceLostResult)); // not a recreation request
    CHECK(monitor.IsLost() && monitor.GetIncident().operation == Operation::QueuePresent);
    CHECK(!health.NeedsRecreation() && health.Stats().recreations == 0);
    CHECK(capture.stops == 1);
    CHECK(IndexOf(capture.lines, "vkQueuePresentKHR (present), result -4 DEVICE LOST") >= 0);
}

void TestDeviceLostSuppressesRecreationAndRetries() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    Health health;
    health.Recreated();
    FakeFrame frame{monitor, health};
    CHECK(!frame.Acquire(DeviceLostResult));
    const int acquire_calls = frame.acquire_calls;
    for (int i = 0; i < 100; ++i) {
        CHECK(!frame.Acquire(kOutOfDate)); // refused without reaching Vulkan, not an out-of-date swapchain
        CHECK(frame.Present(kSuboptimal));
        CHECK(!frame.Create(Success));
    }
    CHECK(frame.acquire_calls == acquire_calls && frame.present_calls == 0 && frame.create_calls == 0);
    CHECK(!health.NeedsRecreation() && health.Stats().recreations == 0 && health.Stats().out_of_date == 0);
    CHECK(monitor.Refused() >= 200);
}

void TestCreateSwapchainDeviceLostRoutesToTheOwner() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    Health health;
    health.Recreated();
    FakeFrame frame{monitor, health};
    health.Acquire(Result::OutOfDate);
    CHECK(!frame.Create(DeviceLostResult));
    CHECK(monitor.IsLost() && monitor.GetIncident().operation == Operation::CreateSwapchain);
    CHECK(std::string(OperationName(Operation::CreateSwapchain)) == "vkCreateSwapchainKHR");
    CHECK(health.NeedsRecreation() && health.Stats().recreations == 0); // nothing was created
    CHECK(!frame.Create(Success) && frame.create_calls == 1);
    CHECK(IndexOf(capture.lines, "vkCreateSwapchainKHR (swapchn), result -4 DEVICE LOST") >= 0);
    CHECK(std::string(monitor.Reason()).find("vkCreateSwapchainKHR") != std::string::npos);
}

void TestRepeatedLossKeepsTheFirstOwner() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    Health health;
    health.Recreated();
    FakeFrame frame{monitor, health};
    CHECK(!frame.Acquire(DeviceLostResult));
    const std::string reason = monitor.Reason();
    CHECK(monitor.Report(Operation::QueuePresent, "present", DeviceLostResult, 1) == Outcome::Repeat);
    CHECK(monitor.Report(Operation::AcquireNextImage, "acquire", DeviceLostResult, 2) == Outcome::Repeat);
    CHECK(monitor.Report(Operation::CreateSwapchain, "swapchn", DeviceLostResult, 3) == Outcome::Repeat);
    for (int i = 0; i < 50; ++i) {
        frame.Acquire(DeviceLostResult);
        frame.Present(DeviceLostResult);
    }
    CHECK(monitor.GetIncident().operation == Operation::AcquireNextImage);
    CHECK(monitor.Reason() == reason);
    CHECK(monitor.Repeats() >= 3 && capture.stops == 1 && monitor.TimelineDumps() == 1 && monitor.FaultQueries() <= 1);
}

void TestReasonIsDeterministic() {
    const auto reason_for = [](Operation operation) {
        Capture capture;
        Monitor monitor{MakeHooks(capture)};
        monitor.SetDevice("Test GPU", "9.9");
        monitor.Report(operation, "stage", DeviceLostResult, 42);
        return monitor.Reason();
    };
    CHECK(reason_for(Operation::AcquireNextImage) == reason_for(Operation::AcquireNextImage));
    CHECK(reason_for(Operation::AcquireNextImage) ==
          "GPU device lost: vkAcquireNextImageKHR (stage) result -4, tick 42; GPU Test GPU; driver 9.9; "
          "device fault unavailable (no VK_EXT_device_fault)");
    CHECK(reason_for(Operation::QueuePresent) != reason_for(Operation::AcquireNextImage));
}

void TestSwapchainResultsAreNotDeviceLoss() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    Health health;
    health.Recreated();
    FakeFrame frame{monitor, health};
    CHECK(frame.Acquire(kSuboptimal) && !monitor.IsLost());
    CHECK(!frame.Acquire(kOutOfDate) && !monitor.IsLost());
    CHECK(!frame.Acquire(kSurfaceLost) && !monitor.IsLost());
    CHECK(!frame.Present(kOutOfDate) && !monitor.IsLost());
    CHECK(monitor.Report(Operation::QueuePresent, "present", kSuboptimal, 0) == Outcome::NotLost);
    CHECK(monitor.Report(Operation::AcquireNextImage, "acquire", kOutOfDate, 0) == Outcome::NotLost);
    CHECK(monitor.Report(Operation::AcquireNextImage, "acquire", kSurfaceLost, 0) == Outcome::NotLost);
    CHECK(capture.stops == 0 && capture.lines.empty() && monitor.TimelineDumps() == 0);
    CHECK(health.NeedsRecreation() && health.NeedsNewSurface());
}

void TestTimelineRecordsTheFailingPresentationOperation() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    Health health;
    health.Recreated();
    FakeFrame frame{monitor, health};
    for (int i = 0; i < 5; ++i) {
        frame.Acquire(Success);
        frame.Present(Success);
    }
    frame.Acquire(kSuboptimal);
    frame.Present(kSuboptimal);
    frame.Acquire(kOutOfDate);
    CHECK(!frame.Acquire(DeviceLostResult));
    CHECK(IndexOf(capture.lines, "vkAcquireNextImageKHR (acquire), result 1000001003") >= 0);
    CHECK(IndexOf(capture.lines, "vkQueuePresentKHR (present), result 1000001003") >= 0);
    CHECK(IndexOf(capture.lines, "vkAcquireNextImageKHR (acquire), result -1000001004") >= 0);
    CHECK(IndexOf(capture.lines, "vkAcquireNextImageKHR (acquire), result -4 DEVICE LOST") >= 0);
    CHECK(IndexOf(capture.lines, "GPU timeline: last successful operation:") >= 0);
}

void TestOrdinaryPresentationsAreSampled() {
    Monitor monitor;
    constexpr int frames = 1000;
    for (int i = 0; i < frames; ++i) {
        monitor.Report(Operation::AcquireNextImage, "acquire", Success, 0);
        monitor.Report(Operation::QueuePresent, "present", Success, 0);
    }
    const std::uint64_t kept_per_operation = (frames + PresentSampleInterval - 1) / PresentSampleInterval;
    CHECK(monitor.Events().Recorded() == 2 * kept_per_operation);
    CHECK(monitor.Events().Recorded() < 2 * frames / 8);
    TimelineEntry first[2];
    CHECK(monitor.Events().Snapshot(first, 2) == 2 && first[0].kind == std::uint32_t(Operation::AcquireNextImage));
    CHECK(first[1].kind == std::uint32_t(Operation::QueuePresent)); // the first of each is kept

    // Anything but a plain success is always kept, between samples too.
    const std::uint64_t before = monitor.Events().Recorded();
    monitor.Report(Operation::AcquireNextImage, "acquire", kSuboptimal, 0);
    monitor.Report(Operation::QueuePresent, "present", kOutOfDate, 0);
    monitor.Report(Operation::AcquireNextImage, "acquire", kSurfaceLost, 0);
    CHECK(monitor.Events().Recorded() == before + 3);
    CHECK(!monitor.IsLost());

    // Submissions are never sampled, and presentations do not push the recent ones out of the ring.
    Monitor busy;
    for (std::uint64_t tick = 1; tick <= 100; ++tick) {
        busy.Report(Operation::QueueSubmit, "submit", Success, tick, Timeline::Work);
    }
    for (int i = 0; i < 160; ++i) {
        busy.Report(Operation::AcquireNextImage, "acquire", Success, 0);
        busy.Report(Operation::QueuePresent, "present", Success, 0);
    }
    TimelineEntry history[TimelineCapacity];
    const std::size_t count = busy.Events().Snapshot(history, TimelineCapacity);
    std::size_t submits = 0;
    for (std::size_t i = 0; i < count; ++i) {
        submits += history[i].kind == std::uint32_t(Operation::QueueSubmit) ? 1 : 0;
    }
    CHECK(submits == 100); // without sampling 320 presentation events would have evicted all of them
    CHECK(busy.Events().Recorded() == 100 + 2 * 10);

    // The poll rule of the previous batch still holds.
    Monitor poll;
    for (int i = 0; i < 100; ++i) {
        poll.Report(Operation::GetSemaphoreCounterValue, "semread", Success, 1, Timeline::Work);
    }
    CHECK(poll.Events().Recorded() == 0);
}

void TestSimulationNameForSwapchainCreation() {
    Operation operation{};
    std::uint64_t calls = 0;
    CHECK(ParseSimulation("swapchain", operation, calls) && operation == Operation::CreateSwapchain && calls == 1);
    CHECK(ParseSimulation("swapchain:3", operation, calls) && operation == Operation::CreateSwapchain && calls == 3);
    CHECK(ParseSimulation("acquire:2", operation, calls) && operation == Operation::AcquireNextImage && calls == 2);
    CHECK(ParseSimulation("present", operation, calls) && operation == Operation::QueuePresent);
    CHECK(!ParseSimulation("swapchain:0", operation, calls) && !ParseSimulation("swapchains", operation, calls));
    CHECK(std::uint32_t(Operation::Count) == 10);
}

void TestSwapchainWiring() {
    const std::string cpp = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    const std::string header = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.h");
    CHECK(Contains(header, "sotb_swapchain_health.h") && !Contains(header, "class SwapchainHealth"));
    CHECK(Contains(cpp, "SotbSwapchain::ClassifyAcquire(acquired)") && Contains(cpp, "SotbSwapchain::ClassifyPresent(presented)"));
    CHECK(Contains(cpp, "kVkErrorDeviceLost == DeviceLost::DeviceLostResult"));
    CHECK(Contains(cpp, "DeviceLost::Operation::CreateSwapchain"));
    CHECK(!Contains(cpp, "eErrorDeviceLost"));

    const std::string create = Between(cpp, "bool Swapchain::Create(", "// Once per change of the image counts");
    CHECK(!create.empty());
    const std::size_t guard = create.find("if (DeviceLost::IsLost()) {\n        return false;");
    CHECK(guard != std::string::npos && guard < create.find("Destroy();"));
    CHECK(Contains(create, "health.NeedsNewSurface()") && Contains(create, "CreateSurface(instance.GetInstance(), window)"));
    CHECK(Contains(create, "health.CreateFailed()") && Contains(create, "health.Recreated(forced)"));
    CHECK(create.find("CallWhileHealthy") < create.find("health.Recreated(forced)"));

    const std::string properties = Between(cpp, "bool Swapchain::SetSurfaceProperties()", "void Swapchain::Destroy()");
    CHECK(Contains(properties, "surface_extent.width == 0 || surface_extent.height == 0"));
    CHECK(properties.find("return false;") < properties.find("extent = surface_extent;"));

    const std::string destroy = Between(cpp, "void Swapchain::Destroy()", "void Swapchain::RefreshSemaphores()");
    CHECK(Contains(destroy, "swapchain = nullptr;") && destroy.find("if (!swapchain && images_view.empty()") < destroy.find("WaitDeviceIdle"));
    CHECK(Contains(cpp, "health.Stop();"));

    const std::string acquire = Between(cpp, "bool Swapchain::AcquireNextImage()", "bool Swapchain::Present()");
    CHECK(acquire.find("if (!swapchain)") < acquire.find("CallWhileHealthy"));
    CHECK(acquire.find("DeviceLostResult") < acquire.find("ClassifyAcquire"));
    const std::string present = Between(cpp, "bool Swapchain::Present()", "void Swapchain::FindPresentFormat()");
    CHECK(present.find("DeviceLostResult") < present.find("ClassifyPresent") && Contains(present, "health.Present(kind)"));
    // The frame path never waits for the device and builds no strings.
    for (const std::string& hot : {acquire, present}) {
        CHECK(!Contains(hot, "waitIdle") && !Contains(hot, "WaitDeviceIdle") && !Contains(hot, "std::string"));
        CHECK(!Contains(hot, "std::printf") && !Contains(hot, "new ") && !Contains(hot, "make_unique"));
    }

    CHECK(Contains(cpp, "SotbSwapchain::Reason::Hdr") && Contains(cpp, "SotbSwapchain::Reason::PresentMode"));
    CHECK(Contains(cpp, "window.DisplayGeneration()") && Contains(cpp, "health.DisplayChanged()"));
    CHECK(Contains(cpp, "ChooseFrameAction(window_width, window_height, width, height"));
}

void TestPresenterWiring() {
    const std::string cpp = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const std::string loading = Between(cpp, "void Presenter::PresentLoadingFrame()", "void Presenter::Present(");
    CHECK(Contains(loading, "swapchain.PrepareFrame()") && !Contains(loading, "Recreate("));
    const std::string present = Between(cpp, "void Presenter::Present(", "void Presenter::RetireFrame(");
    CHECK(Contains(present, "swapchain.PrepareFrame()") && Contains(present, "swapchain.RecreateAndAcquire()"));
    CHECK(Contains(present, "swapchain.RecreateForWindow()"));
    CHECK(!Contains(present, "swapchain.Recreate(") && !Contains(present, "UpdateVsync"));
    CHECK(!Contains(present, "waitIdle") && !Contains(present, "WaitDeviceIdle"));
    CHECK(Contains(present, "if (!DeviceLost::IsLost())"));
    CHECK(Contains(present, "SotbSwapchain::LogGate skipped"));
    CHECK(!Contains(cpp, "eErrorDeviceLost"));
}

void TestWindowAndPresentThreadWiring() {
    const std::string window = ReadSource("gpu/shim/window.cpp");
    CHECK(Contains(window, "case SDL_EVENT_WINDOW_DISPLAY_CHANGED:\n            UpdateDisplayRefresh();\n            display_generation.fetch_add"));
    CHECK(Contains(window, "void WindowSDL::UpdateDisplayRefresh()"));
    CHECK(Contains(window, "SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED") && Contains(window, "SDL_GetWindowSizeInPixels(window, &w, &h);"));
    const std::string window_header = ReadSource("gpu/shim/sdl_window.h");
    CHECK(Contains(window_header, "u32 DisplayGeneration() const") && Contains(window_header, "std::atomic<u32> display_generation"));

    const std::string driver = ReadSource("gpu/shadps4/core/libraries/videoout/driver.cpp");
    const std::string thread = Between(driver, "void VideoOutDriver::PresentThread(", "} // namespace Libraries::VideoOut");
    CHECK(Contains(thread, "if (const auto current = read_inputs(); !(current == inputs))"));
    CHECK(Contains(thread, "SotbFramePacing::CadenceFor(inputs)") && Contains(thread, "report_stall();"));
    CHECK(Contains(thread, "SotbLog::LogGate stall_log"));
    CHECK(Contains(driver, "SotbFramePacing::CoarseSleep(slot, Clock::now())"));
    CHECK(Contains(thread, "std::chrono::steady_clock::now()") && !Contains(thread, "system_clock") && !Contains(thread, "high_resolution_clock"));
    CHECK(!Contains(thread, "FramePacing::RepeatCadenceHz(target_fps"));
    CHECK(Contains(thread, "limiter.Release(tick_start);") && Contains(thread, "limiter.Release(std::chrono::steady_clock::now());"));
}

void TestExitBehaviorStaysWithTheExistingMechanism() {
    CHECK(ExitStatus == 25);
    const std::string instance = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    CHECK(Contains(instance, "std::_Exit(DeviceLost::ExitStatus)"));
    const std::string swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    CHECK(!Contains(swapchain, "_Exit") && !Contains(swapchain, "std::exit") && !Contains(swapchain, "std::abort"));
    CHECK(!Contains(swapchain, "ReportDeviceLost") && !Contains(swapchain, "SetHooks"));
}

} // namespace

int main() {
    TestAcquireDeviceLostRoutesToTheOwner();
    TestPresentDeviceLostRoutesToTheOwner();
    TestDeviceLostSuppressesRecreationAndRetries();
    TestCreateSwapchainDeviceLostRoutesToTheOwner();
    TestRepeatedLossKeepsTheFirstOwner();
    TestReasonIsDeterministic();
    TestSwapchainResultsAreNotDeviceLoss();
    TestTimelineRecordsTheFailingPresentationOperation();
    TestOrdinaryPresentationsAreSampled();
    TestSimulationNameForSwapchainCreation();
    TestSwapchainWiring();
    TestPresenterWiring();
    TestWindowAndPresentThreadWiring();
    TestExitBehaviorStaysWithTheExistingMechanism();
    std::printf("presentation hardening tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// Vulkan device-lost tests: first-fault ownership, refused work, terminating waits, the GPU worker loop, the bounded
// optional device fault query, the reason text and the opt-in simulation, on fake callbacks. No Vulkan device, no window.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "video_core/renderer_vulkan/vk_device_lost.h"

namespace {

using namespace Vulkan;
using namespace Vulkan::DeviceLost;
using namespace std::chrono_literals;

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

struct Fake {
    std::atomic<int> fault_queries{};
    std::atomic<int> state_logs{};
    std::atomic<int> stops{};
    std::vector<std::string> lines;
    std::mutex lines_mutex;
    FaultReport report;
    bool throws{};
    std::string stop_reason;
};

Fake* g_fake = nullptr;

Hooks MakeHooks(Fake& fake) {
    g_fake = &fake;
    return Hooks{
        .context = &fake,
        .query_fault =
            [](void* context) {
                auto& f = *static_cast<Fake*>(context);
                ++f.fault_queries;
                if (f.throws) {
                    throw 1;
                }
                return f.report;
            },
        .log_state = [](void* context) { ++static_cast<Fake*>(context)->state_logs; },
        .print =
            [](const char* line) {
                std::scoped_lock lock{g_fake->lines_mutex};
                g_fake->lines.emplace_back(line);
            },
        .request_stop =
            [](const char* reason) {
                ++g_fake->stops;
                g_fake->stop_reason = reason;
            },
    };
}

FaultReport CapturedReport() {
    FaultReport report;
    report.status = FaultStatus::Captured;
    report.description = "page fault";
    report.address_count = 2;
    report.vendor_count = 1;
    report.addresses.push_back({1, 0x1000, 0x10});
    report.vendors.push_back({0x42, 7, "shader"});
    return report;
}

void TestHealthyInitialState() {
    Monitor monitor;
    CHECK(!monitor.IsLost());
    CHECK(monitor.CurrentPhase() == Phase::Healthy);
    CHECK(!monitor.HasPublishedIncident());
    CHECK(monitor.Repeats() == 0 && monitor.Refused() == 0 && monitor.FaultQueries() == 0);
    CHECK(monitor.StopRequests() == 0 && !monitor.WorkerStopped());
    CHECK(Contains(monitor.Reason(), "in progress"));
    CHECK(!Global().IsLost());
}

void TestFirstLossOwnsTheIncident() {
    Fake fake;
    fake.report = CapturedReport();
    Monitor monitor{MakeHooks(fake)};
    monitor.SetDevice("Test GPU", "1.2.3");
    CHECK(monitor.Report(Operation::QueueSubmit, "submit", DeviceLostResult, 77) == Outcome::First);
    CHECK(monitor.IsLost());
    CHECK(monitor.CurrentPhase() == Phase::Published);
    const Incident& incident = monitor.GetIncident();
    CHECK(incident.operation == Operation::QueueSubmit);
    CHECK(std::string(incident.stage) == "submit");
    CHECK(incident.result == DeviceLostResult && incident.tick == 77);
    CHECK(incident.gpu == "Test GPU" && incident.driver == "1.2.3");
    CHECK(incident.fault.status == FaultStatus::Captured);
    CHECK(fake.stops == 1 && fake.state_logs == 1 && fake.fault_queries == 1);
    CHECK(fake.stop_reason == monitor.Reason());
}

void TestCompetingLossesKeepOneOwner() {
    for (int round = 0; round < 40; ++round) {
        Fake fake;
        fake.report = CapturedReport();
        Monitor monitor{MakeHooks(fake)};
        constexpr int Threads = 8;
        std::atomic<bool> go{};
        std::atomic<int> firsts{};
        std::atomic<int> repeats{};
        std::atomic<int> winner{-1};
        std::vector<std::thread> threads;
        for (int i = 0; i < Threads; ++i) {
            threads.emplace_back([&, i] {
                while (!go.load()) {
                }
                const Outcome outcome =
                    monitor.Report(Operation(i % int(Operation::Count)), "race", DeviceLostResult, 100 + i);
                if (outcome == Outcome::First) {
                    ++firsts;
                    winner = i;
                } else if (outcome == Outcome::Repeat) {
                    ++repeats;
                }
            });
        }
        go = true;
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(firsts == 1 && repeats == Threads - 1);
        CHECK(monitor.GetIncident().tick == std::uint64_t(100 + winner.load()));
        CHECK(monitor.GetIncident().operation == Operation(winner.load() % int(Operation::Count)));
        CHECK(fake.fault_queries == 1 && fake.stops == 1 && monitor.Repeats() == std::uint32_t(Threads - 1));
    }
}

void TestRepeatedLossIsIdempotent() {
    Fake fake;
    fake.report = CapturedReport();
    Monitor monitor{MakeHooks(fake)};
    CHECK(monitor.Report(Operation::WaitSemaphores, "semwait", DeviceLostResult, 5) == Outcome::First);
    const std::string reason = monitor.Reason();
    const auto lines = fake.lines.size();
    for (int i = 0; i < 10; ++i) {
        CHECK(monitor.Report(Operation::QueuePresent, "present", DeviceLostResult, 9) == Outcome::Repeat);
    }
    CHECK(monitor.Reason() == reason);
    CHECK(fake.lines.size() == lines);
    CHECK(fake.fault_queries == 1 && fake.stops == 1 && fake.state_logs == 1);
    CHECK(monitor.Repeats() == 10);
}

void TestFirstReasonIsPreserved() {
    Monitor monitor;
    monitor.Report(Operation::QueueSubmit, "submit", DeviceLostResult, 11);
    const std::string reason = monitor.Reason();
    monitor.Report(Operation::AcquireNextImage, "acquire", DeviceLostResult, 99);
    CHECK(monitor.GetIncident().operation == Operation::QueueSubmit);
    CHECK(monitor.GetIncident().tick == 11);
    CHECK(monitor.Reason() == reason);
    CHECK(Contains(reason, "vkQueueSubmit (submit) result -4, tick 11"));
}

void TestSubmissionsAndSparseBindsRefusedAfterLoss() {
    Monitor monitor;
    int submits = 0;
    int binds = 0;
    const auto submit = [&] {
        ++submits;
        return Success;
    };
    CHECK(CallWhileHealthy(monitor, Operation::QueueSubmit, "submit", 1, submit) == Success);
    CHECK(submits == 1 && !monitor.IsLost());
    monitor.Report(Operation::QueuePresent, "present", DeviceLostResult, 2);
    CHECK(CallWhileHealthy(monitor, Operation::QueueSubmit, "submit", 3, submit) == DeviceLostResult);
    CHECK(CallWhileHealthy(monitor, Operation::QueueBindSparse, "sparse", 3, [&] {
              ++binds;
              return Success;
          }) == DeviceLostResult);
    CHECK(submits == 1 && binds == 0);
    CHECK(monitor.Refused() == 2);
    CHECK(monitor.GetIncident().operation == Operation::QueuePresent);

    const auto scheduler = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp");
    const auto entry = scheduler.find("void Scheduler::SubmitExecution(SubmitInfo& info) {");
    const auto early = scheduler.find("DeviceLost::IsLost()", entry);
    const auto lock = scheduler.find("std::scoped_lock lk{submit_mutex}", entry);
    CHECK(entry != std::string::npos && early < lock);
    CHECK(Contains(scheduler, "DeviceLost::Operation::QueueSubmit"));
    CHECK(!Contains(scheduler, "ReportDeviceFault"));
    const auto buffer_cache = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    const auto binds_entry = buffer_cache.find("void BufferCache::SubmitPendingArenaBinds");
    CHECK(buffer_cache.find("DeviceLost::IsLost()", binds_entry) < buffer_cache.find("bindSparse(", binds_entry));
    CHECK(Contains(buffer_cache, "DeviceLost::Operation::QueueBindSparse"));
}

void TestWaitsTerminateAfterLoss() {
    Monitor monitor;
    std::atomic<int> waits{};
    auto wait = std::async(std::launch::async, [&] {
        return WaitWhileHealthy(monitor, Operation::WaitSemaphores, "semwait", 4, [&] {
            ++waits;
            std::this_thread::sleep_for(2ms);
            return Timeout;
        });
    });
    std::this_thread::sleep_for(30ms);
    CHECK(wait.wait_for(0ms) == std::future_status::timeout);
    monitor.Report(Operation::QueueSubmit, "submit", DeviceLostResult, 1);
    CHECK(wait.wait_for(5s) == std::future_status::ready);
    CHECK(wait.get() == DeviceLostResult);
    CHECK(waits > 0);
    const int after = waits;
    CHECK(WaitWhileHealthy(monitor, Operation::WaitSemaphores, "semwait", 4, [&] {
              ++waits;
              return Success;
          }) == DeviceLostResult);
    CHECK(waits == after);

    Monitor other;
    CHECK(WaitWhileHealthy(other, Operation::WaitForFences, "fence", 0, [] { return Success; }) == Success);
    CHECK(WaitWhileHealthy(other, Operation::WaitForFences, "fence", 0, [] { return DeviceLostResult; }) ==
          DeviceLostResult);
    CHECK(other.IsLost() && other.GetIncident().operation == Operation::WaitForFences);

    const auto semaphore = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp");
    CHECK(Contains(semaphore, "WaitWhileHealthy"));
    CHECK(Contains(semaphore, "WAIT_SLICE_NS"));
    CHECK(!Contains(semaphore, "std::numeric_limits<u64>::max()"));
    const auto presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    CHECK(Contains(presenter, "bool Presenter::WaitFrameFence"));
    CHECK(presenter.find("waitForFences(") == presenter.rfind("waitForFences("));
}

// The same loop shape as Liverpool::Process: idle wait, work, leave on stop or loss.
struct FakeWorker {
    Monitor& monitor;
    std::atomic<int> iterations{};
    std::atomic<bool> left{};

    void Run(std::stop_token stop) {
        while (!ShouldStop(monitor, stop)) {
            ++iterations;
            std::this_thread::sleep_for(1ms);
        }
        if (monitor.IsLost()) {
            monitor.NoteWorkerStopped();
        }
        left = true;
    }
};

void TestWorkerLeavesItsLoopOnLoss() {
    Monitor monitor;
    FakeWorker worker{monitor};
    std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
    while (worker.iterations < 3) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(!worker.left);
    monitor.Report(Operation::QueueSubmit, "submit", DeviceLostResult, 1);
    for (int i = 0; i < 500 && !worker.left; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(worker.left);
    CHECK(monitor.WorkerStopped());
    CHECK(!thread.get_stop_token().stop_requested());
}

void TestStopTokenShutdownStillWorks() {
    Monitor monitor;
    FakeWorker worker{monitor};
    std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
    while (worker.iterations < 3) {
        std::this_thread::sleep_for(1ms);
    }
    thread.request_stop();
    thread.join();
    CHECK(worker.left);
    CHECK(!monitor.IsLost() && !monitor.WorkerStopped());
    std::stop_source source;
    CHECK(!ShouldStop(monitor, source.get_token()));
    source.request_stop();
    CHECK(ShouldStop(monitor, source.get_token()));
}

void TestLossDuringShutdownDoesNotDeadlock() {
    Monitor monitor;
    std::stop_source shutdown;
    // A teardown wait on a GPU that will never signal, with shutdown already requested.
    auto teardown = std::async(std::launch::async, [&] {
        return WaitWhileHealthy(monitor, Operation::WaitSemaphores, "semwait", 8, [&] {
            std::this_thread::sleep_for(2ms);
            return Timeout;
        });
    });
    shutdown.request_stop();
    std::this_thread::sleep_for(20ms);
    CHECK(teardown.wait_for(0ms) == std::future_status::timeout);
    monitor.Report(Operation::DeviceWaitIdle, "idle", DeviceLostResult, 0);
    CHECK(teardown.wait_for(5s) == std::future_status::ready);
    CHECK(teardown.get() == DeviceLostResult);
    CHECK(ShouldStop(monitor, shutdown.get_token()));

    // Loss and shutdown racing against a worker.
    Monitor second;
    FakeWorker worker{second};
    std::jthread thread([&](std::stop_token stop) { worker.Run(stop); });
    std::thread loss([&] { second.Report(Operation::QueueSubmit, "submit", DeviceLostResult, 1); });
    thread.request_stop();
    loss.join();
    for (int i = 0; i < 500 && !worker.left; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(worker.left);
    CHECK(second.IsLost());
}

void TestFaultExtensionOrFunctionUnavailable() {
    int calls = 0;
    const FaultReport report = CollectFaultReport(
        false,
        [&](FaultCounts&) {
            ++calls;
            return ResultSuccess;
        },
        [&](FaultCounts&, FaultBuffers&) {
            ++calls;
            return ResultSuccess;
        });
    CHECK(report.status == FaultStatus::Unsupported);
    CHECK(calls == 0);
    CHECK(FormatFaultSummary(report) == "device fault unavailable (no VK_EXT_device_fault)");

    const auto instance = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    CHECK(Contains(instance, "device_fault && VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT"));
    CHECK(Contains(instance, "add_extension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)"));
}

void TestBoundedFaultCapture() {
    const FaultReport report = CollectFaultReport(
        true,
        [](FaultCounts& counts) {
            counts = {2, 1};
            return ResultSuccess;
        },
        [](FaultCounts& counts, FaultBuffers& out) {
            std::strcpy(out.description, "gpu page fault");
            out.addresses[0] = {1, 0xdead0000, 0x1000};
            out.addresses[1] = {2, 0xbeef0000, 0x10};
            out.vendors[0].code = 0x99;
            out.vendors[0].data = 3;
            std::strcpy(out.vendors[0].description, "vendor text");
            (void)counts;
            return ResultSuccess;
        });
    CHECK(report.status == FaultStatus::Captured);
    CHECK(report.description == "gpu page fault");
    CHECK(report.address_count == 2 && report.addresses.size() == 2 && report.addresses[1].address == 0xbeef0000);
    CHECK(report.vendor_count == 1 && report.vendors.size() == 1 && report.vendors[0].description == "vendor text");
    CHECK(FormatFaultSummary(report) ==
          "device fault: gpu page fault, 2 addresses, first type 1 at 0xdead0000, 1 vendor faults, first 0x99");
}

void TestIncompleteResultIsCaptured() {
    const FaultReport report = CollectFaultReport(
        true,
        [](FaultCounts& counts) {
            counts = {1, 0};
            return ResultSuccess;
        },
        [](FaultCounts&, FaultBuffers& out) {
            std::strcpy(out.description, "truncated");
            out.addresses[0] = {1, 0x10, 1};
            return ResultIncomplete;
        });
    CHECK(report.status == FaultStatus::Captured && report.result == ResultIncomplete);
    CHECK(report.description == "truncated" && report.addresses.size() == 1);
}

void TestFaultQueryFailures() {
    const FaultReport counts_failed = CollectFaultReport(
        true, [](FaultCounts&) { return DeviceLostResult; },
        [](FaultCounts&, FaultBuffers&) { return ResultSuccess; });
    CHECK(counts_failed.status == FaultStatus::Failed && counts_failed.result == DeviceLostResult);

    const FaultReport details_failed = CollectFaultReport(
        true,
        [](FaultCounts& counts) {
            counts = {1, 1};
            return ResultSuccess;
        },
        [](FaultCounts&, FaultBuffers&) { return -3; });
    CHECK(details_failed.status == FaultStatus::Failed && details_failed.result == -3);
    CHECK(details_failed.addresses.empty() && details_failed.vendors.empty());
    CHECK(FormatFaultSummary(details_failed) == "device fault query failed (result -3)");

    Fake fake;
    fake.throws = true;
    Monitor monitor{MakeHooks(fake)};
    CHECK(monitor.Report(Operation::QueueSubmit, "submit", DeviceLostResult, 1) == Outcome::First);
    CHECK(monitor.GetIncident().fault.status == FaultStatus::Failed);
    CHECK(monitor.StopRequests() == 1);
}

void TestFaultCountsAreCapped() {
    std::uint32_t requested_addresses = 0;
    std::uint32_t requested_vendors = 0;
    const FaultReport report = CollectFaultReport(
        true,
        [](FaultCounts& counts) {
            counts = {1'000'000, 4'000'000'000u};
            return ResultSuccess;
        },
        [&](FaultCounts& counts, FaultBuffers& out) {
            requested_addresses = counts.addresses;
            requested_vendors = counts.vendors;
            for (std::uint32_t i = 0; i < counts.addresses; ++i) {
                out.addresses[i] = {1, i, 0};
            }
            for (std::uint32_t i = 0; i < counts.vendors; ++i) {
                out.vendors[i].code = i;
            }
            counts = {1'000'000, 4'000'000'000u};
            return ResultIncomplete;
        });
    CHECK(requested_addresses == MaxFaultAddresses);
    CHECK(requested_vendors == MaxVendorFaults);
    CHECK(report.address_count == 1'000'000 && report.vendor_count == 4'000'000'000u);
    CHECK(report.addresses.size() == MaxFaultAddresses);
    CHECK(report.vendors.size() == MaxVendorFaults);
    CHECK(report.status == FaultStatus::Captured);
    CHECK(Contains(FormatFaultSummary(report), "1000000 addresses"));
}

void TestDriverTextIsBoundedAndSanitized() {
    const FaultReport report = CollectFaultReport(
        true,
        [](FaultCounts& counts) {
            counts = {0, 1};
            return ResultSuccess;
        },
        [](FaultCounts&, FaultBuffers& out) {
            std::memset(out.description, 'A', sizeof(out.description));
            std::memset(out.vendors[0].description, 'B', sizeof(out.vendors[0].description));
            std::memcpy(out.vendors[0].description, "ok\n\t\x01\x7f\xff!", 9);
            out.vendors[0].description[9] = '\0';
            return ResultSuccess;
        });
    CHECK(report.description.size() <= MaxFaultTextBytes);
    CHECK(report.description.size() == 256 && report.description.find('\0') == std::string::npos);
    CHECK(report.vendors[0].description == "ok????" "?!");
    for (const char c : report.vendors[0].description) {
        CHECK(c >= 0x20 && c < 0x7f);
    }
    char unterminated[4] = {'a', 'b', 'c', 'd'};
    CHECK(SanitizeDriverText(unterminated, sizeof(unterminated)) == "abcd");
    const auto reason = FormatReason([&] {
        Incident incident;
        incident.fault = report;
        return incident;
    }());
    CHECK(reason.size() <= MaxReasonBytes);
}

void TestFaultIsQueriedOnce() {
    Fake fake;
    fake.report = CapturedReport();
    Monitor monitor{MakeHooks(fake)};
    for (int i = 0; i < 20; ++i) {
        monitor.Report(Operation(i % int(Operation::Count)), "again", DeviceLostResult, i);
    }
    CHECK(fake.fault_queries == 1 && monitor.FaultQueries() == 1);

    Fake racing;
    Monitor contested{MakeHooks(racing)};
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([&] { contested.Report(Operation::QueueSubmit, "submit", DeviceLostResult, 1); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(racing.fault_queries == 1);
}

void TestDeterministicReason() {
    Incident incident;
    incident.operation = Operation::AcquireNextImage;
    incident.stage = "acquire";
    incident.result = DeviceLostResult;
    incident.tick = 42;
    incident.gpu = "Test GPU";
    incident.driver = "9.9";
    incident.fault = CapturedReport();
    const std::string expected =
        "GPU device lost: vkAcquireNextImageKHR (acquire) result -4, tick 42; GPU Test GPU; driver 9.9; "
        "device fault: page fault, 2 addresses, first type 1 at 0x1000, 1 vendor faults, first 0x42";
    CHECK(FormatReason(incident) == expected);
    CHECK(FormatReason(incident) == expected);
    incident.simulated = true;
    CHECK(FormatReason(incident) == expected + "; simulated, diagnostic only");
    Incident empty;
    CHECK(Contains(FormatReason(empty), "GPU unknown; driver unknown; device fault unavailable"));

    Monitor monitor;
    monitor.Report(Operation::DeviceWaitIdle, "idle", DeviceLostResult, 3);
    Monitor again;
    again.Report(Operation::DeviceWaitIdle, "idle", DeviceLostResult, 3);
    CHECK(monitor.Reason() == again.Reason());
}

void TestSimulationRequiresOptIn() {
    Operation operation{};
    std::uint64_t calls = 0;
    CHECK(!ParseSimulation("", operation, calls));
    CHECK(!ParseSimulation("bogus", operation, calls));
    CHECK(!ParseSimulation("submit:0", operation, calls));
    CHECK(!ParseSimulation("submit:abc", operation, calls));
    CHECK(!ParseSimulation("submit:", operation, calls));
    CHECK(!ParseSimulation("submit:1234567890", operation, calls));
    CHECK(ParseSimulation("submit", operation, calls) && operation == Operation::QueueSubmit && calls == 1);
    CHECK(ParseSimulation("present:3", operation, calls) && operation == Operation::QueuePresent && calls == 3);

    Monitor unarmed;
    CHECK(!ArmSimulationFromValue(unarmed, nullptr));
    CHECK(!ArmSimulationFromValue(unarmed, ""));
    CHECK(!ArmSimulationFromValue(unarmed, "nonsense"));
    for (int i = 0; i < 5; ++i) {
        CHECK(!unarmed.Simulate(Operation::QueueSubmit));
        CHECK(CallWhileHealthy(unarmed, Operation::QueueSubmit, "submit", 0, [] { return Success; }) == Success);
    }
    CHECK(!unarmed.IsLost());

    Fake fake;
    fake.report = CapturedReport();
    Monitor armed{MakeHooks(fake)};
    CHECK(ArmSimulationFromValue(armed, "submit:3"));
    int reached = 0;
    const auto submit = [&] {
        ++reached;
        return Success;
    };
    CHECK(CallWhileHealthy(armed, Operation::QueueSubmit, "submit", 1, submit) == Success);
    CHECK(CallWhileHealthy(armed, Operation::QueuePresent, "present", 1, submit) == Success);
    CHECK(CallWhileHealthy(armed, Operation::QueueSubmit, "submit", 2, submit) == Success);
    CHECK(!armed.IsLost());
    CHECK(CallWhileHealthy(armed, Operation::QueueSubmit, "submit", 3, submit) == DeviceLostResult);
    CHECK(reached == 3);
    CHECK(armed.IsLost() && armed.GetIncident().simulated);
    CHECK(fake.fault_queries == 0 && fake.state_logs == 0);
    CHECK(Contains(armed.Reason(), "simulated, diagnostic only"));

    const auto instance = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    CHECK(Contains(instance, "std::getenv(\"SOTB_SIMULATE_DEVICE_LOST\")"));
    const auto settings = ReadSource("gpu/shim/sotb_settings.cpp");
    CHECK(!Contains(settings, "SIMULATE_DEVICE_LOST"));
}

void TestUnrelatedVulkanErrorsDoNotMarkLoss() {
    Monitor monitor;
    for (const std::int32_t result : {-1, -2, -3, -5, -1000001004, 1, 2, 3, 5}) {
        CHECK(monitor.Report(Operation::QueueSubmit, "submit", result, 1) == Outcome::NotLost);
        CHECK(!monitor.IsLost());
        CHECK(CallWhileHealthy(monitor, Operation::QueueSubmit, "submit", 1, [&] { return result; }) == result);
    }
    CHECK(!monitor.IsLost() && monitor.FaultQueries() == 0 && monitor.StopRequests() == 0);
    CHECK(monitor.Report(Operation::QueueSubmit, "submit", Success, 1) == Outcome::NotLost);
}

void TestProductionWiring() {
    const auto instance = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    CHECK(Contains(instance, "DeviceLost::Global().ClearHooks(this)"));
    CHECK(Contains(instance, "RegisterDeviceLostHooks();"));
    CHECK(Contains(instance, "request_stop = &StopAfterDeviceLoss"));
    CHECK(Contains(instance, "WorkerStopped()"));
    CHECK(Contains(instance, "std::_Exit(DeviceLost::ExitStatus)"));
    CHECK(!Contains(instance, "void Instance::ReportDeviceFault"));

    const auto liverpool = ReadSource("gpu/shadps4/video_core/amdgpu/liverpool.cpp");
    CHECK(Contains(liverpool, "ShouldStop(device_lost, stoken)"));
    CHECK(Contains(liverpool, "NoteWorkerStopped()"));
    const auto process = liverpool.find("void Liverpool::Process(std::stop_token stoken)");
    const auto lost_break = liverpool.find("if (device_lost.IsLost()) {\n            break;", process);
    const auto done = liverpool.find("if (submit_done) {", process);
    CHECK(process != std::string::npos && lost_break != std::string::npos && lost_break < done);

    const auto swapchain = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp");
    CHECK(Contains(swapchain, "DeviceLost::Operation::AcquireNextImage"));
    CHECK(Contains(swapchain, "DeviceLost::Operation::QueuePresent"));
    CHECK(Contains(swapchain, "DeviceLost::Operation::DeviceWaitIdle"));
    const auto presenter = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    CHECK(Contains(presenter, "DeviceLost::IsLost()"));
    CHECK(Contains(presenter, "WaitFrameFence(frame, \"frmfence\")"));
    CHECK(Contains(presenter, "WaitFrameFence(frame, \"prsfence\")"));

    const auto assert_cpp = ReadSource("gpu/shim/common/assert.cpp");
    const auto assert_fail = assert_cpp.find("void assert_fail_impl()");
    CHECK(assert_cpp.find("Vulkan::DeviceLost::IsLost()", assert_fail) < assert_cpp.find("std::_Exit(23)", assert_fail));

    for (const char* path : {"gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp",
                             "gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp"}) {
        CHECK(!Contains(ReadSource(path), "eErrorDeviceLost"));
    }
}

} // namespace

int main() {
    TestHealthyInitialState();
    TestFirstLossOwnsTheIncident();
    TestCompetingLossesKeepOneOwner();
    TestRepeatedLossIsIdempotent();
    TestFirstReasonIsPreserved();
    TestSubmissionsAndSparseBindsRefusedAfterLoss();
    TestWaitsTerminateAfterLoss();
    TestWorkerLeavesItsLoopOnLoss();
    TestStopTokenShutdownStillWorks();
    TestLossDuringShutdownDoesNotDeadlock();
    TestFaultExtensionOrFunctionUnavailable();
    TestBoundedFaultCapture();
    TestIncompleteResultIsCaptured();
    TestFaultQueryFailures();
    TestFaultCountsAreCapped();
    TestDriverTextIsBoundedAndSanitized();
    TestFaultIsQueriedOnce();
    TestDeterministicReason();
    TestSimulationRequiresOptIn();
    TestUnrelatedVulkanErrorsDoNotMarkLoss();
    TestProductionWiring();
    std::printf("device lost tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

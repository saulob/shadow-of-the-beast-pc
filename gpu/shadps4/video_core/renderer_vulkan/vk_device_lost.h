// SPDX-License-Identifier: GPL-2.0-or-later
// The process-wide Vulkan device-lost state. The first thread that sees VK_ERROR_DEVICE_LOST owns the incident: it
// records the failure, queries the driver once and publishes the reason. Every later call stops without reaching the
// lost device. Vulkan-independent so it can be tested headless.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "video_core/renderer_vulkan/vk_fault_report.h"
#include "video_core/renderer_vulkan/vk_gpu_timeline.h"

namespace Vulkan::DeviceLost {

inline constexpr std::int32_t Success = 0;
inline constexpr std::int32_t Timeout = 2;
inline constexpr std::int32_t DeviceLostResult = -4;
inline constexpr int ExitStatus = 25;
inline constexpr std::size_t MaxReasonBytes = 1024;
inline constexpr std::uint32_t PresentSampleInterval = 16;
inline constexpr auto WaitSlice = std::chrono::milliseconds(100);
inline constexpr auto ShutdownGrace = std::chrono::seconds(2);

enum class Phase : std::uint32_t { Healthy, Claimed, Published };

enum class Outcome { NotLost, First, Repeat };

enum class Operation : std::uint32_t {
    QueueSubmit,
    QueueBindSparse,
    WaitSemaphores,
    GetSemaphoreCounterValue,
    WaitForFences,
    AcquireNextImage,
    QueuePresent,
    DeviceWaitIdle,
    AllocateMemory,
    CreateSwapchain,
    Count,
};

inline const char* OperationName(Operation operation) noexcept {
    switch (operation) {
    case Operation::QueueSubmit:
        return "vkQueueSubmit";
    case Operation::QueueBindSparse:
        return "vkQueueBindSparse";
    case Operation::WaitSemaphores:
        return "vkWaitSemaphores";
    case Operation::GetSemaphoreCounterValue:
        return "vkGetSemaphoreCounterValue";
    case Operation::WaitForFences:
        return "vkWaitForFences";
    case Operation::AcquireNextImage:
        return "vkAcquireNextImageKHR";
    case Operation::QueuePresent:
        return "vkQueuePresentKHR";
    case Operation::DeviceWaitIdle:
        return "vkDeviceWaitIdle";
    case Operation::AllocateMemory:
        return "vkAllocateMemory";
    case Operation::CreateSwapchain:
        return "vkCreateSwapchainKHR";
    case Operation::Count:
        break;
    }
    return "unknown Vulkan operation";
}

struct Incident {
    Operation operation{};
    const char* stage = "";
    std::int32_t result{};
    std::uint64_t tick{};
    bool simulated{};
    std::string gpu;
    std::string driver;
    FaultReport fault;
    std::string reason;
};

inline std::string FormatFaultSummary(const FaultReport& fault) {
    char text[MaxFaultTextBytes + 128];
    switch (fault.status) {
    case FaultStatus::Unsupported:
        return "device fault unavailable (no VK_EXT_device_fault)";
    case FaultStatus::Failed:
        std::snprintf(text, sizeof(text), "device fault query failed (result %d)", fault.result);
        return text;
    case FaultStatus::Captured:
        break;
    }
    std::string summary = "device fault: " + (fault.description.empty() ? "(no description)" : fault.description);
    if (fault.address_count != 0 && !fault.addresses.empty()) {
        std::snprintf(text, sizeof(text), ", %u addresses, first type %u at %#llx", fault.address_count,
                      fault.addresses[0].type, static_cast<unsigned long long>(fault.addresses[0].address));
        summary += text;
    }
    if (fault.vendor_count != 0 && !fault.vendors.empty()) {
        std::snprintf(text, sizeof(text), ", %u vendor faults, first %#llx", fault.vendor_count,
                      static_cast<unsigned long long>(fault.vendors[0].code));
        summary += text;
    }
    return summary;
}

inline std::string FormatReason(const Incident& incident) {
    char text[256];
    std::snprintf(text, sizeof(text), "GPU device lost: %s (%s) result %d, tick %llu",
                  OperationName(incident.operation), incident.stage, incident.result,
                  static_cast<unsigned long long>(incident.tick));
    std::string reason = text;
    reason += "; GPU " + (incident.gpu.empty() ? "unknown" : incident.gpu);
    reason += "; driver " + (incident.driver.empty() ? "unknown" : incident.driver);
    reason += "; " + FormatFaultSummary(incident.fault);
    if (incident.simulated) {
        reason += "; simulated, diagnostic only";
    }
    if (reason.size() > MaxReasonBytes) {
        reason.resize(MaxReasonBytes);
    }
    return reason;
}

// One line of the GPU timeline dump, without allocation.
inline void FormatTimelineEntry(char* out, std::size_t size, const TimelineEntry& entry) noexcept {
    char tick[48] = "";
    if (entry.timeline != Timeline::None) {
        std::snprintf(tick, sizeof(tick), ", %s tick %llu", TimelineName(entry.timeline),
                      static_cast<unsigned long long>(entry.tick));
    }
    std::snprintf(out, size, "GPU timeline: #%llu thread %u %s (%s)%s, result %d%s",
                  static_cast<unsigned long long>(entry.sequence), entry.thread,
                  OperationName(Operation(entry.kind)), entry.stage, tick, entry.result,
                  entry.result == DeviceLostResult ? " DEVICE LOST" : "");
}

struct Hooks {
    void* context{};
    FaultReport (*query_fault)(void* context){};
    void (*log_state)(void* context){};
    void (*print)(const char* line){};
    // Runs once after the reason is published; must not block on the GPU.
    void (*request_stop)(const char* reason){};
};

class Monitor {
public:
    Monitor() = default;
    explicit Monitor(const Hooks& hooks_) : hooks{hooks_} {}
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    [[nodiscard]] bool IsLost() const noexcept {
        return phase.load(std::memory_order_acquire) != Phase::Healthy;
    }
    [[nodiscard]] Phase CurrentPhase() const noexcept {
        return phase.load(std::memory_order_acquire);
    }

    void SetHooks(const Hooks& new_hooks) {
        std::scoped_lock lock{state_mutex};
        hooks = new_hooks;
    }
    void ClearHooks(void* context) {
        std::scoped_lock lock{state_mutex};
        if (hooks.context == context) {
            hooks = {};
        }
    }
    void SetDevice(std::string gpu_name, std::string driver_name) {
        std::scoped_lock lock{state_mutex};
        gpu = std::move(gpu_name);
        driver = std::move(driver_name);
    }

    // Only VK_ERROR_DEVICE_LOST changes state; any other result is the caller's to handle. Every result is recorded in
    // the timeline first, so the owner's own failing call is part of the history it dumps. A successful counter poll
    // is not: it would push the submissions out of the ring. Successful acquires and presents happen once per
    // presentation, up to hundreds a second, so one in PresentSampleInterval is kept; any other result is always kept.
    Outcome Report(Operation operation, const char* stage, std::int32_t result, std::uint64_t tick,
                   Timeline timeline = Timeline::None) {
        if (ShouldRecord(operation, result)) {
            events.Record(std::uint32_t(operation), stage, result, tick, timeline);
        }
        if (result != DeviceLostResult) {
            return Outcome::NotLost;
        }
        Phase expected = Phase::Healthy;
        if (!phase.compare_exchange_strong(expected, Phase::Claimed, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            repeats.fetch_add(1, std::memory_order_relaxed);
            return Outcome::Repeat;
        }
        Own(operation, stage, result, tick);
        return Outcome::First;
    }

    [[nodiscard]] bool ShouldRecord(Operation operation, std::int32_t result) noexcept {
        if (result != Success) {
            return true;
        }
        switch (operation) {
        case Operation::GetSemaphoreCounterValue:
            return false;
        case Operation::AcquireNextImage:
            return acquire_successes.fetch_add(1, std::memory_order_relaxed) % PresentSampleInterval == 0;
        case Operation::QueuePresent:
            return present_successes.fetch_add(1, std::memory_order_relaxed) % PresentSampleInterval == 0;
        default:
            return true;
        }
    }

    void NoteRefused() noexcept {
        refused.fetch_add(1, std::memory_order_relaxed);
    }

    // The GPU command thread has left its loop because of the loss.
    void NoteWorkerStopped() noexcept {
        worker_stopped.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool WorkerStopped() const noexcept {
        return worker_stopped.load(std::memory_order_acquire);
    }

    // "submit:600" style arming for tests; the Nth call of the operation reports a loss without reaching Vulkan.
    void ArmSimulation(Operation operation, std::uint64_t calls) noexcept {
        simulated_calls.store(calls ? calls : 1, std::memory_order_relaxed);
        simulated_operation.store(std::uint32_t(operation), std::memory_order_release);
    }
    [[nodiscard]] bool Simulate(Operation operation) noexcept {
        if (simulated_operation.load(std::memory_order_acquire) != std::uint32_t(operation)) {
            return false;
        }
        if (simulated_calls.fetch_sub(1, std::memory_order_relaxed) != 1) {
            return false;
        }
        simulated_hit.store(true, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool HasPublishedIncident() const noexcept {
        return phase.load(std::memory_order_acquire) == Phase::Published;
    }
    // Valid once HasPublishedIncident().
    [[nodiscard]] const Incident& GetIncident() const noexcept {
        return incident;
    }
    [[nodiscard]] std::string Reason() const {
        return HasPublishedIncident() ? incident.reason : std::string("GPU device lost (diagnostics in progress)");
    }
    [[nodiscard]] std::uint32_t Repeats() const noexcept {
        return repeats.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t Refused() const noexcept {
        return refused.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t FaultQueries() const noexcept {
        return fault_queries.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t StopRequests() const noexcept {
        return stop_requests.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const EventRing& Events() const noexcept {
        return events;
    }
    [[nodiscard]] std::uint32_t TimelineDumps() const noexcept {
        return timeline_dumps.load(std::memory_order_relaxed);
    }

private:
    void Print(const Hooks& snapshot, const char* line) const {
        if (snapshot.print) {
            snapshot.print(line);
        } else {
            std::fprintf(stderr, "%s\n", line);
            std::fflush(stderr);
        }
    }
    void Print(const Hooks& snapshot, const std::string& line) const {
        Print(snapshot, line.c_str());
    }

    // The owner's one dump of the recent GPU history; fixed buffers only.
    void PrintTimeline(const Hooks& snapshot) {
        timeline_dumps.fetch_add(1, std::memory_order_relaxed);
        TimelineEntry history[TimelineCapacity];
        const std::size_t count = events.Snapshot(history, TimelineCapacity);
        char line[256];
        std::snprintf(line, sizeof(line), "GPU timeline: latest %zu of %llu GPU operations before the loss", count,
                      static_cast<unsigned long long>(events.Recorded()));
        Print(snapshot, line);
        const TimelineEntry* last_success = nullptr;
        const TimelineEntry* last_submit[std::size_t(Timeline::Count)]{};
        for (std::size_t i = 0; i < count; ++i) {
            const TimelineEntry& entry = history[i];
            if (entry.result != Success) {
                continue;
            }
            last_success = &entry;
            if ((entry.kind == std::uint32_t(Operation::QueueSubmit) ||
                 entry.kind == std::uint32_t(Operation::QueueBindSparse)) &&
                std::size_t(entry.timeline) < std::size_t(Timeline::Count)) {
                last_submit[std::size_t(entry.timeline)] = &entry;
            }
        }
        if (last_success) {
            FormatTimelineEntry(line, sizeof(line), *last_success);
            Print(snapshot, "GPU timeline: last successful operation:");
            Print(snapshot, line);
        } else {
            Print(snapshot, "GPU timeline: no successful operation recorded");
        }
        for (std::size_t i = 1; i < std::size_t(Timeline::Count); ++i) {
            if (last_submit[i]) {
                std::snprintf(line, sizeof(line), "GPU timeline: %s timeline last submitted tick %llu (#%llu)",
                              TimelineName(Timeline(i)), static_cast<unsigned long long>(last_submit[i]->tick),
                              static_cast<unsigned long long>(last_submit[i]->sequence));
                Print(snapshot, line);
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            FormatTimelineEntry(line, sizeof(line), history[i]);
            Print(snapshot, line);
        }
    }

    FaultReport QueryFault(const Hooks& snapshot, bool simulated) {
        FaultReport report;
        if (simulated || !snapshot.query_fault || fault_queried.exchange(true, std::memory_order_acq_rel)) {
            return report;
        }
        fault_queries.fetch_add(1, std::memory_order_relaxed);
        try {
            report = snapshot.query_fault(snapshot.context);
        } catch (...) {
            report = FaultReport{};
            report.status = FaultStatus::Failed;
        }
        return report;
    }

    void Own(Operation operation, const char* stage, std::int32_t result, std::uint64_t tick) {
        Hooks snapshot;
        {
            std::scoped_lock lock{state_mutex};
            snapshot = hooks;
            incident.gpu = gpu;
            incident.driver = driver;
        }
        incident.operation = operation;
        incident.stage = stage ? stage : "";
        incident.result = result;
        incident.tick = tick;
        incident.simulated = simulated_hit.load(std::memory_order_relaxed);
        Print(snapshot, "GPU device lost: first detected in " + std::string(OperationName(operation)) + " (" +
                            incident.stage + ")");
        PrintTimeline(snapshot);
        if (snapshot.log_state && !incident.simulated) {
            snapshot.log_state(snapshot.context);
        }
        incident.fault = QueryFault(snapshot, incident.simulated);
        for (const auto& line : FormatFaultReport(incident.fault)) {
            Print(snapshot, line);
        }
        incident.reason = FormatReason(incident);
        Print(snapshot, incident.reason);
        phase.store(Phase::Published, std::memory_order_release);
        if (snapshot.request_stop) {
            stop_requests.fetch_add(1, std::memory_order_relaxed);
            snapshot.request_stop(incident.reason.c_str());
        }
    }

    std::atomic<Phase> phase{Phase::Healthy};
    std::atomic<bool> fault_queried{};
    std::atomic<bool> worker_stopped{};
    std::atomic<std::uint32_t> repeats{};
    std::atomic<std::uint32_t> refused{};
    std::atomic<std::uint32_t> fault_queries{};
    std::atomic<std::uint32_t> stop_requests{};
    std::atomic<std::uint32_t> timeline_dumps{};
    std::atomic<std::uint32_t> acquire_successes{};
    std::atomic<std::uint32_t> present_successes{};
    EventRing events;
    std::atomic<std::uint32_t> simulated_operation{std::uint32_t(Operation::Count)};
    std::atomic<std::uint64_t> simulated_calls{};
    std::atomic<bool> simulated_hit{};
    std::mutex state_mutex;
    Hooks hooks;
    std::string gpu;
    std::string driver;
    Incident incident;
};

inline Monitor& Global() noexcept {
    static Monitor monitor;
    return monitor;
}

[[nodiscard]] inline bool IsLost() noexcept {
    return Global().IsLost();
}

// "submit:600" arms the 600th queue submission, "submit" the first; anything else arms nothing.
inline bool ParseSimulation(std::string_view value, Operation& operation, std::uint64_t& calls) noexcept {
    constexpr std::string_view names[] = {"submit",  "sparse",  "semwait", "semread", "fence",
                                          "acquire", "present", "idle",    "alloc",  "swapchain"};
    static_assert(std::size(names) == std::size_t(Operation::Count));
    const std::size_t colon = value.find(':');
    const std::string_view name = value.substr(0, colon);
    calls = 1;
    if (colon != std::string_view::npos) {
        const std::string_view digits = value.substr(colon + 1);
        if (digits.empty() || digits.size() > 9) {
            return false;
        }
        std::uint64_t parsed = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9') {
                return false;
            }
            parsed = parsed * 10 + std::uint64_t(c - '0');
        }
        if (parsed == 0) {
            return false;
        }
        calls = parsed;
    }
    for (std::uint32_t i = 0; i < std::uint32_t(Operation::Count); ++i) {
        if (name == names[i]) {
            operation = Operation(i);
            return true;
        }
    }
    return false;
}

// Armed only by an explicit environment value; never read from a settings file.
inline bool ArmSimulationFromValue(Monitor& monitor, const char* value) {
    if (!value || !value[0]) {
        return false;
    }
    Operation operation{};
    std::uint64_t calls = 0;
    if (!ParseSimulation(value, operation, calls)) {
        return false;
    }
    monitor.ArmSimulation(operation, calls);
    return true;
}

// A Vulkan call that must not reach a lost device. Returns VK_ERROR_DEVICE_LOST without calling once the device is lost.
template <typename Call>
std::int32_t CallWhileHealthy(Monitor& monitor, Operation operation, const char* stage, std::uint64_t tick,
                              Call&& call, Timeline timeline = Timeline::None) {
    if (monitor.IsLost()) {
        monitor.NoteRefused();
        return DeviceLostResult;
    }
    const std::int32_t result = monitor.Simulate(operation) ? DeviceLostResult : static_cast<std::int32_t>(call());
    monitor.Report(operation, stage, result, tick, timeline);
    return result;
}

// A wait made of timed slices (`wait_once` returns Timeout when its slice ends): it ends as soon as the device is
// lost instead of waiting for a signal that cannot come.
template <typename WaitOnce>
std::int32_t WaitWhileHealthy(Monitor& monitor, Operation operation, const char* stage, std::uint64_t tick,
                              WaitOnce&& wait_once, Timeline timeline = Timeline::None) {
    for (;;) {
        if (monitor.IsLost()) {
            monitor.NoteRefused();
            return DeviceLostResult;
        }
        const std::int32_t result =
            monitor.Simulate(operation) ? DeviceLostResult : static_cast<std::int32_t>(wait_once());
        if (result != Timeout) {
            monitor.Report(operation, stage, result, tick, timeline);
            return result;
        }
    }
}

template <typename StopToken>
[[nodiscard]] bool ShouldStop(const Monitor& monitor, const StopToken& stop) noexcept {
    return stop.stop_requested() || monitor.IsLost();
}

} // namespace Vulkan::DeviceLost

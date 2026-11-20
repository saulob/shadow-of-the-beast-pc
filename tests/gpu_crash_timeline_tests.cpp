// GPU crash timeline tests: the fixed-size event ring (order, wrap, payloads, concurrent producers and snapshots, no
// heap), its text, and its single dump by the device-lost owner, on fake events. No Vulkan device, no window.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "video_core/renderer_vulkan/vk_device_lost.h"

namespace {

using namespace Vulkan::DeviceLost;

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

std::atomic<bool> g_count_allocations{false};
std::atomic<std::size_t> g_allocations{0};

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

std::size_t CountOf(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

constexpr std::int32_t NoMemory = -2;

struct Capture {
    std::mutex mutex;
    std::vector<std::string> lines;
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
    hooks.request_stop = [](const char*) { ++g_capture->stops; };
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

std::string Joined(const std::vector<std::string>& lines) {
    std::string all;
    for (const auto& line : lines) {
        all += line + '\n';
    }
    return all;
}

std::size_t TimelineLines(const std::vector<std::string>& lines) {
    return std::size_t(std::count_if(lines.begin(), lines.end(), [](const std::string& line) {
        return line.rfind("GPU timeline:", 0) == 0;
    }));
}

void TestEmptyTimeline() {
    EventRing ring;
    TimelineEntry out[4];
    CHECK(ring.Snapshot(out, 4) == 0);
    CHECK(ring.Recorded() == 0);
    CHECK(ring.Snapshot(out, 0) == 0);
}

void TestOneEvent() {
    EventRing ring;
    const std::uint64_t sequence =
        ring.Record(std::uint32_t(Operation::QueueSubmit), "submit", Success, 600, Timeline::Work);
    CHECK(sequence == 1 && ring.Recorded() == 1);
    TimelineEntry out[4];
    CHECK(ring.Snapshot(out, 4) == 1);
    CHECK(out[0].sequence == 1 && out[0].kind == std::uint32_t(Operation::QueueSubmit));
    CHECK(out[0].result == Success && out[0].tick == 600 && out[0].timeline == Timeline::Work);
    CHECK(std::string(out[0].stage) == "submit" && out[0].thread != 0);
}

void TestOrderingAndSequence() {
    EventRing ring;
    for (std::uint64_t i = 1; i <= 50; ++i) {
        CHECK(ring.Record(0, "order", Success, i * 10) == i);
    }
    TimelineEntry out[TimelineCapacity];
    const std::size_t count = ring.Snapshot(out, TimelineCapacity);
    CHECK(count == 50);
    bool ordered = true;
    for (std::size_t i = 0; i < count; ++i) {
        ordered = ordered && out[i].sequence == i + 1 && out[i].tick == (i + 1) * 10;
    }
    CHECK(ordered);
}

void TestRingWrapDropsTheOldest() {
    EventRing ring;
    const std::uint64_t total = TimelineCapacity + 10;
    for (std::uint64_t i = 1; i <= total; ++i) {
        ring.Record(1, "wrap", Success, i);
    }
    TimelineEntry out[TimelineCapacity + 8];
    const std::size_t count = ring.Snapshot(out, TimelineCapacity + 8);
    CHECK(count == TimelineCapacity);
    CHECK(out[0].sequence == 11 && out[0].tick == 11);
    CHECK(out[count - 1].sequence == total && out[count - 1].tick == total);
    CHECK(ring.Recorded() == total);
    CHECK(ring.Snapshot(out, 5) == 5 && out[0].sequence == 11);
}

void TestEveryOperationAndPayload() {
    EventRing ring;
    for (std::uint32_t i = 0; i < std::uint32_t(Operation::Count); ++i) {
        ring.Record(i, "kind", std::int32_t(i) - 5, UINT64_C(0xFFFFFFFFFFFFFF00) + i, Timeline::Memory);
    }
    TimelineEntry out[TimelineCapacity];
    const std::size_t count = ring.Snapshot(out, TimelineCapacity);
    CHECK(count == std::size_t(Operation::Count));
    bool preserved = true;
    for (std::uint32_t i = 0; i < count; ++i) {
        char text[256];
        FormatTimelineEntry(text, sizeof(text), out[i]);
        preserved = preserved && out[i].kind == i && out[i].result == std::int32_t(i) - 5 &&
                    out[i].tick == UINT64_C(0xFFFFFFFFFFFFFF00) + i && out[i].timeline == Timeline::Memory &&
                    Contains(text, OperationName(Operation(i)));
    }
    CHECK(preserved);

    EventRing odd;
    odd.Record(2, "arena backing", -1000000, 0);
    odd.Record(3, nullptr, DeviceLostResult, 1, Timeline::Work);
    odd.Record(4, "", 5, 2);
    TimelineEntry two[3];
    CHECK(odd.Snapshot(two, 3) == 3);
    CHECK(std::string(two[0].stage) == "arena ba" && two[0].result == -1000000);
    CHECK(std::string(two[1].stage).empty() && two[1].result == DeviceLostResult && two[1].timeline == Timeline::Work);
    CHECK(two[2].timeline == Timeline::None && two[2].result == 5);
}

void TestDeterministicText() {
    TimelineEntry entry;
    entry.sequence = 12;
    entry.kind = std::uint32_t(Operation::QueueSubmit);
    entry.result = Success;
    entry.tick = 600;
    entry.thread = 3;
    entry.timeline = Timeline::Work;
    std::snprintf(entry.stage, sizeof(entry.stage), "%s", "submit");
    char text[256];
    FormatTimelineEntry(text, sizeof(text), entry);
    CHECK(std::string(text) == "GPU timeline: #12 thread 3 vkQueueSubmit (submit), work tick 600, result 0");
    char again[256];
    FormatTimelineEntry(again, sizeof(again), entry);
    CHECK(std::string(again) == text);

    entry.kind = std::uint32_t(Operation::QueuePresent);
    entry.result = DeviceLostResult;
    entry.timeline = Timeline::None;
    std::snprintf(entry.stage, sizeof(entry.stage), "%s", "present");
    FormatTimelineEntry(text, sizeof(text), entry);
    CHECK(std::string(text) == "GPU timeline: #12 thread 3 vkQueuePresentKHR (present), result -4 DEVICE LOST");

    entry.kind = 9999;
    entry.timeline = Timeline::Memory;
    FormatTimelineEntry(text, sizeof(text), entry);
    CHECK(Contains(text, "unknown Vulkan operation") && Contains(text, "memory tick 600"));
}

void TestBoundedFormatting() {
    TimelineEntry entry;
    entry.sequence = UINT64_MAX;
    entry.tick = UINT64_MAX;
    entry.thread = UINT32_MAX;
    entry.timeline = Timeline::Work;
    std::memset(entry.stage, 'S', StageBytes);
    char text[40];
    std::memset(text, 'x', sizeof(text));
    FormatTimelineEntry(text, 24, entry);
    CHECK(std::strlen(text) < 24);
    CHECK(text[30] == 'x' && text[sizeof(text) - 1] == 'x');
}

void TestNoHeapInTheFailurePath() {
    static_assert(std::is_trivially_copyable_v<TimelineEntry>);
    static_assert(sizeof(EventRing) <= 8192);
    EventRing ring;
    TimelineEntry out[TimelineCapacity];
    char text[256];
    g_allocations = 0;
    g_count_allocations = true;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        ring.Record(i % std::uint32_t(Operation::Count), "heap", Success, i, Timeline::Work);
    }
    const std::size_t count = ring.Snapshot(out, TimelineCapacity);
    for (std::size_t i = 0; i < count; ++i) {
        FormatTimelineEntry(text, sizeof(text), out[i]);
    }
    g_count_allocations = false;
    CHECK(count == TimelineCapacity && g_allocations == 0);
}

void TestConcurrentProducersAndSnapshots() {
    constexpr int Producers = 8;
    constexpr std::uint64_t PerProducer = 4000;
    EventRing ring;
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> unordered{0};
    std::atomic<int> snapshots{0};
    std::thread reader([&] {
        TimelineEntry out[TimelineCapacity];
        while (!stop.load()) {
            const std::size_t count = ring.Snapshot(out, TimelineCapacity);
            for (std::size_t i = 0; i < count; ++i) {
                if (out[i].tick / 1000000 != out[i].kind || out[i].tick % 1000000 != std::uint32_t(out[i].result)) {
                    ++torn;
                }
                if (i && out[i].sequence <= out[i - 1].sequence) {
                    ++unordered;
                }
            }
            ++snapshots;
        }
    });
    std::vector<std::thread> producers;
    for (int p = 0; p < Producers; ++p) {
        producers.emplace_back([&, p] {
            for (std::uint64_t i = 0; i < PerProducer; ++i) {
                ring.Record(std::uint32_t(p), "race", std::int32_t(i), std::uint64_t(p) * 1000000 + i);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    stop = true;
    reader.join();
    CHECK(torn == 0 && unordered == 0 && snapshots > 0);
    CHECK(ring.Recorded() == Producers * PerProducer);

    TimelineEntry out[TimelineCapacity];
    const std::size_t count = ring.Snapshot(out, TimelineCapacity);
    CHECK(count == TimelineCapacity);
    bool consecutive = true;
    std::uint64_t last_tick[Producers] = {};
    bool per_thread_order = true;
    for (std::size_t i = 0; i < count; ++i) {
        consecutive = consecutive && (i == 0 || out[i].sequence == out[i - 1].sequence + 1);
        const std::uint32_t producer = out[i].kind;
        per_thread_order = per_thread_order && out[i].tick >= last_tick[producer];
        last_tick[producer] = out[i].tick;
    }
    CHECK(consecutive && per_thread_order);
}

void TestWhatIsRecorded() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    const auto ok = [] { return Success; };
    for (int i = 0; i < 10; ++i) {
        CHECK(CallWhileHealthy(monitor, Operation::GetSemaphoreCounterValue, "semread", 5, ok, Timeline::Work) ==
              Success);
    }
    CHECK(monitor.Events().Recorded() == 0);
    CHECK(CallWhileHealthy(monitor, Operation::GetSemaphoreCounterValue, "semread", 5, [] { return NoMemory; },
                           Timeline::Work) == NoMemory);
    CHECK(monitor.Events().Recorded() == 1 && !monitor.IsLost());

    int calls = 0;
    CHECK(WaitWhileHealthy(monitor, Operation::WaitSemaphores, "semwait", 7,
                           [&] { return ++calls < 3 ? Timeout : Success; }, Timeline::Work) == Success);
    CHECK(calls == 3 && monitor.Events().Recorded() == 2);

    CHECK(monitor.Report(Operation::QueuePresent, "present", Success, 0) == Outcome::NotLost);
    CHECK(monitor.Events().Recorded() == 3);
    CHECK(capture.lines.empty() && monitor.TimelineDumps() == 0);
}

void TestFirstLossDumpsTheHistoryOnce() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    monitor.SetDevice("Test GPU", "1.2.3");
    const auto ok = [] { return Success; };
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        CHECK(CallWhileHealthy(monitor, Operation::QueueSubmit, "submit", tick, ok, Timeline::Work) == Success);
    }
    CHECK(CallWhileHealthy(monitor, Operation::QueueBindSparse, "sparse", 1, ok, Timeline::Memory) == Success);
    CHECK(CallWhileHealthy(monitor, Operation::GetSemaphoreCounterValue, "semread", 3, ok, Timeline::Work) == Success);
    CHECK(CallWhileHealthy(monitor, Operation::QueueSubmit, "submit", 4, [] { return DeviceLostResult; },
                           Timeline::Work) == DeviceLostResult);

    CHECK(monitor.IsLost() && monitor.TimelineDumps() == 1 && capture.stops == 1);
    const auto& lines = capture.lines;
    const int detected = IndexOf(lines, "first detected in vkQueueSubmit (submit)");
    const int header = IndexOf(lines, "GPU timeline: latest 5 of 5 GPU operations");
    const int success = IndexOf(lines, "GPU timeline: last successful operation:");
    const int last = IndexOf(lines, "vkQueueBindSparse (sparse), memory tick 1, result 0");
    const int work = IndexOf(lines, "GPU timeline: work timeline last submitted tick 3 (#3)");
    const int memory = IndexOf(lines, "GPU timeline: memory timeline last submitted tick 1 (#4)");
    const int lost = IndexOf(lines, "vkQueueSubmit (submit), work tick 4, result -4 DEVICE LOST");
    const int reason = IndexOf(lines, "GPU device lost: vkQueueSubmit (submit) result -4, tick 4;");
    CHECK(detected >= 0 && header > detected && success > header && last > success);
    CHECK(work > success && memory > work && lost > memory && reason > lost);
    CHECK(TimelineLines(lines) == 5 + 5);
    CHECK(IndexOf(lines, "#1 thread") >= 0 && IndexOf(lines, "#5 thread") >= 0 && IndexOf(lines, "#6 thread") < 0);

    CHECK(monitor.Reason() == FormatReason(monitor.GetIncident()));
    CHECK(Contains(monitor.Reason(), "GPU device lost: vkQueueSubmit (submit) result -4, tick 4; GPU Test GPU; "
                                     "driver 1.2.3; device fault unavailable"));
    CHECK(!Contains(monitor.Reason(), "timeline") && !Contains(monitor.Reason(), "work"));
}

void TestRepeatedLossDoesNotDumpAgain() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    CHECK(monitor.Report(Operation::QueueSubmit, "submit", DeviceLostResult, 9, Timeline::Work) == Outcome::First);
    const std::size_t lines = capture.lines.size();
    CHECK(monitor.TimelineDumps() == 1);
    CHECK(monitor.Report(Operation::QueuePresent, "present", DeviceLostResult, 0) == Outcome::Repeat);
    CHECK(CallWhileHealthy(monitor, Operation::QueueSubmit, "submit", 10, [] { return Success; }) == DeviceLostResult);
    CHECK(monitor.TimelineDumps() == 1 && capture.lines.size() == lines);
    CHECK(monitor.Events().Recorded() == 2);
}

void TestCompetingLossesDumpOnce() {
    for (int round = 0; round < 25; ++round) {
        Capture capture;
        Monitor monitor{MakeHooks(capture)};
        for (std::uint64_t tick = 1; tick <= 5; ++tick) {
            monitor.Report(Operation::QueueSubmit, "submit", Success, tick, Timeline::Work);
        }
        std::atomic<int> first{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&, i] {
                while (!go.load()) {
                }
                if (monitor.Report(Operation(i % int(Operation::Count)), "race", DeviceLostResult, 100 + i,
                                   Timeline::Work) == Outcome::First) {
                    ++first;
                }
            });
        }
        go = true;
        for (auto& thread : threads) {
            thread.join();
        }
        CHECK(first == 1 && monitor.TimelineDumps() == 1);
        CHECK(CountOf(Joined(capture.lines), "GPU timeline: latest") == 1);
    }
}

void TestDumpIsBounded() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    for (std::uint64_t tick = 1; tick <= 1000; ++tick) {
        monitor.Report(Operation::QueueSubmit, "submit", Success, tick, Timeline::Work);
    }
    monitor.Report(Operation::QueueBindSparse, "sparse", Success, 1, Timeline::Memory);
    monitor.Report(Operation::QueuePresent, "present", DeviceLostResult, 0);
    CHECK(IndexOf(capture.lines, "GPU timeline: latest 128 of 1002 GPU operations") >= 0);
    CHECK(TimelineLines(capture.lines) == TimelineCapacity + 5);
    std::size_t longest = 0;
    for (const auto& line : capture.lines) {
        longest = std::max(longest, line.size());
    }
    CHECK(longest < 256);
    CHECK(IndexOf(capture.lines, "#875 thread") >= 0 && IndexOf(capture.lines, "#874 thread") < 0);
}

void TestNoSuccessfulOperation() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    CHECK(monitor.Report(Operation::AcquireNextImage, "acquire", DeviceLostResult, 0) == Outcome::First);
    CHECK(IndexOf(capture.lines, "GPU timeline: no successful operation recorded") >= 0);
    CHECK(IndexOf(capture.lines, "GPU timeline: latest 1 of 1 GPU operations") >= 0);
    CHECK(IndexOf(capture.lines, "vkAcquireNextImageKHR (acquire), result -4 DEVICE LOST") >= 0);
}

void TestShutdownAfterLossKeepsTheTimeline() {
    Capture capture;
    Monitor monitor{MakeHooks(capture)};
    monitor.Report(Operation::QueueSubmit, "submit", Success, 1, Timeline::Work);
    monitor.Report(Operation::QueueSubmit, "submit", DeviceLostResult, 2, Timeline::Work);
    monitor.ClearHooks(&capture);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            for (int n = 0; n < 200; ++n) {
                monitor.Report(Operation::WaitSemaphores, "semwait", Success, 3, Timeline::Work);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    TimelineEntry out[TimelineCapacity];
    const std::size_t count = monitor.Events().Snapshot(out, TimelineCapacity);
    CHECK(count == TimelineCapacity && monitor.Events().Recorded() == 802);
    bool consecutive = true;
    for (std::size_t i = 1; i < count; ++i) {
        consecutive = consecutive && out[i].sequence == out[i - 1].sequence + 1;
    }
    CHECK(consecutive && monitor.TimelineDumps() == 1);
}

void TestProductionWiring() {
    const std::string timeline = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_gpu_timeline.h");
    CHECK(!Contains(timeline, "std::vector") && !Contains(timeline, "std::string") && !Contains(timeline, "malloc") &&
          !Contains(timeline, "std::mutex") && !Contains(timeline, "new "));
    const std::string lost = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_device_lost.h");
    CHECK(CountOf(lost, "PrintTimeline(snapshot);") == 1);
    CHECK(lost.find("events.Record(") < lost.find("compare_exchange_strong"));
    CHECK(lost.find("PrintTimeline(snapshot);") < lost.find("incident.fault = QueryFault"));
    const std::string scheduler = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp");
    CHECK(Contains(scheduler, "DeviceLost::Timeline::Work);"));
    const std::string cache = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    CHECK(CountOf(cache, "DeviceLost::Timeline::Memory") == 2);
    const std::string semaphore = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.cpp");
    CHECK(CountOf(semaphore, "timeline);") == 2 && Contains(semaphore, "timeline{timeline_}"));
    const std::string header = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_semaphore.h");
    CHECK(Contains(header, "DeviceLost::Timeline timeline_ = DeviceLost::Timeline::Work"));
}

} // namespace

void* operator new(std::size_t size) {
    if (g_count_allocations.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    TestEmptyTimeline();
    TestOneEvent();
    TestOrderingAndSequence();
    TestRingWrapDropsTheOldest();
    TestEveryOperationAndPayload();
    TestDeterministicText();
    TestBoundedFormatting();
    TestNoHeapInTheFailurePath();
    TestConcurrentProducersAndSnapshots();
    TestWhatIsRecorded();
    TestFirstLossDumpsTheHistoryOnce();
    TestRepeatedLossDoesNotDumpAgain();
    TestCompetingLossesDumpOnce();
    TestDumpIsBounded();
    TestNoSuccessfulOperation();
    TestShutdownAfterLossKeepsTheTimeline();
    TestProductionWiring();
    std::printf("gpu crash timeline tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

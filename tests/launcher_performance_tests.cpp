// Launcher loop telemetry: render rate, UI-thread CPU, build/submit/present timing, runaway-loop and present-stall
// detection, warning limits and formatting, on synthetic timestamps. No window, no clock.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "launcher_pacing.h"
#include "test_progress.h"

namespace fs = std::filesystem;
using namespace Sotb;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(condition)) {                                                       \
            ++g_failures;                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                         \
        sotb_test_progress(g_checks);                                             \
    } while (0)

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

constexpr FrameTimes kNormal{2 * kNsPerMs, 1 * kNsPerMs, 3 * kNsPerMs};

// A launcher drawing at `hz` for a while, with `cpu_fraction` of the wall time spent on the UI thread (negative:
// unavailable).
struct Sim {
    LoopMonitor monitor;
    int64_t now = 1'000 * kNsPerSecond;
    int64_t cpu = 0;
    double cpu_fraction = 0.1;
    int cpu_calls = 0;
    int frames = 0;
    std::vector<LoopSample> samples;
    int rate_warnings = 0;
    int stall_warnings = 0;
    int cpu_warnings = 0;

    void Frame(int64_t step_ns, const FrameTimes& times) {
        now += step_ns;
        cpu += int64_t(double(step_ns) * (cpu_fraction < 0 ? 0 : cpu_fraction));
        ++frames;
        const auto sample = monitor.FrameDrawn(now, times, [&] {
            ++cpu_calls;
            return cpu_fraction < 0 ? int64_t(-1) : cpu;
        });
        if (sample) {
            samples.push_back(*sample);
            rate_warnings += sample->excessive_rate ? 1 : 0;
            stall_warnings += sample->present_stall ? 1 : 0;
            cpu_warnings += sample->high_cpu ? 1 : 0;
        }
    }

    void Run(double hz, double seconds, const FrameTimes& times = kNormal) {
        const int64_t step = int64_t(double(kNsPerSecond) / hz);
        const int64_t end = now + int64_t(seconds * double(kNsPerSecond));
        while (now < end) {
            Frame(step, times);
        }
    }
};

void TestNormalSixtyHertz() {
    Sim sim;
    sim.Run(60, 60);
    CHECK(sim.samples.size() >= 10 && sim.samples.size() <= 12);
    CHECK(sim.rate_warnings == 0 && sim.stall_warnings == 0 && sim.cpu_warnings == 0);
    for (const LoopSample& sample : sim.samples) {
        CHECK(sample.hz > 59.0 && sample.hz < 61.0 && sample.frames > 290 && sample.frames < 310);
    }
    CHECK(sim.samples.front().first && !sim.samples.back().first);
    CHECK(sim.samples.front().cpu_pct >= 9 && sim.samples.front().cpu_pct <= 11);
}

void TestRenderRateMeasurement() {
    for (const double hz : {30.0, 60.0, 85.0}) {
        Sim sim;
        sim.Run(hz, 20);
        CHECK(!sim.samples.empty() && sim.samples[0].hz > hz * 0.98 && sim.samples[0].hz < hz * 1.02);
        CHECK(sim.rate_warnings == 0);
    }
    Sim fast;
    fast.Run(240, 20);
    CHECK(fast.samples[0].hz > 235 && fast.samples[0].hz < 245);
}

void TestExcessiveRate() {
    Sim sim;
    sim.Run(240, 30);
    CHECK(sim.rate_warnings == 1);
    sim.Run(240, 200);
    CHECK(sim.rate_warnings == 1);

    Sim boundary;
    boundary.Run(85, 30);
    CHECK(boundary.rate_warnings == 0);
    Sim above;
    above.Run(100, 30);
    CHECK(above.rate_warnings == 1);
}

void TestIsolatedFastFrameDoesNotWarn() {
    Sim sim;
    sim.Run(60, 10);
    for (int i = 0; i < 20; ++i) {
        sim.Frame(kNsPerMs, kNormal);
    }
    sim.Run(60, 20);
    CHECK(sim.rate_warnings == 0 && sim.stall_warnings == 0);
}

void TestWarningRateLimit() {
    Sim sim;
    sim.Run(240, 12);
    CHECK(sim.rate_warnings == 1);
    sim.Run(60, 10);
    sim.Run(240, 20);
    CHECK(sim.rate_warnings == 1);
    sim.Run(60, 10);
    sim.Run(240, 40);
    CHECK(sim.rate_warnings == 2);

    Alarm alarm;
    CHECK(alarm.Fire(true, 0) && !alarm.Fire(true, 1) && !alarm.Fire(true, Alarm::kRepeatNs * 10));
    CHECK(!alarm.Fire(false, 2) && !alarm.Fire(true, Alarm::kRepeatNs - 1) && alarm.Fire(true, Alarm::kRepeatNs + 1));
    CHECK(!alarm.Fire(false, 3 * Alarm::kRepeatNs) && alarm.Fire(true, 3 * Alarm::kRepeatNs));
}

void TestPresentStall() {
    Sim normal;
    normal.Run(60, 30, {2 * kNsPerMs, 1 * kNsPerMs, 3 * kNsPerMs});
    CHECK(normal.stall_warnings == 0);
    Sim vsync;
    vsync.Run(60, 30, {2 * kNsPerMs, 1 * kNsPerMs, 16 * kNsPerMs});
    CHECK(vsync.stall_warnings == 0);
    Sim at_threshold;
    at_threshold.Run(60, 30, {2 * kNsPerMs, 1 * kNsPerMs, LoopMonitor::kSlowPresentNs});
    CHECK(at_threshold.stall_warnings == 0);

    Sim slow;
    slow.Run(60, 30, {2 * kNsPerMs, 1 * kNsPerMs, LoopMonitor::kSlowPresentNs + 1});
    CHECK(slow.stall_warnings == 1);

    Sim long_present;
    long_present.Run(60, 12);
    long_present.Frame(16 * kNsPerMs, {2 * kNsPerMs, 1 * kNsPerMs, 400 * kNsPerMs});
    long_present.Run(60, 12);
    CHECK(long_present.stall_warnings == 1);

    Sim short_spike;
    short_spike.Run(60, 12);
    short_spike.Frame(16 * kNsPerMs, {2 * kNsPerMs, 1 * kNsPerMs, LoopMonitor::kHardStallNs - 1});
    short_spike.Run(60, 12);
    CHECK(short_spike.stall_warnings == 0);
}

void TestStallBurstRateLimited() {
    Sim sim;
    sim.Run(60, 10);
    for (int burst = 0; burst < 20; ++burst) {
        sim.Frame(16 * kNsPerMs, {2 * kNsPerMs, 1 * kNsPerMs, 500 * kNsPerMs});
        sim.Run(60, 1.9);
    }
    CHECK(sim.stall_warnings == 1);
    sim.Run(60, 10);
    for (int burst = 0; burst < 20; ++burst) {
        sim.Frame(16 * kNsPerMs, {2 * kNsPerMs, 1 * kNsPerMs, 500 * kNsPerMs});
        sim.Run(60, 1.9);
    }
    CHECK(sim.stall_warnings == 2);
}

void TestRendererInLines() {
    LoopSample sample;
    sample.frames = 300;
    sample.elapsed_ns = 5 * kNsPerSecond;
    sample.hz = 60.0;
    sample.max_present_ns = 400 * kNsPerMs;
    sample.slow_presents = 2;
    sample.cpu_pct = 12;
    const std::string stall = PresentStallLine("direct3d11", sample);
    CHECK(Contains(stall, "event=launcher_present_stall") && Contains(stall, "renderer=direct3d11"));
    CHECK(Contains(stall, "max_present_ms=400.0") && Contains(stall, "slow_presents=2") && Contains(stall, "threshold_ms=50"));
    CHECK(Contains(stall, "ui_thread_cpu_pct=12"));
    CHECK(Contains(PresentStallLine("software", sample), "renderer=software"));
    CHECK(Contains(ExcessiveRateLine("software", sample), "event=launcher_frame_pacing_excessive target_hz=60"));
    CHECK(Contains(HighCpuLine("software", sample), "event=launcher_ui_cpu_high threshold_pct=60"));
    CHECK(Contains(LoopSampleLine("software", sample), "loop_sample renderer=software frames=300 elapsed_ms=5000 effective_hz=60.0"));
    CHECK(Contains(LoopSampleLine("software", sample), "avg_build_ms=") && Contains(LoopSampleLine("software", sample), "avg_present_ms="));
    CHECK(Contains(PacingModeLine("software", {true, false}), "renderer=software requested_vsync=1 effective_vsync=0 pacing=cap target_hz=60"));
    CHECK(Contains(PacingModeLine("direct3d11", {true, true}), "effective_vsync=1 pacing=vsync"));
}

void TestFormattingBounded() {
    LoopSample extreme;
    extreme.frames = std::numeric_limits<int64_t>::max();
    extreme.elapsed_ns = std::numeric_limits<int64_t>::max();
    extreme.hz = 1e300;
    extreme.total = {std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::max()};
    extreme.wait_ns = std::numeric_limits<int64_t>::max();
    extreme.max_present_ns = std::numeric_limits<int64_t>::max();
    extreme.slow_presents = std::numeric_limits<int64_t>::max();
    extreme.cpu_pct = 100;
    for (const std::string& line : {LoopSampleLine("C:\\bad\npath", extreme), ExcessiveRateLine("x", extreme), PresentStallLine("x", extreme),
                                    HighCpuLine("x", extreme)}) {
        CHECK(line.size() < 480 && line.find('\n') == std::string::npos && line.find('\\') == std::string::npos);
        CHECK(Contains(line, "Launcher telemetry: "));
    }
    LoopSample empty;
    CHECK(Contains(LoopSampleLine("", empty), "renderer=unknown frames=0") && !Contains(LoopSampleLine("", empty), "ui_thread_cpu_pct"));
    CHECK(Contains(LoopSampleLine("", empty), "avg_build_ms=0.0") && Contains(LoopSampleLine("", empty), "present_pct=0"));
}

void TestMinimizedResetsMonitor() {
    Sim sim;
    sim.Run(60, 20);
    const size_t before = sim.samples.size();
    sim.monitor.Reset();
    sim.now += 600 * kNsPerSecond;
    sim.Run(60, 4);
    CHECK(sim.samples.size() == before);
    sim.Run(60, 12);
    CHECK(sim.samples.size() > before && sim.samples.back().frames < 320 && sim.samples.back().hz < 61.0);
    CHECK(sim.rate_warnings == 0 && sim.stall_warnings == 0);

    Sim restored;
    restored.Run(60, 7);
    restored.monitor.Reset();
    restored.now += 3600 * kNsPerSecond;
    restored.Frame(16 * kNsPerMs, kNormal);
    CHECK(restored.samples.size() <= 1 && restored.stall_warnings == 0 && restored.rate_warnings == 0);
}

void TestUiCpu() {
    Sim normal;
    normal.cpu_fraction = 0.15;
    normal.Run(60, 30);
    CHECK(normal.cpu_warnings == 0 && normal.samples[0].cpu_pct >= 14 && normal.samples[0].cpu_pct <= 16);
    CHECK(Contains(LoopSampleLine("software", normal.samples[0]), "ui_thread_cpu_pct=15"));

    Sim high;
    high.cpu_fraction = 0.8;
    high.Run(60, 30);
    CHECK(high.cpu_warnings == 1 && high.samples[0].cpu_pct >= 79 && high.samples[0].cpu_pct <= 81);
    high.Run(60, 100);
    CHECK(high.cpu_warnings == 1);

    Sim edge;
    edge.cpu_fraction = 0.59;
    edge.Run(60, 30);
    CHECK(edge.cpu_warnings == 0);

    Sim unavailable;
    unavailable.cpu_fraction = -1;
    unavailable.Run(60, 30);
    CHECK(unavailable.cpu_warnings == 0 && unavailable.samples[0].cpu_pct == -1);
    CHECK(!Contains(LoopSampleLine("software", unavailable.samples[0]), "ui_thread_cpu_pct"));

    Sim saturated;
    saturated.cpu_fraction = 3.0;
    saturated.Run(60, 30);
    CHECK(saturated.samples[0].cpu_pct == 100);
}

void TestNoPerFrameWork() {
    Sim sim;
    sim.Run(60, 120);
    CHECK(sim.frames > 7000);
    CHECK(sim.samples.size() <= 24);
    CHECK(sim.cpu_calls <= int(sim.samples.size()) + 2);

    const fs::path root = SOTB_SOURCE_DIR;
    const std::string host = ReadAll(root / "launcher/host.cpp");
    const size_t note = host.find("void LauncherHost::NoteFrameDrawn");
    const size_t begin = host.find("void LauncherHost::BeginFrame()");
    const size_t end_frame = host.find("void LauncherHost::EndFrame");
    const size_t fullscreen = host.find("bool LauncherHost::Fullscreen()");
    CHECK(note != std::string::npos && begin != std::string::npos && end_frame != std::string::npos && fullscreen != std::string::npos);
    const std::string frame_path = host.substr(end_frame, fullscreen - end_frame);
    CHECK(!Contains(frame_path, "printf") && !Contains(frame_path, "fopen") && !Contains(frame_path, "std::filesystem"));
    const std::string note_body = host.substr(note, begin - note);
    CHECK(note_body.find("if (!sample)") < note_body.find("printf"));
    CHECK(Contains(host, "monitor_.FrameDrawn(now_ns, times, ThreadCpuNs)"));
    CHECK(Contains(host, "NoteFrameDrawn(presented, {submitting - frame_begin_ns_, presenting - submitting, presented - presenting});"));
}

void TestDeterministic() {
    Sim a;
    Sim b;
    a.Run(75, 40);
    b.Run(75, 40);
    CHECK(a.samples.size() == b.samples.size() && !a.samples.empty());
    for (size_t i = 0; i < a.samples.size(); ++i) {
        CHECK(LoopSampleLine("software", a.samples[i]) == LoopSampleLine("software", b.samples[i]));
    }
}

void TestLongSession() {
    Sim sim;
    sim.now = 4'000'000'000'000'000'000ll;
    const int64_t step = kNsPerSecond / 60;
    const int64_t end = sim.now + 7ll * 24 * 3600 * kNsPerSecond;
    while (sim.now < end) {
        sim.Frame(step, kNormal);
        if (sim.samples.size() > 4 && sim.frames % 1000 == 0) {
            sim.samples.erase(sim.samples.begin(), sim.samples.end() - 1);
        }
    }
    CHECK(sim.frames > 36'000'000);
    CHECK(sim.rate_warnings == 0 && sim.stall_warnings == 0 && sim.cpu_warnings == 0);
    CHECK(!sim.samples.empty() && sim.samples.back().hz > 59.0 && sim.samples.back().hz < 61.0);
    CHECK(sim.samples.back().frames < 400 && sim.samples.back().elapsed_ns < 6 * kNsPerSecond);
}

void TestWaitedAccounting() {
    LoopMonitor monitor;
    monitor.Waited(5 * kNsPerSecond);
    int64_t now = 100 * kNsPerSecond;
    const auto cpu = [] { return int64_t(0); };
    monitor.FrameDrawn(now, kNormal, cpu);
    now += LoopMonitor::kWarmupNs;
    monitor.FrameDrawn(now, kNormal, cpu);
    monitor.Waited(7 * kNsPerMs);
    now += LoopMonitor::kWindowNs;
    const std::optional<LoopSample> sample = monitor.FrameDrawn(now, kNormal, cpu);
    CHECK(sample && sample->wait_ns == 7 * kNsPerMs);
}

} // namespace

int main() {
    TestNormalSixtyHertz();
    TestRenderRateMeasurement();
    TestExcessiveRate();
    TestIsolatedFastFrameDoesNotWarn();
    TestWarningRateLimit();
    TestPresentStall();
    TestStallBurstRateLimited();
    TestRendererInLines();
    TestFormattingBounded();
    TestMinimizedResetsMonitor();
    TestUiCpu();
    TestNoPerFrameWork();
    TestDeterministic();
    TestLongSession();
    TestWaitedAccounting();
    std::printf("launcher performance tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

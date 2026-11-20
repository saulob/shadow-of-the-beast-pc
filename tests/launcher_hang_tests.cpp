// Launcher UI stalls: a loop phase (event pump, frame build, present) that keeps the UI thread for seconds is reported
// once per incident, rate limited, with the stage and minimized state, and never overlaps the present-stall warning.
// Synthetic timestamps; no window, no clock.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

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

size_t Count(const std::string& text, const char* needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

// A launcher loop at 60 Hz: each iteration pumps events, builds and presents.
struct Loop {
    StallDetector detector;
    int64_t now = 100 * kNsPerSecond;
    int warnings = 0;
    UiStall last;

    void Phase(LoopPhase phase, int64_t ns, bool covered = false) {
        now += ns;
        if (const auto stall = detector.Finished(phase, ns, now, covered)) {
            ++warnings;
            last = *stall;
        }
    }
    void Iteration(int64_t events = 200'000, int64_t build = 2 * kNsPerMs, int64_t present = 3 * kNsPerMs) {
        Phase(LoopPhase::kEvents, events);
        Phase(LoopPhase::kBuild, build);
        Phase(LoopPhase::kPresent, present);
        now += 11 * kNsPerMs;
    }
    void Seconds(double seconds) {
        const int64_t end = now + int64_t(seconds * double(kNsPerSecond));
        while (now < end) Iteration();
    }
};

void TestNormalLoopQuiet() {
    Loop loop;
    loop.Seconds(600.0);
    CHECK(loop.warnings == 0);
}

void TestPacedLoopQuiet() {
    // The 60 Hz cap sleeps between frames; sleeping is not a phase and never counts.
    Loop loop;
    for (int i = 0; i < 36'000; ++i) {
        loop.Iteration(100'000, 1 * kNsPerMs, 1 * kNsPerMs);
        loop.now += 14 * kNsPerMs;
    }
    CHECK(loop.warnings == 0);
}

void TestMinimizedIdleQuiet() {
    // Minimized: only the event pump runs, every 50 ms, for an hour.
    Loop loop;
    for (int i = 0; i < 72'000; ++i) {
        loop.Phase(LoopPhase::kEvents, 50'000);
        loop.now += kMinimizedIdleNs;
    }
    CHECK(loop.warnings == 0);
}

void TestLongPresent() {
    // Before telemetry has warmed up a long present is reported here; once LoopMonitor measures, it is its warning.
    Loop warmup;
    warmup.Phase(LoopPhase::kPresent, 3 * kNsPerSecond, false);
    CHECK(warmup.warnings == 1 && warmup.last.phase == LoopPhase::kPresent);

    Loop measured;
    measured.Phase(LoopPhase::kPresent, 3 * kNsPerSecond, true);
    CHECK(measured.warnings == 0);

    // The same long present goes to LoopMonitor's present_stall.
    LoopMonitor monitor;
    int64_t now = 0;
    const auto cpu = [] { return int64_t(-1); };
    monitor.FrameDrawn(now, {}, cpu);
    now += LoopMonitor::kWarmupNs;
    monitor.FrameDrawn(now, {}, cpu);
    CHECK(monitor.Measuring());
    now += 3 * kNsPerSecond;
    monitor.FrameDrawn(now, {2 * kNsPerMs, kNsPerMs, 3 * kNsPerSecond}, cpu);
    now += LoopMonitor::kWindowNs;
    const std::optional<LoopSample> sample = monitor.FrameDrawn(now, {}, cpu);
    CHECK(sample && sample->present_stall);
    monitor.Reset();
    CHECK(!monitor.Measuring());
}

void TestLongBuild() {
    Loop loop;
    loop.Seconds(1.0);
    loop.Iteration(200'000, 2500 * kNsPerMs);
    CHECK(loop.warnings == 1 && loop.last.phase == LoopPhase::kBuild);
    CHECK(loop.last.duration_ns == 2500 * kNsPerMs && loop.last.threshold_ns == StallDetector::kBuildNs);

    // Just below the threshold is a slow frame, not a stall.
    Loop below;
    below.Iteration(200'000, StallDetector::kBuildNs - 1);
    CHECK(below.warnings == 0);
    Loop at;
    at.Iteration(200'000, StallDetector::kBuildNs);
    CHECK(at.warnings == 1);

    // A frame waiting in a native file dialog is not a stall, and does not re-arm one either.
    Loop dialog;
    dialog.Phase(LoopPhase::kBuild, 30 * kNsPerSecond, true);
    CHECK(dialog.warnings == 0);
}

void TestEventStall() {
    // Moving the window holds the pump on Windows: a few seconds of that stays quiet.
    Loop drag;
    drag.Iteration(6 * kNsPerSecond);
    CHECK(drag.warnings == 0);

    Loop stuck;
    stuck.Iteration(12 * kNsPerSecond);
    CHECK(stuck.warnings == 1 && stuck.last.phase == LoopPhase::kEvents);
    CHECK(StallDetector::Threshold(LoopPhase::kEvents) > StallDetector::Threshold(LoopPhase::kBuild));
}

void TestOneWarningPerIncident() {
    // A stall that lasts several frames in a row is one incident.
    Loop loop;
    for (int i = 0; i < 10; ++i) loop.Iteration(200'000, 3 * kNsPerSecond);
    CHECK(loop.warnings == 1);
    CHECK(loop.last.count == 1);
}

void TestRateLimited() {
    // Separate incidents a few seconds apart: the second is held back but counted in the next line.
    Loop loop;
    loop.Iteration(200'000, 3 * kNsPerSecond);
    loop.Seconds(2.0);
    loop.Iteration(200'000, 3 * kNsPerSecond);
    CHECK(loop.warnings == 1);
    loop.Seconds(70.0);
    loop.Iteration(200'000, 3 * kNsPerSecond);
    CHECK(loop.warnings == 2 && loop.last.count == 3);
}

void TestRecoveryRearms() {
    Loop loop;
    loop.Iteration(200'000, 3 * kNsPerSecond);
    loop.Iteration(200'000, 3 * kNsPerSecond);
    CHECK(loop.warnings == 1);
    loop.Seconds(61.0);
    loop.Iteration(200'000, 3 * kNsPerSecond);
    CHECK(loop.warnings == 2);

    // Phases are independent: a build stall does not hold back an event stall.
    loop.Iteration(12 * kNsPerSecond);
    CHECK(loop.warnings == 3 && loop.last.phase == LoopPhase::kEvents);
}

void TestLineContents() {
    const UiStall stall{LoopPhase::kBuild, 2345 * kNsPerMs, StallDetector::kBuildNs, 4};
    const std::string line = UiStallLine("software", stall, "launcher first frame", false);
    CHECK(Contains(line, "Launcher telemetry: event=launcher_ui_stall phase=build duration_ms=2345 threshold_ms=2000 stalls=4"));
    CHECK(Contains(line, "stage=launcher first frame minimized=0 renderer=software"));
    CHECK(Contains(UiStallLine("direct3d11", {LoopPhase::kEvents, 11 * kNsPerSecond, StallDetector::kEventsNs, 1},
                               "launcher running", true),
                   "phase=events duration_ms=11000 threshold_ms=10000 stalls=1 stage=launcher running minimized=1"));
    CHECK(Contains(UiStallLine("", stall, nullptr, false), "stage=unknown"));
}

void TestBoundedFormatting() {
    const std::string long_stage(500, 's');
    const UiStall worst{LoopPhase::kPresent, std::numeric_limits<int64_t>::max(), StallDetector::kPresentNs,
                        std::numeric_limits<int64_t>::max()};
    const std::string line = UiStallLine(std::string(400, 'r'), worst, long_stage.c_str(), true);
    CHECK(line.size() < 256 && line.find('\n') == std::string::npos);
    CHECK(Contains(line, ("stage=" + std::string(48, 's') + " ").c_str()));
}

void TestLongUptime() {
    // Thirty days of uptime in nanoseconds, then stalls: no overflow and still one warning per minute at most.
    Loop loop;
    loop.now = int64_t(30) * 24 * 3600 * kNsPerSecond;
    for (int i = 0; i < 600; ++i) {
        loop.Iteration(200'000, 3 * kNsPerSecond);
        loop.Iteration();
    }
    CHECK(loop.warnings >= 1 && loop.warnings <= 1 + int((loop.now - int64_t(30) * 24 * 3600 * kNsPerSecond) / Alarm::kRepeatNs));
    CHECK(loop.last.count >= 1 && loop.last.count <= 600);
}

void TestSourceWiring() {
    const fs::path root = SOTB_SOURCE_DIR;
    const std::string host = ReadAll(root / "launcher/host.cpp");
    const std::string launcher = ReadAll(root / "launcher/launcher.cpp");
    const std::string trophy = ReadAll(root / "launcher/trophy_page.cpp");
    const std::string pacing = ReadAll(root / "launcher/launcher_pacing.h");
    CHECK(!host.empty() && !launcher.empty() && !trophy.empty() && !pacing.empty());

    // Measured on the UI thread from timestamps the loop already takes: no watchdog thread, no process termination.
    for (const std::string* text : {&host, &pacing}) {
        CHECK(!Contains(*text, "std::thread") && !Contains(*text, "CreateThread") && !Contains(*text, "SDL_CreateThread"));
        CHECK(!Contains(*text, "TerminateProcess") && !Contains(*text, "ExitProcess") && !Contains(*text, "abort()") &&
              !Contains(*text, "std::exit"));
    }
    CHECK(!Contains(pacing, "SDL_") && !Contains(pacing, "GetThreadTimes"));

    // The event pump is timed around PumpEvents; build and present from EndFrame's own timestamps.
    const size_t next = host.find("FrameGate LauncherHost::NextFrame()");
    const size_t pumping = host.find("const int64_t pumping = NowNs();", next);
    const size_t pump = host.find("PumpEvents()", next);
    const size_t events = host.find("NoteStall(LoopPhase::kEvents, polled - pumping, polled, false);", next);
    CHECK(next != std::string::npos && pumping < pump && pump < events);
    const size_t end = host.find("void LauncherHost::EndFrame(");
    const size_t present = host.find("SDL_RenderPresent(renderer_);", end);
    const size_t build = host.find("NoteStall(LoopPhase::kBuild, submitting - frame_begin_ns_, presented, std::exchange(dialog_in_frame_, false));", end);
    const size_t long_present = host.find("NoteStall(LoopPhase::kPresent, presented - presenting, presented, monitor_.Measuring());", end);
    CHECK(end != std::string::npos && present < build && build < long_present);
    CHECK(Count(host, "NoteStall(LoopPhase::") == 3);

    // The line carries the current stage and the minimized state; hidden page captures never warn.
    const size_t note = host.find("void LauncherHost::NoteStall(");
    CHECK(host.find("if (hidden_) {", note) < host.find("stalls_.Finished(", note));
    CHECK(Contains(host, "UiStallLine(renderer_name_, *stall, sotb_startup_phase_current(), Minimized())"));

    // Both native file dialogs mark their frame.
    const size_t install = launcher.find("host_.NoteDialogInFrame();");
    CHECK(install != std::string::npos && install < launcher.find("trg::BrowseForFile(", install));
    const size_t import = trophy.find("host_.NoteDialogInFrame();");
    CHECK(import != std::string::npos && import < trophy.find("trg::BrowseForFile(", import));
    CHECK(Count(launcher, "trg::BrowseForFile(") == 1 && Count(trophy, "trg::BrowseForFile(") == 1);

    // Updater and install work stays off the UI thread and is polled, so waiting on them is never a stall.
    const size_t loop = launcher.find("host_.NextFrame()");
    CHECK(launcher.find("updater_->Poll();", loop) < launcher.find("host_.BeginFrame();", loop));
}

} // namespace

int main() {
    TestNormalLoopQuiet();
    TestPacedLoopQuiet();
    TestMinimizedIdleQuiet();
    TestLongPresent();
    TestLongBuild();
    TestEventStall();
    TestOneWarningPerIncident();
    TestRateLimited();
    TestRecoveryRearms();
    TestLineContents();
    TestBoundedFormatting();
    TestLongUptime();
    TestSourceWiring();
    std::printf("launcher hang tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

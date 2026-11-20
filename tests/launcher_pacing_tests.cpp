// Launcher frame cap and minimized behavior: deadline pacing and the loop gate on synthetic monotonic timestamps, plus
// the source wiring. No window, no clock, no sleeping.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

constexpr int64_t kStep = kNsPerSecond / 60;
constexpr int64_t kT0 = 5 * kNsPerSecond;

bool Near(int64_t a, int64_t b, int64_t tolerance = 1) { return a - b <= tolerance && b - a <= tolerance; }

void TestTarget() {
    CHECK(kLauncherTargetHz == 60);
    FramePacer pacer;
    CHECK(pacer.hz() == 60 && pacer.step_ns() == 16'666'666);
    CHECK(LoopGate().pacer().hz() == 60);
    CHECK(FramePacer(30).step_ns() == 33'333'333 && FramePacer(0).step_ns() == 0);
}

void TestFirstAndEarly() {
    FramePacer pacer;
    FrameWait wait = pacer.Next(kT0);
    CHECK(wait.draw && wait.wait_ns == 0);
    wait = pacer.Next(kT0);
    CHECK(!wait.draw && Near(wait.wait_ns, kStep));
    wait = pacer.Next(kT0 + 5 * kNsPerMs);
    CHECK(!wait.draw && Near(wait.wait_ns, kStep - 5 * kNsPerMs));
    wait = pacer.Next(kT0 + kStep - 1);
    CHECK(!wait.draw && wait.wait_ns == 1);
}

void TestOnTimeAndBoundaries() {
    FramePacer pacer;
    pacer.Next(kT0);
    FrameWait wait = pacer.Next(kT0 + kStep);
    CHECK(wait.draw);
    wait = pacer.Next(kT0 + kStep);
    CHECK(!wait.draw && Near(wait.wait_ns, kStep));

    FramePacer exact;
    exact.Next(kT0);
    CHECK(!exact.Next(kT0 + kStep - 1).draw);
    CHECK(exact.Next(kT0 + kStep).draw);
}

void TestSlightlyLate() {
    FramePacer pacer;
    pacer.Next(kT0);
    const int64_t late = 3 * kNsPerMs;
    CHECK(pacer.Next(kT0 + kStep + late).draw);
    const FrameWait wait = pacer.Next(kT0 + kStep + late);
    CHECK(!wait.draw && Near(wait.wait_ns, kStep - late));
    CHECK(pacer.Next(kT0 + 2 * kStep + 1).draw);
}

void TestLargeStallRebases() {
    FramePacer pacer;
    pacer.Next(kT0);
    const int64_t resumed = kT0 + 40 * kStep;
    CHECK(pacer.Next(resumed).draw);
    int draws = 0;
    for (int i = 0; i < 20; ++i) {
        draws += pacer.Next(resumed).draw ? 1 : 0;
    }
    CHECK(draws == 0);
    FrameWait wait = pacer.Next(resumed);
    CHECK(!wait.draw && Near(wait.wait_ns, kStep));
    CHECK(pacer.Next(resumed + kStep).draw);

    FramePacer exactly_one_late;
    exactly_one_late.Next(kT0);
    CHECK(exactly_one_late.Next(kT0 + 2 * kStep).draw);
    CHECK(!exactly_one_late.Next(kT0 + 2 * kStep).draw);
}

void TestNoCatchUpBurst() {
    FramePacer pacer;
    int64_t now = kT0;
    int64_t draws = 0;
    for (int stall = 0; stall < 50; ++stall) {
        pacer.Next(now);
        now += (stall % 2 ? 2 : 700) * kStep;
        int burst = 0;
        for (int i = 0; i < 10; ++i) {
            burst += pacer.Next(now).draw ? 1 : 0;
        }
        CHECK(burst <= 1);
        draws += burst;
    }
    CHECK(draws <= 50);
}

void TestWaitsAreSane() {
    FramePacer pacer;
    uint64_t seed = 7;
    int64_t now = kT0;
    int64_t last_draw = -1;
    for (int i = 0; i < 200000; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        now += int64_t((seed >> 33) % uint64_t(3 * kStep));
        const FrameWait wait = pacer.Next(now);
        if (wait.draw) {
            CHECK(wait.wait_ns == 0);
            CHECK(now > last_draw);
            last_draw = now;
        } else {
            CHECK(wait.wait_ns > 0 && wait.wait_ns <= kStep + 1);
        }
    }
}

void TestIdealSleeperMatchesExactPeriod() {
    FramePacer pacer;
    int64_t now = 4'000'000'000'000'000'000ll;
    const int64_t start = now;
    int64_t frames = 0;
    const int64_t total = 3 * 24 * 3600 * 60;
    while (frames < total) {
        FrameWait wait = pacer.Next(now);
        if (!wait.draw) {
            now += wait.wait_ns;
            wait = pacer.Next(now);
            if (!wait.draw) {
                CHECK(false);
                return;
            }
        }
        ++frames;
    }
    const int64_t expected = (total - 1) * kNsPerSecond / 60;
    CHECK(Near(now - start, expected, 2));
}

void TestHighRefreshDisplays() {
    for (const int refresh : {60, 75, 120, 144, 165, 240, 360}) {
        FramePacer pacer;
        const int64_t period = kNsPerSecond / refresh;
        int64_t now = kT0;
        int64_t draws = 0;
        const int64_t end = kT0 + 20 * kNsPerSecond;
        while (now < end) {
            const FrameWait wait = pacer.Next(now);
            if (wait.draw) {
                ++draws;
                now = (now / period + 1) * period;
            } else {
                now += wait.wait_ns;
            }
        }
        CHECK(draws <= 20 * 60 + 1);
        CHECK(draws >= 20 * 30);
    }
}

void TestClockJumps() {
    FramePacer forward;
    forward.Next(kT0);
    const int64_t hour = 3600 * kNsPerSecond;
    CHECK(forward.Next(kT0 + hour).draw && !forward.Next(kT0 + hour).draw);
    CHECK(Near(forward.Next(kT0 + hour).wait_ns, kStep));

    FramePacer backward;
    backward.Next(kT0 + hour);
    const FrameWait wait = backward.Next(kT0);
    CHECK(!wait.draw && wait.wait_ns > 0 && wait.wait_ns <= kStep + 1);
    CHECK(backward.Next(kT0 + wait.wait_ns).draw);
    CHECK(!backward.Next(kT0 + wait.wait_ns).draw);
}

void TestUnlimited() {
    FramePacer unlimited(0);
    for (int i = 0; i < 100; ++i) {
        const FrameWait wait = unlimited.Next(kT0);
        CHECK(wait.draw && wait.wait_ns == 0);
    }
    LoopGate hidden(0);
    for (int i = 0; i < 10; ++i) {
        CHECK(hidden.Step(kT0, false).action == GateAction::kDraw);
    }
}

void TestVisibleGate() {
    LoopGate gate;
    GateStep step = gate.Step(kT0, false);
    CHECK(step.action == GateAction::kDraw);
    step = gate.Step(kT0 + 1, false);
    CHECK(step.action == GateAction::kSleep && step.sleep_ns > 0 && step.sleep_ns <= kStep);
    CHECK(gate.Step(kT0 + kStep, false).action == GateAction::kDraw);
}

void TestMinimizedGate() {
    LoopGate gate;
    gate.Step(kT0, false);
    int64_t now = kT0;
    int64_t idle_steps = 0;
    for (int64_t second = 0; second < 10; ++second) {
        for (int step = 0; step < 20; ++step) {
            const GateStep gated = gate.Step(now, true);
            CHECK(gated.action == GateAction::kIdle && gated.sleep_ns == kMinimizedIdleNs && gated.sleep_ns > 0);
            now += gated.sleep_ns;
            ++idle_steps;
        }
    }
    CHECK(idle_steps == 200 && now - kT0 == 10 * kNsPerSecond);
    CHECK(kMinimizedIdleNs >= 3 * kStep && kMinimizedIdleNs <= 100 * kNsPerMs);

    GateStep restored = gate.Step(now, false);
    CHECK(restored.action == GateAction::kDraw);
    restored = gate.Step(now, false);
    CHECK(restored.action == GateAction::kSleep && Near(restored.sleep_ns, kStep));
}

void TestRepeatedMinimizeRestore() {
    LoopGate gate;
    int64_t now = kT0;
    for (int i = 0; i < 1000; ++i) {
        CHECK(gate.Step(now, true).action == GateAction::kIdle);
        now += (i % 7 + 1) * 3 * kNsPerSecond;
        CHECK(gate.Step(now, false).action == GateAction::kDraw);
        const GateStep wait = gate.Step(now, false);
        CHECK(wait.action == GateAction::kSleep && Near(wait.sleep_ns, kStep));
        int draws = 0;
        for (int burst = 0; burst < 5; ++burst) {
            draws += gate.Step(now, false).action == GateAction::kDraw ? 1 : 0;
        }
        CHECK(draws == 0);
    }
}

void TestSourceWiring() {
    const fs::path root = SOTB_SOURCE_DIR;
    const std::string host = ReadAll(root / "launcher/host.cpp");
    const std::string header = ReadAll(root / "launcher/host.h");
    const std::string launcher = ReadAll(root / "launcher/launcher.cpp");
    const std::string pacing = ReadAll(root / "launcher/launcher_pacing.h");
    CHECK(!host.empty() && !header.empty() && !launcher.empty() && !pacing.empty());

    // Independent of the game's frame rate, the renderer and the monitor.
    CHECK(Contains(host, "gate_ = LoopGate(hidden_ ? 0 : kLauncherTargetHz);"));
    CHECK(!Contains(host, "sotb_frame_pacing") && !Contains(pacing, "sotb_frame_pacing") && !Contains(pacing, "gpu/shim"));
    CHECK(!Contains(pacing, "ClassifyRenderer") && !Contains(pacing, "SDL_") && !Contains(pacing, "GetDisplay"));
    CHECK(!Contains(host, "refresh_rate") && !Contains(host, "SDL_GetDisplayMode") && !Contains(host, "SDL_GetDesktopDisplayMode"));

    // Events first, then the gate; every wait is a real sleep, never a spin.
    const size_t next = host.find("FrameGate LauncherHost::NextFrame()");
    const size_t pump = host.find("PumpEvents()", next);
    const size_t step = host.find("gate_.Step(polled, Minimized())", next);
    const size_t sleep_case = host.find("case GateAction::kSleep:", next);
    const size_t idle_case = host.find("case GateAction::kIdle:", next);
    CHECK(next != std::string::npos && pump < step && idle_case < sleep_case);
    CHECK(host.find("SDL_DelayNS(static_cast<Uint64>(step.sleep_ns))", idle_case) < sleep_case);
    CHECK(host.find("SDL_DelayNS(static_cast<Uint64>(step.sleep_ns))", sleep_case) != std::string::npos);
    const std::string body = host.substr(next, host.find("void LauncherHost::NoteFrameDrawn", next) - next);
    CHECK(!Contains(body, "SDL_Delay(") && !Contains(body, "Sleep(") && !Contains(body, "yield") && !Contains(body, "_mm_pause"));
    CHECK(Contains(body, "monitor_.Reset();"));

    // The launcher loop keeps polling the install and updater while minimized, and draws only on kDraw.
    const size_t loop = launcher.find("host_.NextFrame()");
    const size_t install = launcher.find("PollInstall();", loop);
    const size_t update = launcher.find("updater_->Poll();", loop);
    const size_t idle = launcher.find("gate == Sotb::FrameGate::kIdle && !first", loop);
    const size_t draw = launcher.find("host_.BeginFrame();", loop);
    CHECK(loop != std::string::npos && install < idle && update < idle && idle < draw);
    CHECK(!Contains(launcher, "SDL_Delay(30)") && !Contains(launcher, "host_.Minimized()"));
}

} // namespace

int main() {
    TestTarget();
    TestFirstAndEarly();
    TestOnTimeAndBoundaries();
    TestSlightlyLate();
    TestLargeStallRebases();
    TestNoCatchUpBurst();
    TestWaitsAreSane();
    TestIdealSleeperMatchesExactPeriod();
    TestHighRefreshDisplays();
    TestClockJumps();
    TestUnlimited();
    TestVisibleGate();
    TestMinimizedGate();
    TestRepeatedMinimizeRestore();
    TestSourceWiring();
    std::printf("launcher pacing tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

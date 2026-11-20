// Frame pacing and high frame rate tests: the presentation limiter's deadlines, stalls, clock jumps and long-run
// exactness, the sleep decision, the Frame rate setting's cadence against VSync and the display, and the log gate, on
// synthetic timestamps. Nothing sleeps. No Vulkan, no window.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "sotb_frame_pacing.h"
#include "sotb_log_gate.h"
#include "sotb_present.h"

namespace {

using namespace SotbFramePacing;
using Clock = FrameLimiter::Clock;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using std::chrono::seconds;

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

const Clock::time_point kStart = Clock::time_point{} + seconds(1000);

nanoseconds Since(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<nanoseconds>(to - from);
}

// Exactly floor(n * 1e9 / fps) nanoseconds, in 128-bit-free arithmetic for the sizes used here.
std::int64_t ExactSlotNs(std::int64_t n, std::int64_t fps) {
    return (n / fps) * 1'000'000'000LL + ((n % fps) * 1'000'000'000LL) / fps;
}

void TestFirstFrame() {
    FrameLimiter limiter{60, kStart};
    CHECK(limiter.Enabled() && limiter.Due(kStart));
    CHECK(limiter.NextSlot() == kStart);
    CHECK(limiter.Period() == nanoseconds(16'666'666));
    limiter.Release(kStart);
    CHECK(limiter.NextSlot() == kStart + nanoseconds(16'666'666) || limiter.NextSlot() == kStart + nanoseconds(16'666'667));
    CHECK(limiter.Resets() == 0 && limiter.LongestLate() == nanoseconds(0));

    FrameLimiter off{0, kStart};
    CHECK(!off.Enabled() && off.Due(kStart - seconds(5)));
    off.Release(kStart + seconds(1));
    CHECK(off.Resets() == 0 && off.NextSlot() == kStart + seconds(1));
}

void TestOnTimeFramesKeepTheCadence() {
    FrameLimiter limiter{90, kStart};
    for (int i = 0; i < 90 * 60; ++i) {
        CHECK(limiter.Due(limiter.NextSlot()));
        limiter.Release(limiter.NextSlot());
    }
    CHECK(limiter.Resets() == 0 && limiter.LongestLate() == nanoseconds(0));
    CHECK(Since(kStart, limiter.NextSlot()) == seconds(60));
}

void TestEarlyFrameWaits() {
    FrameLimiter limiter{60, kStart};
    limiter.Release(kStart);
    const auto slot = limiter.NextSlot();
    CHECK(!limiter.Due(kStart + milliseconds(1)));
    CHECK(limiter.Due(slot));
    const nanoseconds sleep = CoarseSleep(slot, kStart + milliseconds(1));
    CHECK(sleep > nanoseconds(0) && sleep == Since(kStart + milliseconds(1), slot) - kSpinWindow);
}

void TestLateFramesDoNotShiftLaterSlots() {
    FrameLimiter limiter{60, kStart};
    const auto period = limiter.Period();
    limiter.Release(kStart);
    const auto second = limiter.NextSlot();
    limiter.Release(second + period / 2); // woke half a period late
    CHECK(limiter.Resets() == 0);
    CHECK(Since(second, limiter.NextSlot()) >= period); // anchored to the slot, not to the late wake
    CHECK(limiter.NextSlot() <= second + period + nanoseconds(1));
    CHECK(limiter.LongestLate() == period / 2);
}

void TestDeadlineBoundaryOfAReset() {
    for (const unsigned fps : {60u, 90u, 120u, 240u}) {
        FrameLimiter exact{fps, kStart};
        exact.Release(kStart + exact.Period()); // exactly one period late: still on the slot grid
        CHECK(exact.Resets() == 0);
        FrameLimiter beyond{fps, kStart};
        const auto now = kStart + beyond.Period() + nanoseconds(1);
        beyond.Release(now);
        CHECK(beyond.Resets() == 1 && beyond.NextSlot() == now + beyond.Period());
    }
}

void TestLargeStallRebasesWithoutCatchUp() {
    FrameLimiter limiter{90, kStart};
    limiter.Release(kStart);
    const auto resume = kStart + seconds(10); // shader compile, window drag, debugger break
    int released = 0;
    while (limiter.Due(resume) && released < 100) {
        limiter.Release(resume);
        ++released;
    }
    CHECK(released == 1); // one present, not 900 to catch up
    CHECK(limiter.Resets() == 1);
    CHECK(limiter.NextSlot() == resume + limiter.Period());
    CHECK(limiter.LongestLate() >= seconds(10) - limiter.Period());
    // The next frames run on the new grid.
    limiter.Release(limiter.NextSlot());
    CHECK(limiter.Resets() == 1);
}

void TestSlightlyMissedDeadlineRepaysAtMostOneSlot() {
    for (const unsigned fps : {60u, 90u, 144u}) {
        FrameLimiter limiter{fps, kStart};
        const auto now = kStart + limiter.Period(); // exactly one period late
        int released = 0;
        while (limiter.Due(now) && released < 100) {
            limiter.Release(now);
            ++released;
        }
        CHECK(released <= 2 && limiter.Resets() == 0);
    }
}

void TestDeadlinesNeverGoBackwards() {
    FrameLimiter limiter{120, kStart};
    std::uint32_t seed = 7;
    auto now = kStart;
    auto previous = limiter.NextSlot();
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const std::uint32_t pick = (seed >> 16) % 8;
        const nanoseconds step = pick == 0 ? milliseconds(80) : microseconds(2000 + (seed >> 8) % 9000);
        now += step;
        limiter.Release(now);
        CHECK(limiter.NextSlot() > previous || limiter.NextSlot() >= now);
        CHECK(limiter.NextSlot() > now - limiter.Period() * 2);
        previous = limiter.NextSlot();
    }
    CHECK(limiter.Resets() > 0);
}

void TestWaitsAreNeverNegative() {
    const auto now = kStart;
    CHECK(CoarseSleep(now - seconds(1), now) == nanoseconds(0));
    CHECK(CoarseSleep(now, now) == nanoseconds(0));
    CHECK(CoarseSleep(now + microseconds(100), now) == nanoseconds(0));
    CHECK(CoarseSleep(now + kSpinWindow, now) == nanoseconds(0));
    CHECK(CoarseSleep(now + kSpinWindow + nanoseconds(1), now) == nanoseconds(1));
    CHECK(CoarseSleep(now + milliseconds(16), now) == milliseconds(16) - kSpinWindow);
    std::uint32_t seed = 99;
    for (int i = 0; i < 10000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const auto slot = now + nanoseconds(std::int64_t(seed >> 8) - 4'000'000);
        const nanoseconds sleep = CoarseSleep(slot, now);
        CHECK(sleep >= nanoseconds(0) && sleep <= std::max(Since(now, slot), nanoseconds(0)));
    }
}

void TestNoBusyWaitPolicy() {
    // Only the last stretch before a slot is yielded; everything before it is a sleep.
    CHECK(kSpinWindow <= microseconds(500));
    const nanoseconds shortest_period = FrameLimiter{kUnlimitedCadenceHz, kStart}.Period();
    CHECK(kSpinWindow < shortest_period / 4);
    for (const int mode : kFrameRateModes) {
        const unsigned cadence = RepeatCadenceHz(mode, false, 0.0);
        if (cadence != 0) {
            const nanoseconds period = FrameLimiter{cadence, kStart}.Period();
            CHECK(CoarseSleep(kStart + period, kStart) > nanoseconds(0));
        }
    }
}

void TestEveryRateIsDriftFree() {
    for (const std::int64_t fps : {30, 60, 90, 120, 144, 165, 240, 480}) {
        FrameLimiter limiter{unsigned(fps), kStart};
        for (std::int64_t i = 0; i < fps * 3600; ++i) {
            limiter.Release(limiter.NextSlot());
        }
        // An hour of on-time releases lands on the hour to the nanosecond; dropping the remainder of 1e9 / fps every
        // slot would lose up to 0.67 ns per slot, 144 microseconds at 60 FPS.
        CHECK(Since(kStart, limiter.NextSlot()) == seconds(3600));
        CHECK(limiter.Resets() == 0);
    }
}

void TestSlotsFollowTheExactRationalGrid() {
    for (const std::int64_t fps : {60, 90, 120, 144, 165}) {
        FrameLimiter limiter{unsigned(fps), kStart};
        bool exact = true;
        for (std::int64_t n = 0; n < 5000; ++n) {
            exact = exact && Since(kStart, limiter.NextSlot()).count() == ExactSlotNs(n, fps);
            limiter.Release(limiter.NextSlot());
        }
        CHECK(exact);
    }
}

void TestLongRuntimeArithmetic() {
    // Twelve days at 90 FPS, then far from the epoch: no overflow, no reset, still exact.
    FrameLimiter limiter{90, kStart};
    const std::int64_t releases = 90LL * 86400 * 12;
    for (std::int64_t i = 0; i < releases; ++i) {
        limiter.Release(limiter.NextSlot());
    }
    CHECK(Since(kStart, limiter.NextSlot()) == seconds(86400 * 12));
    CHECK(limiter.Resets() == 0);

    const auto far = Clock::time_point{} + std::chrono::hours(24 * 365 * 100);
    FrameLimiter distant{60, far};
    for (int i = 0; i < 600; ++i) {
        distant.Release(distant.NextSlot());
    }
    CHECK(distant.Resets() == 0 && Since(far, distant.NextSlot()) == seconds(10));
}

void TestClockJumps() {
    FrameLimiter limiter{60, kStart};
    limiter.Release(kStart);
    limiter.Release(limiter.NextSlot() + std::chrono::hours(3)); // the machine slept
    CHECK(limiter.Resets() == 1);
    const auto after_sleep = limiter.NextSlot();
    CHECK(after_sleep > kStart + std::chrono::hours(3));

    // The clock going backwards by more than two periods is rebased instead of waited for.
    FrameLimiter backwards{60, kStart + seconds(100)};
    backwards.Release(kStart + seconds(100));
    backwards.Release(kStart + seconds(50));
    CHECK(backwards.Resets() == 1);
    CHECK(backwards.NextSlot() == kStart + seconds(50) + backwards.Period());
    CHECK(CoarseSleep(backwards.NextSlot(), kStart + seconds(50)) < backwards.Period());

    // A slot one period ahead is ordinary: a guest flip consumes it early.
    FrameLimiter early{60, kStart};
    early.Release(kStart);
    early.Release(kStart);
    CHECK(early.Resets() == 0);
}

void TestMinimizeRestoreAndResizeStalls() {
    FrameLimiter limiter{90, kStart};
    auto now = kStart;
    for (int i = 0; i < 100; ++i) {
        now = limiter.NextSlot();
        limiter.Release(now);
    }
    // Minimized for a minute: nothing is presented, the slots go by.
    now += seconds(60);
    limiter.Release(now);
    CHECK(limiter.Resets() == 1 && limiter.NextSlot() == now + limiter.Period());
    // Restored: normal cadence again, no burst.
    int released = 0;
    while (limiter.Due(now) && released < 10) {
        limiter.Release(now);
        ++released;
    }
    CHECK(released == 0);
    // A 300 ms window drag is also one reset.
    now = limiter.NextSlot() + milliseconds(300);
    limiter.Release(now);
    CHECK(limiter.Resets() == 2);
}

void TestSettingChangeStartsAFreshLimiter() {
    Inputs before{.frame_rate = 90, .vsync = false, .display_hz = 60.0};
    CHECK(before == before);
    Inputs changed = before;
    changed.frame_rate = 120;
    CHECK(!(changed == before));
    changed = before;
    changed.vsync = true;
    CHECK(!(changed == before));
    changed = before;
    changed.display_hz = 144.0;
    CHECK(!(changed == before));

    FrameLimiter old_limiter{CadenceFor(before), kStart};
    old_limiter.Release(kStart + seconds(5)); // reset, late
    FrameLimiter fresh{CadenceFor(changed), kStart + seconds(5)};
    CHECK(fresh.Resets() == 0 && fresh.LongestLate() == nanoseconds(0) && fresh.NextSlot() == kStart + seconds(5));
}

void TestDeterministicSyntheticClock() {
    const auto run = [] {
        FrameLimiter limiter{144, kStart};
        std::uint32_t seed = 31337;
        auto now = kStart;
        std::uint64_t digest = 0;
        for (int i = 0; i < 5000; ++i) {
            seed = seed * 1664525u + 1013904223u;
            now += microseconds(1000 + (seed >> 12) % 12000);
            limiter.Release(now);
            digest = digest * 131 + std::uint64_t(Since(kStart, limiter.NextSlot()).count());
        }
        return std::pair{digest, limiter.Resets()};
    };
    CHECK(run() == run());
}

void TestStallLine() {
    char line[160];
    FormatStall(line, sizeof(line), 3, milliseconds(412), 90);
    CHECK(std::string(line) == "VideoOut: pacing re-anchored 3 times at a 90 FPS cadence, longest lateness 412.0 ms");
}

void TestLogGate() {
    SotbLog::LogGate gate{2, 10'000'000'000ull};
    CHECK(gate.Allow(100) && gate.Allow(101));
    CHECK(!gate.Allow(1'000'000'000ull) && !gate.Allow(2'000'000'000ull));
    CHECK(gate.TakeSuppressed() == 2 && gate.TakeSuppressed() == 0);
    CHECK(!gate.Allow(10'000'000'100ull)); // the interval runs from the last shown line
    CHECK(gate.Allow(10'000'000'101ull));
    CHECK(!gate.Allow(5)); // a clock that went backwards shows nothing
    CHECK(gate.TakeSuppressed() == 2);
    SotbLog::LogGate once{0, 5};
    CHECK(once.Allow(10) && !once.Allow(12) && once.Allow(15)); // no burst: the interval alone decides
}

void TestHighFrameRateSettingsAreUnchanged() {
    CHECK((kFrameRateModes == std::array{30, 60, 90, 120, 144, 165, 240, 0}));
    CHECK(kGameHz == 60 && kUnlimitedCadenceHz == 480);
    CHECK(ParseFrameRate("90") == 90 && ParseFrameRate("60") == 60 && ParseFrameRate("0") == 0);
    CHECK(ParseFrameRate("30") == 30 && ParseFrameRate("240") == 240);
    // Anything but a stored value is the original 60: unsupported and invalid rates fall back.
    for (const char* bad : {"", "75", "-1", "abc", "90.0", " 90", "9999999999", "00"}) {
        CHECK(ParseFrameRate(bad) == 60);
    }
    CHECK(RenderFps(75) == 60 && RenderFps(-5) == 60 && RenderFps(0) == 0 && RenderFps(90) == 90);
    CHECK(!IsFrameRateMode(75) && IsFrameRateMode(144));
    CHECK(ModeName(0) == "Unlimited" && ModeName(90) == "90");
}

void TestSixtyAndNinetyKeepTheirBehavior() {
    // 60 shows the game's own flips: no repeat cadence at all.
    for (const bool vsync : {false, true}) {
        for (const double hz : {0.0, 59.94, 60.0, 144.0}) {
            CHECK(RepeatCadenceHz(60, vsync, hz) == 0);
            CHECK(RepeatCadenceHz(30, vsync, hz) == 0);
        }
    }
    // 90 repeats the game's frames at 90 presentations a second.
    CHECK(RepeatCadenceHz(90) == 90);
    CHECK(RepeatCadenceHz(90, false, 60.0) == 90);
    CHECK(RepeatCadenceHz(90, false, 144.0) == 90);
    CHECK(RepeatCadenceHz(90, true, 144.0) == 90);
    CHECK(RepeatCadenceHz(90, true, 165.0) == 90);
    CHECK(RepeatCadenceHz(90, true, 120.0) == 90);
    CHECK(RepeatCadenceHz(90, true, 59.94) == 0);  // VSync on a 60 Hz display: nothing above it can be shown
    CHECK(RepeatCadenceHz(90, true, 60.0) == 0);
    CHECK(RepeatCadenceHz(90, true, 0.0) == 90);   // an unknown refresh bounds nothing
    CHECK(RepeatCadenceHz(120, true, 119.88) == 120);
    CHECK(RepeatCadenceHz(144, true, 143.856) == 144);
    CHECK(RepeatCadenceHz(165, true, 144.0) == 144);
    CHECK(RepeatCadenceHz(240, false, 60.0) == 240);
    CHECK(RepeatCadenceHz(0, false, 0.0) == unsigned(kUnlimitedCadenceHz));
    CHECK(RepeatCadenceHz(0, true, 144.0) == 144);
    CHECK(RepeatCadenceHz(0, true, 60.0) == 0);
    CHECK(RepeatCadenceHz(0, false, 144.0) == unsigned(kUnlimitedCadenceHz)); // only VSync bounds it by the display
}

void TestGameplayTimingIsNotMultiplied() {
    // The game's own tick stays 60 Hz at every setting; only presentations are added or dropped.
    CHECK(kGameHz == 60);
    for (const int mode : kFrameRateModes) {
        int presented = 0;
        for (std::uint64_t flip = 0; flip < 600; ++flip) {
            presented += PresentsFlip(mode, flip) ? 1 : 0;
        }
        CHECK(presented == (mode == 30 ? 300 : 600)); // every flip is consumed; 30 shows every second one
    }
    CHECK(PresentsFlip(90, 0) && PresentsFlip(90, 1) && PresentsFlip(60, 1));
    CHECK(PresentsFlip(30, 0) && !PresentsFlip(30, 1) && PresentsFlip(30, 2));
}

void TestVsyncAndPresentModePolicy() {
    using SotbPresent::ChooseMode;
    using SotbPresent::Mode;
    CHECK(ChooseMode(true, true, true) == Mode::Fifo);
    CHECK(ChooseMode(true, false, false) == Mode::Fifo);
    CHECK(ChooseMode(false, true, true) == Mode::Mailbox);
    CHECK(ChooseMode(false, false, true) == Mode::Immediate);
    CHECK(ChooseMode(false, false, false) == Mode::Fifo);
    CHECK(ChooseMode(false, true, false) == Mode::Mailbox);

    // With VSync the queue never goes past the display; without it the host cadence is what the setting asks.
    CHECK(EffectiveRenderHz(144, true, 60.0) == 60 && EffectiveRenderHz(144, false, 60.0) == 144);
    CHECK(EffectiveRenderHz(60, true, 144.0) == 60 && EffectiveRenderHz(0, true, 120.0) == 120);
    CHECK(EffectiveRenderHz(0, false, 120.0) == 0);
    CHECK(VsyncBoundsRate(true, 60.0) && !VsyncBoundsRate(true, 0.0) && !VsyncBoundsRate(false, 60.0));
}

void TestCadenceTracksTheDisplayWhenItChanges() {
    // A window moved from a 60 Hz to a 144 Hz display under VSync: the inputs differ, so a new limiter starts.
    Inputs on_60{.frame_rate = 120, .vsync = true, .display_hz = 59.94};
    Inputs on_144{.frame_rate = 120, .vsync = true, .display_hz = 143.856};
    CHECK(!(on_60 == on_144));
    CHECK(CadenceFor(on_60) == 0u && CadenceFor(on_144) == 120u);
    CHECK(CadenceFor(Inputs{}) == 0u);
    Inputs without_vsync = on_60;
    without_vsync.vsync = false;
    CHECK(CadenceFor(without_vsync) == 120u);
}

void TestNoCumulativeDriftBeyondTheRoundingPolicy() {
    // The period itself is truncated, the carry restores the rest: the error of any slot is below one nanosecond.
    for (const std::int64_t fps : {60, 90, 120, 144, 165, 240}) {
        FrameLimiter limiter{unsigned(fps), kStart};
        std::int64_t worst = 0;
        for (std::int64_t n = 0; n < fps * 20; ++n) {
            const std::int64_t exact_scaled = n * 1'000'000'000LL; // slot n is exactly exact_scaled / fps
            const std::int64_t actual_scaled = Since(kStart, limiter.NextSlot()).count() * fps;
            worst = std::max(worst, exact_scaled - actual_scaled < 0 ? actual_scaled - exact_scaled : exact_scaled - actual_scaled);
            limiter.Release(limiter.NextSlot());
        }
        CHECK(worst < fps);
    }
}

} // namespace

int main() {
    TestFirstFrame();
    TestOnTimeFramesKeepTheCadence();
    TestEarlyFrameWaits();
    TestLateFramesDoNotShiftLaterSlots();
    TestDeadlineBoundaryOfAReset();
    TestLargeStallRebasesWithoutCatchUp();
    TestSlightlyMissedDeadlineRepaysAtMostOneSlot();
    TestDeadlinesNeverGoBackwards();
    TestWaitsAreNeverNegative();
    TestNoBusyWaitPolicy();
    TestEveryRateIsDriftFree();
    TestSlotsFollowTheExactRationalGrid();
    TestLongRuntimeArithmetic();
    TestClockJumps();
    TestMinimizeRestoreAndResizeStalls();
    TestSettingChangeStartsAFreshLimiter();
    TestDeterministicSyntheticClock();
    TestStallLine();
    TestLogGate();
    TestHighFrameRateSettingsAreUnchanged();
    TestSixtyAndNinetyKeepTheirBehavior();
    TestGameplayTimingIsNotMultiplied();
    TestVsyncAndPresentModePolicy();
    TestCadenceTracksTheDisplayWhenItChanges();
    TestNoCumulativeDriftBeyondTheRoundingPolicy();
    std::printf("frame pacing tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

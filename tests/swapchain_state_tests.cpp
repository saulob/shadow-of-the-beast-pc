// Swapchain state tests: how acquire and present results, resizes, display and present mode changes, minimizing and
// shutdown drive recreation, on the pure state machine. No Vulkan, no window.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "sotb_swapchain_health.h"

namespace {

using namespace SotbSwapchain;

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

// One whole frame: an acquire and a present with the given results.
void Frame(Health& health, Result acquire, Result present) {
    if (health.Acquire(acquire)) {
        health.Present(present);
    }
}

// A swapchain that has been created once, as after startup.
Health Started() {
    Health health;
    health.Recreated();
    return health;
}

void TestClassification() {
    CHECK(ClassifyAcquire(0) == Result::Success);
    CHECK(ClassifyAcquire(kVkSuboptimal) == Result::Suboptimal);
    CHECK(ClassifyAcquire(kVkErrorOutOfDate) == Result::OutOfDate);
    CHECK(ClassifyAcquire(kVkErrorUnknown) == Result::OutOfDate);
    CHECK(ClassifyAcquire(kVkErrorSurfaceLost) == Result::SurfaceLost);
    CHECK(ClassifyPresent(0) == Result::Success);
    CHECK(ClassifyPresent(kVkSuboptimal) == Result::Suboptimal);
    CHECK(ClassifyPresent(kVkErrorOutOfDate) == Result::OutOfDate);
    CHECK(ClassifyPresent(kVkErrorSurfaceLost) == Result::SurfaceLost);
    CHECK(ClassifyPresent(kVkErrorUnknown) == Result::Failure);
    // Device lost is the global owner's, never a swapchain result; other errors stay failures.
    CHECK(ClassifyAcquire(kVkErrorDeviceLost) == Result::Failure);
    CHECK(ClassifyPresent(kVkErrorDeviceLost) == Result::Failure);
    CHECK(ClassifyAcquire(-1) == Result::Failure && ClassifyPresent(-2) == Result::Failure);
    CHECK(ClassifyAcquire(2) == Result::Failure); // VK_TIMEOUT is not a swapchain outcome here
}

void TestInitialAndCleanFrames() {
    Health health;
    CHECK(!health.NeedsRecreation() && !health.Suppressing() && !health.Stopped());
    CHECK(health.Pending() == Reason::None && health.Consistent());
    CHECK(health.Stats().recreations == 0 && health.Stats().suboptimal == 0);

    health.Recreated();
    CHECK(health.Stats().recreations == 0); // the first swapchain is not a recreation
    CHECK(health.Acquire(Result::Success));
    CHECK(!health.NeedsRecreation());
    CHECK(health.Present(Result::Success));
    CHECK(!health.NeedsRecreation() && health.Consistent());
}

void TestFirstSuboptimalSchedulesOneRecreation() {
    Health health = Started();
    CHECK(health.Acquire(Result::Suboptimal)); // the image is usable
    CHECK(health.NeedsRecreation() && health.Pending() == Reason::Suboptimal);
    CHECK(health.Present(Result::Success)); // and it is presented before the swapchain is replaced
    CHECK(health.NeedsRecreation());
    CHECK(health.Stats().suboptimal == 1);

    health.Recreated();
    CHECK(!health.NeedsRecreation());
    CHECK(health.Stats().recreations == 1);
    CHECK(health.Stats().by_reason[std::size_t(Reason::Suboptimal)] == 1);
    CHECK(health.LastReason() == Reason::Suboptimal);
}

void TestSuboptimalOnPresentSchedulesRecreation() {
    Health health = Started();
    CHECK(health.Acquire(Result::Success));
    CHECK(health.Present(Result::Suboptimal)); // queued for display
    CHECK(health.NeedsRecreation() && health.Pending() == Reason::Suboptimal);
}

void TestPersistentSuboptimalDoesNotLoop() {
    Health health = Started();
    int recreations = 0;
    for (int frame = 0; frame < 1000; ++frame) {
        Frame(health, Result::Suboptimal, Result::Suboptimal);
        if (health.NeedsRecreation()) {
            health.Recreated();
            ++recreations;
        }
        CHECK(health.Consistent());
    }
    CHECK(recreations == 1);
    CHECK(health.Suppressing());
    CHECK(health.Stats().recreations == 1);
    CHECK(health.Stats().suboptimal == 2000);
    CHECK(health.Stats().suboptimal_kept == 1998);
}

void TestPersistentSuboptimalOnAcquireOnly() {
    Health health = Started();
    int recreations = 0;
    for (int frame = 0; frame < 300; ++frame) {
        Frame(health, Result::Suboptimal, Result::Success);
        if (health.NeedsRecreation()) {
            health.Recreated();
            ++recreations;
        }
    }
    // A clean present alone is not a clean frame: the acquire was suboptimal.
    CHECK(recreations == 1 && health.Suppressing());
}

void TestCleanFrameAllowsSuboptimalAgain() {
    Health health = Started();
    Frame(health, Result::Suboptimal, Result::Success);
    health.Recreated();
    CHECK(health.Suppressing());
    Frame(health, Result::Suboptimal, Result::Suboptimal);
    CHECK(!health.NeedsRecreation()); // held back

    Frame(health, Result::Success, Result::Success);
    CHECK(!health.Suppressing());
    Frame(health, Result::Suboptimal, Result::Success);
    CHECK(health.NeedsRecreation()); // a later suboptimal result is acted on once more
    health.Recreated();
    CHECK(health.Stats().recreations == 2);
}

void TestCleanAcquireWithoutCleanPresentKeepsSuppression() {
    Health health = Started();
    Frame(health, Result::Suboptimal, Result::Success);
    health.Recreated();
    Frame(health, Result::Success, Result::Suboptimal);
    CHECK(health.Suppressing() && !health.NeedsRecreation());
    Frame(health, Result::Suboptimal, Result::Success);
    CHECK(health.Suppressing() && !health.NeedsRecreation());
}

void TestOutOfDateAlwaysRecreates() {
    Health health = Started();
    CHECK(!health.Acquire(Result::OutOfDate)); // no image was handed out
    CHECK(health.NeedsRecreation() && health.Pending() == Reason::OutOfDate);
    health.Recreated();
    CHECK(health.Stats().by_reason[std::size_t(Reason::OutOfDate)] == 1);
    CHECK(!health.Suppressing()); // a genuine invalidation allows a suboptimal recreation later

    Health present = Started();
    CHECK(present.Acquire(Result::Success));
    CHECK(!present.Present(Result::OutOfDate)); // not queued
    CHECK(present.NeedsRecreation() && present.Pending() == Reason::OutOfDate);
}

void TestOutOfDateOverridesSuppression() {
    Health health = Started();
    Frame(health, Result::Suboptimal, Result::Suboptimal);
    health.Recreated();
    CHECK(health.Suppressing());
    CHECK(!health.Acquire(Result::OutOfDate));
    CHECK(health.NeedsRecreation() && health.Pending() == Reason::OutOfDate);
    health.Recreated();
    CHECK(!health.Suppressing());
    Frame(health, Result::Suboptimal, Result::Success);
    CHECK(health.NeedsRecreation()); // not stuck in a suppressed state
}

void TestOutOfDateIsNotDowngradedBySuboptimal() {
    Health health = Started();
    CHECK(!health.Acquire(Result::OutOfDate));
    CHECK(health.Acquire(Result::Suboptimal));
    CHECK(health.Pending() == Reason::OutOfDate);
    health.Recreated();
    CHECK(health.Stats().by_reason[std::size_t(Reason::OutOfDate)] == 1);
    CHECK(health.Stats().by_reason[std::size_t(Reason::Suboptimal)] == 0);
}

void TestSurfaceLostIsItsOwnPath() {
    Health health = Started();
    CHECK(!health.Acquire(Result::SurfaceLost));
    CHECK(health.NeedsRecreation() && health.NeedsNewSurface());
    CHECK(health.Pending() == Reason::SurfaceLost);
    CHECK(health.Stats().surface_lost == 1 && health.Stats().out_of_date == 0 && health.Stats().suboptimal == 0);
    health.Recreated();
    CHECK(!health.NeedsNewSurface());
    CHECK(health.Stats().by_reason[std::size_t(Reason::SurfaceLost)] == 1);

    // Out of date does not need a new surface, and lost outranks it.
    Health mixed = Started();
    mixed.Acquire(Result::OutOfDate);
    CHECK(!mixed.NeedsNewSurface());
    mixed.Present(Result::SurfaceLost);
    CHECK(mixed.NeedsNewSurface());
    mixed.Acquire(Result::OutOfDate);
    CHECK(mixed.NeedsNewSurface()); // severity only rises
}

void TestFailureChangesNothing() {
    Health health = Started();
    CHECK(!health.Acquire(Result::Failure));
    CHECK(!health.NeedsRecreation() && health.Stats().suboptimal == 0 && health.Stats().out_of_date == 0);
    CHECK(!health.Present(Result::Failure));
    CHECK(!health.NeedsRecreation());
}

void TestForcedRecreationIsEvidence() {
    for (const Reason forced : {Reason::Resize, Reason::PresentMode, Reason::Hdr}) {
        Health health = Started();
        Frame(health, Result::Suboptimal, Result::Success);
        health.Recreated();
        CHECK(health.Suppressing());
        health.Recreated(forced); // a resize, VSync or HDR change made a different swapchain
        CHECK(!health.Suppressing());
        CHECK(health.Stats().by_reason[std::size_t(forced)] == 1);
        Frame(health, Result::Suboptimal, Result::Suboptimal);
        CHECK(health.NeedsRecreation());
    }
}

void TestForcedWithPendingSuboptimal() {
    Health health = Started();
    health.Acquire(Result::Suboptimal);
    health.Recreated(Reason::Resize); // the replacement differs: not a suboptimal-only replacement
    CHECK(!health.Suppressing());
    CHECK(health.LastReason() == Reason::Resize);
    CHECK(health.Stats().by_reason[std::size_t(Reason::Resize)] == 1);
    CHECK(health.Stats().by_reason[std::size_t(Reason::Suboptimal)] == 0);

    Health invalid = Started();
    invalid.Acquire(Result::OutOfDate);
    invalid.Recreated(Reason::Resize);
    CHECK(invalid.LastReason() == Reason::OutOfDate); // the invalid swapchain is the stronger reason
}

void TestDisplayChangeIsEvidence() {
    Health health = Started();
    Frame(health, Result::Suboptimal, Result::Suboptimal);
    health.Recreated();
    CHECK(health.Suppressing());
    health.DisplayChanged();
    CHECK(!health.Suppressing() && health.Stats().display_changes == 1);
    Frame(health, Result::Suboptimal, Result::Suboptimal);
    CHECK(health.NeedsRecreation()); // one more suboptimal recreation for the new display
    health.Recreated();
    for (int i = 0; i < 100; ++i) {
        Frame(health, Result::Suboptimal, Result::Suboptimal);
        CHECK(!health.NeedsRecreation()); // and then it stays bounded
    }
    // A display change alone never asks for a recreation.
    Health quiet = Started();
    quiet.DisplayChanged();
    CHECK(!quiet.NeedsRecreation());
}

void TestSameExtentNeedsNothing() {
    CHECK(ChooseFrameAction(1280, 720, 1280, 720, false) == FrameAction::Keep);
    for (int i = 0; i < 100; ++i) {
        CHECK(ChooseFrameAction(1280, 720, 1280, 720, false) == FrameAction::Keep);
    }
    CHECK(ChooseFrameAction(1280, 720, 1280, 720, true) == FrameAction::Recreate); // a pending recreation
}

void TestExtentChangeRecreates() {
    CHECK(ChooseFrameAction(1920, 1080, 1280, 720, false) == FrameAction::Recreate);
    CHECK(ChooseFrameAction(1280, 1080, 1280, 720, false) == FrameAction::Recreate);
    CHECK(ChooseFrameAction(1920, 720, 1280, 720, false) == FrameAction::Recreate);
    CHECK(ForcedReason(1280, 720, 1920, 1080) == Reason::Resize);
    CHECK(ForcedReason(1280, 720, 1280, 720) == Reason::None);
}

void TestMinimizeAndRestore() {
    for (const bool pending : {false, true}) {
        CHECK(ChooseFrameAction(0, 0, 1280, 720, pending) == FrameAction::Skip);
        CHECK(ChooseFrameAction(0, 720, 1280, 720, pending) == FrameAction::Skip);
        CHECK(ChooseFrameAction(1280, 0, 1280, 720, pending) == FrameAction::Skip);
        CHECK(ChooseFrameAction(-1, 720, 1280, 720, pending) == FrameAction::Skip);
    }
    // Minimized frames never recreate, however long it lasts and whatever is pending.
    Health health = Started();
    health.Acquire(Result::OutOfDate);
    int recreations = 0;
    for (int frame = 0; frame < 1000; ++frame) {
        if (ChooseFrameAction(0, 0, 1280, 720, health.NeedsRecreation()) == FrameAction::Recreate) {
            ++recreations;
        }
    }
    CHECK(recreations == 0 && health.NeedsRecreation()); // the pending recreation survives
    // Restored at the same size: the swapchain is kept unless something is pending.
    CHECK(ChooseFrameAction(1280, 720, 1280, 720, health.NeedsRecreation()) == FrameAction::Recreate);
    CHECK(ChooseFrameAction(1280, 720, 1280, 720, false) == FrameAction::Keep);
    // Restored at another size, or maximized: new dimensions are used, not stale ones.
    CHECK(ChooseFrameAction(1920, 1080, 1280, 720, false) == FrameAction::Recreate);
}

void TestCreateFailedRetriesWithoutLooping() {
    Health health = Started();
    Frame(health, Result::Suboptimal, Result::Suboptimal);
    health.Recreated();
    CHECK(health.Suppressing());
    health.CreateFailed(); // the surface reported 0x0
    CHECK(health.NeedsRecreation() && health.Pending() == Reason::OutOfDate && !health.Suppressing());
    CHECK(health.Stats().recreations == 1); // nothing was created
    health.Recreated();
    CHECK(health.Stats().recreations == 2 && !health.NeedsRecreation());
}

void TestShutdown() {
    Health health = Started();
    health.Acquire(Result::OutOfDate);
    health.Stop();
    CHECK(health.Stopped() && !health.NeedsRecreation() && health.Consistent());
    CHECK(!health.Acquire(Result::Success));
    CHECK(!health.Present(Result::Success));
    CHECK(!health.Acquire(Result::OutOfDate) && !health.NeedsRecreation());
    health.Recreated();
    health.DisplayChanged();
    health.CreateFailed();
    CHECK(!health.NeedsRecreation() && health.Stats().recreations == 0 && health.Stats().display_changes == 0);
    CHECK(health.Stats().out_of_date == 1); // nothing counted after stopping
}

void TestCounters() {
    Health health = Started();
    Frame(health, Result::Suboptimal, Result::Success);
    health.Recreated();
    Frame(health, Result::Suboptimal, Result::Suboptimal);
    health.Acquire(Result::OutOfDate);
    health.Recreated();
    health.Present(Result::SurfaceLost);
    health.Recreated();
    health.Recreated(Reason::Hdr);
    const Counters& stats = health.Stats();
    CHECK(stats.suboptimal == 3 && stats.suboptimal_kept == 2);
    CHECK(stats.out_of_date == 1 && stats.surface_lost == 1);
    CHECK(stats.recreations == 4);
    CHECK(stats.by_reason[std::size_t(Reason::Suboptimal)] == 1);
    CHECK(stats.by_reason[std::size_t(Reason::OutOfDate)] == 1);
    CHECK(stats.by_reason[std::size_t(Reason::SurfaceLost)] == 1);
    CHECK(stats.by_reason[std::size_t(Reason::Hdr)] == 1);
    std::uint32_t total = 0;
    for (const std::uint32_t count : stats.by_reason) {
        total += count;
    }
    CHECK(total == stats.recreations);
}

void TestReasonFormatting() {
    CHECK(std::string(ReasonName(Reason::Suboptimal)) == "suboptimal");
    CHECK(std::string(ReasonName(Reason::OutOfDate)) == "out of date");
    CHECK(std::string(ReasonName(Reason::SurfaceLost)) == "surface lost");
    CHECK(std::string(ReasonName(Reason::Resize)) == "resize");
    CHECK(std::string(ReasonName(Reason::PresentMode)) == "present mode");
    CHECK(std::string(ReasonName(Reason::Hdr)) == "HDR");
    CHECK(std::string(ReasonName(Reason::None)) == "requested");

    Health health = Started();
    Frame(health, Result::Suboptimal, Result::Suboptimal);
    health.Recreated();
    Frame(health, Result::Suboptimal, Result::Suboptimal);
    char line[256];
    FormatRecreation(line, sizeof(line), Reason::Suboptimal, health.Stats(), 1280, 720, 3, "VK_PRESENT_MODE_FIFO_KHR");
    CHECK(std::string(line) == "Swapchain: recreated #1 (suboptimal), 1280x720, 3 images, VK_PRESENT_MODE_FIFO_KHR; "
                               "suboptimal 4 (2 kept), out of date 0, surface lost 0");
    char tiny[16];
    FormatRecreation(tiny, sizeof(tiny), Reason::Resize, health.Stats(), 1, 1, 1, nullptr);
    CHECK(std::strlen(tiny) == sizeof(tiny) - 1); // truncated, never overrun
}

// Every sequence of results, resizes and display changes keeps the state consistent and the recreations bounded.
void TestRepeatedTransitionsAreDeterministic() {
    for (int run = 0; run < 2; ++run) {
        std::uint32_t seed = 12345;
        const auto next = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return seed >> 16;
        };
        Health health = Started();
        int recreations = 0;
        std::uint64_t digest = 0;
        for (int step = 0; step < 20000; ++step) {
            const std::uint32_t pick = next() % 10;
            const Result acquire = pick < 5 ? Result::Success : pick < 8 ? Result::Suboptimal : Result::OutOfDate;
            const Result present = next() % 4 == 0 ? Result::Suboptimal : Result::Success;
            Frame(health, acquire, present);
            if (next() % 50 == 0) {
                health.DisplayChanged();
            }
            if (health.NeedsRecreation()) {
                health.Recreated(next() % 20 == 0 ? Reason::Resize : Reason::None);
                ++recreations;
            }
            CHECK(health.Consistent());
            digest = digest * 31 + std::uint64_t(health.Stats().recreations) + (health.Suppressing() ? 7 : 0);
        }
        CHECK(recreations == int(health.Stats().recreations));
        CHECK(recreations < 20000 / 2);
        static std::uint64_t first_digest = 0;
        if (run == 0) {
            first_digest = digest;
        } else {
            CHECK(digest == first_digest);
        }
    }
}

void TestNoImpossibleCombination() {
    Health health;
    for (const Result a : {Result::Success, Result::Suboptimal, Result::OutOfDate, Result::SurfaceLost, Result::Failure}) {
        for (const Result p : {Result::Success, Result::Suboptimal, Result::OutOfDate, Result::SurfaceLost, Result::Failure}) {
            for (const bool recreate : {false, true}) {
                Health h = Started();
                Frame(h, Result::Suboptimal, Result::Suboptimal);
                h.Recreated();
                Frame(h, a, p);
                CHECK(h.Consistent());
                if (recreate && h.NeedsRecreation()) {
                    h.Recreated();
                    CHECK(h.Consistent() && !h.NeedsRecreation());
                }
            }
        }
    }
    CHECK(health.Consistent());
}

} // namespace

int main() {
    TestClassification();
    TestInitialAndCleanFrames();
    TestFirstSuboptimalSchedulesOneRecreation();
    TestSuboptimalOnPresentSchedulesRecreation();
    TestPersistentSuboptimalDoesNotLoop();
    TestPersistentSuboptimalOnAcquireOnly();
    TestCleanFrameAllowsSuboptimalAgain();
    TestCleanAcquireWithoutCleanPresentKeepsSuppression();
    TestOutOfDateAlwaysRecreates();
    TestOutOfDateOverridesSuppression();
    TestOutOfDateIsNotDowngradedBySuboptimal();
    TestSurfaceLostIsItsOwnPath();
    TestFailureChangesNothing();
    TestForcedRecreationIsEvidence();
    TestForcedWithPendingSuboptimal();
    TestDisplayChangeIsEvidence();
    TestSameExtentNeedsNothing();
    TestExtentChangeRecreates();
    TestMinimizeAndRestore();
    TestCreateFailedRetriesWithoutLooping();
    TestShutdown();
    TestCounters();
    TestReasonFormatting();
    TestRepeatedTransitionsAreDeterministic();
    TestNoImpossibleCombination();
    std::printf("swapchain state tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

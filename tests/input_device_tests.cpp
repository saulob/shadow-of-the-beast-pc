// Controller activity onset (buttons, D-pad, sticks, triggers) and per-controller tracking. Pure policy: no
// controller, no window, no cursor.
#include <climits>
#include <cstdio>

#include "sotb_cursor.h"

using namespace SotbCursor;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);              \
        }                                                                                  \
    } while (0)

void TestButtons() {
    ControllerActivityFilter f;
    CHECK(f.ButtonChanged(0, true));
    CHECK(!f.ButtonChanged(0, true));
    CHECK(!f.ButtonChanged(0, false));
    CHECK(f.ButtonChanged(0, true));
    for (int dpad = 11; dpad <= 14; ++dpad) CHECK(f.ButtonChanged(dpad, true));
    CHECK(!f.ButtonChanged(-1, true) && !f.ButtonChanged(32, true) && !f.ButtonChanged(1000, true));
    CHECK(f.ButtonChanged(31, true));
}

void TestSticks() {
    ControllerActivityFilter f;
    CHECK(!f.AxisMoved(Axis::LeftX, 0));
    CHECK(!f.AxisMoved(Axis::LeftX, 1200));
    CHECK(!f.AxisMoved(Axis::LeftX, -3000));
    CHECK(!f.AxisMoved(Axis::LeftX, kStickDeadzone - 1));
    CHECK(f.AxisMoved(Axis::LeftX, kStickDeadzone));
    CHECK(!f.AxisMoved(Axis::LeftX, kStickDeadzone));
    int repeats = 0;
    for (int i = 0; i < 10000; ++i) repeats += f.AxisMoved(Axis::LeftX, 20000 + (i % 3000));
    CHECK(repeats == 0);
    CHECK(!f.AxisMoved(Axis::LeftX, kStickDeadzone - 100));
    CHECK(!f.AxisMoved(Axis::LeftX, kStickDeadzone));
    CHECK(!f.AxisMoved(Axis::LeftX, kStickRelease - 1));
    CHECK(f.AxisMoved(Axis::LeftX, kStickDeadzone));
    CHECK(!f.AxisMoved(Axis::LeftX, 0));
    CHECK(f.AxisMoved(Axis::LeftX, -kStickDeadzone));

    int jitter = 0;
    for (int i = 0; i < 5000; ++i) jitter += f.AxisMoved(Axis::RightY, i % 2 ? kStickDeadzone + 5 : kStickDeadzone - 5);
    CHECK(jitter == 1);

    ControllerActivityFilter g;
    CHECK(g.AxisMoved(Axis::LeftX, 30000));
    CHECK(g.AxisMoved(Axis::LeftY, 30000));
    CHECK(g.AxisMoved(Axis::RightX, -30000));
    CHECK(g.AxisMoved(Axis::RightY, 30000));
    CHECK(!g.AxisMoved(Axis::LeftX, 32767));
    CHECK(g.AxisMoved(Axis::RightTrigger, 32767));

    ControllerActivityFilter e;
    CHECK(e.AxisMoved(Axis::LeftX, -32768));
    CHECK(e.AxisMoved(Axis::LeftY, 32767));
    CHECK(!e.AxisMoved(Axis::RightX, 0));
    CHECK(e.AxisMoved(Axis::RightY, INT_MIN));
    CHECK(!e.AxisMoved(Axis::LeftTrigger, INT_MIN));
    CHECK(e.AxisMoved(Axis::LeftTrigger, INT_MAX));
}

void TestTriggers() {
    ControllerActivityFilter f;
    CHECK(!f.AxisMoved(Axis::LeftTrigger, 0));
    CHECK(!f.AxisMoved(Axis::LeftTrigger, 500));
    CHECK(!f.AxisMoved(Axis::LeftTrigger, kTriggerThreshold - 1));
    CHECK(f.AxisMoved(Axis::LeftTrigger, kTriggerThreshold));
    int repeats = 0;
    for (int i = 0; i < 10000; ++i) repeats += f.AxisMoved(Axis::LeftTrigger, 10000 + (i % 20000));
    CHECK(repeats == 0);
    CHECK(!f.AxisMoved(Axis::LeftTrigger, 0));
    CHECK(f.AxisMoved(Axis::LeftTrigger, 32767));
    CHECK(!f.AxisMoved(Axis::RightTrigger, 100));
    CHECK(f.AxisMoved(Axis::RightTrigger, 20000));
    CHECK(kTriggerThreshold == 30 * 128);
}

void TestTracker() {
    ControllerActivityTracker t;
    CHECK(t.Tracked() == 0);
    CHECK(t.ButtonChanged(7, 0, true));
    CHECK(!t.ButtonChanged(7, 0, true));
    CHECK(t.ButtonChanged(8, 0, true));
    CHECK(t.AxisMoved(7, Axis::LeftX, 30000));
    CHECK(!t.AxisMoved(8, Axis::LeftX, 100));
    CHECK(t.Tracked() == 2);
    t.Forget(7);
    CHECK(t.Tracked() == 1);
    CHECK(t.ButtonChanged(7, 0, true));
    t.Forget(99);
    t.Forget(7);
    t.Forget(8);
    CHECK(t.Tracked() == 0);
    t.Forget(8);
    for (std::uint32_t id = 0; id < 1000; ++id) {
        t.ButtonChanged(id, 1, true);
        t.Forget(id);
    }
    CHECK(t.Tracked() == 0);
    CHECK(!t.AxisMoved(5, Axis::LeftX, 0) && t.Tracked() == 1);
}

} // namespace

int main() {
    TestButtons();
    TestSticks();
    TestTriggers();
    TestTracker();
    std::printf("input device tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

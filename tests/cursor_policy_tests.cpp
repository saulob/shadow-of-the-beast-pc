// Cursor visibility from the last meaningful input device and the overlay's force-visible condition. Pure policy:
// no window, no cursor, no controller. Also checks the window wiring in source.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "sotb_cursor.h"

namespace fs = std::filesystem;
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

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

bool Visible(State& s, bool overlay = false, bool focused = true) {
    s.Update(true, focused, overlay);
    return s.Visible();
}

void TestDeviceSelection() {
    State s;
    CHECK(s.LastDevice() == Device::Mouse && s.Visible() && Visible(s));
    s.KeyboardActivity();
    CHECK(s.LastDevice() == Device::Keyboard && !Visible(s));
    s.MouseMoved(3, 0);
    CHECK(s.LastDevice() == Device::Mouse && Visible(s));
    s.ControllerActivity();
    CHECK(s.LastDevice() == Device::Controller && !Visible(s));
    s.MouseButtonPressed();
    CHECK(s.LastDevice() == Device::Mouse && Visible(s));
    s.KeyboardActivity();
    s.MouseWheelMoved();
    CHECK(s.LastDevice() == Device::Mouse && Visible(s));
    s.ControllerActivity();
    s.KeyboardActivity();
    CHECK(s.LastDevice() == Device::Keyboard && !Visible(s));

    State z;
    z.KeyboardActivity();
    z.MouseMoved(0, 0);
    CHECK(z.LastDevice() == Device::Keyboard);
    z.MouseMoved(0, -1);
    CHECK(z.LastDevice() == Device::Mouse);

    CHECK(IsGameplayKey(false, false) && !IsGameplayKey(true, false) && !IsGameplayKey(false, true));
}

void TestUpdateResults() {
    State s;
    CHECK(!s.Update(true, true, false).has_value());
    s.KeyboardActivity();
    const auto hidden = s.Update(true, true, false);
    CHECK(hidden.has_value() && !*hidden);
    CHECK(!s.Update(true, true, false).has_value());
    s.MouseMoved(1, 1);
    const auto shown = s.Update(true, true, false);
    CHECK(shown.has_value() && *shown);
    int changes = 0;
    for (int i = 0; i < 10000; ++i) {
        s.ControllerActivity();
        changes += s.Update(true, true, false).has_value();
        s.ControllerActivity();
        changes += s.Update(true, true, false).has_value();
    }
    CHECK(changes == 1);
}

void TestFocusAndLauncher() {
    State s;
    s.ControllerActivity();
    CHECK(!Visible(s));
    CHECK(Visible(s, false, false));
    CHECK(!Visible(s, false, true));
    s.ControllerActivity();
    s.Update(false, true, false);
    CHECK(s.Visible() && s.LastDevice() == Device::Mouse);
    CHECK(Visible(s));
}

void TestOverlay() {
    State s;
    s.ControllerActivity();
    CHECK(!Visible(s));
    CHECK(Visible(s, true));
    s.ControllerActivity();
    CHECK(s.LastDevice() == Device::Controller && Visible(s, true));
    s.KeyboardActivity();
    CHECK(s.LastDevice() == Device::Keyboard && Visible(s, true));
    CHECK(!Visible(s, false));

    State m;
    m.ControllerActivity();
    Visible(m);
    Visible(m, true);
    m.MouseMoved(5, 5);
    CHECK(m.LastDevice() == Device::Mouse && Visible(m, true));
    CHECK(Visible(m, false));

    State c;
    Visible(c, true);
    c.ControllerActivity();
    CHECK(Visible(c, true));
    CHECK(!Visible(c, false));

    State quiet;
    quiet.ControllerActivity();
    Visible(quiet);
    Visible(quiet, true);
    CHECK(!Visible(quiet, false));

    State mouse_open;
    Visible(mouse_open);
    Visible(mouse_open, true);
    CHECK(Visible(mouse_open, false));

    State stale;
    stale.MouseMoved(2, 2);
    Visible(stale);
    Visible(stale, true);
    stale.ControllerActivity();
    CHECK(!Visible(stale, false));

    State repeat;
    repeat.KeyboardActivity();
    for (int i = 0; i < 100; ++i) {
        CHECK(Visible(repeat, true));
        CHECK(!Visible(repeat, false));
    }
}

void TestWindowWiring() {
    const std::string window = ReadAll(fs::path(SOTB_SOURCE_DIR) / "gpu/shim/window.cpp");
    const size_t track = window.find("TrackCursorActivity(event, controller_activity);");
    const size_t handle = window.find("SotbOverlay::HandleEvent(event)");
    CHECK(track != std::string::npos && handle != std::string::npos && track < handle);
    CHECK(!Contains(window, "!ui") && Contains(window, "pad_activity.Forget("));
    CHECK(Contains(window, "cursor.MouseMoved(static_cast<int>(event.motion.xrel)"));
    CHECK(Contains(window, "cursor.Update(true, focused, SotbOverlay::CapturesInput())"));
    CHECK(!Contains(window, "SDL_WarpMouse"));
    const std::string header = ReadAll(fs::path(SOTB_SOURCE_DIR) / "gpu/shim/sotb_cursor.h");
    CHECK(!Contains(header, "device_before_overlay_"));
}

} // namespace

int main() {
    TestDeviceSelection();
    TestUpdateResults();
    TestFocusAndLauncher();
    TestOverlay();
    TestWindowWiring();
    std::printf("cursor policy tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

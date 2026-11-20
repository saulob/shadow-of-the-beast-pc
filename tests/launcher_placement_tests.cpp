// Launcher window placement on synthetic display layouts: default centering, restoring, clamping, fallback after a
// display change, DPI, the placement line, and source wiring. No window, no SDL video.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "placement.h"
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

Rect R(int left, int top, int width, int height) { return {left, top, left + width, top + height}; }

WindowPlacement Saved(const Rect& rect, bool maximized = false, bool fullscreen = false) { return {rect, maximized, fullscreen}; }

// A 1920x1080 primary with a 40 px taskbar.
const DisplayArea kPrimary{R(0, 0, 1920, 1040), 1.0f};

bool Inside(const Rect& rect, const Rect& area) {
    return rect.left >= area.left && rect.top >= area.top && rect.right <= area.right && rect.bottom <= area.bottom;
}

void TestDefaultCentered() {
    const std::vector<DisplayArea> displays{kPrimary};
    const PlacementChoice choice = ChoosePlacement(std::nullopt, displays, 1280, 720);
    CHECK(choice.source == PlacementSource::kCentered && choice.centered && !choice.stored_invalid);
    CHECK(choice.display == 0);
    CHECK(choice.rect == R(320, 160, 1280, 720));
    CHECK(!choice.maximized && !choice.fullscreen);

    // Nothing saved yet reads as zeros: that is "none", not an invalid placement.
    const PlacementChoice zeros = ChoosePlacement(Saved(Rect{}), displays, 1280, 720);
    CHECK(zeros.source == PlacementSource::kCentered && !zeros.stored_invalid);
}

void TestValidRestored() {
    const std::vector<DisplayArea> displays{kPrimary};
    const Rect normal = R(100, 120, 1300, 760);
    const PlacementChoice choice = ChoosePlacement(Saved(normal), displays, 1280, 720);
    CHECK(choice.source == PlacementSource::kRestored && !choice.centered && !choice.stored_invalid);
    CHECK(choice.rect == normal);

    // An Aero-snapped half keeps its place: half a title bar above it is on screen.
    const Rect snapped = R(0, 31, 960, 1001);
    CHECK(ChoosePlacement(Saved(snapped), displays, 1280, 720).source == PlacementSource::kRestored);

    // Partly off the right edge with the title bar reachable is the player's choice, kept as it is.
    const Rect overhang = R(1500, 200, 1000, 600);
    const PlacementChoice kept = ChoosePlacement(Saved(overhang), displays, 1280, 720);
    CHECK(kept.source == PlacementSource::kRestored && kept.rect == overhang);
}

void TestOffScreenRejected() {
    const std::vector<DisplayArea> displays{kPrimary};
    const PlacementChoice choice = ChoosePlacement(Saved(R(5000, 5000, 1280, 720)), displays, 1280, 720);
    CHECK(choice.source == PlacementSource::kFallback && choice.stored_invalid && choice.centered);
    CHECK(choice.display == 0);
    CHECK(choice.rect.width() == 1280 && choice.rect.height() == 720);
    CHECK(Inside(choice.rect, kPrimary.usable));

    // A sliver of 60x30 on screen cannot be grabbed and counts as off screen.
    CHECK(ChoosePlacement(Saved(R(1860, 1010, 1280, 720)), displays, 1280, 720).source == PlacementSource::kFallback);
}

void TestRemovedDisplay() {
    const Rect on_second = R(2100, 100, 1400, 800);
    const std::vector<DisplayArea> both{kPrimary, {R(1920, 0, 2560, 1400), 1.0f}};
    const PlacementChoice before = ChoosePlacement(Saved(on_second), both, 1280, 720);
    CHECK(before.source == PlacementSource::kRestored && before.display == 1);

    const std::vector<DisplayArea> only_primary{kPrimary};
    const PlacementChoice after = ChoosePlacement(Saved(on_second), only_primary, 1280, 720);
    CHECK(after.source == PlacementSource::kFallback && after.stored_invalid && after.display == 0);
    CHECK(after.rect.width() == 1400 && after.rect.height() == 800);
    CHECK(Inside(after.rect, kPrimary.usable));
}

void TestNegativeAndLeftMonitor() {
    const DisplayArea left{R(-2560, 0, 2560, 1400), 1.0f};
    const std::vector<DisplayArea> displays{kPrimary, left};
    const Rect normal = R(-2000, 100, 1300, 720);
    const PlacementChoice choice = ChoosePlacement(Saved(normal), displays, 1280, 720);
    CHECK(choice.source == PlacementSource::kRestored && choice.display == 1 && choice.rect == normal);

    // The left monitor was moved to the right of the primary: its old negative coordinates are on nothing now.
    const std::vector<DisplayArea> moved{kPrimary, {R(1920, 0, 2560, 1400), 1.0f}};
    const PlacementChoice after = ChoosePlacement(Saved(normal), moved, 1280, 720);
    CHECK(after.source == PlacementSource::kFallback && after.display == 0 && Inside(after.rect, kPrimary.usable));
}

void TestMonitorAbove() {
    const DisplayArea above{R(0, -1440, 2560, 1440), 1.0f};
    const std::vector<DisplayArea> displays{kPrimary, above};
    const Rect normal = R(200, -1300, 1280, 720);
    const PlacementChoice choice = ChoosePlacement(Saved(normal), displays, 1280, 720);
    CHECK(choice.source == PlacementSource::kRestored && choice.display == 1 && choice.rect == normal);

    // Its title bar above the upper display's top edge is moved down into it.
    const PlacementChoice clamped = ChoosePlacement(Saved(R(200, -1440, 1280, 720)), displays, 1280, 720);
    CHECK(clamped.source == PlacementSource::kClamped && clamped.display == 1);
    CHECK(clamped.rect.top == -1440 + kEstimatedTitleBarHeight && clamped.rect.width() == 1280);
}

void TestPrimaryChanged() {
    // The display that used to be primary is now listed second; the saved window still lies on it.
    const DisplayArea old_primary{R(0, 0, 1920, 1040), 1.0f};
    const DisplayArea new_primary{R(-2560, 0, 2560, 1400), 1.0f};
    const std::vector<DisplayArea> displays{new_primary, old_primary};
    const Rect normal = R(300, 200, 1280, 720);
    const PlacementChoice choice = ChoosePlacement(Saved(normal), displays, 1280, 720);
    CHECK(choice.source == PlacementSource::kRestored && choice.display == 1 && choice.rect == normal);
}

void TestResolutionReduced() {
    // Saved on a 2560x1440 desktop, which is now 1920x1080: shrunk to the display and kept below its top edge.
    const std::vector<DisplayArea> displays{kPrimary};
    const PlacementChoice choice = ChoosePlacement(Saved(R(0, 40, 2500, 1360)), displays, 1280, 720);
    CHECK(choice.source == PlacementSource::kClamped && !choice.centered && !choice.stored_invalid);
    CHECK(choice.rect == R(0, kEstimatedTitleBarHeight, 1920, 1040 - kEstimatedTitleBarHeight));
    CHECK(Inside(choice.rect, kPrimary.usable));

    // Larger than the display and partly off it: moved in as well as shrunk.
    const PlacementChoice moved = ChoosePlacement(Saved(R(900, 500, 2000, 1200)), displays, 1280, 720);
    CHECK(moved.source == PlacementSource::kClamped && Inside(moved.rect, kPrimary.usable));
}

void TestUsableSmallerThanLauncher() {
    const DisplayArea tiny{R(0, 0, 800, 560), 1.0f};
    const std::vector<DisplayArea> displays{tiny};
    const PlacementChoice choice = ChoosePlacement(std::nullopt, displays, 1280, 720);
    CHECK(choice.rect.width() == 720 && choice.rect.height() == 504);
    CHECK(Inside(choice.rect, tiny.usable));

    const PlacementChoice fallback = ChoosePlacement(Saved(R(4000, 0, 1280, 720)), displays, 1280, 720);
    CHECK(fallback.source == PlacementSource::kFallback && Inside(fallback.rect, tiny.usable));
}

void TestClampedWherePossible() {
    // The title bar is above the primary's top edge: moved down only as far as needed, the size kept.
    const std::vector<DisplayArea> displays{kPrimary};
    const PlacementChoice choice = ChoosePlacement(Saved(R(100, -10, 1280, 720)), displays, 1280, 720);
    CHECK(choice.source == PlacementSource::kClamped);
    CHECK(choice.rect == R(100, kEstimatedTitleBarHeight, 1280, 720));
}

void TestDpi() {
    // 150%: the default size grows with the display scale and still fits.
    const DisplayArea scaled{R(0, 0, 2560, 1380), 1.5f};
    const std::vector<DisplayArea> displays{scaled};
    const PlacementChoice choice = ChoosePlacement(std::nullopt, displays, 1280, 720);
    CHECK(choice.rect.width() == 1920 && choice.rect.height() == 1080);

    // 200% on a 1080p panel: capped at 90% of the usable area.
    const DisplayArea dense{R(0, 0, 1920, 1040), 2.0f};
    const std::vector<DisplayArea> dense_displays{dense};
    const PlacementChoice capped = ChoosePlacement(std::nullopt, dense_displays, 1280, 720);
    CHECK(capped.rect.width() == 1728 && capped.rect.height() == 936);

    // The title bar reserve follows the target display's scale.
    const PlacementChoice clamped = ChoosePlacement(Saved(R(100, 0, 1280, 720)), dense_displays, 1280, 720);
    CHECK(clamped.source == PlacementSource::kClamped && clamped.rect.top == 2 * kEstimatedTitleBarHeight);

    // A window saved at 200% and restored on a 100% panel keeps its pixel size while it fits.
    const std::vector<DisplayArea> large{{R(0, 0, 3840, 2120), 1.0f}};
    CHECK(ChoosePlacement(Saved(R(200, 200, 2560, 1440)), large, 1280, 720).source == PlacementSource::kRestored);
}

void TestInvalidDisplayIndex() {
    // No display at all: nothing to place on, and nothing breaks.
    const std::vector<DisplayArea> none;
    const PlacementChoice choice = ChoosePlacement(Saved(R(0, 0, 1280, 720), true), none, 1280, 720);
    CHECK(choice.display == 0 && choice.maximized);

    // A display index that no longer exists formats as an empty area instead of reading out of bounds.
    PlacementChoice stale = ChoosePlacement(std::nullopt, std::vector<DisplayArea>{kPrimary}, 1280, 720);
    stale.display = 7;
    const std::string line = PlacementLine(stale, std::vector<DisplayArea>{kPrimary}, "", PlacementResult{});
    CHECK(Contains(line, "display=8/1") && Contains(line, "usable=0,0 0x0"));
}

void TestStatesKept() {
    const std::vector<DisplayArea> displays{kPrimary};
    const PlacementChoice restored = ChoosePlacement(Saved(R(100, 100, 1280, 720), true), displays, 1280, 720);
    CHECK(restored.maximized && !restored.fullscreen);
    const PlacementChoice fallback = ChoosePlacement(Saved(R(9000, 9000, 1280, 720), true, true), displays, 1280, 720);
    CHECK(fallback.source == PlacementSource::kFallback && fallback.maximized && fallback.fullscreen);
    const PlacementChoice tiny = ChoosePlacement(Saved(R(10, 10, 100, 80), false, true), displays, 1280, 720);
    CHECK(tiny.source == PlacementSource::kCentered && tiny.stored_invalid && tiny.fullscreen);
}

void TestBestDisplay() {
    const DisplayArea second{R(1920, 0, 1920, 1040), 1.0f};
    const std::vector<DisplayArea> displays{kPrimary, second};
    CHECK(BestDisplay(R(1700, 100, 1000, 600), displays) == 1);  // 780 px on the second, 220 on the primary
    CHECK(BestDisplay(R(1420, 100, 1000, 600), displays) == 0);  // ties go to the earlier display
    CHECK(BestDisplay(R(9000, 9000, 10, 10), displays) == 0);
    const PlacementChoice spanning = ChoosePlacement(Saved(R(1700, 100, 1000, 600)), displays, 1280, 720);
    CHECK(spanning.source == PlacementSource::kRestored && spanning.display == 1);
}

void TestDeterministic() {
    const std::vector<DisplayArea> displays{kPrimary, {R(-1920, -200, 1920, 1080), 1.25f}};
    const std::optional<WindowPlacement> saved = Saved(R(-1800, -260, 2100, 900));
    const PlacementChoice a = ChoosePlacement(saved, displays, 1280, 720);
    const PlacementChoice b = ChoosePlacement(saved, displays, 1280, 720);
    CHECK(a.rect == b.rect && a.display == b.display && a.source == b.source && a.centered == b.centered);
    CHECK(PlacementLine(a, displays, "X", {}) == PlacementLine(b, displays, "X", {}));
}

void TestPlacementLine() {
    const std::vector<DisplayArea> displays{kPrimary, {R(1920, 0, 2560, 1400), 1.5f}};
    const PlacementChoice choice = ChoosePlacement(Saved(R(2100, 100, 1400, 800)), displays, 1280, 720);
    PlacementResult result;
    result.final_rect = R(2100, 100, 1400, 800);
    result.pixel_width = 1400;
    result.pixel_height = 800;
    result.window_scale = 1.5f;
    const std::string line = PlacementLine(choice, displays, "DELL U2720Q", result);
    CHECK(Contains(line, "Launcher telemetry: window_placement source=restored stored=valid display=2/2 primary=0"));
    CHECK(Contains(line, "name=DELL_U2720Q"));
    CHECK(Contains(line, "usable=1920,0 2560x1400 display_scale=1.50"));
    CHECK(Contains(line, "requested=2100,100 1400x800 final=2100,100 1400x800 pixels=1400x800 window_scale=1.50"));
    CHECK(Contains(line, "maximized=0 fullscreen=0"));
    CHECK(line.find('\n') == std::string::npos);

    const PlacementChoice fallback = ChoosePlacement(Saved(R(9000, 0, 1280, 720)), displays, 1280, 720);
    CHECK(Contains(PlacementLine(fallback, displays, "", {}), "source=fallback stored=invalid display=1/2 primary=1 name=unknown"));
    CHECK(Contains(PlacementLine(ChoosePlacement(std::nullopt, displays, 1280, 720), displays, "A", {}), "source=centered stored=none"));

    // A display name cannot inject text, quotes or a second line, and is bounded.
    const std::string hostile(200, 'A');
    CHECK(DisplayNameForLog(hostile).size() == 32);
    CHECK(DisplayNameForLog("a\"b\\c\nd\te") == "a?b?c?d?e");
    CHECK(DisplayNameForLog("\xC3\xA9") == "??");
    const std::string worst = PlacementLine(fallback, displays, hostile, {R(-2147483000, -2147483000, 2000000000, 2000000000), 99999, 99999, 999.0f});
    CHECK(worst.size() < 512 && worst.find('\n') == std::string::npos);
}

void TestTrackerKeepsStateWhileMinimized() {
    PlacementTracker tracker(Saved(R(100, 100, 1280, 720), true));
    tracker.Observe({R(100, 100, 1280, 720), true, false}, false, 0.0);
    tracker.Observe({R(0, 0, 0, 0), false, true}, false, 1.0);
    CHECK(tracker.Current() && tracker.Current()->maximized);
    CHECK(tracker.Current()->normal == R(100, 100, 1280, 720));
    CHECK(!tracker.TakeSettled(5.0));
}

void TestSourceWiring() {
    const fs::path root = SOTB_SOURCE_DIR;
    const std::string host = ReadAll(root / "launcher/host.cpp");
    const std::string launcher = ReadAll(root / "launcher/launcher.cpp");
    CHECK(!host.empty() && !launcher.empty());

    // Chosen once, from the display list, before the hidden window is created; logged once from Open.
    const size_t open = host.find("bool LauncherHost::Open(");
    const size_t choose = host.find("ChoosePlacement(options.restore, areas, options.width, options.height)", open);
    const size_t create = host.find("SDL_CreateWindowWithProperties(props)", open);
    const size_t log = host.find("LogPlacement(placement, areas, target->name);", open);
    CHECK(open != std::string::npos && choose != std::string::npos && choose < create && create < log);
    CHECK(Count(host, "ChoosePlacement(") == 1 && Count(host, "LogPlacement(placement") == 1);
    CHECK(!Contains(launcher, "ChoosePlacement") && !Contains(launcher, "PlacementLine"));
    CHECK(Contains(host, "SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true"));

    // A centered window gets SDL's centering and leaves nothing restored, so its first position is not saved.
    CHECK(Contains(host, "placement.centered ? int(SDL_WINDOWPOS_CENTERED_DISPLAY(target->id))"));
    CHECK(Contains(host, "restored_ = placement.centered ? std::nullopt"));

    // No per-frame placement or display work.
    const size_t frames = host.find("FrameGate LauncherHost::NextFrame()");
    const std::string per_frame = host.substr(frames, host.find("bool LauncherHost::Fullscreen()") - frames);
    CHECK(frames != std::string::npos && !Contains(per_frame, "ListDisplays") && !Contains(per_frame, "SDL_GetDisplay") &&
          !Contains(per_frame, "Placement"));
    const size_t loop = launcher.find("host_.NextFrame()");
    const std::string loop_body = launcher.substr(loop, launcher.find("launcher loop ended") - loop);
    CHECK(loop != std::string::npos && !Contains(loop_body, "ListDisplays") && !Contains(loop_body, "ReadPlacement"));

    // Fullscreen applies before the next frame records the window's state.
    CHECK(host.find("SDL_SyncWindow(window_);", host.find("void LauncherHost::SetFullscreen")) != std::string::npos);
}

} // namespace

int main() {
    TestDefaultCentered();
    TestValidRestored();
    TestOffScreenRejected();
    TestRemovedDisplay();
    TestNegativeAndLeftMonitor();
    TestMonitorAbove();
    TestPrimaryChanged();
    TestResolutionReduced();
    TestUsableSmallerThanLauncher();
    TestClampedWherePossible();
    TestDpi();
    TestInvalidDisplayIndex();
    TestStatesKept();
    TestBestDisplay();
    TestDeterministic();
    TestPlacementLine();
    TestTrackerKeepsStateWhileMinimized();
    TestSourceWiring();
    std::printf("launcher placement tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

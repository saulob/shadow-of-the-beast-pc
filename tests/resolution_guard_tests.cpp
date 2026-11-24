#include "guest_display.h"
#include "resolution_guard.h"
#include "resolution_recovery.h"
#include "startup_phase.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* expr, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

Sotb::SessionOutcome Outcome(Sotb::SessionEnd end, const char* stage, const char* detail = "") {
    Sotb::SessionOutcome outcome;
    outcome.end = end;
    outcome.stage = stage;
    outcome.detail = detail;
    return outcome;
}

void TestDecision() {
    using Sotb::SessionEnd;
    CHECK(Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kCrash, "first frame presented")));
    CHECK(Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kCrash, "patches installed")));
    CHECK(Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kStartupFailure, "guest entry")));
    CHECK(Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kDeviceLost, "first frame presented")));
    CHECK(Sotb::ShouldRestoreSafeResolution(
        Outcome(SessionEnd::kStartupFailure, "imports resolved", "render resolution is not available: unexpected display size")));

    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kClean, "first frame presented")));
    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kCrash, "gameplay started")));
    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kCrash, "first frame presented"), true));
    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kDeviceLost, "gameplay started")));
    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kCrash, "process exit")));
    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kStartupFailure, "GPU initialization")));
    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kStartupFailure, "imports resolved", "GPU initialization failed")));
    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kStopped, "first frame presented")));
    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kUnsupportedBuild, "first frame presented")));
    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kUnknown, "first frame presented")));
    CHECK(!Sotb::ShouldRestoreSafeResolution(Outcome(SessionEnd::kCrash, "")));

    CHECK(Sotb::IsResolutionStartupStage("patches installed"));
    CHECK(Sotb::IsResolutionStartupStage("first frame presented"));
    CHECK(!Sotb::IsResolutionStartupStage("gameplay started"));
    CHECK(!Sotb::IsResolutionStartupStage(""));
}

void TestResolutionChoices() {
    CHECK(std::strcmp(SOTB_RESOLUTION_GUARD_SAFE_KEY, "1440p") == 0);
    CHECK(sotb_resolution_index("1440p") == 3);
    CHECK(sotb_resolution_index("1800p") == 4);
    CHECK(sotb_resolution_index("4K") == 5);
    CHECK(std::strcmp(sotb_resolutions[5].key, "4K") == 0);
    CHECK(sotb_resolutions[5].width == 3840 && sotb_resolutions[5].height == 2160);
    CHECK(sotb_resolutions[3].width == 2560 && sotb_resolutions[3].height == 1440);
}

void TestMarker() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "sotb-resolution-guard-tests";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir.string();

    char key[64] = {};
    CHECK(!sotb_resolution_guard_pending(path.c_str(), key, sizeof(key)));

    // The safe and lower resolutions are not guarded.
    CHECK(!sotb_resolution_guard_arm(path.c_str(), "1440p"));
    CHECK(!sotb_resolution_guard_pending(path.c_str(), key, sizeof(key)));
    CHECK(!sotb_resolution_guard_arm(path.c_str(), "720p"));
    CHECK(!sotb_resolution_guard_pending(path.c_str(), key, sizeof(key)));

    CHECK(sotb_resolution_guard_arm(path.c_str(), "4K"));
    CHECK(sotb_resolution_guard_pending(path.c_str(), key, sizeof(key)));
    CHECK(std::strcmp(key, "4K") == 0);

    // Arming a safe resolution removes an earlier pending attempt.
    CHECK(!sotb_resolution_guard_arm(path.c_str(), "native"));
    CHECK(!sotb_resolution_guard_pending(path.c_str(), key, sizeof(key)));

    CHECK(sotb_resolution_guard_arm(path.c_str(), "1800p"));
    CHECK(sotb_resolution_guard_pending(path.c_str(), key, sizeof(key)));
    CHECK(std::strcmp(key, "1800p") == 0);
    sotb_resolution_guard_clear(path.c_str());
    CHECK(!sotb_resolution_guard_pending(path.c_str(), key, sizeof(key)));

    // An unknown key is never a guarded attempt.
    CHECK(!sotb_resolution_guard_arm(path.c_str(), "not-a-resolution"));
    CHECK(!sotb_resolution_guard_pending(path.c_str(), key, sizeof(key)));

    // The game-side milestone clears the armed attempt and publishes the stable stage.
    CHECK(sotb_resolution_guard_arm(path.c_str(), "4K"));
    sotb_resolution_guard_init(path.c_str());
    sotb_resolution_guard_loading_screen_finished();
    CHECK(!sotb_resolution_guard_pending(path.c_str(), key, sizeof(key)));
    CHECK(std::strcmp(sotb_startup_phase_current(), "gameplay started") == 0);
    sotb_resolution_guard_loading_screen_finished();
    CHECK(std::strcmp(sotb_startup_phase_current(), "gameplay started") == 0);

    std::filesystem::remove_all(dir, ec);
}

}  // namespace

int main() {
    TestDecision();
    TestResolutionChoices();
    TestMarker();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("resolution_guard_tests: OK");
    return 0;
}

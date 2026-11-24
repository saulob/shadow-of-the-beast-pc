#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "incident_report.h"
#include "resolution_guard.h"

namespace Sotb {

class LauncherSettings;

struct ResolutionRecovery {
    bool recovered = false;
    bool attempted = false;
    std::string requested;
    std::string reason;
    std::string stage;
};

// The startup stages in which a render-resolution failure is plausible: the display size and the
// loading-screen layout are applied in "patches installed", and the guest does not reach gameplay
// before "first frame presented".
inline bool IsResolutionStartupStage(std::string_view stage) {
    return stage == "patches installed" || stage == "guest image mapped" ||
           stage == "linked modules initialization" || stage == "guest entry" ||
           stage == "first frame presented";
}

inline bool ShouldRestoreSafeResolution(const SessionOutcome& outcome, bool gameplay_started = false) {
    if (gameplay_started) {
        return false;
    }
    if (outcome.end == SessionEnd::kStartupFailure && outcome.detail.find("render resolution") != std::string::npos) {
        return true;
    }
    if (outcome.end != SessionEnd::kCrash && outcome.end != SessionEnd::kStartupFailure &&
        outcome.end != SessionEnd::kDeviceLost) {
        return false;
    }
    return IsResolutionStartupStage(outcome.stage);
}

// Restores the known-good resolution when the last pending high-resolution attempt died during
// startup. A clean run, a gameplay crash and a failure before the display patch leave the setting alone.
ResolutionRecovery RecoverFailedRenderResolution(LauncherSettings& settings, const std::filesystem::path& user_dir,
                                                const std::filesystem::path& logs_dir,
                                                const std::filesystem::path& running_log);

}  // namespace Sotb

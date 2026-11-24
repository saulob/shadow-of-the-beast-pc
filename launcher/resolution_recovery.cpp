#include "resolution_recovery.h"

#include <fstream>
#include <iterator>

#include "settings.h"
#include "utf8.h"

namespace Sotb {
namespace {

// The game allocates the level's blood map once the loading screen is past; a crash after this
// line is not a startup failure even when the last startup stage is still "first frame presented".
bool LogHasGameplayMarker(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    constexpr std::uintmax_t kTail = 2 * 1024 * 1024;
    if (!ec && size > kTail) in.seekg(std::streamoff(size - kTail));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.find("BLOODMAP:") != std::string::npos;
}

}  // namespace

ResolutionRecovery RecoverFailedRenderResolution(LauncherSettings& settings, const std::filesystem::path& user_dir,
                                                const std::filesystem::path& logs_dir,
                                                const std::filesystem::path& running_log) {
    ResolutionRecovery result;
    char key[64];
    const std::string user = Utf8(user_dir);
    if (!sotb_resolution_guard_pending(user.c_str(), key, sizeof(key))) {
        sotb_resolution_guard_clear(user.c_str());
        return result;
    }
    result.attempted = true;
    result.requested = key;

    const std::vector<GameLog> logs = FindGameLogs(logs_dir, running_log, 1);
    const GameLog* last = nullptr;
    for (const GameLog& log : logs) {
        if (log.activity == LogActivity::kGameplay && log.role != LogRole::kCurrentGameplay) {
            last = &log;
            break;
        }
    }
    if (!last) {
        result.reason = "no game log";
    } else {
        result.stage = last->outcome.stage;
        result.reason = SessionEndKey(last->outcome.end);
        if (ShouldRestoreSafeResolution(last->outcome, LogHasGameplayMarker(last->path))) {
            settings.Set(kRenderResolutionKey, SOTB_RESOLUTION_GUARD_SAFE_KEY);
            result.recovered = settings.Save();
            if (!result.recovered) result.reason = "settings not saved";
        }
    }
    sotb_resolution_guard_clear(user.c_str());
    return result;
}

}  // namespace Sotb

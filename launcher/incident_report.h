// The Bug Report page's work without a window: the logs it offers and the report it builds. The preview, the saved copy and
// the request to the service all read one IncidentPackage, so what the player can read is what is sent.
#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "log_scrubber.h"

namespace Sotb {
namespace json {
class Value;
}

inline constexpr size_t kMostGameLogs = 5;
inline constexpr size_t kFewestDescriptionCharacters = 10;
inline constexpr size_t kMostDescriptionCharacters = 500;
inline constexpr size_t kMostLogBytes = 64 * 1024;
// A dump holds thread stacks and module lists, no heap (src/crash_dump.c); server/shadow-bug-reports refuses more.
inline constexpr uintmax_t kMostDumpBytes = 8 * 1024 * 1024;
// A minidump header and its stream directory alone are smaller; any dump the runtime writes is far larger.
inline constexpr uintmax_t kFewestDumpBytes = 1024;
// The runtime writes the dump, then names it in its log: both files end within this of each other.
inline constexpr int kDumpPairingSeconds = 120;
inline constexpr size_t kMostSessionDetailCharacters = 400;

// How far a session got: kGameStarted left the launcher for the game, kGameplay reached the game's own code.
enum class LogActivity { kLauncher, kGameStarted, kGameplay };

// kNone is a session that started the game without reaching its own code.
enum class LogRole { kNone, kCurrentLauncher, kCurrentGameplay, kLastGameplay, kOtherGameplay, kOtherLauncher };

// How a finished session ended, from the lines its own exit path wrote. kUnknown: no such line (still running, killed,
// power lost).
enum class SessionEnd { kUnknown, kClean, kCrash, kDeviceLost, kStopped, kStartupFailure, kUnsupportedBuild };

struct SessionOutcome {
    SessionEnd end = SessionEnd::kUnknown;
    std::string detail; // the line that names the failure, as written
    std::string stage;  // the last startup stage the log names
};

// kNone: the dump can be sent. kMissing and kNotThisLog: no dump of this session exists.
enum class DumpProblem { kNone, kMissing, kNotThisLog, kNotAFile, kEmpty, kTooSmall, kTooLarge, kNotADump, kUnreadable, kChanged };

struct CrashDump {
    std::filesystem::path path;
    std::string name;
    uintmax_t bytes = 0;
    std::string sha256; // set once the dump is chosen for a report
};

struct GameLog {
    std::filesystem::path path;
    std::string name;
    std::time_t written = 0;
    uintmax_t bytes = 0;
    LogActivity activity = LogActivity::kLauncher;
    LogRole role = LogRole::kNone;
    SessionOutcome outcome;
    std::optional<CrashDump> dump; // this session's own dump, sendable or not
    DumpProblem dump_problem = DumpProblem::kMissing;

    bool Crashed() const { return outcome.end == SessionEnd::kCrash || dump.has_value(); }
    bool CanAttachDump() const { return dump.has_value() && dump_problem == DumpProblem::kNone; }
};

// The logs in `folder`: `running_log` (this launcher's own session) first, then newest first. Left out: other files and
// sessions that closed in the launcher without starting the game. Finished sessions carry their outcome and dump.
std::vector<GameLog> FindGameLogs(const std::filesystem::path& folder, const std::filesystem::path& running_log,
                                  size_t most = kMostGameLogs);

// The outcome of a session from the start and the end of its log (the whole log when it is short).
SessionOutcome ReadSessionOutcome(std::string_view log_head, std::string_view log_tail);

// "crash", "device_lost", ...: the value the service stores.
const char* SessionEndKey(SessionEnd end);
// English, for Tr; nullptr when the session ended normally or its end is unknown.
const char* SessionEndText(const GameLog& log);

// "shadow_of_the_beast_012.dmp" for "shadow_of_the_beast_012.log"; empty for any other name.
std::string DumpNameOf(std::string_view log_name);

// Whether `dump` is this session's own dump and can be sent: exactly the log's name with .dmp, in the same folder, a
// regular file that the log names as written, of a minidump's size and signature, written within kDumpPairingSeconds of the
// log's last line. A dump of another run, a link and a directory are never sent.
DumpProblem CheckCrashDump(const GameLog& log, const std::filesystem::path& dump);

// English, for Tr: why the dump of a crashed session cannot be sent; nullptr for kNone.
const char* DumpProblemText(DumpProblem problem);

// The default of the "Attach crash dump" box for a selected log.
bool DefaultAttachCrashDump(const GameLog& log);

// The log a report is most likely about: the newest that started the game, not this launcher's own session.
size_t DefaultGameLog(const std::vector<GameLog>& logs);

// English, for Tr; nullptr for kNone.
const char* LogRoleText(LogRole role);
const char* PreviewButtonText(bool preview_open);

// "Nov 5, 2026 03:51"; local time.
std::string LogDateText(std::time_t written);
// "184 KB", "1.2 MB".
std::string LogSizeText(uintmax_t bytes);

// The bytes of `text` that hold its first `most` UTF-8 characters.
size_t BytesOfCharacters(std::string_view text, size_t most);
size_t CountCharacters(std::string_view text);

enum class DraftProblem { kNone, kDescriptionTooShort, kNoLog };
// Spaces around the description do not count toward its length.
DraftProblem CheckDraft(std::string_view description, bool has_log);

// The fields of the installed game and this PC that go with every report. The renderer, graphics card, driver and
// render resolution are read from the start of the selected log.
struct ReportFacts {
    std::string launcher_version;
    std::string game_build;
    std::string game_version;
    std::string region;
    std::string cusa;
    std::string eboot_sha256;
    std::string operating_system;
};

// "Windows 11 (build 26200)", "Steam Deck: SteamOS 3.6".
std::string OperatingSystemText();

struct LogDiagnostics {
    std::string cpu;
    std::string renderer_selected;
    std::string renderer;
    std::string gpu;
    std::string driver;
    std::string render_resolution;
};

// What the start of a game log says about the CPU, renderer and graphics card; empty fields for what it does not say.
LogDiagnostics ReadLogDiagnostics(std::string_view log_start);

// `text` without control characters or surrounding spaces, at most `most` characters.
std::string Clamped(std::string_view text, size_t most);

struct IncidentField {
    std::string_view key;
    std::string_view label; // English; the preview translates it
    std::string value;
};

struct IncidentPackage {
    std::vector<IncidentField> fields;
    // Announced in the request and sent as a file after it, when the player asked for it and it passed the check.
    std::optional<CrashDump> crash_dump;
    bool crash_dump_requested = false;
    DumpProblem crash_dump_problem = DumpProblem::kMissing; // why a requested dump is not attached
    const std::string& Value(std::string_view key) const;
};

// The end of `text`, at most `most` bytes, starting on a whole line.
std::string LastBytes(std::string_view text, size_t most);

// Scrubs the log's text and bounds it. nullopt when the log cannot be read or holds nothing. With `attach_crash_dump` the
// session's own dump goes along when it passes CheckCrashDump; only its name, size and SHA-256 are kept, never its bytes.
std::optional<IncidentPackage> BuildIncidentPackage(std::string_view description, const GameLog& log, const ReportFacts& facts,
                                                    const PrivateFacts& private_facts, bool attach_crash_dump = false);

std::string IncidentPreviewText(const IncidentPackage& package);

// The request body for the report service: every field of the package under its key, in the package's order, then the
// announced dump and, when given, the submission key that makes a repeated request return the same report.
std::string IncidentRequestJson(const IncidentPackage& package, std::string_view submission = {});

// The stored report the service returns to its owner, as the same fields a preview shows.
IncidentPackage SentReportOf(const json::Value& report);

// SOB-XXXXXX and 64 lowercase hex digits, as the service issues them.
bool IsTicket(std::string_view text);
bool IsReceipt(std::string_view text);

// Writes the report as text into `folder`, named after the time; the file's path, empty when it could not be written.
std::filesystem::path SaveIncidentReport(const IncidentPackage& package, const std::filesystem::path& folder);

} // namespace Sotb

#include "incident_report.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include "crypto.h"
#include "json_lite.h"
#include "localization/ui_text.h"
#include "utf8.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Sotb {
namespace fs = std::filesystem;
namespace {

constexpr std::string_view kLogPrefix = "shadow_of_the_beast_";
constexpr std::string_view kLogSuffix = ".log";
constexpr std::string_view kDumpSuffix = ".dmp";
constexpr char kMinidumpSignature[] = {'M', 'D', 'M', 'P'};
constexpr size_t kDumpNoteBytes = 64 * 1024;
constexpr size_t kFewestLogDigits = 3, kMostLogDigits = 10;
constexpr size_t kLogsLookedAt = 200;
constexpr size_t kMostMetadataCharacters = 64, kMostDetailCharacters = 96;
constexpr std::string_view kTicketPrefix = "SOB-";
constexpr std::string_view kTicketAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr size_t kTicketLength = 10, kSha256Length = 64;
constexpr size_t kSessionHeadBytes = 128 * 1024;
// More than the report keeps, so that scrubbing cannot leave it short.
constexpr size_t kLogTailBytes = 256 * 1024;

// launcher/launcher.cpp, src/loader.c and src/runtime_thread.c write these.
constexpr std::string_view kGameStartMarks[] = {"Launcher: closed, starting the game", "Build fingerprint: "};
constexpr std::string_view kLauncherClosedMark = "Launcher: closed";
constexpr std::string_view kGuestCodeMark = "Runtime: guest thread '";

// How each exit path ends its log: src/loader.c, src/runtime*.c, gpu/shim/sotbgpu.cpp and the device-lost owner in
// gpu/shadps4/video_core/renderer_vulkan/vk_device_lost.h.
constexpr std::string_view kDeviceLostFirstMark = "GPU device lost: first detected in ";
constexpr std::string_view kDeviceLostReasonMark = "GPU device lost: vk";
constexpr std::string_view kFatalMark = "FATAL exception 0x";
constexpr std::string_view kCrashMarks[] = {"Fault: ", "STOP: abort() was called", "STOP: pure virtual call", kFatalMark};
constexpr std::string_view kStopMarks[] = {"STOP: ", "Controlled stop: "};
constexpr std::string_view kErrorMark = "ERROR: ";
constexpr std::string_view kUnsupportedBuildMark = "ERROR: unsupported build: ";
constexpr std::string_view kCleanMarks[] = {"Runtime: window closed by user", "Runtime: guest requested exit(0)"};
constexpr std::string_view kStageMarks[] = {"Stage: ", "Crash: startup phase: "};
constexpr std::string_view kDumpWrittenMark = "Crash: minidump ";

constexpr char kDescriptionKey[] = "description";
constexpr char kLogTextKey[] = "log_text";

struct FieldName {
    std::string_view key;
    std::string_view label;
};
constexpr FieldName kFieldNames[] = {
    {kDescriptionKey, "Description"},
    {"launcher_version", "Launcher version"},
    {"game_build", "Game build"},
    {"game_version", "Game version"},
    {"region", "Region"},
    {"cusa", "CUSA"},
    {"eboot_sha256", "Executable SHA-256"},
    {"operating_system", "Operating system"},
    {"cpu", "CPU"},
    {"renderer_selected", "Renderer selected"},
    {"renderer", "Renderer active"},
    {"gpu", "Graphics card"},
    {"driver", "Driver"},
    {"render_resolution", "Render resolution"},
    {"session_end", "Session end"},
    {"session_detail", "Failure"},
    {"session_stage", "Last stage"},
    {"log_file", "Selected log"},
    {"log_clipped", "Log shortened to its end"},
    {kLogTextKey, "Log after privacy filtering"},
};

constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool IsGameLogName(std::string_view name) {
    if (!name.starts_with(kLogPrefix) || !name.ends_with(kLogSuffix)) return false;
    name.remove_prefix(kLogPrefix.size());
    name.remove_suffix(kLogSuffix.size());
    return name.size() >= kFewestLogDigits && name.size() <= kMostLogDigits &&
           std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string ReadPart(const fs::path& file, uintmax_t offset, size_t size) {
    std::ifstream in(file, std::ios::binary);
    std::string text(size, '\0');
    in.seekg(std::streamoff(offset));
    in.read(text.data(), std::streamsize(size));
    text.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
    return text;
}

std::string ReadTail(const fs::path& file, size_t most) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) return {};
    const uintmax_t offset = size > most ? size - most : 0;
    return ReadPart(file, offset, size_t(size - offset));
}

template <typename Visit>
void ForEachLine(std::string_view text, Visit&& visit) {
    for (size_t at = 0; at < text.size();) {
        const size_t end = std::min(text.find('\n', at), text.size());
        std::string_view line = text.substr(at, end - at);
        if (line.ends_with('\r')) line.remove_suffix(1);
        visit(at, line);
        at = end + 1;
    }
}

template <size_t N>
bool StartsWithAny(std::string_view line, const std::string_view (&marks)[N]) {
    return std::any_of(std::begin(marks), std::end(marks), [&](std::string_view mark) { return line.starts_with(mark); });
}

struct FirstLine {
    size_t at = std::string_view::npos;
    std::string_view line;
    void Note(size_t where, std::string_view text) {
        if (at == std::string_view::npos) {
            at = where;
            line = text;
        }
    }
};

std::string_view LastStage(std::string_view text) {
    std::string_view stage;
    ForEachLine(text, [&](size_t, std::string_view line) {
        for (const std::string_view mark : kStageMarks) {
            if (line.starts_with(mark)) stage = line.substr(mark.size());
        }
    });
    return stage;
}

// The runtime names the dump in its own log once the dump is complete (src/crash_dump.c), on a line of its own.
bool LogNamesDump(const fs::path& log, std::string_view name) {
    const std::string tail = ReadTail(log, kDumpNoteBytes);
    const std::string line = std::string(kDumpWrittenMark) + std::string(name) + " written";
    for (size_t at = tail.find(line); at != std::string::npos; at = tail.find(line, at + 1)) {
        const size_t end = at + line.size();
        if ((at == 0 || tail[at - 1] == '\n') && (end == tail.size() || tail[end] == '\n' || tail[end] == '\r')) return true;
    }
    return false;
}

DumpProblem ReadMinidumpSignature(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return DumpProblem::kUnreadable;
    char head[sizeof(kMinidumpSignature)] = {};
    in.read(head, sizeof(head));
    const bool whole = size_t(in.gcount()) == sizeof(head);
    return whole && std::equal(std::begin(head), std::end(head), std::begin(kMinidumpSignature)) ? DumpProblem::kNone
                                                                                                   : DumpProblem::kNotADump;
}

std::string HexOf(const uint8_t (&digest)[32]) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    for (const uint8_t byte : digest) {
        hex += kDigits[byte >> 4];
        hex += kDigits[byte & 15];
    }
    return hex;
}

std::optional<CrashDump> HashedDump(const fs::path& path, uintmax_t expected_bytes, DumpProblem& problem) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        problem = DumpProblem::kUnreadable;
        return std::nullopt;
    }
    crypto::Sha256Stream hash;
    std::vector<char> buffer(64 * 1024);
    uintmax_t total = 0;
    while (in && total <= expected_bytes) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        const size_t got = size_t(std::max<std::streamsize>(in.gcount(), 0));
        if (got && !hash.Update(buffer.data(), got)) break;
        total += got;
    }
    uint8_t digest[32];
    std::error_code ec;
    if (total != expected_bytes || fs::file_size(path, ec) != expected_bytes || ec) {
        problem = DumpProblem::kChanged;
        return std::nullopt;
    }
    if (!in.eof() || !hash.Finish(digest)) {
        problem = DumpProblem::kUnreadable;
        return std::nullopt;
    }
    problem = DumpProblem::kNone;
    return CrashDump{path, Utf8(path.filename()), total, HexOf(digest)};
}

void NoteDump(GameLog& log) {
    const std::string name = DumpNameOf(log.name);
    if (name.empty()) return;
    const fs::path path = log.path.parent_path() / name;
    log.dump_problem = CheckCrashDump(log, path);
    if (log.dump_problem == DumpProblem::kMissing || log.dump_problem == DumpProblem::kNotThisLog ||
        log.dump_problem == DumpProblem::kNotAFile) {
        return;
    }
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    log.dump = CrashDump{path, name, ec ? 0 : size, {}};
}

struct Session {
    LogActivity activity = LogActivity::kLauncher;
    bool closed_in_launcher = false;
};

Session ReadSession(const fs::path& file) {
    const std::string head = ReadPart(file, 0, kSessionHeadBytes);
    const auto holds = [&](std::string_view mark) { return head.find(mark) != std::string::npos; };
    if (holds(kGuestCodeMark)) return {LogActivity::kGameplay, false};
    if (std::any_of(std::begin(kGameStartMarks), std::end(kGameStartMarks), holds)) return {LogActivity::kGameStarted, false};
    return {LogActivity::kLauncher, holds(kLauncherClosedMark)};
}

std::time_t SystemTimeOf(fs::file_time_type written) {
    const auto system = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(written - fs::file_time_type::clock::now());
    return std::chrono::system_clock::to_time_t(system);
}

struct Candidate {
    fs::path path;
    std::string name;
    fs::file_time_type written;
    uintmax_t bytes = 0;
    bool running = false;
};

bool IsRunningLog(const fs::path& file, const fs::path& running_log) {
    std::error_code ec;
    return !running_log.empty() && (file == running_log || fs::equivalent(file, running_log, ec));
}

// A directory listing lags behind a file that is still open, so the running log is asked for its own size and time.
std::vector<Candidate> LogCandidates(const fs::path& folder, const fs::path& running_log) {
    std::vector<Candidate> found;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry;
        const std::string name = Utf8(it->path().filename());
        if (!IsGameLogName(name) || !it->is_regular_file(entry) || it->is_symlink(entry)) continue;
        const bool running = IsRunningLog(it->path(), running_log);
        const uintmax_t bytes = running ? fs::file_size(it->path(), entry) : it->file_size(entry);
        const fs::file_time_type written = running ? fs::last_write_time(it->path(), entry) : it->last_write_time(entry);
        if (entry || (bytes == 0 && !running)) continue;
        found.push_back({it->path(), name, written, bytes, running});
    }
    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        if (a.running != b.running) return a.running;
        return a.written != b.written ? a.written > b.written : a.name > b.name;
    });
    return found;
}

void MarkLastGameplay(std::vector<GameLog>& logs) {
    const auto played = [](const GameLog& log) { return log.activity == LogActivity::kGameplay; };
    const auto last = std::find_if(logs.begin(), logs.end(), played);
    if (last != logs.end() && last->role == LogRole::kNone) last->role = LogRole::kLastGameplay;
    for (GameLog& log : logs) {
        if (log.role != LogRole::kNone) continue;
        if (log.activity == LogActivity::kGameplay) log.role = LogRole::kOtherGameplay;
        else if (log.activity == LogActivity::kLauncher) log.role = LogRole::kOtherLauncher;
    }
}

std::string TrimmedDescription(std::string_view text) {
    constexpr std::string_view kSpace = " \t\r\n";
    const size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(kSpace) - first + 1));
}

#ifdef _WIN32
std::string WindowsVersionText() {
    // GetVersionEx answers with the version the manifest declares, not the running one.
    using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto get_version = ntdll ? reinterpret_cast<GetVersion>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!get_version || get_version(&version) != 0) return "Windows";
    constexpr DWORD kFirstWindows11Build = 22000;
    const char* name = version.dwMajorVersion == 10 ? (version.dwBuildNumber >= kFirstWindows11Build ? "Windows 11" : "Windows 10")
                                                    : "Windows";
    return std::string(name) + " (build " + std::to_string(version.dwBuildNumber) + ")";
}
#else
std::string LinuxDistributionText() {
    std::ifstream in("/etc/os-release");
    constexpr std::string_view kKey = "PRETTY_NAME=";
    for (std::string line; std::getline(in, line);) {
        if (!line.starts_with(kKey)) continue;
        std::string value = line.substr(kKey.size());
        std::erase(value, '"');
        return value;
    }
    return {};
}
#endif

bool AllOf(std::string_view text, bool (*accepts)(char)) {
    return std::all_of(text.begin(), text.end(), [&](char c) { return accepts(c); });
}

bool IsDigitOrDot(char c) { return (c >= '0' && c <= '9') || c == '.'; }
bool IsDigitChar(char c) { return c >= '0' && c <= '9'; }
bool IsLowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

bool IsGameVersion(std::string_view text) {
    return !text.empty() && text.size() <= 16 && text.front() >= '0' && text.front() <= '9' && AllOf(text, IsDigitOrDot);
}

bool IsTitleId(std::string_view text) { return text.size() == 9 && text.starts_with("CUSA") && AllOf(text.substr(4), IsDigitChar); }

bool IsSha256(std::string_view text) { return text.size() == kSha256Length && AllOf(text, IsLowerHex); }

// The rest of the first line that starts with `label`, or nothing.
std::string_view AfterLabel(std::string_view text, std::string_view label) {
    for (size_t at = text.find(label); at != std::string_view::npos; at = text.find(label, at + 1)) {
        if (at != 0 && text[at - 1] != '\n') continue;
        const size_t start = at + label.size();
        return text.substr(start, text.find('\n', start) - start);
    }
    return {};
}

// What follows `from` in `text`, up to `to` (the rest when `to` is empty); nothing when `from` is absent.
std::string_view Between(std::string_view text, std::string_view from, std::string_view to) {
    const size_t start = text.find(from);
    if (start == std::string_view::npos) return {};
    text.remove_prefix(start + from.size());
    const size_t end = to.empty() ? std::string_view::npos : text.find(to);
    return text.substr(0, end);
}
} // namespace

LogDiagnostics ReadLogDiagnostics(std::string_view log_start) {
    LogDiagnostics found;
    found.cpu = std::string(AfterLabel(log_start, "CPU: "));
    const std::string_view renderer = AfterLabel(log_start, "Renderer: selected ");
    found.renderer_selected = std::string(Between(renderer, "", ", active "));
    found.renderer = std::string(Between(renderer, ", active ", ""));
    if (const std::string_view vulkan = AfterLabel(log_start, "GPU: Vulkan device "); !vulkan.empty()) {
        const size_t open = vulkan.rfind(" (");
        found.gpu = std::string(vulkan.substr(0, open));
        if (open != std::string_view::npos && vulkan.back() == ')') found.driver = std::string(vulkan.substr(open + 2, vulkan.size() - open - 3));
    } else {
        found.gpu = std::string(Between(AfterLabel(log_start, "Settings: Renderer "), ", graphics card ", ", VSync"));
    }
    found.render_resolution = std::string(AfterLabel(log_start, "Settings: Render resolution "));
    return found;
}

std::string Clamped(std::string_view text, size_t most) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) out += (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) ? ' ' : c;
    out = TrimmedDescription(out);
    out.resize(BytesOfCharacters(out, most));
    return TrimmedDescription(out);
}

std::vector<GameLog> FindGameLogs(const fs::path& folder, const fs::path& running_log, size_t most) {
    std::vector<GameLog> logs;
    std::optional<GameLog> last_gameplay;
    std::vector<Candidate> candidates = LogCandidates(folder, running_log);
    if (candidates.size() > kLogsLookedAt) candidates.resize(kLogsLookedAt);
    for (const Candidate& candidate : candidates) {
        if (logs.size() >= most && last_gameplay) break;
        const Session session = ReadSession(candidate.path);
        // A session that ended without closing the launcher stays: that is what a launcher crash leaves.
        if (session.closed_in_launcher && !candidate.running) continue;
        LogRole role = LogRole::kNone;
        if (candidate.running) {
            role = session.activity == LogActivity::kGameplay ? LogRole::kCurrentGameplay : LogRole::kCurrentLauncher;
        }
        GameLog log{candidate.path, candidate.name, SystemTimeOf(candidate.written), candidate.bytes, session.activity, role, {}, std::nullopt,
                    DumpProblem::kMissing};
        if (!candidate.running && session.activity == LogActivity::kGameplay && !last_gameplay) last_gameplay = log;
        if (logs.size() < most) logs.push_back(std::move(log));
    }
    const auto has_gameplay = [](const GameLog& log) { return log.activity == LogActivity::kGameplay && log.role != LogRole::kCurrentGameplay; };
    if (last_gameplay && !logs.empty() && !std::any_of(logs.begin(), logs.end(), has_gameplay)) {
        // The newest gameplay stays in the list even when newer launcher sessions fill it.
        logs.back() = std::move(*last_gameplay);
    }
    MarkLastGameplay(logs);
    for (GameLog& log : logs) {
        if (log.role == LogRole::kCurrentLauncher || log.role == LogRole::kCurrentGameplay) continue;
        log.outcome = ReadSessionOutcome(ReadPart(log.path, 0, kSessionHeadBytes), ReadTail(log.path, kLogTailBytes));
        NoteDump(log);
    }
    return logs;
}

SessionOutcome ReadSessionOutcome(std::string_view log_head, std::string_view log_tail) {
    FirstLine device_lost, crash, stop, error, clean;
    std::string_view reason, fatal;
    ForEachLine(log_tail, [&](size_t at, std::string_view line) {
        if (const size_t mark = line.find(kDeviceLostFirstMark); mark != std::string_view::npos) device_lost.Note(at, line.substr(mark));
        if (const size_t mark = line.find(kDeviceLostReasonMark); mark != std::string_view::npos && reason.empty()) reason = line.substr(mark);
        if (line.starts_with(kFatalMark) && fatal.empty()) fatal = line;
        if (StartsWithAny(line, kCrashMarks)) crash.Note(at, line);
        else if (StartsWithAny(line, kStopMarks)) stop.Note(at, line);
        if (line.starts_with(kErrorMark)) error.Note(at, line);
        if (StartsWithAny(line, kCleanMarks)) clean.Note(at, line);
    });
    SessionOutcome outcome;
    const std::string_view stage = LastStage(log_tail);
    outcome.stage = std::string(stage.empty() ? LastStage(log_head) : stage);

    // The first failure owns the session: a later stop or assertion is its consequence.
    const FirstLine* first = nullptr;
    for (const FirstLine* found : {&device_lost, &crash, &stop, &error}) {
        if (found->at != std::string_view::npos && (!first || found->at < first->at)) first = found;
    }
    if (!first) {
        outcome.end = clean.at != std::string_view::npos ? SessionEnd::kClean : SessionEnd::kUnknown;
    } else if (first == &device_lost) {
        outcome.end = SessionEnd::kDeviceLost;
        outcome.detail = std::string(reason.empty() ? device_lost.line : reason);
    } else if (first == &crash) {
        outcome.end = SessionEnd::kCrash;
        outcome.detail = std::string(fatal.empty() ? crash.line : fatal);
    } else if (first == &stop) {
        outcome.end = SessionEnd::kStopped;
        outcome.detail = std::string(stop.line);
    } else {
        outcome.end = error.line.starts_with(kUnsupportedBuildMark) ? SessionEnd::kUnsupportedBuild : SessionEnd::kStartupFailure;
        outcome.detail = std::string(error.line);
    }
    return outcome;
}

const char* SessionEndKey(SessionEnd end) {
    switch (end) {
    case SessionEnd::kClean: return "clean";
    case SessionEnd::kCrash: return "crash";
    case SessionEnd::kDeviceLost: return "device_lost";
    case SessionEnd::kStopped: return "stopped";
    case SessionEnd::kStartupFailure: return "startup_failure";
    case SessionEnd::kUnsupportedBuild: return "unsupported_build";
    case SessionEnd::kUnknown: break;
    }
    return "unknown";
}

const char* SessionEndText(const GameLog& log) {
    if (log.outcome.end == SessionEnd::kDeviceLost) return "GPU device lost";
    if (log.Crashed()) return "Crash detected";
    switch (log.outcome.end) {
    case SessionEnd::kStopped: return "Stopped by an error";
    case SessionEnd::kStartupFailure: return "Could not start";
    case SessionEnd::kUnsupportedBuild: return "Unsupported game build";
    default: break;
    }
    return nullptr;
}

std::string DumpNameOf(std::string_view log_name) {
    if (!IsGameLogName(log_name)) return {};
    return std::string(log_name.substr(0, log_name.size() - kLogSuffix.size())) + std::string(kDumpSuffix);
}

DumpProblem CheckCrashDump(const GameLog& log, const fs::path& dump) {
    const std::string expected = DumpNameOf(log.name);
    if (expected.empty() || Utf8(dump.filename()) != expected || Utf8(log.path.filename()) != log.name) return DumpProblem::kNotThisLog;
    if (dump.lexically_normal().parent_path() != log.path.lexically_normal().parent_path()) return DumpProblem::kNotThisLog;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(dump, ec);
    if (ec || !fs::exists(status)) return DumpProblem::kMissing;
    if (!fs::is_regular_file(status)) return DumpProblem::kNotAFile;
    if (!LogNamesDump(log.path, expected)) return DumpProblem::kNotThisLog;
    const fs::file_time_type dump_written = fs::last_write_time(dump, ec);
    if (ec) return DumpProblem::kUnreadable;
    const fs::file_time_type log_written = fs::last_write_time(log.path, ec);
    if (ec) return DumpProblem::kNotThisLog;
    const auto apart = dump_written > log_written ? dump_written - log_written : log_written - dump_written;
    if (apart > std::chrono::seconds(kDumpPairingSeconds)) return DumpProblem::kNotThisLog;
    const uintmax_t size = fs::file_size(dump, ec);
    if (ec) return DumpProblem::kUnreadable;
    if (size == 0) return DumpProblem::kEmpty;
    if (size < kFewestDumpBytes) return DumpProblem::kTooSmall;
    if (size > kMostDumpBytes) return DumpProblem::kTooLarge;
    return ReadMinidumpSignature(dump);
}

const char* DumpProblemText(DumpProblem problem) {
    switch (problem) {
    case DumpProblem::kNone: break;
    case DumpProblem::kMissing: return "The crash dump of this session was not found.";
    case DumpProblem::kNotThisLog: return "The crash dump does not belong to this session.";
    case DumpProblem::kNotAFile: return "The crash dump is not a regular file.";
    case DumpProblem::kEmpty: return "The crash dump is empty.";
    case DumpProblem::kTooSmall: return "The crash dump is too small to be valid.";
    case DumpProblem::kTooLarge: return "The crash dump is larger than 8 MB.";
    case DumpProblem::kNotADump: return "The file is not a valid crash dump.";
    case DumpProblem::kUnreadable: return "The crash dump could not be read.";
    case DumpProblem::kChanged: return "The crash dump changed while it was being read.";
    }
    return nullptr;
}

bool DefaultAttachCrashDump(const GameLog& log) { return log.CanAttachDump(); }

size_t DefaultGameLog(const std::vector<GameLog>& logs) {
    const auto running = [](const GameLog& log) {
        return log.role == LogRole::kCurrentLauncher || log.role == LogRole::kCurrentGameplay;
    };
    const auto first = [&](auto wanted) {
        const auto found = std::find_if(logs.begin(), logs.end(), [&](const GameLog& log) { return !running(log) && wanted(log); });
        return found == logs.end() ? logs.size() : size_t(found - logs.begin());
    };
    for (const LogActivity activity : {LogActivity::kGameplay, LogActivity::kGameStarted}) {
        const size_t found = first([&](const GameLog& log) { return log.activity == activity; });
        if (found < logs.size()) return found;
    }
    const size_t earlier = first([](const GameLog&) { return true; });
    return earlier < logs.size() ? earlier : 0;
}

const char* LogRoleText(LogRole role) {
    switch (role) {
    case LogRole::kCurrentLauncher: return "Current launcher session";
    case LogRole::kCurrentGameplay: return "Current session \xC2\xB7 Launcher + gameplay";
    case LogRole::kLastGameplay: return "Last gameplay";
    case LogRole::kOtherGameplay: return "Gameplay session";
    case LogRole::kOtherLauncher: return "Launcher session";
    case LogRole::kNone: break;
    }
    return nullptr;
}

const char* PreviewButtonText(bool preview_open) { return preview_open ? "Close preview" : "Open preview"; }

std::string LogDateText(std::time_t written) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &written);
#else
    localtime_r(&written, &local);
#endif
    char text[40];
    std::snprintf(text, sizeof(text), "%s %d, %04d %02d:%02d", kMonths[std::clamp(local.tm_mon, 0, 11)], local.tm_mday,
                  local.tm_year + 1900, local.tm_hour, local.tm_min);
    return text;
}

std::string LogSizeText(uintmax_t bytes) {
    constexpr uintmax_t kKilobyte = 1024, kMegabyte = kKilobyte * 1024;
    char text[32];
    if (bytes < kMegabyte) {
        std::snprintf(text, sizeof(text), "%llu KB", static_cast<unsigned long long>(std::max<uintmax_t>(1, (bytes + kKilobyte / 2) / kKilobyte)));
    } else {
        std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / double(kMegabyte));
    }
    return text;
}

size_t BytesOfCharacters(std::string_view text, size_t most) {
    size_t characters = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
        if (characters == most) return i;
        ++characters;
    }
    return text.size();
}

size_t CountCharacters(std::string_view text) {
    return size_t(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

DraftProblem CheckDraft(std::string_view description, bool has_log) {
    if (CountCharacters(TrimmedDescription(description)) < kFewestDescriptionCharacters) return DraftProblem::kDescriptionTooShort;
    return has_log ? DraftProblem::kNone : DraftProblem::kNoLog;
}

std::string OperatingSystemText() {
#ifdef _WIN32
    return WindowsVersionText();
#else
    const std::string distribution = LinuxDistributionText();
    return distribution.empty() ? "Linux" : "Linux: " + distribution;
#endif
}

const std::string& IncidentPackage::Value(std::string_view key) const {
    static const std::string missing;
    const auto found = std::find_if(fields.begin(), fields.end(), [&](const IncidentField& field) { return field.key == key; });
    return found == fields.end() ? missing : found->value;
}

std::string LastBytes(std::string_view text, size_t most) {
    if (text.size() <= most) return std::string(text);
    std::string_view tail = text.substr(text.size() - most);
    const size_t line = tail.find('\n');
    if (line != std::string_view::npos && line + 1 < tail.size()) tail.remove_prefix(line + 1);
    return std::string(tail);
}

std::optional<IncidentPackage> BuildIncidentPackage(std::string_view description, const GameLog& log, const ReportFacts& facts,
                                                    const PrivateFacts& private_facts, bool attach_crash_dump) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(log.path, ec);
    if (ec || size == 0) return std::nullopt;
    const uintmax_t offset = size > kLogTailBytes ? size - kLogTailBytes : 0;
    std::string raw = ReadPart(log.path, offset, size_t(size - offset));
    if (raw.empty()) return std::nullopt;
    if (offset > 0) {
        const size_t line = raw.find('\n');
        if (line != std::string::npos) raw.erase(0, line + 1);
    }
    const std::string scrubbed = ScrubText(raw, private_facts);
    std::string text = LastBytes(scrubbed, kMostLogBytes);
    if (text.find_first_not_of(" \t\n") == std::string::npos) return std::nullopt;
    const bool clipped = offset > 0 || text.size() < scrubbed.size();

    std::string summary = ScrubText(TrimmedDescription(description), private_facts);
    summary.resize(BytesOfCharacters(summary, kMostDescriptionCharacters));
    summary = TrimmedDescription(summary);
    if (CountCharacters(summary) < kFewestDescriptionCharacters) return std::nullopt;

    const LogDiagnostics graphics = ReadLogDiagnostics(ScrubText(ReadPart(log.path, 0, kSessionHeadBytes), private_facts));
    std::string values[] = {std::move(summary),
                            Clamped(facts.launcher_version, kMostMetadataCharacters),
                            Clamped(facts.game_build, kMostMetadataCharacters),
                            IsGameVersion(facts.game_version) ? facts.game_version : std::string(),
                            Clamped(facts.region, kMostMetadataCharacters),
                            IsTitleId(facts.cusa) ? facts.cusa : std::string(),
                            IsSha256(facts.eboot_sha256) ? facts.eboot_sha256 : std::string(),
                            Clamped(facts.operating_system, kMostMetadataCharacters),
                            Clamped(graphics.cpu, kMostDetailCharacters),
                            Clamped(graphics.renderer_selected, kMostMetadataCharacters),
                            Clamped(graphics.renderer, kMostMetadataCharacters),
                            Clamped(graphics.gpu, kMostDetailCharacters),
                            Clamped(graphics.driver, kMostDetailCharacters),
                            Clamped(graphics.render_resolution, kMostMetadataCharacters),
                            SessionEndKey(log.outcome.end),
                            Clamped(ScrubText(log.outcome.detail, private_facts), kMostSessionDetailCharacters),
                            Clamped(ScrubText(log.outcome.stage, private_facts), kMostMetadataCharacters),
                            log.name,
                            clipped ? "yes" : "no",
                            std::move(text)};
    static_assert(std::size(values) == std::size(kFieldNames), "one value per field");
    IncidentPackage package;
    for (size_t i = 0; i < std::size(kFieldNames); ++i) package.fields.push_back({kFieldNames[i].key, kFieldNames[i].label, std::move(values[i])});
    package.crash_dump_requested = attach_crash_dump;
    if (attach_crash_dump) {
        const std::string name = DumpNameOf(log.name);
        const fs::path dump = log.path.parent_path() / name;
        package.crash_dump_problem = name.empty() ? DumpProblem::kMissing : CheckCrashDump(log, dump);
        if (package.crash_dump_problem == DumpProblem::kNone) {
            std::error_code size_error;
            package.crash_dump = HashedDump(dump, fs::file_size(dump, size_error), package.crash_dump_problem);
        }
    }
    return package;
}

std::string IncidentPreviewText(const IncidentPackage& package) {
    std::string out;
    for (const IncidentField& field : package.fields) {
        const std::string label = Tr(std::string(field.label));
        if (field.key == kDescriptionKey || field.key == kLogTextKey) {
            out += (out.empty() ? "" : "\n") + label + ":\n" + field.value + "\n";
            if (field.key == kDescriptionKey) out += "\n";
        } else {
            out += label + ": " + (field.value.empty() ? "-" : field.value) + "\n";
        }
    }
    if (package.crash_dump) {
        out += "\n" + std::string(Tr("Crash dump")) + ": " + package.crash_dump->name + " (" + LogSizeText(package.crash_dump->bytes) + ", " +
               Tr("sent as a file after the report, not shown here") + ")\n";
    } else if (package.crash_dump_requested) {
        const char* why = DumpProblemText(package.crash_dump_problem);
        out += "\n" + std::string(Tr("Crash dump")) + ": " + Tr("not attached") + "." + (why ? std::string(" ") + Tr(why) : "") + "\n";
    }
    return out;
}

std::string IncidentRequestJson(const IncidentPackage& package, std::string_view submission) {
    std::string out = "{";
    for (const IncidentField& field : package.fields) {
        if (out.size() > 1) out += ',';
        out += json::Quote(field.key) + ':' + json::Quote(field.value);
    }
    if (package.crash_dump) {
        out += ",\"dump_file\":" + json::Quote(package.crash_dump->name) + ",\"dump_bytes\":" + std::to_string(package.crash_dump->bytes) +
               ",\"dump_sha256\":" + json::Quote(package.crash_dump->sha256);
    }
    if (!submission.empty()) out += ",\"submission\":" + json::Quote(submission);
    return out + '}';
}

IncidentPackage SentReportOf(const json::Value& report) {
    IncidentPackage package;
    for (const FieldName& name : kFieldNames) package.fields.push_back({name.key, name.label, report.StringOr(name.key)});
    return package;
}

bool IsTicket(std::string_view text) {
    if (text.size() != kTicketLength || !text.starts_with(kTicketPrefix)) return false;
    return std::all_of(text.begin() + kTicketPrefix.size(), text.end(), [](char c) { return kTicketAlphabet.find(c) != std::string_view::npos; });
}

bool IsReceipt(std::string_view text) { return IsSha256(text); }

fs::path SaveIncidentReport(const IncidentPackage& package, const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));
    fs::path file = folder / ("sotb_report_" + std::string(stamp) + ".txt");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << IncidentPreviewText(package);
    return out.flush() ? file : fs::path();
}

} // namespace Sotb

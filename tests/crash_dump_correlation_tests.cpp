// Which crash dump belongs to which session: the runtime's numbering and dump names, local retention, and the Bug Report
// page's exact pairing and checks of a dump. Synthetic logs and dumps in a temporary folder only.
#include <algorithm>
#include <cstring>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "crash_dump.h"
#include "crash_report_test_support.h"
#include "crypto.h"
#include "json_lite.h"
#include "runtime_log.h"

using namespace crash_test;
using namespace Sotb;

namespace {

using std::chrono::seconds;

std::string Sha256Hex(const std::string& bytes) {
    uint8_t digest[32];
    crypto::Sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), digest);
    std::string hex;
    for (const uint8_t byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex;
}

void TestRuntimeNames() {
    CHECK(runtime_log_session_number("shadow_of_the_beast_007.log") == 7);
    CHECK(runtime_log_session_number("shadow_of_the_beast_007.dmp") == 7);
    CHECK(runtime_log_session_number("shadow_of_the_beast_4294967295.log") == 4294967295u);
    CHECK(runtime_log_session_number("shadow_of_the_beast_4294967296.log") == 0);
    CHECK(runtime_log_session_number("shadow_of_the_beast_12345678901.log") == 0);
    for (const char* name : {"shadow_of_the_beast_.dmp", "shadow_of_the_beast_5.dmpx", "shadow_of_the_beast_5.dmp.txt", "shadow_of_the_beast_-1.log",
                             "shadow_of_the_beast_ 1.log", "shadow_of_the_beast_5_2.dmp", "other_game_005.dmp", "SHADOW_OF_THE_BEAST_005.dmp", ""}) {
        CHECK(runtime_log_session_number(name) == 0);
    }
    CHECK(runtime_log_session_number(nullptr) == 0);

    char out[64];
    CHECK(runtime_log_dump_path("C:\\logs\\shadow_of_the_beast_012.log", out, sizeof(out)) && !std::strcmp(out, "C:\\logs\\shadow_of_the_beast_012.dmp"));
    CHECK(!runtime_log_dump_path("C:\\logs\\custom.txt", out, sizeof(out)));
    CHECK(!runtime_log_dump_path(".log", out, sizeof(out)));
    CHECK(!runtime_log_dump_path(nullptr, out, sizeof(out)));
    char tiny[8];
    CHECK(!runtime_log_dump_path("C:\\logs\\shadow_of_the_beast_012.log", tiny, sizeof(tiny)));
}

// A crash dump keeps its number taken: the next log never lands beside the dump of another session.
void TestNumbersStayTaken(const fs::path& scratch) {
    const fs::path folder = scratch / "numbers";
    Write(folder / LogName(3), "x");
    Write(folder / DumpName(7), DumpBytes(2048));
    Write(folder / "shadow_of_the_beast_900.txt", "x");
    char path[4200];
    void* handle = nullptr;
    CHECK(runtime_log_create(folder.string().c_str(), nullptr, path, sizeof(path), &handle));
    if (handle) CloseHandle(handle);
    CHECK(fs::path(path).filename() == LogName(8));
}

void TestLocalRetention(const fs::path& scratch) {
    const fs::path folder = scratch / "retention";
    for (int number : {3, 9, 11, 12, 20, 21, 22, 40}) Write(folder / DumpName(number), DumpBytes(2048));
    Write(folder / LogName(3), "log");
    Write(folder / "notes.dmp", "keep");
    Write(folder / "shadow_of_the_beast_050.dmp.txt", "keep");
    fs::create_directories(folder / DumpName(99));
    CHECK(crash_dump_prune(folder.string().c_str(), CRASH_DUMPS_KEPT) == 3);
    for (int number : {12, 20, 21, 22, 40}) CHECK(fs::exists(folder / DumpName(number)));
    for (int number : {3, 9, 11}) CHECK(!fs::exists(folder / DumpName(number)));
    CHECK(fs::exists(folder / LogName(3)) && fs::exists(folder / "notes.dmp") && fs::exists(folder / "shadow_of_the_beast_050.dmp.txt"));
    CHECK(fs::is_directory(folder / DumpName(99)));
    CHECK(crash_dump_prune(folder.string().c_str(), CRASH_DUMPS_KEPT) == 0);
    CHECK(crash_dump_prune((scratch / "missing").string().c_str(), 5) == 0 && crash_dump_prune("", 5) == 0 && crash_dump_prune(nullptr, 5) == 0);
    CHECK(CRASH_DUMPS_KEPT == kMostGameLogs);
}

void TestDumpNames() {
    CHECK(DumpNameOf("shadow_of_the_beast_012.log") == "shadow_of_the_beast_012.dmp");
    CHECK(DumpNameOf("shadow_of_the_beast_0123456789.log") == "shadow_of_the_beast_0123456789.dmp");
    for (const char* name : {"shadow_of_the_beast_12.log", "shadow_of_the_beast_01234567890.log", "shadow_of_the_beast_012.log.txt",
                             "../shadow_of_the_beast_012.log", "shadow_of_the_beast_012.LOG", "shadow_of_the_beast_0x1.log", "x.log", ""}) {
        CHECK(DumpNameOf(name).empty());
    }
}

// 1, 5, 13, 22: the session's own dump, chosen among others, and nothing else.
void TestExactSession(const fs::path& scratch) {
    const fs::path folder = scratch / "exact";
    for (int number = 10; number <= 13; ++number) {
        MakeLog(folder, number, kGameplayHead + CrashTail(number), seconds(600 - number * 10));
        MakeDump(folder, number, DumpBytes(4096, unsigned(number)), seconds(600 - number * 10));
    }
    for (int number = 10; number <= 13; ++number) {
        const GameLog log = Found(folder / LogName(number));
        CHECK(log.Crashed() && log.CanAttachDump() && DefaultAttachCrashDump(log));
        CHECK(log.dump && log.dump->name == DumpName(number) && log.dump->path == folder / DumpName(number) && log.dump->bytes == 4096);
    }
    const GameLog log = Found(folder / LogName(12));
    const auto package = BuildIncidentPackage("The game crashed after the boss fight", log, Facts(), Private(), true);
    CHECK(package && package->crash_dump && package->crash_dump->name == DumpName(12));
    CHECK(package && package->crash_dump->sha256 == Sha256Hex(DumpBytes(4096, 12)));
    const auto request = package ? json::Parse(IncidentRequestJson(*package)) : std::nullopt;
    CHECK(request && request->StringOr("dump_file") == DumpName(12) && request->NumberOr("dump_bytes") == 4096);
    CHECK(request && request->StringOr("dump_sha256") == Sha256Hex(DumpBytes(4096, 12)) && request->StringOr("log_file") == LogName(12));
    CHECK(request && request->StringOr("session_end") == "crash");
}

// 2, 3, 4: a dump of an earlier or later run, or the newest dump of no listed session, never pairs.
void TestOtherSessions(const fs::path& scratch) {
    const fs::path folder = scratch / "others";
    const fs::path log_file = MakeLog(folder, 20, kGameplayHead + CrashTail(20));
    const fs::path earlier = MakeDump(folder, 19, DumpBytes(4096));
    const fs::path later = MakeDump(folder, 21, DumpBytes(4096));
    MakeDump(folder, 99, DumpBytes(4096), seconds(1));
    const GameLog log = Found(log_file);
    CHECK(log.Crashed() && !log.dump && log.dump_problem == DumpProblem::kMissing && !DefaultAttachCrashDump(log));
    CHECK(CheckCrashDump(log, earlier) == DumpProblem::kNotThisLog);
    CHECK(CheckCrashDump(log, later) == DumpProblem::kNotThisLog);
    CHECK(CheckCrashDump(log, folder / DumpName(99)) == DumpProblem::kNotThisLog);

    // A dump with the session's own name that its log never named was not written by that session.
    const fs::path unnamed_log = MakeLog(folder, 30, kGameplayHead + CrashTail(30, false));
    MakeDump(folder, 30, DumpBytes(4096));
    const GameLog unnamed = Found(unnamed_log);
    CHECK(unnamed.Crashed() && !unnamed.dump && unnamed.dump_problem == DumpProblem::kNotThisLog);
    const fs::path other_names = MakeLog(folder, 31, kGameplayHead + CrashTail(32));
    MakeDump(folder, 31, DumpBytes(4096));
    CHECK(!Found(other_names).dump);
    const fs::path inline_mark = MakeLog(folder, 33, kGameplayHead + "note: Crash: minidump " + DumpName(33) + " written\n" + kCleanTail);
    MakeDump(folder, 33, DumpBytes(4096));
    CHECK(!Found(inline_mark).dump && !Found(inline_mark).Crashed());
}

// 6, 19: a crash without its dump still makes a report, with the reason the dump is missing.
void TestCrashWithoutDump(const fs::path& scratch) {
    const fs::path folder = scratch / "nodump";
    const GameLog log = Found(MakeLog(folder, 5, kGameplayHead + CrashTail(5)));
    CHECK(log.Crashed() && !log.dump && log.outcome.end == SessionEnd::kCrash);
    const auto package = BuildIncidentPackage("The game crashed without a dump", log, Facts(), Private(), true);
    CHECK(package && !package->crash_dump && package->crash_dump_requested && package->crash_dump_problem == DumpProblem::kMissing);
    CHECK(package && package->Value("session_end") == "crash" && Has(package->Value("session_detail"), "FATAL exception 0xc0000005"));
    const std::string request = package ? IncidentRequestJson(*package) : "";
    CHECK(!Has(request, "dump_file") && !Has(request, "dump_bytes") && !Has(request, "dump_sha256"));
    CHECK(package && Has(IncidentPreviewText(*package), "Crash dump: not attached. The crash dump of this session was not found."));
}

// 7 to 15: size, signature and age.
void TestDumpChecks(const fs::path& scratch) {
    const fs::path folder = scratch / "checks";
    struct Case {
        int number;
        std::string bytes;
        DumpProblem expected;
    };
    const Case cases[] = {
        {40, "", DumpProblem::kEmpty},
        {41, DumpBytes(kFewestDumpBytes - 1), DumpProblem::kTooSmall},
        {42, DumpBytes(kFewestDumpBytes), DumpProblem::kNone},
        {43, DumpBytes(kMostDumpBytes), DumpProblem::kNone},
        {44, DumpBytes(kMostDumpBytes + 1), DumpProblem::kTooLarge},
        {45, DumpBytes(4096, 1, false).replace(0, 4, "MZ\x90\0", 4), DumpProblem::kNotADump},
        {46, std::string("MDMX") + DumpBytes(4092), DumpProblem::kNotADump},
        {47, "MDM", DumpProblem::kTooSmall},
        {48, DumpBytes(4096), DumpProblem::kNone},
    };
    for (const Case& item : cases) {
        const fs::path log_file = MakeLog(folder, item.number, kGameplayHead + CrashTail(item.number));
        const fs::path dump = MakeDump(folder, item.number, item.bytes);
        const GameLog log = Found(log_file);
        const DumpProblem problem = CheckCrashDump(log, dump);
        if (problem != item.expected) std::printf("  dump %d: problem %d\n", item.number, int(problem));
        CHECK(problem == item.expected);
        CHECK(log.dump.has_value() && log.dump_problem == item.expected);
        CHECK(log.CanAttachDump() == (item.expected == DumpProblem::kNone));
        CHECK(log.Crashed());
        if (item.expected != DumpProblem::kNone) {
            CHECK(DumpProblemText(item.expected) != nullptr && !DefaultAttachCrashDump(log));
            const auto package = BuildIncidentPackage("A crash with an unusable dump", log, Facts(), Private(), true);
            CHECK(package && !package->crash_dump && package->crash_dump_problem == item.expected);
        }
    }
    CHECK(DumpProblemText(DumpProblem::kNone) == nullptr);

    // The dump and the last line of its log are written moments apart; a dump of another time is not the session's.
    const fs::path stale_log = MakeLog(folder, 50, kGameplayHead + CrashTail(50), seconds(60));
    MakeDump(folder, 50, DumpBytes(4096), seconds(60 + kDumpPairingSeconds + 30));
    CHECK(Found(stale_log).dump_problem == DumpProblem::kNotThisLog && !Found(stale_log).dump);
    const fs::path near_log = MakeLog(folder, 51, kGameplayHead + CrashTail(51), seconds(60));
    MakeDump(folder, 51, DumpBytes(4096), seconds(60 + kDumpPairingSeconds - 20));
    CHECK(Found(near_log).CanAttachDump());
    const fs::path newer_log = MakeLog(folder, 52, kGameplayHead + CrashTail(52), seconds(600));
    MakeDump(folder, 52, DumpBytes(4096), seconds(1));
    CHECK(Found(newer_log).dump_problem == DumpProblem::kNotThisLog);
}

// 16, 17: only the file of that name in the log's own folder; a directory or a link is never read.
void TestPlaces(const fs::path& scratch) {
    const fs::path folder = scratch / "places";
    const fs::path log_file = MakeLog(folder, 60, kGameplayHead + CrashTail(60));
    const fs::path elsewhere = scratch / "elsewhere";
    MakeDump(elsewhere, 60, DumpBytes(4096));
    const GameLog log = Found(log_file);
    CHECK(CheckCrashDump(log, elsewhere / DumpName(60)) == DumpProblem::kNotThisLog);
    CHECK(CheckCrashDump(log, folder / ".." / "elsewhere" / DumpName(60)) == DumpProblem::kNotThisLog);
    CHECK(CheckCrashDump(log, folder / "sub" / ".." / DumpName(60)) == DumpProblem::kMissing);
    CHECK(CheckCrashDump(log, folder / "renamed.dmp") == DumpProblem::kNotThisLog);
    GameLog forged = log;
    forged.name = LogName(61);
    CHECK(CheckCrashDump(forged, folder / DumpName(61)) == DumpProblem::kNotThisLog);

    const fs::path dir_log = MakeLog(folder, 62, kGameplayHead + CrashTail(62));
    fs::create_directories(folder / DumpName(62));
    const GameLog with_dir = Found(dir_log);
    CHECK(CheckCrashDump(with_dir, folder / DumpName(62)) == DumpProblem::kNotAFile && !with_dir.dump && with_dir.Crashed());

    const fs::path link_log = MakeLog(folder, 63, kGameplayHead + CrashTail(63));
    std::error_code ec;
    fs::create_symlink(elsewhere / DumpName(60), folder / DumpName(63), ec);
    if (!ec) {
        CHECK(CheckCrashDump(Found(link_log), folder / DumpName(63)) == DumpProblem::kNotAFile);
        CHECK(!Found(link_log).dump);
    } else {
        std::printf("  symbolic link check skipped: this account cannot create links (%s)\n", ec.message().c_str());
    }
}

// 20, 21: a normal end, a quit in the launcher and a refused build are no crash; a device loss is its own kind.
void TestSessionKinds(const fs::path& scratch) {
    const fs::path folder = scratch / "kinds";
    const GameLog clean = Found(MakeLog(folder, 70, kGameplayHead + kCleanTail));
    CHECK(clean.outcome.end == SessionEnd::kClean && !clean.Crashed() && !SessionEndText(clean) && !clean.dump);
    const GameLog exited = Found(MakeLog(folder, 71, kGameplayHead + "Runtime: guest requested exit(0)\n"));
    CHECK(exited.outcome.end == SessionEnd::kClean && !exited.Crashed());

    const std::string quit = "[run]\nStage: launcher handoff\nLauncher: closed\nStage: launcher returned\n";
    MakeLog(folder, 72, quit);
    for (const GameLog& log : FindGameLogs(folder, {}, 200)) CHECK(log.name != LogName(72));
    CHECK(ReadSessionOutcome(quit, quit).end == SessionEnd::kUnknown);

    const std::string refused = "[run]\nStage: launcher returned\nUnsupported build diagnostics begin\nUnsupported build diagnostics end\n"
                                "ERROR: unsupported build: eboot.bin differs (install from a supported Shadow of the Beast package)\n"
                                "Stage: launcher returned\n";
    const SessionOutcome build = ReadSessionOutcome(refused, refused);
    CHECK(build.end == SessionEnd::kUnsupportedBuild && Has(build.detail, "unsupported build") && build.stage == "launcher returned");

    const std::string lost = kGameplayHead +
                             "GPU [Render_Vulkan] <Critical> vk_instance.cpp:358 operator(): GPU device lost: first detected in vkQueueSubmit (submit)\n"
                             "GPU [Render_Vulkan] <Critical> vk_instance.cpp:358 operator(): GPU device lost: vkQueueSubmit (submit) result -4, "
                             "tick 77; GPU NVIDIA GeForce RTX 4070; driver 621.1.0.0; device fault unavailable (no VK_EXT_device_fault)\n";
    const GameLog device = Found(MakeLog(folder, 73, lost));
    CHECK(device.outcome.end == SessionEnd::kDeviceLost && !device.Crashed() && SessionEndText(device) == std::string("GPU device lost"));
    const GameLog crash = Found(MakeLog(folder, 74, kGameplayHead + CrashTail(74)));
    CHECK(SessionEndText(crash) == std::string("Crash detected"));
    const GameLog unknown = Found(MakeLog(folder, 75, kGameplayHead + "frame 1\n"));
    CHECK(unknown.outcome.end == SessionEnd::kUnknown && !unknown.Crashed() && !SessionEndText(unknown));

    const std::string stopped = kGameplayHead + "STOP: first unsupported PS4 import: abc#libX (index 3)\nControlled stop: unsupported import in stage 'guest entry'\n";
    const SessionOutcome stop = ReadSessionOutcome(stopped, stopped);
    CHECK(stop.end == SessionEnd::kStopped && stop.detail == "STOP: first unsupported PS4 import: abc#libX (index 3)");
    CHECK(std::string(SessionEndKey(SessionEnd::kDeviceLost)) == "device_lost" && std::string(SessionEndKey(SessionEnd::kUnknown)) == "unknown");
}

// 23: what leaves the PC names the dump, never the folders it is in.
void TestNoLocalPaths(const fs::path& scratch) {
    const fs::path folder = scratch / "ana.silva" / "private-folder";
    const fs::path log_file = MakeLog(folder, 80, kGameplayHead + CrashTail(80) + "Settings: C:\\Users\\ana.silva\\Documents\\sotb.ini\n");
    MakeDump(folder, 80, DumpBytes(4096));
    const GameLog log = Found(log_file);
    const auto package = BuildIncidentPackage("A crash in a private folder", log, Facts(), Private(), true);
    CHECK(package && package->crash_dump);
    const std::string request = package ? IncidentRequestJson(*package, std::string(64, 'e')) : "";
    const std::string preview = package ? IncidentPreviewText(*package) : "";
    for (const std::string& text : {request, preview}) {
        CHECK(!Has(text, "private-folder") && !Has(text, "ana.silva") && !Has(text, scratch.string()) && !Has(text, folder.filename().string()));
        CHECK(!Has(text, "\\" + DumpName(80)) && !Has(text, "/" + DumpName(80)));
    }
    CHECK(Has(request, "\"dump_file\":\"" + DumpName(80) + "\""));
    for (const DumpProblem problem : {DumpProblem::kMissing, DumpProblem::kNotThisLog, DumpProblem::kNotAFile, DumpProblem::kEmpty, DumpProblem::kTooSmall,
                                      DumpProblem::kTooLarge, DumpProblem::kNotADump, DumpProblem::kUnreadable, DumpProblem::kChanged}) {
        const std::string text = DumpProblemText(problem);
        CHECK(!Has(text, "\\") && !Has(text, "/") && !Has(text, ".dmp"));
    }
}

void TestRuntimeWiring() {
    const std::string loader = Source("src/loader.c");
    const std::string fatal = loader.substr(loader.find("static LONG fatal_exception("));
    const size_t dump = fatal.find("crash_dump_write(info, dump_line, sizeof(dump_line));");
    CHECK(dump != std::string::npos && dump < fatal.find("runtime_log_write_direct(dump_line);") &&
          fatal.find("runtime_log_write_direct(dump_line);") < fatal.find("runtime_log_write_direct(fatal_line);\n    TerminateProcess"));
    CHECK(Has(loader, "if (log_started) printf(\"Crash dump: %s\\n\", crash_dump_prepare()"));
    CHECK(loader.find("crash_dump_prepare()") > loader.find("SetUnhandledExceptionFilter(unhandled_fault);"));

    const std::string writer = Source("src/crash_dump.c");
    CHECK(Has(writer, "LoadLibraryExW(L\"dbghelp.dll\", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)") && !Has(writer, "LoadLibraryA("));
    CHECK(Has(writer, "CREATE_NEW") && !Has(writer, "CREATE_ALWAYS") && !Has(writer, "OPEN_ALWAYS"));
    CHECK(Has(writer, "MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules"));
    for (const char* wide : {"MiniDumpWithFullMemory", "MiniDumpWithProcessThreadData", "MiniDumpWithPrivateReadWriteMemory",
                             "MiniDumpWithIndirectlyReferencedMemory", "MiniDumpWithHandleData", "MiniDumpWithFullMemoryInfo"}) {
        CHECK(!Has(writer, wide));
    }
    const std::string comment = writer.substr(writer.find("char comment["), 300);
    CHECK(Has(comment, "exception 0x%08lx on thread %lu") && !Has(comment, "path") && !Has(comment, "session_path"));
    CHECK(Has(writer, "\"Crash: minidump %s written\\n\", file_name(request.path)"));
    CHECK(Has(Source("CMakeLists.txt"), "src/crash_dump.c"));
    CHECK(Has(Source("launcher/updater/update_paths.cpp"), "\".dmp\""));
}

} // namespace

int main() {
    const fs::path scratch = Scratch("sotb-crash-dump-correlation-tests");
    TestRuntimeNames();
    TestNumbersStayTaken(scratch);
    TestLocalRetention(scratch);
    TestDumpNames();
    TestExactSession(scratch);
    TestOtherSessions(scratch);
    TestCrashWithoutDump(scratch);
    TestDumpChecks(scratch);
    TestPlaces(scratch);
    TestSessionKinds(scratch);
    TestNoLocalPaths(scratch);
    TestRuntimeWiring();
    return Finish("crash dump correlation", scratch);
}

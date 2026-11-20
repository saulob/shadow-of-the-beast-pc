// Shared by the crash report suites: checks that report progress to tools\test.ps1, and synthetic logs and dumps in a
// temporary folder. Nothing here starts the game, opens a window or reaches the network.
#pragma once

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "incident_report.h"
#include "test_progress.h"

namespace crash_test {

namespace fs = std::filesystem;

inline int checks = 0;
inline int failures = 0;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        ++crash_test::checks;                                                                                          \
        if (!(condition)) {                                                                                            \
            ++crash_test::failures;                                                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #condition);                                                   \
        }                                                                                                              \
        sotb_test_progress(crash_test::checks);                                                                        \
    } while (0)

inline bool Has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

inline size_t Count(std::string_view text, std::string_view part) {
    size_t count = 0;
    for (size_t at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size())) ++count;
    return count;
}

inline std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

inline std::string Source(const char* relative) { return Read(fs::path(SOTB_SOURCE_DIR) / relative); }

inline void Write(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
}

inline void Age(const fs::path& file, std::chrono::seconds ago) {
    fs::last_write_time(file, fs::file_time_type::clock::now() - ago);
}

inline std::string LogName(int number) {
    char name[64];
    std::snprintf(name, sizeof(name), "shadow_of_the_beast_%03d.log", number);
    return name;
}

inline std::string DumpName(int number) {
    char name[64];
    std::snprintf(name, sizeof(name), "shadow_of_the_beast_%03d.dmp", number);
    return name;
}

// A gameplay session's start, as the loader and the launcher write it.
inline const std::string kGameplayHead =
    "[run] 2026-11-20T10:00:00\n"
    "CPU: Intel(R) Core(TM) i9-14900\n"
    "Stage: launcher handoff\n"
    "Launcher: closed, starting the game\n"
    "Stage: launcher returned\n"
    "Build fingerprint: CUSA03762 1.00 Korea, region Korea, title CUSA03762, version 1.00, code CUSA03762-1.00\n"
    "Stage: game build verified\n"
    "Renderer: selected auto, active Vulkan\n"
    "GPU: Vulkan device NVIDIA GeForce RTX 4070 (NVIDIA 621.1.0.0, Vulkan 1.4.357)\n"
    "Stage: GPU initialization\n"
    "Stage: GPU initialized\n"
    "Stage: guest entry\n"
    "Runtime: guest thread 'main' created (stack=1, prio=1)\n"
    "Stage: first frame presented\n";

// The end of a session the fatal handler reported (src/loader.c), with or without the dump it names.
inline std::string CrashTail(int number, bool dump_written = true) {
    std::string tail =
        "Fault: write access to 0000000000000010 at guest offset 0x1234 (thread 4242)\n"
        "Crash: startup phase: first frame presented\n"
        "  rax=0000000000000000 rbx=0000000000000001 rcx=0000000000000002 rdx=0000000000000003\n";
    tail += dump_written ? "Crash: minidump " + DumpName(number) + " written\n" : std::string("Crash: minidump not written (timed out)\n");
    tail += "FATAL exception 0xc0000005 at 0000000801234567 (guest offset 0x1234) thread 4242\n";
    return tail;
}

inline const std::string kCleanTail = "Runtime: window closed by user\nStage: process exit\n";

inline fs::path MakeLog(const fs::path& folder, int number, const std::string& text, std::chrono::seconds ago = std::chrono::seconds(60)) {
    const fs::path file = folder / LogName(number);
    Write(file, text);
    Age(file, ago);
    return file;
}

// A synthetic minidump: the signature, then filler that is the same for the same size and seed.
inline std::string DumpBytes(size_t size, unsigned seed = 1, bool signature = true) {
    std::string bytes(size, '\0');
    uint32_t state = seed;
    for (char& byte : bytes) {
        state = state * 1103515245u + 12345u;
        byte = char(state >> 24);
    }
    if (signature && size >= 4) bytes.replace(0, 4, "MDMP");
    return bytes;
}

inline fs::path MakeDump(const fs::path& folder, int number, const std::string& bytes, std::chrono::seconds ago = std::chrono::seconds(60)) {
    const fs::path file = folder / DumpName(number);
    Write(file, bytes);
    Age(file, ago);
    return file;
}

// The log the Bug Report page would find for `file`, with its outcome and dump.
inline Sotb::GameLog Found(const fs::path& file) {
    for (const Sotb::GameLog& log : Sotb::FindGameLogs(file.parent_path(), {}, 200)) {
        if (log.path.filename() == file.filename()) return log;
    }
    return {};
}

inline Sotb::ReportFacts Facts() {
    return {"0.1.0", "CUSA03762 1.00 Korea", "1.00", "Korea", "CUSA03762", std::string(64, 'a'), "Windows 11 (build 26200)"};
}

inline Sotb::PrivateFacts Private() {
    Sotb::PrivateFacts facts;
    facts.user_names = {"ana.silva"};
    facts.machine_names = {"DESKTOP-ANA7"};
    facts.profile_folders = {"C:\\Users\\ana.silva"};
    return facts;
}

inline fs::path Scratch(const char* name) {
    const fs::path folder = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder);
    return folder;
}

inline int Finish(const char* suite, const fs::path& scratch) {
    std::error_code ec;
    fs::remove_all(scratch, ec);
    std::printf("%s tests: %d checks, %d failures\n", suite, checks, failures);
    return failures == 0 ? 0 : 1;
}

} // namespace crash_test

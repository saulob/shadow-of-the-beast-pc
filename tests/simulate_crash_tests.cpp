// SOTB_SIMULATE_CRASH: what arms it, and where the game's start calls it. Nothing here crashes.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "simulate_crash.h"

namespace fs = std::filesystem;

namespace {

int checks = 0;
int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(condition)) {                                                                                            \
            ++failures;                                                                                                \
            std::printf("FAIL line %d: %s\n", __LINE__, #condition);                                                   \
        }                                                                                                              \
    } while (0)

std::string Source(const char* relative) {
    std::ifstream in(fs::path(SOTB_SOURCE_DIR) / relative, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void SetVariable(const char* value) {
#ifdef _WIN32
    _putenv_s("SOTB_SIMULATE_CRASH", value);
#else
    setenv("SOTB_SIMULATE_CRASH", value, 1);
#endif
}

void ClearVariable() {
#ifdef _WIN32
    _putenv_s("SOTB_SIMULATE_CRASH", "");
#else
    unsetenv("SOTB_SIMULATE_CRASH");
#endif
}

void TestValues() {
    CHECK(!sotb_simulate_crash_requested(nullptr));
    CHECK(!sotb_simulate_crash_requested(""));
    for (const char* other : {"0", "2", "true", "yes", "on", "01", "11", "1.0", " 1", "1 ", "\t1", "1\n", " ", "TRUE"}) {
        CHECK(!sotb_simulate_crash_requested(other));
    }
    CHECK(sotb_simulate_crash_requested("1"));
}

// Returning proves a disabled value never reaches the fault.
void TestDisabledValuesReturn() {
    ClearVariable();
    sotb_simulate_crash_from_environment();
    for (const char* value : {"0", "true", "yes", "01", " 1", "2"}) {
        SetVariable(value);
        sotb_simulate_crash_from_environment();
    }
    ClearVariable();
    CHECK(true);
}

size_t After(const std::string& text, const std::string& marker, size_t from = 0) {
    const size_t at = text.find(marker, from);
    return at == std::string::npos ? std::string::npos : at + marker.size();
}

void TestStartupWiring() {
    const std::string loader = Source("src/loader.c");
    CHECK(!loader.empty());
    const std::string call = "sotb_simulate_crash_from_environment();";
    const size_t logged = After(loader, "const int log_started = runtime_log_start(");
    const size_t vectored = After(loader, "AddVectoredExceptionHandler(1, vectored_fault);", logged);
    const size_t unhandled = After(loader, "SetUnhandledExceptionFilter(unhandled_fault);", vectored);
    const size_t dump = After(loader, "crash_dump_prepare()", unhandled);
    const size_t returned = After(loader, "sotb_startup_phase(\"launcher returned\");", dump);
    const size_t trigger = After(loader, call, returned);
    CHECK(logged != std::string::npos && vectored != std::string::npos && unhandled != std::string::npos);
    CHECK(dump != std::string::npos && returned != std::string::npos && trigger != std::string::npos);
    CHECK(After(loader, call) == trigger && After(loader, call, trigger) == std::string::npos);
    CHECK(loader.find("if (!cpu_only && !check_imports && !verify_slots) " + call) != std::string::npos);
    CHECK(After(loader, "sotb_launcher_run(&launcher);") < trigger);
    CHECK(loader.find("SOTB_SIMULATE_CRASH") == std::string::npos);
}

void TestSource() {
    const std::string source = Source("src/simulate_crash.c");
    CHECK(!source.empty());
    for (const char* forbidden : {"abort(", "exit(", "TerminateProcess", "RaiseException", "raise(", "__debugbreak", "MiniDump", "crash_dump"}) {
        if (source.find(forbidden) != std::string::npos) std::printf("  simulate_crash.c mentions %s\n", forbidden);
        CHECK(source.find(forbidden) == std::string::npos);
    }
    CHECK(source.find("*(volatile int *)0x10 = 1;") != std::string::npos);
    CHECK(source.find("!strcmp(value, \"1\")") != std::string::npos);
    CHECK(source.find("Diagnostics: SOTB_SIMULATE_CRASH=1, deliberately triggering an access violation") != std::string::npos);
    CHECK(source.find("Diagnostics: SOTB_SIMULATE_CRASH=1 is only supported on Windows; ignored") != std::string::npos);
    CHECK(Source("CMakeLists.txt").find("    src/loader.c\n    src/simulate_crash.c\n") != std::string::npos);
}

} // namespace

int main() {
    TestValues();
    TestDisabledValuesReturn();
    TestStartupWiring();
    TestSource();
    std::printf("simulate crash tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

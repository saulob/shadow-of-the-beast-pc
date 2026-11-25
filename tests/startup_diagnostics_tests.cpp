// How far a session got: the startup stage (src/startup_phase.c), the loader's stage order, the one-time first-frame
// milestone, and the stage a crash report carries. No game, no window, no GPU.
#include <cstring>
#include <string>
#include <vector>

#include "test_platform.h"

#include "crash_report_test_support.h"
#include "startup_phase.h"
extern "C" {
#include "module_history.h"
}

using namespace crash_test;
using namespace Sotb;

namespace {

// What sotb_startup_phase prints while `work` runs.
template <typename Work>
std::string Printed(const fs::path& scratch, Work&& work) {
    const fs::path file = scratch / "stdout.txt";
    std::fflush(stdout);
    const int saved = test_dup(test_fileno(stdout));
    FILE* sink = std::fopen(file.string().c_str(), "wb");
    if (saved < 0 || !sink) return {};
    test_dup2(test_fileno(sink), test_fileno(stdout));
    work();
    std::fflush(stdout);
    test_dup2(saved, test_fileno(stdout));
    test_close(saved);
    std::fclose(sink);
    std::string text = Read(file);
    std::erase(text, '\r');
    return text;
}

// 1, 3, 11, 12: one current stage, printed once when it changes, never for empty text.
void TestCurrentStage(const fs::path& scratch) {
    CHECK(std::strcmp(sotb_startup_phase_current(), "process start") == 0);
    const std::string printed = Printed(scratch, [] {
        sotb_startup_phase("launcher handoff");
        sotb_startup_phase("launcher handoff");
        sotb_startup_phase(nullptr);
        sotb_startup_phase("");
        sotb_startup_phase("GPU initialization");
        static int reached;
        for (int frame = 0; frame < 10000; ++frame) sotb_startup_phase_once(&reached, "first frame presented");
        sotb_startup_phase_once(nullptr, "never");
    });
    CHECK(printed == "Stage: launcher handoff\nStage: GPU initialization\nStage: first frame presented\n");
    CHECK(std::strcmp(sotb_startup_phase_current(), "first frame presented") == 0);

    // A milestone already reached does not take the stage back from a later one.
    const std::string later = Printed(scratch, [] {
        sotb_startup_phase("guest exit requested");
        static int reached = 1;
        sotb_startup_phase_once(&reached, "first frame presented");
    });
    CHECK(later == "Stage: guest exit requested\n" && std::strcmp(sotb_startup_phase_current(), "guest exit requested") == 0);
}

// 2, 13: the loader sets the stages in a fixed order, and the new ones only name what follows.
void TestLoaderOrder() {
    const std::string loader = Source("src/loader.c");
    const char* order[] = {"launcher handoff", "launcher returned", "game build verified", "filesystem configured", "runtime initialization",
                           "GPU initialization", "GPU initialized", "imports resolved", "patches installed", "guest image mapped",
                           "linked modules initialization", "guest entry"};
    size_t at = 0;
    for (const char* stage : order) {
        const size_t found = loader.find("sotb_startup_phase(\"" + std::string(stage) + "\")", at);
        if (found == std::string::npos) std::printf("  loader.c: stage '%s' missing or out of order\n", stage);
        CHECK(found != std::string::npos);
        if (found != std::string::npos) at = found;
    }
    CHECK(Has(loader, "gpu_enabled = 1;\n        sotb_startup_phase(\"GPU initialization\");\n        if (sotbgpu_init(&gpu)) fail(\"GPU initialization failed\");\n"
                      "        sotb_startup_phase(\"GPU initialized\");"));
    CHECK(Count(loader, "sotb_startup_phase(\"GPU initialization\")") == 1);

    const std::string phase = Source("src/startup_phase.c");
    CHECK(Has(phase, "if (previous && !strcmp(previous, phase)) return;"));
    CHECK(Has(phase, "__atomic_load_n(reached, __ATOMIC_RELAXED) || __atomic_exchange_n(reached, 1, __ATOMIC_ACQ_REL)"));
}

// 9: the first game frame that reached the screen, once; a frame that failed, a reused frame or a blank one does not count.
void TestFirstFrameMilestone() {
    const std::string presenter = Source("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const size_t present = presenter.find("void Presenter::Present(Frame* frame");
    const std::string body = presenter.substr(present, presenter.find("void Presenter::RetireFrame(", present) - present);
    const size_t presented = body.find("presented = swapchain.Present();");
    const size_t milestone = body.find("sotb_startup_phase_once(&first_game_frame, \"first frame presented\");");
    CHECK(presented != std::string::npos && milestone != std::string::npos && presented < milestone && milestone < body.find("free_frame();\n    if (!is_reusing_frame"));
    CHECK(Has(body, "} else if (is_game_frame && !is_reusing_frame) {\n        static int first_game_frame;"));
    CHECK(Count(presenter, "sotb_startup_phase") == 1 && Has(presenter, "#include \"startup_phase.h\""));
    CHECK(!Has(Source("gpu/shadps4/core/libraries/videoout/driver.cpp"), "sotb_startup_phase"));
}

std::string StageOf(const std::string& log) { return ReadSessionOutcome(log, log).stage; }

// 5 to 8, 10: the stage a failure happened in reaches the report.
void TestStageInReports(const fs::path& scratch) {
    const std::string before_renderer = "[run]\nStage: launcher returned\nStage: game build verified\nStage: runtime initialization\n"
                                        "Fault: read access to 0000000000000000 at kernel32.dll+0x10 (thread 1)\nCrash: startup phase: runtime initialization\n"
                                        "FATAL exception 0xc0000005 at 00007FF900000010 (kernel32.dll+0x10) thread 1\n";
    CHECK(StageOf(before_renderer) == "runtime initialization" && ReadSessionOutcome(before_renderer, before_renderer).end == SessionEnd::kCrash);

    const std::string vulkan_failed = "[run]\nStage: runtime initialization\nStage: GPU initialization\nERROR: GPU initialization failed\nStage: GPU initialization\n";
    const SessionOutcome failed = ReadSessionOutcome(vulkan_failed, vulkan_failed);
    CHECK(failed.end == SessionEnd::kStartupFailure && failed.stage == "GPU initialization" && failed.detail == "ERROR: GPU initialization failed");
    const std::string vulkan_crash = "[run]\nStage: GPU initialization\nFault: exception 0xc0000094 at nvoglv64.dll+0x1 (thread 2)\n"
                                     "Crash: startup phase: GPU initialization\nFATAL exception 0xc0000094 at 00007FF9 (nvoglv64.dll+0x1) thread 2\n";
    CHECK(StageOf(vulkan_crash) == "GPU initialization");

    const std::string after_renderer = "[run]\nStage: GPU initialized\nStage: imports resolved\nFault: x\nCrash: startup phase: imports resolved\nFATAL exception 0x1\n";
    CHECK(StageOf(after_renderer) == "imports resolved");
    const std::string guest = "[run]\nStage: guest entry\nRuntime: guest thread 'main' created\nFault: x\nCrash: startup phase: guest entry\nFATAL exception 0x1\n";
    CHECK(StageOf(guest) == "guest entry");
    const std::string playing = kGameplayHead + CrashTail(5);
    CHECK(StageOf(playing) == "first frame presented");

    // A long session's end holds no stage line any more; its start does.
    std::string long_log = kGameplayHead;
    while (long_log.size() < 400 * 1024) long_log += "frame presented in 16.6 ms, nothing unusual here\n";
    const fs::path file = MakeLog(scratch / "stages", 6, long_log + "Fault: y\n" + "FATAL exception 0xc0000005 at 1 (guest offset 0x1) thread 9\n");
    const GameLog found = Found(file);
    CHECK(found.outcome.end == SessionEnd::kCrash && found.outcome.stage == "first frame presented");
    const auto package = BuildIncidentPackage("The game crashed after an hour of play", found, Facts(), Private(), false);
    CHECK(package && package->Value("session_stage") == "first frame presented");
    CHECK(ReadSessionOutcome("", "").stage.empty());
    CHECK(StageOf(std::string("Stage: ") + std::string(200, 's') + "\n").size() == 200);
    const auto clamped = BuildIncidentPackage("A stage text far too long for the report", Found(MakeLog(scratch / "stages", 7, kGameplayHead + "Stage: " + std::string(200, 's') + "\n")),
                                              Facts(), Private(), false);
    CHECK(clamped && clamped->Value("session_stage").size() == 64);
}

// 14: module history still records the stage a module was unloaded in.
void TestModuleHistoryKeepsTheStage() {
    static ModuleHistory history;
    std::memset(&history, 0, sizeof(history));
    sotb_startup_phase("GPU initialization");
    module_history_loaded(&history, 0x7ff9a7200000, 0x200000, "overlay.dll", nullptr);
    module_history_unloaded(&history, 0x7ff9a7200000, 0, nullptr, nullptr, 1000, sotb_startup_phase_current());
    char text[200];
    CHECK(module_history_describe(&history, 0x7ff9a72ef215, 1018, text, sizeof(text)));
    CHECK(std::strcmp(text, "UNLOADED overlay.dll+0xef215, unloaded 18 ms ago during GPU initialization") == 0);
    CHECK(Has(Source("src/module_diagnostics.c"), "fprintf(stderr, \"Crash: startup phase: %s\\n\", sotb_startup_phase_current());"));
}

} // namespace

int main() {
    const fs::path scratch = Scratch("sotb-startup-diagnostics-tests");
    TestCurrentStage(scratch);
    TestLoaderOrder();
    TestFirstFrameMilestone();
    TestStageInReports(scratch);
    TestModuleHistoryKeepsTheStage();
    return Finish("startup diagnostics", scratch);
}

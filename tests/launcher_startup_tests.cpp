// Launcher startup: the window stays hidden until a complete launcher frame was presented, its placement is final
// before it is shown, it is shown once, and the launcher's startup stages are reached once each in a fixed order.
// Source wiring and the stage store; no window.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "startup_phase.h"
#include "test_progress.h"

namespace fs = std::filesystem;

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

std::string Between(const std::string& text, const char* from, const char* to) {
    const size_t begin = text.find(from);
    if (begin == std::string::npos) return {};
    const size_t end = text.find(to, begin);
    return text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

bool InOrder(const std::string& text, std::initializer_list<const char*> needles) {
    size_t at = 0;
    for (const char* needle : needles) {
        at = text.find(needle, at);
        if (at == std::string::npos) return false;
        ++at;
    }
    return true;
}

const fs::path kRoot = SOTB_SOURCE_DIR;
const std::string kHost = ReadAll(kRoot / "launcher/host.cpp");
const std::string kLauncher = ReadAll(kRoot / "launcher/launcher.cpp");
const std::string kOpen = Between(kHost, "bool LauncherHost::Open(", "void LauncherHost::LogPlacement(");
const std::string kShow = Between(kHost, "void LauncherHost::Show()", "ImTextureID LauncherHost::CreateTexture(");
const std::string kRun = Between(kLauncher, "SotbLauncherResult Show(bool ready)", "std::filesystem::path InstallDir() const");
const std::string kLoop = Between(kRun, "while (true) {", "sotb_startup_phase(\"launcher loop ended\");");

void TestSourcesRead() {
    CHECK(!kHost.empty() && !kLauncher.empty());
    CHECK(!kOpen.empty() && !kShow.empty() && !kRun.empty() && !kLoop.empty());
}

void TestStartsHidden() {
    CHECK(Contains(kOpen, "SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);"));
    CHECK(Count(kHost, "SDL_ShowWindow(") == 1 && Contains(kShow, "SDL_ShowWindow(window_);"));
    CHECK(!Contains(kOpen, "SDL_ShowWindow") && !Contains(kOpen, "SDL_RaiseWindow"));
}

void TestReadyBeforeVisible() {
    // Window, placement, renderer and ImGui all come from Open, which never shows the window.
    CHECK(InOrder(kOpen, {"ChoosePlacement(", "SDL_CreateWindowWithProperties(props)", "sotb_startup_phase(\"launcher window created\")",
                          "CreateLauncherRenderer(window_, plan, selection, pinned)", "ImGui::CreateContext()",
                          "ImGui_ImplSDLRenderer3_Init(renderer_)", "SDL_MaximizeWindow(window_)",
                          "sotb_startup_phase(\"launcher renderer ready\")", "return true;"}));
    // Fonts and textures are loaded, and a complete launcher frame is built and presented, before the window shows.
    CHECK(InOrder(kRun, {"host_.Open(options)", "trg::LoadSystemFonts(", "LoadSponsorIcon(host_)",
                         "launcher_ = std::make_unique<trg::Launcher>(MakeConfig());", "sotb_startup_phase(\"launcher first frame\")",
                         "while (true) {", "host_.BeginFrame();", "launcher_->Frame();", "DrawUpdateBanner(",
                         "host_.EndFrame(LauncherTheme().bg);", "host_.Show();", "sotb_startup_phase(\"launcher running\")"}));
    // Show needs a drawn frame, and presents it again once the window is visible.
    CHECK(Contains(kShow, "if (!window_ || hidden_ || shown_ || !drawn_) {"));
    CHECK(InOrder(kShow, {"shown_ = true;", "SDL_ShowWindow(window_);", "SDL_RaiseWindow(window_);", "Submit(last_clear_);",
                          "SDL_RenderPresent(renderer_);"}));
    CHECK(InOrder(Between(kHost, "void LauncherHost::EndFrame(", "bool LauncherHost::Fullscreen()"),
                  {"Submit(clear);", "last_clear_ = clear;", "drawn_ = true;", "SDL_RenderPresent(renderer_);"}));
}

void TestShownOnce() {
    CHECK(Count(kLauncher, "host_.Show();") == 1);
    CHECK(InOrder(kLoop, {"if (!shown) {", "host_.Show();", "shown = true;", "sotb_startup_phase(\"launcher running\");"}));
    CHECK(Contains(kShow, "shown_ = true;"));
    // The old empty pre-show frame is gone: nothing is drawn between Open and the loop.
    const std::string before_loop = Between(kRun, "host_.Open(options)", "while (true) {");
    CHECK(!Contains(before_loop, "host_.BeginFrame()") && !Contains(before_loop, "host_.EndFrame(") && !Contains(before_loop, "host_.Show()"));
}

void TestNoVisibleReposition() {
    // Placement, maximized and fullscreen are applied while hidden; nothing moves or resizes the window later.
    CHECK(!Contains(kHost, "SDL_SetWindowPosition") && !Contains(kLauncher, "SDL_SetWindowPosition"));
    CHECK(!Contains(kHost, "SDL_SetWindowSize(") && !Contains(kLauncher, "SDL_SetWindowSize("));
    CHECK(!Contains(kShow, "SDL_MaximizeWindow") && !Contains(kShow, "SDL_SetWindowFullscreen"));
    CHECK(Contains(kOpen, "SDL_SetWindowFullscreen(window_, true);"));
}

void TestFailureBeforeReady() {
    // A failed Open returns before anything is drawn or shown.
    CHECK(InOrder(kRun, {"if (!host_.Open(options)) {", "return SOTB_LAUNCHER_BLOCKED;", "host_.Show();"}));
    // Play, Quit and an update restart on the first frame leave before Show: the window never flashes.
    CHECK(InOrder(kLoop, {"if (update_restart) {", "break;", "if (result == trg::Result::kPlay) {", "break;",
                          "if (result == trg::Result::kQuit", "break;", "if (!shown) {"}));
}

void TestRendererFailurePath() {
    // No renderer: reported once, everything closed, false returned before ImGui or the ready stage.
    CHECK(InOrder(kOpen, {"if (!renderer_) {", "RendererFailedLine(plan, selection)", "Close();", "return false;",
                          "ImGui::CreateContext()", "launcher renderer ready"}));
    CHECK(InOrder(kOpen, {"if (!window_) {", "Close();", "return false;", "launcher window created"}));
}

void TestStageOrder() {
    CHECK(InOrder(kRun, {"sotb_startup_phase(\"launcher opening\")", "host_.Open(options)", "sotb_startup_phase(\"launcher first frame\")",
                         "sotb_startup_phase(\"launcher running\")", "sotb_startup_phase(\"launcher loop ended\")"}));
    CHECK(Count(kLauncher, "\"launcher first frame\"") == 1 && Count(kLauncher, "\"launcher running\"") == 1);
    CHECK(Count(kHost, "\"launcher window created\"") == 1 && Count(kHost, "\"launcher renderer ready\"") == 1);
}

void TestNoPerFrameStages() {
    // The only stage call inside the loop is the guarded one that runs once.
    CHECK(Count(kLoop, "sotb_startup_phase(") == 1);
    const std::string per_frame = Between(kHost, "FrameGate LauncherHost::NextFrame()", "bool LauncherHost::Fullscreen()");
    CHECK(!per_frame.empty() && !Contains(per_frame, "sotb_startup_phase("));
    CHECK(Contains(per_frame, "sotb_startup_phase_current()"));
}

void TestExistingStagesKept() {
    for (const char* stage : {"launcher host closing", "launcher destroying renderer", "launcher destroying window", "launcher SDL shutdown",
                              "launcher host closed"}) {
        CHECK(Count(kHost, stage) == 1);
    }
    CHECK(Contains(kLauncher, "sotb_startup_phase(\"launcher opening\");") && Contains(kLauncher, "sotb_startup_phase(\"launcher loop ended\");"));
    const std::string loader = ReadAll(kRoot / "src/loader.c");
    CHECK(InOrder(loader, {"sotb_startup_phase(\"launcher handoff\")", "sotb_startup_phase(\"launcher returned\")"}));
}

void TestStageStore() {
    // The stage infrastructure the launcher reuses: a repeated stage is not a change, each new one is current.
    const char* sequence[] = {"launcher opening", "launcher window created", "launcher renderer ready", "launcher first frame",
                              "launcher running"};
    for (const char* stage : sequence) {
        sotb_startup_phase(stage);
        CHECK(std::strcmp(sotb_startup_phase_current(), stage) == 0);
    }
    sotb_startup_phase("launcher running");
    sotb_startup_phase("");
    sotb_startup_phase(nullptr);
    CHECK(std::strcmp(sotb_startup_phase_current(), "launcher running") == 0);
    int reached = 0;
    sotb_startup_phase_once(&reached, "launcher loop ended");
    sotb_startup_phase("launcher host closed");
    sotb_startup_phase_once(&reached, "launcher loop ended");
    CHECK(reached == 1 && std::strcmp(sotb_startup_phase_current(), "launcher host closed") == 0);
}

} // namespace

int main() {
    TestSourcesRead();
    TestStartsHidden();
    TestReadyBeforeVisible();
    TestShownOnce();
    TestNoVisibleReposition();
    TestFailureBeforeReady();
    TestRendererFailurePath();
    TestStageOrder();
    TestNoPerFrameStages();
    TestExistingStagesKept();
    TestStageStore();
    std::printf("launcher startup tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

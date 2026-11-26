// Launcher startup: the window stays hidden until a complete launcher frame was presented, its placement is final
// before it is shown, it is shown once, the launcher's startup stages are reached once each in a fixed order, and
// Vulkan is asked for the graphics cards only after the visible launcher has drawn a few frames.
// Source wiring, the stage store and the graphics card policy; no window, no Vulkan.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "graphics_cards.h"
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
const std::string kWindow = Between(kHost, "SDL_Window* LauncherHost::OpenWindow(", "std::string LauncherHost::ProbeFirstFrame()");
const std::string kAttempt = Between(kHost, "RendererAttempt LauncherHost::OpenAttempt(", "bool LauncherHost::Open(");
const std::string kOpen = Between(kHost, "bool LauncherHost::Open(", "void LauncherHost::LogPlacement(");
const std::string kShow = Between(kHost, "void LauncherHost::Show()", "ImTextureID LauncherHost::CreateTexture(");
const std::string kRun = Between(kLauncher, "SotbLauncherResult Show(bool ready)", "std::filesystem::path InstallDir() const");
const std::string kLoop = Between(kRun, "while (true) {", "sotb_startup_phase(\"launcher loop ended\");");
const std::string kDetect = Between(kLauncher, "void DetectGraphicsCards() {", "void GraphicsCardCombo() {");
const std::string kCombo = Between(kLauncher, "void GraphicsCardCombo() {", "void NormalizeGraphicsCard() {");

void TestSourcesRead() {
    CHECK(!kHost.empty() && !kLauncher.empty());
    CHECK(!kWindow.empty() && !kAttempt.empty() && !kOpen.empty() && !kShow.empty() && !kRun.empty() && !kLoop.empty());
    CHECK(!kDetect.empty() && !kCombo.empty());
}

void TestStartsHidden() {
    CHECK(Contains(kWindow, "SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);"));
    CHECK(Count(kHost, "SDL_ShowWindow(") == 1 && Contains(kShow, "SDL_ShowWindow(window_);"));
    for (const std::string* part : {&kWindow, &kAttempt, &kOpen}) {
        CHECK(!Contains(*part, "SDL_ShowWindow") && !Contains(*part, "SDL_RaiseWindow"));
    }
    // The first-frame probe of a renderer attempt presents into the hidden window only.
    CHECK(InOrder(kAttempt, {"window_ = OpenWindow(options, geometry);", "ProbeFirstFrame()"}));
}

void TestReadyBeforeVisible() {
    // Window, placement, renderer and ImGui all come from Open and its attempts, which never show the window.
    CHECK(InOrder(kOpen, {"ChoosePlacement(", "sotb_startup_phase(\"launcher renderer selection\")", "SelectRenderer(",
                          "OpenAttempt(options, geometry, name, pinned)", "if (!selection.created) {", "LogPlacement(",
                          "ConfigureVsync();", "SDL_MaximizeWindow(window_)", "sotb_startup_phase(\"launcher renderer ready\")",
                          "return true;"}));
    CHECK(InOrder(kAttempt, {"OpenWindow(", "sotb_startup_phase(\"launcher window created\")", "SDL_CreateRenderer(",
                             "ProbeFirstFrame()", "ImGui::CreateContext()", "ImGui_ImplSDLRenderer3_Init(renderer_)",
                             "RendererAttempt::Opened("}));
    CHECK(Contains(kWindow, "SDL_CreateWindowWithProperties(props)"));
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
    // A window that a failed attempt opened is destroyed, never moved; the next attempt opens its own.
    CHECK(!Contains(kAttempt, "SDL_SetWindowPosition") && !Contains(kAttempt, "SDL_SetWindowSize("));
}

void TestFailureBeforeReady() {
    // A failed Open returns before anything is drawn or shown, tells the player why and never starts the game.
    CHECK(InOrder(kRun, {"if (!host_.Open(options)) {", "ShowStartupError(options.title, host_.FailureText()",
                         "return SOTB_LAUNCHER_BLOCKED;", "host_.Show();"}));
    CHECK(Contains(ReadAll(kRoot / "src/loader.c"), "if (launched == SOTB_LAUNCHER_BLOCKED) exit(1);"));
    CHECK(Count(kHost, "void ShowStartupError(") == 2 && Contains(kHost, "MessageBoxW(") && Contains(kHost, "SDL_ShowSimpleMessageBox("));
    // Play, Quit and an update restart on the first frame leave before Show: the window never flashes.
    CHECK(InOrder(kLoop, {"if (update_restart) {", "break;", "if (result == trg::Result::kPlay) {", "break;",
                          "if (result == trg::Result::kQuit", "break;", "if (!shown) {"}));
}

void TestRendererFailurePath() {
    // No renderer: reported once, everything closed through Fail, false returned before the ready stage.
    CHECK(InOrder(kOpen, {"if (!selection.created) {", "RendererFailedLine(plan, selection)",
                          "return Fail(RendererFailureText(selection));", "launcher renderer ready"}));
    // SDL video or the displays failing also leave the reason for the player.
    CHECK(InOrder(kOpen, {"if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {", "return Fail(", "if (displays.empty()) {", "return Fail(",
                          "SelectRenderer("}));
    // Each attempt step names its own stage; the window comes first so nothing is created without one.
    CHECK(InOrder(kAttempt, {"if (!window_) {", "RendererStage::kWindow", "launcher window created", "RendererStage::kPin",
                             "if (!renderer_) {", "RendererStage::kCreate", "RendererStage::kFirstFrame", "if (!platform_backend_) {",
                             "RendererStage::kBackend", "if (!renderer_backend_) {", "RendererStage::kBackend"}));
}

void TestStageOrder() {
    CHECK(InOrder(kRun, {"sotb_startup_phase(\"launcher opening\")", "host_.Open(options)", "sotb_startup_phase(\"launcher first frame\")",
                         "sotb_startup_phase(\"launcher running\")", "sotb_startup_phase(\"launcher loop ended\")"}));
    // "launcher running" is set when the window shows, and again after the one graphics card detection.
    CHECK(Count(kLauncher, "\"launcher first frame\"") == 1 && Count(kLauncher, "\"launcher running\"") == 2);
    CHECK(Count(kDetect, "\"launcher running\"") == 1);
    CHECK(InOrder(kOpen, {"\"launcher renderer selection\"", "\"launcher renderer ready\""}));
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

void TestGraphicsProbeTiming() {
    using namespace Sotb;
    CHECK(kGraphicsProbeAfterFrames == 3 && kGraphicsProbeAfterSeconds == 0.5);
    CHECK(!GraphicsProbeDue(0, 0.0) && !GraphicsProbeDue(1, 0.02));
    CHECK(!GraphicsProbeDue(kGraphicsProbeAfterFrames - 1, kGraphicsProbeAfterSeconds + 1.0));
    CHECK(!GraphicsProbeDue(kGraphicsProbeAfterFrames + 100, kGraphicsProbeAfterSeconds - 0.001));
    CHECK(GraphicsProbeDue(kGraphicsProbeAfterFrames, kGraphicsProbeAfterSeconds));
    CHECK(GraphicsProbeDue(kGraphicsProbeAfterFrames + 1, kGraphicsProbeAfterSeconds + 3.0));
}

void TestGraphicsProbeDeferred() {
    // Nothing asks Vulkan before the window shows: not Open, not the setup before the loop, not the first frame.
    const std::string before_show = Between(kRun, "SotbLauncherResult Show(bool ready)", "host_.Show();");
    for (const char* vulkan : {"sotb_gpu_names", "sotb_check_vulkan", "sotb_check_renderer", "check_graphics", "DetectGraphicsCards"}) {
        CHECK(!Contains(before_show, vulkan) && !Contains(kHost, vulkan));
    }
    CHECK(Count(kLauncher, "sotb_gpu_names(gpu_names, 16)") == 1 && Contains(kDetect, "sotb_gpu_names(gpu_names, 16)"));
    CHECK(Count(kLauncher, "DetectGraphicsCards();") == 1);
    // Detection waits for complete frames after the window shows, and runs once.
    CHECK(InOrder(kLoop, {"host_.EndFrame(LauncherTheme().bg);", "if (!shown) {", "host_.Show();", "shown_at = ",
                          "} else if (graphics_cards_ == GraphicsCardsState::kPending &&", "GraphicsProbeDue(++frames_shown,",
                          "DetectGraphicsCards();"}));
    CHECK(Contains(kLauncher, "GraphicsCardsState graphics_cards_ = GraphicsCardsState::kPending;"));
    CHECK(!Contains(kDetect, "kPending"));
    // Software recovery leaves Vulkan alone; otherwise the probe is a stage of its own and one bounded line follows.
    CHECK(InOrder(kDetect, {"LauncherSoftwareRequested(std::getenv(kLauncherRendererVariable))", "GraphicsCardsState::kSkipped",
                            "sotb_startup_phase(\"launcher graphics card detection\")", "sotb_gpu_names(", "GraphicsCardsState::kProbed",
                            "sotb_startup_phase(\"launcher running\")", "GraphicsCardsLine(graphics_cards_, gpu_names_.size())"}));
}

void TestVulkanUnavailable() {
    using namespace Sotb;
    // No card from Vulkan leaves the row with a reason and every other setting as it was; the saved card is never
    // replaced by an empty list.
    CHECK(std::string(GraphicsCardsProblem(GraphicsCardsState::kProbed, 0)) == "No graphics card found");
    CHECK(std::string(GraphicsCardsProblem(GraphicsCardsState::kSkipped, 0)).find("software rendering") != std::string::npos);
    CHECK(std::string(GraphicsCardsProblem(GraphicsCardsState::kPending, 0)).empty());
    CHECK(std::string(GraphicsCardsProblem(GraphicsCardsState::kProbed, 2)).empty());
    CHECK(Contains(kLauncher, "if (found >= 0 && gpu_names_[found] != current) {"));
    // Before detection the row shows the saved choice, disabled, in the place the list will take.
    CHECK(GraphicsCardSavedText("") == "First available" && GraphicsCardSavedText("automatic") == "First available");
    CHECK(GraphicsCardSavedText("AMD Radeon RX 7800 XT") == "AMD Radeon RX 7800 XT");
    CHECK(InOrder(kCombo, {"if (graphics_cards_ == GraphicsCardsState::kPending) {", "ImGui::BeginDisabled();",
                           "ImGui::BeginCombo(\"##graphics_card\", GraphicsCardSavedText(settings_.Get(kGraphicsCardKey))",
                           "ImGui::EndDisabled();", "return;", "GraphicsCardsProblem(graphics_cards_, 0)",
                           "ImGui::BeginCombo(\"##graphics_card\", current.c_str())"}));
    CHECK(GraphicsCardsLine(GraphicsCardsState::kProbed, 0) == "Launcher: graphics cards: 0 usable");
    CHECK(GraphicsCardsLine(GraphicsCardsState::kProbed, 2) == "Launcher: graphics cards: 2 usable");
    CHECK(GraphicsCardsLine(GraphicsCardsState::kSkipped, 0).find("software launcher renderer requested") != std::string::npos);
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
    TestGraphicsProbeTiming();
    TestGraphicsProbeDeferred();
    TestVulkanUnavailable();
    TestStageStore();
    std::printf("launcher startup tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

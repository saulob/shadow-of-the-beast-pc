// Launcher renderer policy: which SDL renderer is asked for, fallback, release of a failed attempt, failure and
// reporting as a pure model, plus the source wiring that prefers Direct3D 11 on Windows with its module pins. No window,
// no SDL, no GPU.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "launcher_renderer.h"
#include "test_progress.h"

namespace fs = std::filesystem;
using namespace Sotb;

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
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

std::string Name(const RendererPlan& plan, size_t i) { return std::string(plan.names[i]); }

void TestDefaultPolicy() {
    for (const char* value : {static_cast<const char*>(nullptr), "", "direct3d11", "Direct3D11", "DIRECT3D11"}) {
        const RendererPlan windows = PlanLauncherRenderer(true, value);
        CHECK(windows.count == 2 && Name(windows, 0) == "direct3d11" && Name(windows, 1) == "software");
        CHECK(!windows.ignored_request);
    }
    const RendererPlan other = PlanLauncherRenderer(false, nullptr);
    CHECK(other.count == 2 && Name(other, 0).empty() && Name(other, 1) == "software");
    CHECK(!other.ignored_request);
}

void TestSoftwareStaysAvailable() {
    const RendererPlan windows = PlanLauncherRenderer(true, "software");
    CHECK(windows.count == 1 && Name(windows, 0) == "software" && !windows.ignored_request);
    const RendererPlan other = PlanLauncherRenderer(false, "SOFTWARE");
    CHECK(other.count == 1 && Name(other, 0) == "software" && !other.ignored_request);
    for (const bool windows_platform : {true, false}) {
        for (const char* value : {static_cast<const char*>(nullptr), "direct3d11", "bogus"}) {
            const RendererPlan plan = PlanLauncherRenderer(windows_platform, value);
            CHECK(plan.count == 2 && Name(plan, 1) == "software");
        }
    }
}

void TestDirect3D11OnlyOnWindows() {
    const RendererPlan elsewhere = PlanLauncherRenderer(false, "direct3d11");
    CHECK(elsewhere.count == 2 && Name(elsewhere, 0).empty() && Name(elsewhere, 1) == "software" && elsewhere.ignored_request);
    for (const char* value : {static_cast<const char*>(nullptr), "", "software", "opengl"}) {
        const RendererPlan plan = PlanLauncherRenderer(false, value);
        for (size_t i = 0; i < plan.count; ++i) {
            CHECK(Name(plan, i) != "direct3d11");
        }
    }
}

void TestUnsafeRequestsRejected() {
    for (const char* value : {"direct3d12", "direct3d", "opengl", "opengles2", "vulkan", "gpu", "metal", "garbage", " direct3d11",
                              "direct3d11 ", "direct3d11,software", "d3d11", "direct3d11\n"}) {
        const RendererPlan plan = PlanLauncherRenderer(true, value);
        CHECK(plan.count == 2 && Name(plan, 0) == "direct3d11" && Name(plan, 1) == "software");
        CHECK(plan.ignored_request);
    }
}

void TestWindowsPreference() {
    // Only Direct3D 11 and software can be on the Windows plan, and Direct3D 11 is always first with software behind it.
    for (const char* value : {static_cast<const char*>(nullptr), "", "software", "opengl", "vulkan", "direct3d12", "x", "direct3d11"}) {
        const RendererPlan plan = PlanLauncherRenderer(true, value);
        for (size_t i = 0; i < plan.count; ++i) {
            CHECK(Name(plan, i) == "direct3d11" || Name(plan, i) == "software");
            CHECK(Name(plan, i) != "direct3d11" || (i == 0 && plan.count == 2 && Name(plan, 1) == "software"));
        }
    }
    const std::vector<std::string_view> pinned = RendererModulesToPin("direct3d11");
    CHECK(pinned.size() == 2 && pinned[0] == "d3d11.dll" && pinned[1] == "dxgi.dll");
    CHECK(RendererModulesToPin("software").empty() && RendererModulesToPin("").empty() && RendererModulesToPin("opengl").empty());
}

// A fake host: each renderer either opens, or fails at a given stage after opening what comes before that stage.
// `open` counts what is alive, `discard` releases it, so a leak or a double release shows up.
struct Attempts {
    std::vector<std::string> tried;
    std::vector<std::string> succeed;  // names that can be created
    RendererStage fails_at = RendererStage::kCreate;
    int alive = 0;
    int discards = 0;
    int leaked = 0;

    RendererAttempt Open(std::string_view name) {
        tried.emplace_back(name);
        leaked += alive;
        for (const std::string& ok : succeed) {
            if (ok == name || (name.empty() && ok == "default")) {
                alive = 5;  // window, renderer, context and both backends
                return RendererAttempt::Opened(name, name.empty() ? "default" : name);
            }
        }
        alive = static_cast<int>(fails_at) + 1;
        return RendererAttempt::Failed(name, fails_at, "no device");
    }
    void Discard() {
        ++discards;
        alive = 0;
    }
};

RendererSelection Select(const RendererPlan& plan, Attempts& attempts) {
    return SelectRenderer(plan, [&](std::string_view name) { return attempts.Open(name); }, [&] { attempts.Discard(); });
}

void TestSelection() {
    const RendererPlan standard = PlanLauncherRenderer(true, nullptr);

    Attempts first{{}, {"direct3d11", "software"}};
    RendererSelection selection = Select(standard, first);
    CHECK(selection.created && selection.actual == "direct3d11" && !selection.fallback && selection.failures == 0);
    CHECK(first.tried.size() == 1 && first.discards == 0 && selection.attempts.size() == 1);

    Attempts fallback{{}, {"software"}};
    selection = Select(standard, fallback);
    CHECK(selection.created && selection.actual == "software" && selection.fallback && selection.failures == 1);
    CHECK(fallback.tried.size() == 2 && fallback.tried[0] == "direct3d11" && fallback.tried[1] == "software");
    CHECK(selection.attempts.size() == 2 && !selection.attempts[0].ok() && selection.attempts[1].ok());
    CHECK(selection.attempts[0].requested == "direct3d11" && selection.attempts[1].requested == "software");

    const RendererPlan forced = PlanLauncherRenderer(true, "software");
    Attempts software{{}, {"software", "direct3d11"}};
    selection = Select(forced, software);
    CHECK(selection.created && selection.actual == "software" && !selection.fallback && software.tried.size() == 1);

    Attempts nothing{{}, {}};
    selection = Select(forced, nothing);
    CHECK(!selection.created && selection.failures == 1 && nothing.tried.size() == 1);
    Attempts nothing_standard{{}, {}};
    selection = Select(standard, nothing_standard);
    CHECK(!selection.created && selection.failures == 2 && nothing_standard.tried.size() == 2);

    const RendererPlan unix_default = PlanLauncherRenderer(false, nullptr);
    Attempts unix_software{{}, {"software"}};
    selection = Select(unix_default, unix_software);
    CHECK(selection.created && selection.fallback && selection.actual == "software" && unix_software.tried.size() == 2);
    CHECK(selection.attempts[0].requested.empty());
    Attempts unix_first{{}, {"default"}};
    selection = Select(unix_default, unix_first);
    CHECK(selection.created && !selection.fallback && selection.actual == "default");
}

void TestPartialAttemptsReleased() {
    // Whatever stage an attempt fails at, it is released once before the next attempt starts, and the opened renderer
    // is never released.
    for (const bool windows : {true, false}) {
        for (const RendererStage stage : {RendererStage::kWindow, RendererStage::kPin, RendererStage::kCreate,
                                          RendererStage::kFirstFrame, RendererStage::kBackend}) {
            Attempts fallback{{}, {"software"}, stage};
            const RendererSelection selection = Select(PlanLauncherRenderer(windows, nullptr), fallback);
            CHECK(selection.created && selection.fallback && fallback.discards == 1 && fallback.leaked == 0);
            CHECK(fallback.alive == 5 && selection.attempts[0].stage == stage);

            Attempts none{{}, {}, stage};
            const RendererSelection failed = Select(PlanLauncherRenderer(windows, nullptr), none);
            CHECK(!failed.created && none.discards == 2 && none.leaked == 0 && none.alive == 0);
        }
    }
}

void TestNoLoops() {
    for (const bool windows : {true, false}) {
        for (const char* value : {static_cast<const char*>(nullptr), "software", "direct3d11", "bogus"}) {
            const RendererPlan plan = PlanLauncherRenderer(windows, value);
            Attempts none{{}, {}};
            Select(plan, none);
            CHECK(none.tried.size() == plan.count && plan.count <= 2 && none.discards == int(plan.count));
            for (size_t i = 0; i < none.tried.size(); ++i) {
                for (size_t j = i + 1; j < none.tried.size(); ++j) {
                    CHECK(none.tried[i] != none.tried[j]);
                }
            }
        }
    }
}

void TestClassification() {
    CHECK(ClassifyRenderer("software") == RendererKind::kSoftware);
    for (const char* name : {"direct3d11", "direct3d12", "direct3d", "opengl", "opengles2", "vulkan", "gpu", "metal"}) {
        CHECK(ClassifyRenderer(name) == RendererKind::kAccelerated);
    }
    CHECK(ClassifyRenderer("") == RendererKind::kUnknown && ClassifyRenderer("something") == RendererKind::kUnknown);
    CHECK(std::string(RendererKindName(RendererKind::kSoftware)) == "software");
    CHECK(std::string(RendererKindName(RendererKind::kAccelerated)) == "accelerated");
    CHECK(std::string(RendererKindName(RendererKind::kUnknown)) == "unknown");
}

void TestReporting() {
    const RendererPlan standard = PlanLauncherRenderer(true, nullptr);
    RendererSelection selection;
    selection.created = true;
    selection.actual = "direct3d11";
    std::string line = RendererSelectedLine(standard, selection, 2);
    CHECK(Contains(line, "renderer_selected requested=direct3d11 actual=direct3d11 class=accelerated fallback=0 failures=0"));
    CHECK(Contains(line, "ignored_request=0 pinned_modules=2") && !Contains(line, "experimental"));

    selection.fallback = true;
    selection.failures = 1;
    selection.actual = "software";
    line = RendererSelectedLine(standard, selection, 2);
    CHECK(Contains(line, "requested=direct3d11 actual=software class=software fallback=1 failures=1 ignored_request=0"));

    const RendererPlan forced = PlanLauncherRenderer(true, "software");
    selection.fallback = false;
    selection.failures = 0;
    line = RendererSelectedLine(forced, selection, 0);
    CHECK(Contains(line, "requested=software actual=software class=software fallback=0") && Contains(line, "pinned_modules=0"));

    const RendererPlan ignored = PlanLauncherRenderer(true, "vulkan");
    CHECK(Contains(RendererSelectedLine(ignored, selection, 0), "ignored_request=1"));

    RendererSelection failed;
    failed.failures = 2;
    line = RendererFailedLine(standard, failed);
    CHECK(line == "Launcher telemetry: renderer_failed requested=direct3d11 tried=2");
    CHECK(RendererFailedLine(PlanLauncherRenderer(false, nullptr), failed) == "Launcher telemetry: renderer_failed requested=default tried=2");

    selection.actual = "C:\\Users\\x\\r\nline";
    line = RendererSelectedLine(standard, selection, 0);
    CHECK(line.find('\n') == std::string::npos && line.find('\\') == std::string::npos && Count(line, ":") == 1);
    CHECK(line.size() < 320 && RendererLabel("") == "unknown" && RendererLabel("Direct3D 11") == "direct3d_11");
}

void TestAttemptReporting() {
    const RendererPlan standard = PlanLauncherRenderer(true, nullptr);
    Attempts fallback{{}, {"software"}, RendererStage::kFirstFrame};
    RendererSelection selection = Select(standard, fallback);
    CHECK(RendererFallbackReason(selection) == "direct3d11_first_frame");
    CHECK(Contains(RendererSelectedLine(standard, selection, 2), "fallback=1 failures=1 ignored_request=0 pinned_modules=2 fallback_reason=direct3d11_first_frame"));
    CHECK(RendererAttemptLine(selection.attempts[0]) ==
          "Launcher telemetry: renderer_attempt requested=direct3d11 result=failed stage=first_frame reason=no device");

    Attempts first{{}, {"direct3d11"}};
    selection = Select(standard, first);
    CHECK(RendererFallbackReason(selection) == "none" && Contains(RendererSelectedLine(standard, selection, 2), "fallback_reason=none"));

    const char* stages[] = {"window", "pin", "create", "first_frame", "imgui_backend"};
    int i = 0;
    for (const RendererStage stage : {RendererStage::kWindow, RendererStage::kPin, RendererStage::kCreate,
                                      RendererStage::kFirstFrame, RendererStage::kBackend}) {
        CHECK(std::string(RendererStageName(stage)) == stages[i++]);
    }

    // Driver and SDL text is one bounded line of printable ASCII.
    const std::string noisy = "C:\\Windows\\System32\\d3d11.dll\r\nfailed\t" + std::string(500, 'x');
    const RendererAttempt attempt = RendererAttempt::Failed("", RendererStage::kCreate, noisy);
    CHECK(attempt.reason.size() == kRendererReasonMax && attempt.reason.find('\\') == std::string::npos);
    const std::string attempt_line = RendererAttemptLine(attempt);
    CHECK(attempt_line.size() < 320 && attempt_line.find('\n') == std::string::npos && attempt_line.find('\r') == std::string::npos);
    CHECK(Contains(attempt_line, "requested=default result=failed stage=create"));
    CHECK(RendererReason("") == "unknown error");

    // Every renderer failed: the player is told each one and why, in bounded text.
    Attempts none{{}, {}, RendererStage::kCreate};
    selection = Select(standard, none);
    const std::string text = RendererFailureText(selection);
    CHECK(Contains(text, "could not start its window renderer") && Contains(text, "\ndirect3d11 (create): no device") &&
          Contains(text, "\nsoftware (create): no device"));
    CHECK(text.size() < 512);
}

void TestAttemptPhases() {
    // A process that ends inside an attempt leaves the renderer it was creating as its startup stage.
    CHECK(std::string(RendererAttemptPhase("direct3d11")) == "launcher renderer direct3d11");
    CHECK(std::string(RendererAttemptPhase("software")) == "launcher renderer software");
    CHECK(std::string(RendererAttemptPhase("")) == "launcher renderer default");
    CHECK(std::string(RendererAttemptPhase("opengl")) == "launcher renderer other");
}

void TestSoftwareRecoveryRequest() {
    for (const char* value : {"software", "SOFTWARE", "Software"}) {
        CHECK(LauncherSoftwareRequested(value));
    }
    for (const char* value : {static_cast<const char*>(nullptr), "", "direct3d11", "soft", "software "}) {
        CHECK(!LauncherSoftwareRequested(value));
    }
    for (const bool windows : {true, false}) {
        const RendererPlan plan = PlanLauncherRenderer(windows, "software");
        CHECK(plan.count == 1 && plan.names[0] == "software");
    }
}

void TestSeparateFromGameRenderer() {
    const fs::path root = SOTB_SOURCE_DIR;
    const std::string host = ReadAll(root / "launcher/host.cpp");
    const std::string policy = ReadAll(root / "launcher/launcher_renderer.h");
    const std::string launcher = ReadAll(root / "launcher/launcher.cpp");
    // The launcher's renderer never reads or writes the game's Renderer setting, and the recovery request lives only in
    // the environment of that run: never saved, never read by the game.
    CHECK(!Contains(host, "kRendererKey") && !Contains(policy, "kRendererKey") && !Contains(host, "settings"));
    CHECK(!Contains(host, "Vulkan") && !Contains(host, "vulkan") && !Contains(host, "sotb_gpu_names"));
    for (const char* file : {"launcher/settings.cpp", "launcher/settings.h", "gpu/shim/sotb_settings.cpp", "gpu/shim/sotbgpu.cpp",
                             "src/loader.c", "launcher/update_restart_arguments.h"}) {
        const std::string text = ReadAll(root / file);
        CHECK(!text.empty() && !Contains(text, "SOTB_LAUNCHER_RENDERER") && !Contains(text, "kLauncherRendererVariable"));
    }
    CHECK(Count(launcher, "kLauncherRendererVariable") == 1 && !Contains(launcher, "SetEnvironmentVariable") &&
          !Contains(launcher, "setenv(") && !Contains(launcher, "SDL_setenv"));
}

void TestSourceWiring() {
    const fs::path root = SOTB_SOURCE_DIR;
    const std::string host = ReadAll(root / "launcher/host.cpp");
    const std::string launcher = ReadAll(root / "launcher/launcher.cpp");
    const std::string policy = ReadAll(root / "launcher/launcher_renderer.h");
    CHECK(!host.empty() && !launcher.empty() && !policy.empty());

    // One creation site, in the attempt that Open runs once per plan entry; never per frame and never from the loop.
    CHECK(Count(host, "SDL_CreateRenderer(") == 1 && Count(host, "SDL_CreateWindowWithProperties(") == 1);
    CHECK(Count(host, "SelectRenderer(") == 1 && Count(host, "OpenAttempt(options, geometry, name, pinned)") == 1);
    CHECK(Count(host, "[this] { ReleaseWindow(false); }") == 1);
    CHECK(!Contains(launcher, "SDL_CreateRenderer") && !Contains(launcher, "SelectRenderer") && !Contains(launcher, "PlanLauncherRenderer"));
    const size_t begin = host.find("void LauncherHost::BeginFrame()");
    CHECK(begin != std::string::npos && host.find("SDL_CreateRenderer", begin) == std::string::npos);

    // An attempt opens its window, pins, creates the renderer, draws a first frame and starts the ImGui backends, and
    // every step can fail it.
    const std::string attempt = host.substr(host.find("RendererAttempt LauncherHost::OpenAttempt("),
                                            host.find("bool LauncherHost::Open(") - host.find("RendererAttempt LauncherHost::OpenAttempt("));
    size_t at = 0;
    for (const char* step : {"window_ = OpenWindow(options, geometry);", "RendererStage::kWindow", "sotb_startup_phase(RendererAttemptPhase(name));",
                             "PinRendererModules(name, pinned)", "RendererStage::kPin", "SDL_CreateRenderer(window_", "RendererStage::kCreate",
                             "ProbeFirstFrame()", "RendererStage::kFirstFrame", "platform_backend_ = ImGui_ImplSDL3_InitForSDLRenderer(",
                             "RendererStage::kBackend", "renderer_backend_ = ImGui_ImplSDLRenderer3_Init(renderer_);", "RendererStage::kBackend",
                             "return RendererAttempt::Opened("}) {
        at = attempt.find(step, at);
        CHECK(at != std::string::npos);
        at = at == std::string::npos ? 0 : at + 1;
    }
    const std::string probe = host.substr(host.find("std::string LauncherHost::ProbeFirstFrame()"),
                                          host.find("RendererAttempt LauncherHost::OpenAttempt(") - host.find("std::string LauncherHost::ProbeFirstFrame()"));
    for (const char* step : {"SDL_CreateTexture(", "SDL_UpdateTexture(", "SDL_RenderClear(", "SDL_RenderTexture(", "SDL_RenderPresent(",
                             "SDL_DestroyTexture(texture);"}) {
        CHECK(Contains(probe, step));
    }

    // The Windows plan is Direct3D 11 then software; the host asks no backend by name and holds no renderer flags.
    CHECK(Contains(host, "PlanLauncherRenderer(kWindows, std::getenv(kLauncherRendererVariable))"));
    CHECK(!Contains(host, "direct3d11") && !Contains(host, "SDL_SOFTWARE_RENDERER") && !Contains(host, "SDL_HINT_RENDER_DRIVER"));
    CHECK(Contains(policy, "{\"direct3d11\", \"software\"}") && Contains(policy, "SOTB_LAUNCHER_RENDERER"));
    CHECK(Contains(policy, "return {\"d3d11.dll\", \"dxgi.dll\"};"));
    CHECK(Contains(host, "GET_MODULE_HANDLE_EX_FLAG_PIN") && !Contains(host, "FreeLibrary") && !Contains(policy, "FreeLibrary"));
    CHECK(!Contains(host, "GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT"));
    CHECK(!Contains(host, "DEBUG_" "SAULO") && !Contains(policy, "DEBUG_" "SAULO") &&
          !Contains(ReadAll(root / "launcher/launcher_pacing.h"), "DEBUG_" "SAULO"));

    // One wait per frame, in the loop gate only; VSync is switched off only where it is not real, never on top of the cap.
    CHECK(Count(host, "SDL_DelayNS(") == 2 && !Contains(host, "SDL_Delay("));
    CHECK(Contains(host, "if (set && !state.effective) {\n        SDL_SetRenderVSync(renderer_, 0);"));
    CHECK(Contains(host, "ClassifyRenderer(renderer_name_) != RendererKind::kSoftware ||"));

    // Requested and actual renderer are reported once; each failed attempt once, flushed; a complete failure once, with
    // the text for the player, and the host closes.
    CHECK(Count(host, "RendererSelectedLine(plan, selection, pinned)") == 1 && Count(host, "RendererFailedLine(plan, selection)") == 1);
    CHECK(Count(host, "RendererAttemptLine(attempt)") == 1 && Contains(host, "if (!attempt.ok()) {"));
    const size_t failed = host.find("RendererFailedLine(plan, selection)");
    CHECK(host.find("return Fail(RendererFailureText(selection));", failed) < host.find("RendererSelectedLine(plan, selection, pinned)"));
    CHECK(Contains(host, "bool LauncherHost::Fail(std::string text) {\n    failure_text_ = std::move(text);\n    Close();\n    return false;"));

    // A module that cannot be pinned skips that renderer instead of creating it.
    const size_t pin = host.find("PinRendererModules(name, pinned); !unpinned.empty()");
    CHECK(pin != std::string::npos && pin < host.find("SDL_CreateRenderer(window"));

    // Release: textures, then the ImGui backends that started, context, renderer, window; Close adds SDL. No sleeps
    // and no process shortcuts.
    const size_t release = host.find("void LauncherHost::ReleaseWindow(bool closing)");
    const size_t textures = host.find("SDL_DestroyTexture(texture);", release);
    const size_t renderer_backend = host.find("if (renderer_backend_) {\n        ImGui_ImplSDLRenderer3_Shutdown();", textures);
    const size_t platform_backend = host.find("if (platform_backend_) {\n        ImGui_ImplSDL3_Shutdown();", renderer_backend);
    const size_t context = host.find("ImGui::DestroyContext(imgui_);", platform_backend);
    const size_t renderer = host.find("SDL_DestroyRenderer(renderer_);", context);
    const size_t window = host.find("SDL_DestroyWindow(window_);", renderer);
    const size_t close = host.find("void LauncherHost::Close()", window);
    const size_t quit = host.find("SDL_Quit();", close);
    CHECK(release != std::string::npos && textures != std::string::npos && renderer_backend != std::string::npos &&
          platform_backend != std::string::npos && context != std::string::npos && renderer != std::string::npos &&
          window != std::string::npos && close != std::string::npos && quit != std::string::npos);
    CHECK(host.find("ReleaseWindow(true);", close) < quit);
    const std::string teardown = host.substr(release, host.find("bool LauncherHost::PumpEvents()") - release);
    CHECK(!Contains(teardown.c_str(), "SDL_Delay") && !Contains(teardown.c_str(), "Sleep(") && !Contains(teardown.c_str(), "ExitProcess") &&
          !Contains(teardown.c_str(), "TerminateProcess") && !Contains(teardown.c_str(), "_exit(") && !Contains(teardown.c_str(), "quick_exit"));

    // The launcher closes before the game starts, and the game's window never creates an SDL renderer.
    const size_t loop_end = launcher.find("launcher loop ended");
    CHECK(loop_end != std::string::npos && launcher.find("host_.Close();", loop_end) != std::string::npos);
    CHECK(!Contains(ReadAll(root / "gpu/shim/window.cpp").c_str(), "SDL_CreateRenderer"));
}

} // namespace

int main() {
    TestDefaultPolicy();
    TestSoftwareStaysAvailable();
    TestDirect3D11OnlyOnWindows();
    TestUnsafeRequestsRejected();
    TestWindowsPreference();
    TestSelection();
    TestPartialAttemptsReleased();
    TestNoLoops();
    TestClassification();
    TestReporting();
    TestAttemptReporting();
    TestAttemptPhases();
    TestSoftwareRecoveryRequest();
    TestSeparateFromGameRenderer();
    TestSourceWiring();
    std::printf("launcher renderer tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

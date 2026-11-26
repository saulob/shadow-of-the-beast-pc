#include "host.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "launcher_renderer.h"
#include "sotb_displays.h"
#include "startup_phase.h"

namespace Sotb {
namespace {

Rect ToRect(const SDL_Rect& rect) {
    return {rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
}

#ifdef _WIN32
constexpr bool kWindows = true;

bool PinModule(std::string_view name) {
    const std::wstring wide(name.begin(), name.end());
    if (!LoadLibraryW(wide.c_str())) {
        return false;
    }
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, wide.c_str(), &pinned) != 0;
}

int64_t ThreadCpuNs() {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        return -1;
    }
    const auto ticks = [](const FILETIME& time) {
        return (static_cast<int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return (ticks(kernel) + ticks(user)) * 100;
}
#else
constexpr bool kWindows = false;

bool PinModule(std::string_view) {
    return false;
}

int64_t ThreadCpuNs() {
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
        return -1;
    }
    return static_cast<int64_t>(time.tv_sec) * kNsPerSecond + time.tv_nsec;
}
#endif

int64_t NowNs() {
    return static_cast<int64_t>(SDL_GetTicksNS());
}

// The first module that could not be kept loaded; empty when all are.
std::string PinRendererModules(std::string_view renderer, int& pinned) {
    for (const std::string_view module : RendererModulesToPin(renderer)) {
        if (!PinModule(module)) {
            return std::string(module);
        }
        ++pinned;
    }
    return {};
}

std::string SdlError(const char* what) {
    const char* error = SDL_GetError();
    return std::string(what) + ": " + (error && *error ? error : "unknown error");
}

} // namespace

#ifdef _WIN32
void ShowStartupError(const std::string& title, const std::string& text) {
    const auto wide = [](const std::string& utf8) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        std::wstring result(static_cast<size_t>(length > 0 ? length : 1), L'\0');
        if (length > 0) {
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), length);
        }
        return result;
    };
    MessageBoxW(nullptr, wide(text).c_str(), wide(title).c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}
#else
void ShowStartupError(const std::string& title, const std::string& text) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), nullptr);
}
#endif

struct LauncherHost::Geometry {
    int x = 0, y = 0, width = 0, height = 0;
    float scale = 1.0f;
};

bool LauncherHost::Fail(std::string text) {
    failure_text_ = std::move(text);
    Close();
    return false;
}

SDL_Window* LauncherHost::OpenWindow(const HostOptions& options, const Geometry& geometry) {
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, options.title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, geometry.x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, geometry.y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, geometry.width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, geometry.height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    SDL_Window* window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (window) {
        SDL_SetWindowMinimumSize(window, std::min(static_cast<int>(options.min_width * geometry.scale), geometry.width),
                                 std::min(static_cast<int>(options.min_height * geometry.scale), geometry.height));
    }
    return window;
}

// One texture upload, one clear, one draw and one present: what the first launcher frame needs from the renderer.
// Empty when all of them worked.
std::string LauncherHost::ProbeFirstFrame() {
    constexpr int kSize = 512;
    const std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4, 255);
    SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kSize, kSize);
    if (!texture) {
        return SdlError("texture");
    }
    std::string problem;
    if (!SDL_UpdateTexture(texture, nullptr, pixels.data(), kSize * 4)) {
        problem = SdlError("texture upload");
    } else if (!SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255) || !SDL_RenderClear(renderer_)) {
        problem = SdlError("clear");
    } else if (!SDL_RenderTexture(renderer_, texture, nullptr, nullptr)) {
        problem = SdlError("draw");
    } else if (!SDL_RenderPresent(renderer_)) {
        problem = SdlError("present");
    }
    SDL_DestroyTexture(texture);
    return problem;
}

RendererAttempt LauncherHost::OpenAttempt(const HostOptions& options, const Geometry& geometry, std::string_view name,
                                          int& pinned) {
    window_ = OpenWindow(options, geometry);
    if (!window_) {
        return RendererAttempt::Failed(name, RendererStage::kWindow, SdlError("window"));
    }
    sotb_startup_phase("launcher window created");
    sotb_startup_phase(RendererAttemptPhase(name));
    if (const std::string unpinned = PinRendererModules(name, pinned); !unpinned.empty()) {
        return RendererAttempt::Failed(name, RendererStage::kPin, "cannot keep " + unpinned + " loaded");
    }
    const std::string sdl_name(name);
    renderer_ = SDL_CreateRenderer(window_, sdl_name.empty() ? nullptr : sdl_name.c_str());
    if (!renderer_) {
        return RendererAttempt::Failed(name, RendererStage::kCreate, SdlError("renderer"));
    }
    const char* created = SDL_GetRendererName(renderer_);
    renderer_name_ = created && *created ? created : "unknown";
    if (const std::string problem = ProbeFirstFrame(); !problem.empty()) {
        return RendererAttempt::Failed(name, RendererStage::kFirstFrame, problem);
    }

    IMGUI_CHECKVERSION();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    platform_backend_ = ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    if (!platform_backend_) {
        return RendererAttempt::Failed(name, RendererStage::kBackend, "ImGui SDL backend");
    }
    renderer_backend_ = ImGui_ImplSDLRenderer3_Init(renderer_);
    if (!renderer_backend_) {
        return RendererAttempt::Failed(name, RendererStage::kBackend, "ImGui renderer backend");
    }
    return RendererAttempt::Opened(name, renderer_name_);
}

bool LauncherHost::Open(const HostOptions& options) {
    failure_text_.clear();
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        const std::string error = SdlError("SDL video failed");
        std::printf("Launcher: %s\n", error.c_str());
        return Fail("The launcher could not start: " + RendererReason(error));
    }
    video_ = true;
    gamepad_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    const std::vector<Display> displays = ListDisplays();
    if (displays.empty()) {
        std::printf("Launcher: no display found\n");
        return Fail("The launcher could not start: no display found.");
    }
    std::vector<DisplayArea> areas;
    for (const Display& display : displays) {
        areas.push_back({ToRect(display.usable), display.scale});
    }
    const PlacementChoice placement = ChoosePlacement(options.restore, areas, options.width, options.height);
    restored_ = placement.centered ? std::nullopt
                                   : std::optional<WindowPlacement>(
                                         WindowPlacement{placement.rect, placement.maximized, placement.fullscreen});
    const Display* target = &displays[placement.display];
    Geometry geometry;
    geometry.x = placement.centered ? int(SDL_WINDOWPOS_CENTERED_DISPLAY(target->id)) : placement.rect.left;
    geometry.y = placement.centered ? int(SDL_WINDOWPOS_CENTERED_DISPLAY(target->id)) : placement.rect.top;
    geometry.width = placement.rect.width();
    geometry.height = placement.rect.height();
    geometry.scale = target->scale;

    sotb_startup_phase("launcher renderer selection");
    const RendererPlan plan = PlanLauncherRenderer(kWindows, std::getenv(kLauncherRendererVariable));
    int pinned = 0;
    const RendererSelection selection = SelectRenderer(
        plan,
        [&](std::string_view name) {
            RendererAttempt attempt = OpenAttempt(options, geometry, name, pinned);
            if (!attempt.ok()) {
                std::printf("%s\n", RendererAttemptLine(attempt).c_str());
                std::fflush(stdout);
            }
            return attempt;
        },
        [this] { ReleaseWindow(false); });
    if (!selection.created) {
        std::printf("%s\n", RendererFailedLine(plan, selection).c_str());
        return Fail(RendererFailureText(selection));
    }
    LogPlacement(placement, areas, target->name);
    std::printf("%s\n", RendererSelectedLine(plan, selection, pinned).c_str());
    ConfigureVsync();

    hidden_ = options.hidden;
    gate_ = LoopGate(hidden_ ? 0 : kLauncherTargetHz);
    if (placement.maximized) {
        SDL_MaximizeWindow(window_);
    }
    if (placement.fullscreen) {
        SDL_SetWindowFullscreen(window_, true);
    }
    std::printf("Launcher: window %dx%d at %.2fx display scale, %s renderer\n", geometry.width, geometry.height,
                target->scale, renderer_name_.c_str());
    sotb_startup_phase("launcher renderer ready");
    return true;
}

void LauncherHost::LogPlacement(const PlacementChoice& placement, std::span<const DisplayArea> areas, const std::string& name) const {
    PlacementResult result;
    int x = 0, y = 0, width = 0, height = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    SDL_GetWindowSize(window_, &width, &height);
    result.final_rect = {x, y, x + width, y + height};
    SDL_GetWindowSizeInPixels(window_, &result.pixel_width, &result.pixel_height);
    result.window_scale = SDL_GetWindowDisplayScale(window_);
    std::printf("%s\n", PlacementLine(placement, areas, name, result).c_str());
}

// SDL only simulates VSync on a software renderer, so it stays on only when the display really paces; the frame cap
// paces the loop either way.
void LauncherHost::ConfigureVsync() {
    VsyncState state;
    state.requested = true;
    const bool set = SDL_SetRenderVSync(renderer_, 1);
    int value = 0;
    SDL_GetRenderVSync(renderer_, &value);
    int surface_value = 0;
    const bool real = ClassifyRenderer(renderer_name_) != RendererKind::kSoftware ||
                      (SDL_GetWindowSurfaceVSync(window_, &surface_value) && surface_value != 0);
    state.effective = set && value == 1 && real;
    if (set && !state.effective) {
        SDL_SetRenderVSync(renderer_, 0);
    }
    std::printf("%s\n", PacingModeLine(renderer_name_, state).c_str());
}

// The frame drawn while hidden is presented again once the window is visible, so its client area is never left
// unpainted until the next frame is due.
void LauncherHost::Show() {
    if (!window_ || hidden_ || shown_ || !drawn_) {
        return;
    }
    shown_ = true;
    SDL_ShowWindow(window_);
    SDL_RaiseWindow(window_);
    Submit(last_clear_);
    SDL_RenderPresent(renderer_);
}

ImTextureID LauncherHost::CreateTexture(const uint8_t* rgba, int width, int height) {
    if (!renderer_ || !rgba || width <= 0 || height <= 0) {
        return ImTextureID{};
    }
    SDL_Texture* texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture) {
        return ImTextureID{};
    }
    if (!SDL_UpdateTexture(texture, nullptr, rgba, width * 4)) {
        SDL_DestroyTexture(texture);
        return ImTextureID{};
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    textures_.push_back(texture);
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture));
}

void LauncherHost::DestroyTexture(ImTextureID texture) {
    SDL_Texture* sdl_texture = reinterpret_cast<SDL_Texture*>(static_cast<uintptr_t>(texture));
    const auto found = std::find(textures_.begin(), textures_.end(), sdl_texture);
    if (found != textures_.end()) {
        SDL_DestroyTexture(sdl_texture);
        textures_.erase(found);
    }
}

// Whatever a renderer attempt opened, newest first. Safe after an attempt that stopped part way.
void LauncherHost::ReleaseWindow(bool closing) {
    for (SDL_Texture* texture : textures_) {
        SDL_DestroyTexture(texture);
    }
    textures_.clear();
    if (renderer_backend_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        renderer_backend_ = false;
    }
    if (platform_backend_) {
        ImGui_ImplSDL3_Shutdown();
        platform_backend_ = false;
    }
    if (imgui_) {
        ImGui::DestroyContext(imgui_);
        imgui_ = nullptr;
    }
    if (closing) {
        sotb_startup_phase("launcher destroying renderer");
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (closing) {
        sotb_startup_phase("launcher destroying window");
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    renderer_name_.clear();
}

void LauncherHost::Close() {
    sotb_startup_phase("launcher host closing");
    ReleaseWindow(true);
    sotb_startup_phase("launcher SDL shutdown");
    if (video_ || gamepad_) {
        // Nothing else uses SDL yet: the game window thread starts it again from scratch.
        SDL_Quit();
        video_ = gamepad_ = false;
    }
    sotb_startup_phase("launcher host closed");
}

bool LauncherHost::PumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        switch (event.type) {
        case SDL_EVENT_QUIT:
            quit_ = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == SDL_GetWindowID(window_)) {
                quit_ = true;
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.key == SDLK_F11 && !event.key.repeat) {
                SetFullscreen(!Fullscreen());
            }
            break;
        default:
            break;
        }
    }
    return !quit_;
}

bool LauncherHost::Minimized() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
}

FrameGate LauncherHost::NextFrame() {
    for (;;) {
        const int64_t pumping = NowNs();
        if (!PumpEvents()) {
            return FrameGate::kQuit;
        }
        const int64_t polled = NowNs();
        NoteStall(LoopPhase::kEvents, polled - pumping, polled, false);
        const GateStep step = gate_.Step(polled, Minimized());
        switch (step.action) {
        case GateAction::kDraw:
            return FrameGate::kDraw;
        case GateAction::kIdle:
            monitor_.Reset();
            SDL_DelayNS(static_cast<Uint64>(step.sleep_ns));
            return FrameGate::kIdle;
        case GateAction::kSleep:
            SDL_DelayNS(static_cast<Uint64>(step.sleep_ns));
            monitor_.Waited(NowNs() - polled);
            break;
        }
    }
}

void LauncherHost::NoteFrameDrawn(int64_t now_ns, const FrameTimes& times) {
    if (gate_.pacer().hz() <= 0) {
        return;
    }
    const std::optional<LoopSample> sample = monitor_.FrameDrawn(now_ns, times, ThreadCpuNs);
    if (!sample) {
        return;
    }
    if (sample->first) {
        std::printf("%s\n", LoopSampleLine(renderer_name_, *sample).c_str());
    }
    if (sample->excessive_rate) {
        std::printf("%s\n", ExcessiveRateLine(renderer_name_, *sample).c_str());
    }
    if (sample->present_stall) {
        std::printf("%s\n", PresentStallLine(renderer_name_, *sample).c_str());
    }
    if (sample->high_cpu) {
        std::printf("%s\n", HighCpuLine(renderer_name_, *sample).c_str());
    }
}

void LauncherHost::BeginFrame() {
    frame_begin_ns_ = NowNs();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    const float scale = SDL_GetWindowDisplayScale(window_);
    ImGui::GetStyle().FontScaleDpi = scale > 0.0f ? scale : 1.0f;
    ImGui::NewFrame();
}

void LauncherHost::NoteStall(LoopPhase phase, int64_t duration_ns, int64_t now_ns, bool covered) {
    if (hidden_) {
        return;
    }
    if (const std::optional<UiStall> stall = stalls_.Finished(phase, duration_ns, now_ns, covered)) {
        std::printf("%s\n", UiStallLine(renderer_name_, *stall, sotb_startup_phase_current(), Minimized()).c_str());
    }
}

void LauncherHost::Submit(const ImVec4& clear) {
    const ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(renderer_, clear.x, clear.y, clear.z, 1.0f);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
}

void LauncherHost::EndFrame(const ImVec4& clear, const char* capture_path) {
    const int64_t submitting = NowNs();
    ImGui::Render();
    Submit(clear);
    last_clear_ = clear;
    drawn_ = true;
    if (capture_path) {
        if (SDL_Surface* frame = SDL_RenderReadPixels(renderer_, nullptr)) {
            SDL_SavePNG(frame, capture_path);
            SDL_DestroySurface(frame);
        }
    }
    const int64_t presenting = NowNs();
    SDL_RenderPresent(renderer_);
    const int64_t presented = NowNs();
    NoteStall(LoopPhase::kBuild, submitting - frame_begin_ns_, presented, std::exchange(dialog_in_frame_, false));
    NoteStall(LoopPhase::kPresent, presented - presenting, presented, monitor_.Measuring());
    NoteFrameDrawn(presented, {submitting - frame_begin_ns_, presenting - submitting, presented - presenting});
}

bool LauncherHost::Fullscreen() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

void LauncherHost::SetFullscreen(bool fullscreen) {
    SDL_SetWindowFullscreen(window_, fullscreen);
    // X11 and Wayland apply it later; a frame in between could record the fullscreen size as the window's own.
    SDL_SyncWindow(window_);
}

std::optional<WindowState> LauncherHost::State() const {
    if (!window_) {
        return std::nullopt;
    }
    int x = 0, y = 0, width = 0, height = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    SDL_GetWindowSize(window_, &width, &height);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
    return WindowState{{x, y, x + width, y + height}, (flags & SDL_WINDOW_MAXIMIZED) != 0,
                       (flags & SDL_WINDOW_MINIMIZED) != 0};
}

} // namespace Sotb

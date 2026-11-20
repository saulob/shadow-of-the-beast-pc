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

bool PinRendererModules(std::string_view renderer, int& pinned) {
    for (const std::string_view module : RendererModulesToPin(renderer)) {
        if (!PinModule(module)) {
            std::printf("Launcher: cannot keep %.*s loaded, skipping the %.*s renderer\n", int(module.size()), module.data(),
                        int(renderer.size()), renderer.data());
            return false;
        }
        ++pinned;
    }
    return true;
}

SDL_Renderer* CreateLauncherRenderer(SDL_Window* window, const RendererPlan& plan, RendererSelection& selection, int& pinned) {
    SDL_Renderer* renderer = nullptr;
    selection = SelectRenderer(plan, [&](std::string_view requested) -> std::string {
        const std::string name(requested);
        if (!PinRendererModules(name, pinned)) {
            return {};
        }
        renderer = SDL_CreateRenderer(window, name.empty() ? nullptr : name.c_str());
        if (!renderer) {
            std::printf("Launcher: no %s renderer (%s)\n", name.empty() ? "default" : name.c_str(), SDL_GetError());
            return {};
        }
        const char* actual = SDL_GetRendererName(renderer);
        return actual && *actual ? actual : "unknown";
    });
    return renderer;
}

} // namespace

bool LauncherHost::Open(const HostOptions& options) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::printf("Launcher: SDL video failed: %s\n", SDL_GetError());
        return false;
    }
    video_ = true;
    gamepad_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    const std::vector<Display> displays = ListDisplays();
    if (displays.empty()) {
        std::printf("Launcher: no display found\n");
        Close();
        return false;
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
    const int x = placement.centered ? int(SDL_WINDOWPOS_CENTERED_DISPLAY(target->id)) : placement.rect.left;
    const int y = placement.centered ? int(SDL_WINDOWPOS_CENTERED_DISPLAY(target->id)) : placement.rect.top;
    const int width = placement.rect.width();
    const int height = placement.rect.height();

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, options.title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    window_ = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window_) {
        std::printf("Launcher: cannot create the window: %s\n", SDL_GetError());
        Close();
        return false;
    }
    sotb_startup_phase("launcher window created");
    SDL_SetWindowMinimumSize(window_, std::min(static_cast<int>(options.min_width * target->scale), width),
                             std::min(static_cast<int>(options.min_height * target->scale), height));
    LogPlacement(placement, areas, target->name);

    const RendererPlan plan = PlanLauncherRenderer(kWindows, std::getenv(kLauncherRendererVariable));
    RendererSelection selection;
    int pinned = 0;
    renderer_ = CreateLauncherRenderer(window_, plan, selection, pinned);
    if (!renderer_) {
        std::printf("%s\n", RendererFailedLine(plan, selection).c_str());
        std::printf("Launcher: cannot create a renderer: %s\n", SDL_GetError());
        Close();
        return false;
    }
    renderer_name_ = selection.actual;
    std::printf("%s\n", RendererSelectedLine(plan, selection, pinned).c_str());
    ConfigureVsync();

    IMGUI_CHECKVERSION();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);

    hidden_ = options.hidden;
    gate_ = LoopGate(hidden_ ? 0 : kLauncherTargetHz);
    if (placement.maximized) {
        SDL_MaximizeWindow(window_);
    }
    if (placement.fullscreen) {
        SDL_SetWindowFullscreen(window_, true);
    }
    std::printf("Launcher: window %dx%d at %.2fx display scale, %s renderer\n", width, height, target->scale,
                renderer_name_.c_str());
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

void LauncherHost::Close() {
    sotb_startup_phase("launcher host closing");
    for (SDL_Texture* texture : textures_) {
        SDL_DestroyTexture(texture);
    }
    textures_.clear();
    if (imgui_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(imgui_);
        imgui_ = nullptr;
    }
    sotb_startup_phase("launcher destroying renderer");
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    sotb_startup_phase("launcher destroying window");
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
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

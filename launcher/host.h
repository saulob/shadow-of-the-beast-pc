// The launcher's window: SDL3, an SDL renderer and Dear ImGui. It exists only until Play or Quit, before the loader
// creates the game window and its Vulkan presenter, so only one SDL window and one ImGui context are alive at a time.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "launcher_pacing.h"
#include "launcher_renderer.h"
#include "placement.h"

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

namespace Sotb {

enum class FrameGate { kQuit, kDraw, kIdle };

struct HostOptions {
    std::string title;
    int width = 1280;  // logical, scaled by the display's content scale
    int height = 720;
    int min_width = 900;
    int min_height = 560;
    std::optional<WindowPlacement> restore;  // used when it lies on a display
    bool hidden = false;                     // page screenshots without showing the window
};

// A message box of the system itself, for when the launcher window cannot open. Needs no SDL renderer and no Vulkan.
void ShowStartupError(const std::string& title, const std::string& text);

class LauncherHost {
public:
    LauncherHost() = default;
    ~LauncherHost() { Close(); }
    LauncherHost(const LauncherHost&) = delete;
    LauncherHost& operator=(const LauncherHost&) = delete;

    // False when no window or renderer could be created; nothing is left open then and FailureText says why.
    bool Open(const HostOptions& options);
    const std::string& FailureText() const { return failure_text_; }
    // Shows the window once, after its first frame has been drawn, and presents that frame again. Does nothing before
    // a frame was drawn or when it was opened hidden.
    void Show();
    void Close();

    // Handles the window's events (F11 switches the launcher's own fullscreen), then waits until the next frame is
    // due: kDraw at most 60 times a second (a hidden window is not limited), kQuit once the window was closed, kIdle
    // after a short sleep while it is minimized, when the caller does its background work and does not draw.
    FrameGate NextFrame();

    // Between these two the caller draws the launcher with ImGui. A capture path saves the frame as a PNG.
    void BeginFrame();
    void EndFrame(const ImVec4& clear, const char* capture_path = nullptr);

    bool Fullscreen() const;
    void SetFullscreen(bool fullscreen);
    std::optional<WindowState> State() const;

    // A picture for ImGui to draw (RGBA8, width * height * 4 bytes). The host owns it until Close; 0 when it could not be made.
    ImTextureID CreateTexture(const uint8_t* rgba, int width, int height);
    void DestroyTexture(ImTextureID texture);

    // The saved placement the window was opened with (moved into a display when needed); empty when it was centered.
    const std::optional<WindowPlacement>& Restored() const { return restored_; }

    // The frame being built waits in a native dialog, so its build time is not a stall.
    void NoteDialogInFrame() { dialog_in_frame_ = true; }

private:
    struct Geometry;
    bool Fail(std::string text);
    SDL_Window* OpenWindow(const HostOptions& options, const Geometry& geometry);
    RendererAttempt OpenAttempt(const HostOptions& options, const Geometry& geometry, std::string_view name, int& pinned);
    std::string ProbeFirstFrame();
    void ReleaseWindow(bool closing);
    bool PumpEvents();
    bool Minimized() const;
    void ConfigureVsync();
    void NoteFrameDrawn(int64_t now_ns, const FrameTimes& times);
    void NoteStall(LoopPhase phase, int64_t duration_ns, int64_t now_ns, bool covered);
    void Submit(const ImVec4& clear);
    void LogPlacement(const PlacementChoice& placement, std::span<const DisplayArea> areas, const std::string& name) const;

    std::optional<WindowPlacement> restored_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    std::vector<SDL_Texture*> textures_;
    bool platform_backend_ = false;
    bool renderer_backend_ = false;
    std::string failure_text_;
    bool video_ = false;
    bool gamepad_ = false;
    bool hidden_ = false;
    bool quit_ = false;
    bool drawn_ = false;
    bool shown_ = false;
    bool dialog_in_frame_ = false;
    ImVec4 last_clear_;
    LoopGate gate_;
    LoopMonitor monitor_;
    StallDetector stalls_;
    std::string renderer_name_;
    int64_t frame_begin_ns_ = 0;
};

} // namespace Sotb

// The launcher's view of user\sotb.ini, the file the video core reads at start.
#pragma once

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "trg/settings.h"

namespace Sotb {

inline constexpr char kShowLauncherKey[] = "show_launcher";
inline constexpr char kDisplayModeKey[] = "display_mode";
inline constexpr char kWindowWidthKey[] = "window_width";
inline constexpr char kWindowHeightKey[] = "window_height";
inline constexpr char kMonitorKey[] = "monitor";
inline constexpr char kVSyncKey[] = "vsync";
inline constexpr char kPresentationKey[] = "presentation";
inline constexpr char kUpscaleKey[] = "fsr1";
inline constexpr char kSharpenKey[] = "rcas";
inline constexpr char kSharpenStrengthKey[] = "rcas_strength";
inline constexpr char kRendererKey[] = "renderer";
inline constexpr char kGraphicsCardKey[] = "graphics_card";
inline constexpr char kRenderResolutionKey[] = "render_resolution";
inline constexpr char kAntiAliasingKey[] = "smaa";
inline constexpr char kAnisotropyKey[] = "anisotropy";
inline constexpr char kSkipIntroKey[] = "skip_intro";
inline constexpr char kFrameRateKey[] = "frame_rate";
inline constexpr char kShowFpsTitleKey[] = "show_fps_title";
inline constexpr char kShowResolutionTitleKey[] = "show_resolution_title";
inline constexpr char kShowFrameCounterKey[] = "show_fps";
inline constexpr char kControllerRumbleKey[] = "controller_rumble";
inline constexpr char kDetailedLoggingKey[] = "detailed_logging";

inline constexpr char kLauncherXKey[] = "launcher_x";
inline constexpr char kLauncherYKey[] = "launcher_y";
inline constexpr char kLauncherWidthKey[] = "launcher_width";
inline constexpr char kLauncherHeightKey[] = "launcher_height";
inline constexpr char kLauncherMaximizedKey[] = "launcher_maximized";
inline constexpr char kLauncherFullscreenKey[] = "launcher_fullscreen";

inline constexpr char kDefaultRenderer[] = "vulkan";
inline constexpr char kDefaultPresentation[] = "letterbox";
inline constexpr char kNativeResolution[] = "native";

struct Choice {
    std::string_view key;
    std::string_view name;
};

inline constexpr Choice kRendererChoices[] = {
    {"auto", "Auto"},
    {"vulkan", "Vulkan"},
    {"opengl", "OpenGL (experimental)"},
#ifdef _WIN32
    {"d3d11", "Direct3D 11 (experimental)"},
#endif
};
inline constexpr Choice kPresentationChoices[] = {
    {"letterbox", "Letterbox"},
    {"stretch", "Stretch"},
};
inline constexpr std::string_view kFrameRates[] = {"30", "60", "90", "120", "144", "165", "240", "0"};
inline constexpr std::string_view kAnisotropyLevels[] = {"0", "2", "4", "8", "16"};

inline constexpr int kDefaultWindowWidth = 1920;
inline constexpr int kDefaultWindowHeight = 1080;

inline constexpr char kDisplayModeHelp[] =
    "Borderless Fullscreen fills the monitor. F11 also switches it while playing.";
inline constexpr char kWindowSizeHelp[] =
    "The game window's size in pixels in Windowed mode. The picture keeps its shape: other window shapes "
    "get black bars.";
inline constexpr char kRenderResolutionHelp[] =
    "The resolution the game renders at. It is separate from the window size; the picture is scaled to the window. "
    "1440p and above need a powerful graphics card.";
inline constexpr char kMonitorHelp[] = "The display the game opens on.";
inline constexpr char kVSyncHelp[] =
    "On prevents tearing. Off allows it, for the lowest latency and G-Sync/FreeSync.";
inline constexpr char kPresentationHelp[] =
    "Controls how the rendered image fits a window or display with a different aspect ratio.";
inline constexpr char kUpscaleHelp[] =
    "Enlarges the game's picture to a bigger window with AMD FidelityFX Super Resolution 1. It has no "
    "effect when the window is the size of the render resolution or smaller.";
inline constexpr char kSharpenHelp[] =
    "Sharpens the final picture (AMD RCAS). It applies when the window is the size of the render "
    "resolution, or together with FSR 1 in a bigger window.";
inline constexpr char kRendererHelp[] =
    "Vulkan is the supported renderer. OpenGL and Direct3D 11 are experimental and render only part of "
    "the game's features. Direct3D 11 is intended mainly for Windows systems where Vulkan or OpenGL has "
    "problems. Auto uses Vulkan, and OpenGL when this system cannot run Vulkan. It takes effect the next "
    "time the game starts.";
inline constexpr char kGraphicsCardHelp[] = "Selects which graphics card renders the game. If the selected card is no longer available, the first available card is used.";
inline constexpr char kAntiAliasingHelp[] =
    "SMAA smooths jagged edges on the finished picture, at a small cost in performance.";
inline constexpr char kAnisotropyHelp[] =
    "Sharper textures on floors and walls seen at an angle. Game uses the game's own filtering.";
inline constexpr char kFrameRateHelp[] =
    "Frames reaching the screen. The game's logic stays at its own rate.";
inline constexpr char kShowFpsTitleHelp[] = "Shows the frame rate in the window title.";
inline constexpr char kShowResolutionTitleHelp[] = "Shows the game's render resolution in the window title.";
inline constexpr char kFrameCounterHelp[] =
    "Shows the frame rate and frame time in the game. F2 also switches it for this session.";
inline constexpr char kSkipIntroHelp[] = "Skip startup logos and intro and go directly to the Press Start screen. Applies at next start.";
inline constexpr char kTextLanguageHelp[] = "Language used by game menus and subtitles.";
inline constexpr char kControllerRumbleHelp[] = "Off disables controller vibration. Applies at the next game start.";
inline constexpr char kDetailedLoggingHelp[] = "Keeps logs for normal sessions. Crash diagnostics are always preserved.";

using Values = std::map<std::string, std::string, std::less<>>;

struct ManagedSetting {
    std::string_view key;
    std::string_view default_value;
};

std::span<const ManagedSetting> ManagedSettings();
bool IsLauncherPlacementSetting(std::string_view key);
bool IsValidSetting(std::string_view key, std::string_view value);
std::string FrameRateLabel(int fps);

class LauncherSettings final : public trg::SettingsStore {
public:
    explicit LauncherSettings(std::filesystem::path file);

    std::string Get(std::string_view key, std::string_view def = {}) const override;
    void Set(std::string_view key, std::string value) override;
    bool Save() override;
    void ResetToDefaults() override;
    bool SaveNow(const Values& values);
    bool Has(std::string_view key) const { return pending_.find(key) != pending_.end(); }
    const std::filesystem::path& file() const { return file_; }

private:
    bool WriteFile(const Values& changes);

    std::filesystem::path file_;
    Values saved_;
    Values pending_;
};

std::string SettingsSummary(const LauncherSettings& settings, const char* game_build);

} // namespace Sotb

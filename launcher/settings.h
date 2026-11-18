// The launcher's view of user\sotb.ini, the file the video core reads at start.
#pragma once

#include <map>
#include <string>

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
inline constexpr char kFrameRateKey[] = "frame_rate";
inline constexpr char kShowFpsTitleKey[] = "show_fps_title";
inline constexpr char kShowResolutionTitleKey[] = "show_resolution_title";
inline constexpr char kShowFrameCounterKey[] = "show_fps";
inline constexpr char kOverlayScaleKey[] = "overlay_scale";
inline constexpr char kOverlayCornerKey[] = "overlay_corner";

inline constexpr char kLauncherXKey[] = "launcher_x";
inline constexpr char kLauncherYKey[] = "launcher_y";
inline constexpr char kLauncherWidthKey[] = "launcher_width";
inline constexpr char kLauncherHeightKey[] = "launcher_height";
inline constexpr char kLauncherMaximizedKey[] = "launcher_maximized";
inline constexpr char kLauncherFullscreenKey[] = "launcher_fullscreen";

inline constexpr int kDefaultWindowWidth = 1920;
inline constexpr int kDefaultWindowHeight = 1080;
inline constexpr char kNativeResolution[] = "native";

inline constexpr char kDisplayModeHelp[] =
    "Borderless Fullscreen fills the monitor. F11 also switches it while playing.";
inline constexpr char kWindowSizeHelp[] =
    "The size of the game window. The game's own picture is 1920 \xC3\x97 1080.";
inline constexpr char kMonitorHelp[] = "The display the game opens on.";
inline constexpr char kVSyncHelp[] =
    "On prevents tearing. Off allows it, for the lowest latency and G-Sync/FreeSync.";
inline constexpr char kPresentationHelp[] = "How the picture fits a window or display with a different aspect ratio.";
inline constexpr char kUpscaleHelp[] = "Enlarges the picture to a bigger window with AMD FidelityFX Super Resolution 1.";
inline constexpr char kSharpenHelp[] = "Sharpens the final picture (AMD RCAS), alone or together with FSR 1.";
inline constexpr char kRendererHelp[] =
    "Vulkan is the supported renderer. OpenGL and Direct3D 11 are experimental. Auto uses OpenGL only when Vulkan "
    "cannot run.";
inline constexpr char kGraphicsCardHelp[] = "The Vulkan device the game renders with; Automatic picks the first usable one.";
inline constexpr char kRenderResolutionHelp[] =
    "The size of the game's own render targets, scaled to the window. Native keeps the PS4 picture.";
inline constexpr char kRestrictedRenderResolutionHelp[] =
    "This build renders at the game's own picture size; the window scales it.";
inline constexpr char kAntiAliasingHelp[] =
    "SMAA smooths jagged edges on the finished picture, at a small cost in speed.";
inline constexpr char kAnisotropyHelp[] = "Sharper textures seen at an angle. Game uses the game's own filtering.";
inline constexpr char kFrameRateHelp[] =
    "Frames reaching the screen. The game's logic stays at its own rate.";
inline constexpr char kShowFpsTitleHelp[] = "Shows the frame rate in the window title.";
inline constexpr char kShowResolutionTitleHelp[] = "Shows the game's render resolution in the window title.";
inline constexpr char kFrameCounterHelp[] =
    "Shows the frame rate and frame time in a corner of the game. F2 also switches it for this session.";
inline constexpr char kOverlayScaleHelp[] = "The frame counter's size.";
inline constexpr char kOverlayCornerHelp[] = "The corner where the frame counter is drawn.";

using Values = std::map<std::string, std::string, std::less<>>;

} // namespace Sotb

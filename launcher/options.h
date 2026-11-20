// What the launcher's pages offer and how they label it.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "guest_display.h"
#include "sotb_displays.h"
#include "sotb_keys.h"
#include "trg/settings.h"

namespace Sotb {

struct WindowSize {
    int width = 0;
    int height = 0;
    bool operator==(const WindowSize&) const = default;
};

// In pixels. The game's own picture is 1920 x 1080.
inline constexpr int kWindowHeights[] = {720, 900, 1080, 1440, 2160};
inline constexpr WindowSize kGamePictureSize{1920, 1080};

std::vector<WindowSize> WindowSizes();

enum class PageId { kPlay, kDisplay, kGraphics, kGameplay, kControls, kTrophies, kBugReport, kAbout };

inline constexpr PageId kPageOrder[] = {PageId::kPlay,     PageId::kDisplay,  PageId::kGraphics, PageId::kGameplay,
                                        PageId::kControls, PageId::kTrophies, PageId::kBugReport, PageId::kAbout};

const char* PageName(PageId page);

struct PadMapping {
    const char* xbox;
    const char* ps4;
};

// What each Xbox controller input does in the game, as src/runtime_pad.c maps it.
std::span<const PadMapping> XboxMapping();

// "1920 × 1080", with a note on the game's own picture size.
std::string WindowSizeLabel(WindowSize size);

// "1440p — 2560 × 1440".
std::string ResolutionLabel(const SotbResolution& resolution);

// "Display 2 (primary)   2560 × 1440   Name". Monitor settings are 1-based.
std::string MonitorLabel(int monitor, const std::vector<Display>& displays);

// "Space", "Left Shift", "Enter", "F11".
std::string KeyDisplayName(int scancode);
// The saved key of an action; the default when the setting is missing or names no usable key.
int SavedKey(const trg::SettingsStore& settings, SotbKeyAction action);
void SetKey(trg::SettingsStore& settings, SotbKeyAction action, int scancode);
// Puts every keyboard control and port hotkey back to its default and nothing else.
void ResetKeys(trg::SettingsStore& settings);

} // namespace Sotb

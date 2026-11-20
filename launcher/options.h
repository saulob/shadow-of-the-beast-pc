// What the launcher's pages offer and how they label it.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "guest_display.h"
#include "sotb_displays.h"
#include "sotb_keys.h"
#include "sotb_pad.h"
#include "sotb_pad_prompts.h"
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

// The Controls page's controller bindings, one SDL gamepad name per PS4 button in the settings (sotb_pad.h).
// The saved source of a PS4 button; the default when the setting is missing or names no bindable button or trigger.
int SavedPadSource(const trg::SettingsStore& settings, SotbPadAction action);
void SetPadSource(trg::SettingsStore& settings, SotbPadAction action, int source);
// Puts every controller binding back to its default and nothing else.
void ResetPads(trg::SettingsStore& settings);
// The saved prompt mode; Auto when the setting is missing or unknown. It never changes a binding.
SotbPromptMode SavedPromptMode(const trg::SettingsStore& settings);
// "Auto", "Xbox", "PlayStation".
const char* PromptModeLabel(SotbPromptMode mode);
// "A" or "Cross" for the source of an action, in the prompt family that applies.
std::string PadBindingLabel(const trg::SettingsStore& settings, SotbPadAction action, SotbPromptFamily family);

} // namespace Sotb

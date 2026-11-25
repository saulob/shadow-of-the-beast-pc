#include "options.h"
#include "settings.h"

#include <SDL3/SDL_keyboard.h>

namespace Sotb {
namespace {

constexpr char kTimes[] = " \xC3\x97 ";

std::string Dimensions(int width, int height) {
    return std::to_string(width) + kTimes + std::to_string(height);
}

std::string DefaultKeyName(SotbKeyAction action) {
    char name[32];
    return sotb_key_name(sotb_key_actions[action].default_scancode, name, sizeof(name));
}

} // namespace

WindowSize WindowSizeOf(int height, const SotbAspectRatio& aspect) {
    const long width = (static_cast<long>(height) * aspect.width * 2 + aspect.height) / (2 * aspect.height);
    return {static_cast<int>(width), height};
}

std::vector<WindowSize> WindowSizesFor(const SotbAspectRatio& aspect) {
    std::vector<WindowSize> sizes;
    for (const int height : kWindowHeights) {
        sizes.push_back(WindowSizeOf(height, aspect));
    }
    return sizes;
}

std::span<const PadMapping> XboxMapping() {
    static constexpr PadMapping kMapping[] = {
        {"A", "Cross"},          {"B", "Circle"},         {"X", "Square"},         {"Y", "Triangle"},
        {"LB", "L1"},            {"RB", "R1"},            {"LT", "L2"},            {"RT", "R2"},
        {"Left stick", "Left stick"}, {"Right stick", "Right stick"}, {"LS (press)", "L3"},
        {"RS (press)", "R3"},    {"Menu", "Options"},     {"View", "Touchpad"},    {"D-pad", "D-pad"},
    };
    return kMapping;
}

const char* PageName(PageId page) {
    switch (page) {
    case PageId::kPlay: return "Play";
    case PageId::kDisplay: return "Display";
    case PageId::kGraphics: return "Graphics";
    case PageId::kGameplay: return "Gameplay";
    case PageId::kControls: return "Controls";
    case PageId::kTrophies: return "Trophies";
    case PageId::kBugReport: return "Bug Report";
    case PageId::kAbout: return "About";
    }
    return "";
}

std::string WindowSizeLabel(WindowSize size) {
    std::string label = Dimensions(size.width, size.height);
    if (size == kGamePictureSize) {
        label += "   (the game's own size)";
    }
    return label;
}

std::string ResolutionLabel(const SotbResolution& resolution) {
    return std::string(resolution.name) + " \xE2\x80\x94 " + Dimensions(resolution.width, resolution.height);
}

std::string MonitorLabel(int monitor, const std::vector<Display>& displays) {
    const std::string number = std::to_string(monitor);
    if (monitor < 1 || monitor > static_cast<int>(displays.size())) {
        return "Display " + number + " (not connected)";
    }
    const Display& display = displays[static_cast<size_t>(monitor - 1)];
    std::string label = "Display " + number + (display.primary ? " (primary)" : "") + "   " +
                        Dimensions(display.bounds.w, display.bounds.h);
    if (!display.name.empty() && display.name != "Generic PnP Monitor") {
        label += "   " + display.name;
    }
    return label;
}

std::string KeyDisplayName(int scancode) {
    if (scancode == SDL_SCANCODE_RETURN) {
        return "Enter";
    }
    return sotb_key_bindable(scancode) ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : std::string();
}

int SavedKey(const trg::SettingsStore& settings, SotbKeyAction action) {
    const int saved = sotb_key_from_name(settings.Get(sotb_key_actions[action].ini_key).c_str());
    return saved ? saved : sotb_key_actions[action].default_scancode;
}

void SetKey(trg::SettingsStore& settings, SotbKeyAction action, int scancode) {
    char name[32];
    if (sotb_key_name(scancode, name, sizeof(name))[0]) {
        settings.Set(sotb_key_actions[action].ini_key, name);
    }
}

void ResetKeys(trg::SettingsStore& settings) {
    for (int action = 0; action < SOTB_KEY_COUNT; ++action) {
        settings.Set(sotb_key_actions[action].ini_key, DefaultKeyName(static_cast<SotbKeyAction>(action)));
    }
}

int SavedPadSource(const trg::SettingsStore& settings, SotbPadAction action) {
    const int saved = sotb_pad_source_from_name(settings.Get(sotb_pad_actions[action].ini_key).c_str());
    return saved != SOTB_PAD_SOURCE_NONE ? saved : sotb_pad_actions[action].default_source;
}

void SetPadSource(trg::SettingsStore& settings, SotbPadAction action, int source) {
    char name[32];
    if (sotb_pad_source_name(source, name, sizeof(name))[0]) {
        settings.Set(sotb_pad_actions[action].ini_key, name);
    }
}

void ResetPads(trg::SettingsStore& settings) {
    for (int action = 0; action < SOTB_PAD_COUNT; ++action) {
        SetPadSource(settings, static_cast<SotbPadAction>(action), sotb_pad_actions[action].default_source);
    }
}

SotbPromptMode SavedPromptMode(const trg::SettingsStore& settings) {
    return sotb_prompt_mode_from_name(settings.Get(kControllerPromptsKey).c_str());
}

const char* PromptModeLabel(SotbPromptMode mode) {
    switch (mode) {
    case SOTB_PROMPT_XBOX: return "Xbox";
    case SOTB_PROMPT_PLAYSTATION: return "PlayStation";
    default: return "Auto";
    }
}

std::string PadBindingLabel(const trg::SettingsStore& settings, SotbPadAction action, SotbPromptFamily family) {
    return sotb_pad_source_label(SavedPadSource(settings, action), family);
}

} // namespace Sotb

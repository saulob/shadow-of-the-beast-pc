#include "options.h"

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

std::vector<WindowSize> WindowSizes() {
    std::vector<WindowSize> sizes;
    for (const int height : kWindowHeights) {
        sizes.push_back({(height * 16 + 4) / 9, height});
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

} // namespace Sotb

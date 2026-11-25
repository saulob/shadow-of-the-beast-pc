#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <system_error>
#include <utility>
#include <vector>

#include "guest_display.h"
#include "sotb_keys.h"
#include "sotb_pad.h"
#include "sotb_pad_prompts.h"
#include "sotb_launcher.h"
#include "text_languages.h"
#include "utf8.h"

#ifndef SOTB_PORT_VERSION
#define SOTB_PORT_VERSION "unknown"
#endif

namespace Sotb {
namespace {

constexpr ManagedSetting kBaseSettings[] = {
    {kShowLauncherKey, "1"},
    {kDisplayModeKey, "windowed"},
    {kWindowWidthKey, "1920"},
    {kWindowHeightKey, "1080"},
    {kMonitorKey, "1"},
    {kVSyncKey, "1"},
    {kPresentationKey, kDefaultPresentation},
    {kUpscaleKey, "0"},
    {kSharpenKey, "1"},
    {kSharpenStrengthKey, "75"},
    {kRenderResolutionKey, kNativeResolution},
    {kAspectRatioKey, kDefaultAspectRatio},
    {kGraphicsCardKey, ""},
    {kRendererKey, kDefaultRenderer},
    {kAntiAliasingKey, "0"},
    {kAnisotropyKey, "0"},
    {kTextLanguageKey, kDefaultTextLanguage},
    {kSkipIntroKey, "0"},
    {kFrameRateKey, "60"},
    {kShowFpsTitleKey, "0"},
    {kShowResolutionTitleKey, "0"},
    {kShowFrameCounterKey, "0"},
    {kControllerRumbleKey, "1"},
    {kControllerPromptsKey, "auto"},
    {kMenuButtonLayoutKey, kDefaultMenuButtonLayout},
    {kDetailedLoggingKey, "1"},
    {kLauncherXKey, ""},
    {kLauncherYKey, ""},
    {kLauncherWidthKey, ""},
    {kLauncherHeightKey, ""},
    {kLauncherMaximizedKey, "0"},
    {kLauncherFullscreenKey, "0"},
};

constexpr std::string_view kPlacementKeys[] = {kLauncherXKey,         kLauncherYKey,         kLauncherWidthKey,
                                                kLauncherHeightKey,    kLauncherMaximizedKey, kLauncherFullscreenKey};

constexpr char kFileHeader[] =
    "# Shadow of the Beast user settings (written by the launcher; F11 saves the display mode).\n"
    "# Remove a line to use the default.\n";

const char* OnOff(bool on) { return on ? "on" : "off"; }

} // namespace

std::span<const ManagedSetting> ManagedSettings() {
    static const std::vector<ManagedSetting> settings = [] {
        static std::deque<std::string> default_names;
        std::vector<ManagedSetting> all(std::begin(kBaseSettings), std::end(kBaseSettings));
        for (const SotbKeyInfo& action : sotb_key_actions) {
            char name[32];
            default_names.emplace_back(sotb_key_name(action.default_scancode, name, sizeof(name)));
            all.push_back({action.ini_key, default_names.back()});
        }
        for (const SotbPadInfo& action : sotb_pad_actions) {
            char name[32];
            default_names.emplace_back(sotb_pad_source_name(action.default_source, name, sizeof(name)));
            all.push_back({action.ini_key, default_names.back()});
        }
        return all;
    }();
    return settings;
}

bool IsValidSetting(std::string_view key, std::string_view value) {
    const auto one_of = [&](std::initializer_list<std::string_view> values) {
        return std::find(values.begin(), values.end(), value) != values.end();
    };
    if (std::any_of(std::begin(sotb_key_actions), std::end(sotb_key_actions),
                    [&](const SotbKeyInfo& action) { return key == action.ini_key; })) {
        return sotb_key_from_name(std::string(value).c_str()) != 0;
    }
    if (std::any_of(std::begin(sotb_pad_actions), std::end(sotb_pad_actions),
                    [&](const SotbPadInfo& action) { return key == action.ini_key; })) {
        return sotb_pad_source_from_name(std::string(value).c_str()) != SOTB_PAD_SOURCE_NONE;
    }
    if (key == kControllerPromptsKey) {
        return one_of({"auto", "xbox", "playstation"});
    }
    if (key == kMenuButtonLayoutKey) {
        return std::any_of(std::begin(kMenuButtonLayoutChoices), std::end(kMenuButtonLayoutChoices),
                           [&](const Choice& choice) { return value == choice.key; });
    }
    if (key == kDisplayModeKey) {
        return one_of({"windowed", "borderless"});
    }
    if (key == kRenderResolutionKey) {
        return std::any_of(std::begin(sotb_resolutions), std::end(sotb_resolutions),
                           [&](const SotbResolution& resolution) { return value == resolution.key; });
    }
    if (key == kAspectRatioKey) {
        return std::any_of(std::begin(sotb_aspect_ratios), std::end(sotb_aspect_ratios),
                           [&](const SotbAspectRatio& aspect) { return value == aspect.key; });
    }
    if (key == kAnisotropyKey) {
        return std::find(std::begin(kAnisotropyLevels), std::end(kAnisotropyLevels), value) != std::end(kAnisotropyLevels);
    }
    if (key == kTextLanguageKey) {
        const std::span<const TextLanguage> languages = TextLanguages();
        return std::any_of(languages.begin(), languages.end(),
                           [&](const TextLanguage& language) { return value == language.key; });
    }
    if (key == kFrameRateKey) {
        return std::find(std::begin(kFrameRates), std::end(kFrameRates), value) != std::end(kFrameRates);
    }
    if (key == kPresentationKey) {
        return std::any_of(std::begin(kPresentationChoices), std::end(kPresentationChoices),
                           [&](const Choice& choice) { return value == choice.key; });
    }
    if (key == kRendererKey) {
        return std::any_of(std::begin(kRendererChoices), std::end(kRendererChoices),
                           [&](const Choice& choice) { return value == choice.key; });
    }
    return true;
}

bool IsLauncherPlacementSetting(std::string_view key) {
    return std::find(std::begin(kPlacementKeys), std::end(kPlacementKeys), key) != std::end(kPlacementKeys);
}

std::string FrameRateLabel(int fps) {
    if (fps <= 0) {
        return "Unlimited";
    }
    return std::to_string(fps) + " FPS";
}

LauncherSettings::LauncherSettings(std::filesystem::path file) : file_(std::move(file)) {
    true_value = "1";
    false_value = "0";
    const trg::TextFileSettings stored(Utf8(file_));
    for (const ManagedSetting& setting : ManagedSettings()) {
        if (stored.Has(setting.key)) {
            saved_[std::string(setting.key)] = stored.Get(setting.key);
        }
    }
    pending_ = saved_;
}

std::string LauncherSettings::Get(std::string_view key, std::string_view def) const {
    const auto it = pending_.find(key);
    if (it == pending_.end() || it->second.empty() || !IsValidSetting(key, it->second)) {
        return std::string(def);
    }
    return it->second;
}

void LauncherSettings::Set(std::string_view key, std::string value) {
    pending_[std::string(key)] = std::move(value);
}

bool LauncherSettings::Save() {
    Values changes;
    for (const auto& [key, value] : pending_) {
        const auto saved = saved_.find(key);
        if (saved == saved_.end() || saved->second != value) {
            changes.emplace(key, value);
        }
    }
    if (changes.empty()) {
        return true;
    }
    if (!WriteFile(changes)) {
        return false;
    }
    for (const auto& [key, value] : changes) {
        saved_[key] = value;
    }
    return true;
}

bool LauncherSettings::SaveNow(const Values& values) {
    if (!WriteFile(values)) {
        return false;
    }
    for (const auto& [key, value] : values) {
        saved_[key] = value;
        pending_[key] = value;
    }
    return true;
}

void LauncherSettings::ResetToDefaults() {
    for (const ManagedSetting& setting : ManagedSettings()) {
        if (!IsLauncherPlacementSetting(setting.key)) {
            pending_[std::string(setting.key)] = std::string(setting.default_value);
        }
    }
}

bool LauncherSettings::WriteFile(const Values& changes) {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    if (!std::filesystem::exists(file_, ec)) {
        std::ofstream(file_) << kFileHeader;
    }
    trg::TextFileSettings file(Utf8(file_));
    for (const auto& [key, value] : changes) {
        file.Set(key, value);
    }
    return file.Save();
}

std::string SettingsSummary(const LauncherSettings& settings, const char* game_build) {
    std::string out;
    const auto line = [&out](const std::string& text) { out += "Settings: " + text + "\n"; };
    const auto renderer = settings.Get(kRendererKey, kDefaultRenderer);
    const auto card = settings.Get(kGraphicsCardKey);
    line(std::string("Shadow of the Beast PC v") + SOTB_PORT_VERSION + ", game build " +
         (game_build ? game_build : "not a supported build"));
    line("Renderer " + renderer + ", graphics card " + (card.empty() || card == "automatic" ? "first available" : card) +
         ", VSync " + OnOff(settings.GetBool(kVSyncKey, true)) + ", presentation " +
         settings.Get(kPresentationKey, kDefaultPresentation));
    const SotbResolution& resolution =
        sotb_resolutions[sotb_resolution_index(settings.Get(kRenderResolutionKey, kNativeResolution).c_str())];
    const SotbAspectRatio& aspect =
        sotb_aspect_ratios[sotb_aspect_index(settings.Get(kAspectRatioKey, kDefaultAspectRatio).c_str())];
    const SotbRenderTarget target = sotb_render_target(&resolution, &aspect);
    line("Render resolution " + std::string(resolution.key) + " " + std::to_string(target.width) + "x" +
         std::to_string(target.height) + ", aspect ratio " + aspect.key);
    line(std::string("Window ") + (settings.Get(kDisplayModeKey, "windowed") == "borderless" ? "Borderless Fullscreen"
                                                                                           : "Windowed") +
         " " + settings.Get(kWindowWidthKey, "1920") + "x" + settings.Get(kWindowHeightKey, "1080") + ", monitor " +
         settings.Get(kMonitorKey, "1") + "");
    line(std::string("Upscaling FSR 1 ") + OnOff(settings.GetBool(kUpscaleKey, false)) + ", sharpening " +
         OnOff(settings.GetBool(kSharpenKey, true)) + " " + settings.Get(kSharpenStrengthKey, "75") + "%, SMAA " +
         OnOff(settings.GetBool(kAntiAliasingKey, false)) + ", anisotropy " + settings.Get(kAnisotropyKey, "0"));
    line(std::string("Gameplay: Skip Intro ") + OnOff(settings.GetBool(kSkipIntroKey, false)));
    line("Frame rate " + FrameRateLabel(std::atoi(settings.Get(kFrameRateKey, "60").c_str())) + ", frame counter " +
         OnOff(settings.GetBool(kShowFrameCounterKey, false)) + ", frame rate in title " +
         OnOff(settings.GetBool(kShowFpsTitleKey, false)) + ", render resolution in title " +
         OnOff(settings.GetBool(kShowResolutionTitleKey, false)));
    const TextLanguage& language = ResolveTextLanguage(settings.Get(kTextLanguageKey));
    line("Menu & Subtitles " + std::string(language.name) + " (" + std::string(language.key) + ", system language " +
         std::to_string(language.ps4_id) + ")");
    int custom_bindings = 0;
    for (const SotbPadInfo& action : sotb_pad_actions) {
        const int saved = sotb_pad_source_from_name(settings.Get(action.ini_key).c_str());
        custom_bindings += saved != SOTB_PAD_SOURCE_NONE && saved != action.default_source;
    }
    line(std::string("Controller rumble ") + OnOff(settings.GetBool(kControllerRumbleKey, true)) + ", prompts " +
         settings.Get(kControllerPromptsKey, "auto") + ", " + std::to_string(custom_bindings) +
         " custom controller bindings, detailed logging " + OnOff(settings.GetBool(kDetailedLoggingKey, true)));
    line(std::string("Menu button layout ") +
         (settings.Get(kMenuButtonLayoutKey, kDefaultMenuButtonLayout) == "original" ? "Original" : "Western"));
    return out;
}

} // namespace Sotb

extern "C" int sotb_detailed_logging(const char* settings_file) {
    if (!settings_file || !*settings_file) {
        return 1;
    }
    const Sotb::LauncherSettings settings(Sotb::PathFromUtf8(settings_file));
    return settings.GetBool(Sotb::kDetailedLoggingKey, true) ? 1 : 0;
}

extern "C" int sotb_skip_intro(const char* settings_file) {
    const Sotb::LauncherSettings settings(Sotb::PathFromUtf8(settings_file ? settings_file : ""));
    return settings.GetBool(Sotb::kSkipIntroKey, false) ? 1 : 0;
}

extern "C" int sotb_render_resolution(const char* settings_file) {
    const Sotb::LauncherSettings settings(Sotb::PathFromUtf8(settings_file ? settings_file : ""));
    return sotb_resolution_index(settings.Get(Sotb::kRenderResolutionKey, Sotb::kNativeResolution).c_str());
}

extern "C" int sotb_aspect_ratio(const char* settings_file) {
    const Sotb::LauncherSettings settings(Sotb::PathFromUtf8(settings_file ? settings_file : ""));
    return sotb_aspect_index(settings.Get(Sotb::kAspectRatioKey, Sotb::kDefaultAspectRatio).c_str());
}

extern "C" void sotb_log_settings_summary(const char* settings_file, const char* game_build) {
    const Sotb::LauncherSettings settings(Sotb::PathFromUtf8(settings_file ? settings_file : ""));
    std::fputs(Sotb::SettingsSummary(settings, game_build).c_str(), stdout);
}

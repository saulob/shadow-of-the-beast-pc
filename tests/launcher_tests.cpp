#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "guest_display.h"
#include "launcher_quit.h"
#include "options.h"
#include "page_icons.h"
#include "settings.h"
#include "sotb_frame_pacing.h"
#include "sotb_graphics.h"
#include "sotb_keys.h"
#include "sotb_launcher.h"
#include "sotb_renderer.h"
#include "sotb_settings.h"
#include "text_languages.h"

namespace fs = std::filesystem;
using namespace Sotb;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                                      \
    do {                                                                                 \
        ++checks;                                                                        \
        if (!(expr)) {                                                                   \
            ++failures;                                                                  \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr);                \
        }                                                                                \
    } while (0)

void WriteAll(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void TestPages() {
    const PageId expected[] = {PageId::kPlay,   PageId::kDisplay,  PageId::kGraphics, PageId::kGameplay,
                               PageId::kControls, PageId::kTrophies, PageId::kBugReport, PageId::kAbout};
    CHECK(std::size(kPageOrder) == std::size(expected));
    CHECK(std::equal(std::begin(expected), std::end(expected), std::begin(kPageOrder)));
    const char* names[] = {"Play", "Display", "Graphics", "Gameplay", "Controls", "Trophies", "Bug Report", "About"};
    for (size_t i = 0; i < std::size(expected); ++i) {
        CHECK(std::string(PageName(expected[i])) == names[i]);
    }
}

void TestXboxMapping() {
    const std::span<const PadMapping> mapping = XboxMapping();
    CHECK(mapping.size() == 15);
    const auto ps4_of = [&](const std::string& xbox) {
        for (const PadMapping& entry : mapping) {
            if (xbox == entry.xbox) return std::string(entry.ps4);
        }
        return std::string();
    };
    CHECK(ps4_of("A") == "Cross" && ps4_of("B") == "Circle" && ps4_of("X") == "Square" && ps4_of("Y") == "Triangle");
    CHECK(ps4_of("LB") == "L1" && ps4_of("RB") == "R1" && ps4_of("LT") == "L2" && ps4_of("RT") == "R2");
    CHECK(ps4_of("Menu") == "Options" && ps4_of("View") == "Touchpad" && ps4_of("LS (press)") == "L3");
}

void TestTextLanguages(const fs::path& base) {
    const std::span<const TextLanguage> languages = TextLanguages();
    CHECK(languages.size() == 19);
    std::set<std::string_view> keys;
    std::set<int> ids;
    for (const TextLanguage& language : languages) {
        CHECK(keys.insert(language.key).second && ids.insert(language.ps4_id).second);
        CHECK(language.archive.ends_with("/Game.locres") && !language.name.empty());
        CHECK(IsValidSetting(kTextLanguageKey, language.key));
    }
    CHECK(languages[0].key == kDefaultTextLanguage && languages[0].ps4_id == 1);
    const char* order[] = {"English (US)",      "English (UK)",           "Chinese (Traditional)",
                           "Danish",            "Dutch",                  "Finnish",
                           "French",            "German",                 "Italian",
                           "Japanese",          "Korean",                 "Norwegian",
                           "Polish",            "Portuguese (Brazil)",    "Portuguese (Portugal)",
                           "Russian",           "Spanish (Latin America)", "Spanish (Spain)",
                           "Swedish"};
    CHECK(languages.size() == std::size(order));
    for (size_t i = 0; i < languages.size() && i < std::size(order); ++i) {
        CHECK(languages[i].name == order[i]);
    }
    const struct {
        const char* key;
        int id;
    } mapping[] = {{"en_US", 1},  {"en_GB", 18}, {"zh_Hant", 10}, {"da", 14},    {"nl", 6},     {"fi", 12},  {"fr", 2},
                   {"de", 4},     {"it", 5},     {"ja", 0},       {"ko", 9},     {"nb", 15},    {"pl", 16},  {"pt_BR", 17},
                   {"pt_PT", 7},  {"ru", 8},     {"es_MX", 20},   {"es_ES", 3},  {"sv", 13}};
    for (const auto& entry : mapping) {
        CHECK(ResolveTextLanguage(entry.key).ps4_id == entry.id);
    }
    std::vector<TextLanguage> shuffled(languages.rbegin(), languages.rend());
    SortTextLanguages(shuffled);
    for (size_t i = 0; i < shuffled.size(); ++i) {
        CHECK(shuffled[i].key == languages[i].key);
    }
    CHECK(ResolveTextLanguage("pt_BR").ps4_id == 17 && ResolveTextLanguage("es_MX").ps4_id == 20);
    CHECK(ResolveTextLanguage("klingon").key == "en_US" && ResolveTextLanguage("").key == "en_US");
    CHECK(!IsValidSetting(kTextLanguageKey, "klingon") && !IsValidSetting(kTextLanguageKey, "zh_Hans"));
    const fs::path file = base / "language.ini";
    WriteAll(file, "text_language = ja\n");
    CHECK(sotb_text_language(file.string().c_str()) == 0);
    WriteAll(file, "text_language = fr\n");
    CHECK(sotb_text_language(file.string().c_str()) == 2);
    WriteAll(file, "text_language = removed_language\n");
    CHECK(sotb_text_language(file.string().c_str()) == 1);
    CHECK(sotb_text_language(nullptr) == 1 && sotb_text_language((base / "none.ini").string().c_str()) == 1);
}

void TestPageIcons() {
    for (const PageId page : kPageOrder) {
        CHECK(FindPageIcon(page) != nullptr);
        for (const int pixels : {24, 36, 54}) {
            const std::vector<uint8_t> rgba = RenderPageIcon(page, pixels);
            CHECK(rgba.size() == size_t(pixels) * size_t(pixels) * 4);
            bool inked = false;
            for (size_t i = 3; i < rgba.size(); i += 4) {
                inked |= rgba[i] > 0;
            }
            CHECK(inked);
        }
    }
    CHECK(PageIconAssets().size() == std::size(kPageOrder));
}

void TestSidebarIcons() {
    struct Expected {
        PageId page;
        const char* file;
        float scale;
    };
    const Expected expected[] = {{PageId::kPlay, "play.svg", 0.84f},         {PageId::kDisplay, "display.svg", 0.90f},
                                 {PageId::kGraphics, "graphics.svg", 0.84f}, {PageId::kGameplay, "gameplay.svg", 0.92f},
                                 {PageId::kControls, "controls.svg", 0.94f}, {PageId::kTrophies, "trophies.svg", 0.86f},
                                 {PageId::kBugReport, "bug-report.svg", 0.90f}, {PageId::kAbout, "about.svg", 0.88f}};
    CHECK(PageIconAssets().size() == std::size(expected));
    for (const Expected& e : expected) {
        const PageIconAsset* asset = FindPageIcon(e.page);
        CHECK(asset && std::string(asset->file) == e.file && std::fabs(asset->optical_scale - e.scale) < 1e-6f);
        CHECK(fs::exists(fs::path(SOTB_SOURCE_DIR) / "assets/launcher/icons" / e.file));
    }
    size_t svg_files = 0;
    for (const auto& entry : fs::directory_iterator(fs::path(SOTB_SOURCE_DIR) / "assets/launcher/icons")) {
        svg_files += entry.path().extension() == ".svg";
    }
    CHECK(svg_files == std::size(expected));
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        for (const Expected& e : expected) {
            const int pixels = static_cast<int>(20.0f * scale + 0.5f);
            const std::vector<uint8_t> rgba = RenderPageIcon(e.page, pixels);
            CHECK(rgba.size() == size_t(pixels) * size_t(pixels) * 4);
        }
    }
}

void TestAppIcon() {
    const fs::path root = SOTB_SOURCE_DIR;
    for (const char* name : {"app-icon.svg", "app-icon.ico", "app-icon-16.png", "app-icon-24.png", "app-icon-32.png",
                             "app-icon-48.png", "app-icon-64.png", "app-icon-128.png", "app-icon-256.png",
                             "app-icon-512.png"}) {
        CHECK(fs::exists(root / "packaging/icons" / name));
    }
    const std::string resource = ReadAll(root / "src/shadow_of_the_beast.rc");
    CHECK(Contains(resource, "1 ICON \"../packaging/icons/app-icon.ico\""));
    const std::string cmake = ReadAll(root / "CMakeLists.txt");
    CHECK(Contains(cmake, "src/shadow_of_the_beast.rc"));
    CHECK(Contains(cmake, "${CMAKE_SOURCE_DIR}/packaging/icons/app-icon.ico"));
    CHECK(Contains(cmake, "OBJECT_DEPENDS"));
    const std::string ico = ReadAll(root / "packaging/icons/app-icon.ico");
    CHECK(ico.size() > 6 && ico[0] == 0 && ico[1] == 0 && ico[2] == 1 && ico[3] == 0);
    for (const auto& entry : fs::recursive_directory_iterator(root / "launcher")) {
        if (entry.is_regular_file() && (entry.path().extension() == ".cpp" || entry.path().extension() == ".h")) {
            CHECK(!Contains(ReadAll(entry.path()), "SetWindowIcon") && !Contains(ReadAll(entry.path()), "app-icon"));
        }
    }
    CHECK(!fs::exists(root / "assets/launcher/app-icon.svg") && !fs::exists(root / "packaging/icons/shadow-icon.svg"));
}

void TestLauncherQuit() {
    CHECK(!LauncherQuitAllowed(true));
    CHECK(LauncherQuitAllowed(false));
    const std::string launcher = ReadAll(fs::path(SOTB_SOURCE_DIR) / "launcher/launcher.cpp");
    CHECK(Contains(launcher, "result == trg::Result::kQuit && LauncherQuitAllowed(ImGui::IsKeyPressed(ImGuiKey_Escape, false))"));
    CHECK(Contains(launcher, "config.hints = \"Enter  Play     Alt+F4  Quit     Ctrl+S  Save\""));
    CHECK(!Contains(launcher, "Esc  Quit"));
    CHECK(Contains(launcher, "if (!host_.PumpEvents())"));
}

void TestSkipIntro(const fs::path& base) {
    const fs::path file = base / "skip_intro.ini";
    WriteAll(file, "");
    LauncherSettings defaults(file);
    CHECK(!defaults.GetBool(kSkipIntroKey, false));
    bool managed_off = false;
    for (const ManagedSetting& entry : ManagedSettings()) managed_off |= entry.key == kSkipIntroKey && entry.default_value == "0";
    CHECK(managed_off);
    CHECK(sotb_skip_intro(file.string().c_str()) == 0 && sotb_skip_intro(nullptr) == 0);
    CHECK(Contains(SettingsSummary(defaults, nullptr), "Gameplay: Skip Intro off"));
    CHECK(IsValidSetting(kSkipIntroKey, "0") && IsValidSetting(kSkipIntroKey, "1"));
    WriteAll(file, "skip_intro = 1\n");
    LauncherSettings on(file);
    CHECK(on.GetBool(kSkipIntroKey, false) && sotb_skip_intro(file.string().c_str()) == 1);
    CHECK(Contains(SettingsSummary(on, nullptr), "Gameplay: Skip Intro on"));
    WriteAll(file, "skip_intro = 0\n");
    CHECK(sotb_skip_intro(file.string().c_str()) == 0);
    WriteAll(file, "skip_intro = 1\n");
    LauncherSettings reset(file);
    reset.ResetToDefaults();
    CHECK(reset.Get(kSkipIntroKey, "x") == "0");
    WriteAll(file, "# Set from the launcher\nframe_rate = 90\n");
    LauncherSettings edited(file);
    edited.SetBool(kSkipIntroKey, true);
    edited.SetBool(kControllerRumbleKey, false);
    CHECK(edited.Save());
    const std::string saved = ReadAll(file);
    CHECK(Contains(saved, "skip_intro = 1") && Contains(saved, "controller_rumble = 0") && Contains(saved, "frame_rate = 90"));
    LauncherSettings reopened(file);
    CHECK(reopened.GetBool(kSkipIntroKey, false) && !reopened.GetBool(kControllerRumbleKey, true));
    CHECK(sotb_skip_intro(file.string().c_str()) == 1);
    CHECK(Contains(SettingsSummary(reopened, nullptr), "Gameplay: Skip Intro on"));
    reopened.SetBool(kSkipIntroKey, false);
    CHECK(reopened.Save());
    CHECK(sotb_skip_intro(file.string().c_str()) == 0 && Contains(ReadAll(file), "skip_intro = 0"));
    LauncherSettings off(file);
    CHECK(!off.GetBool(kSkipIntroKey, true) && Contains(SettingsSummary(off, nullptr), "Gameplay: Skip Intro off"));
    const std::string launcher = ReadAll(fs::path(SOTB_SOURCE_DIR) / "launcher/launcher.cpp");
    const std::string rows_text = ReadAll(fs::path(SOTB_SOURCE_DIR) / "launcher/gameplay_rows.h");
    const size_t language = rows_text.find("ui.Combo(\"Menu & Subtitles\"");
    const size_t skip = rows_text.find("ui.Toggle(\"Skip Intro\"");
    const size_t start_rows = launcher.find("DrawGameplayStartRows(ui);");
    const size_t rate = launcher.find("ui.Combo(\"Frame rate\"");
    CHECK(language != std::string::npos && language < skip && start_rows != std::string::npos && start_rows < rate);
    CHECK(std::string(kSkipIntroHelp).find("Press Start") != std::string::npos);
    CHECK(std::string(kSkipIntroKey) == "skip_intro");
    CHECK(Contains(launcher, "DrawGameplayStartRows(ui);"));
    const std::string rows = ReadAll(fs::path(SOTB_SOURCE_DIR) / "launcher/gameplay_rows.h");
    CHECK(Contains(rows, "ui.Toggle(\"Skip Intro\", kSkipIntroHelp, kSkipIntroKey, false, \"Off\", \"On\")"));
    int definitions = 0;
    for (const char* dir : {"launcher", "src", "gpu/shim"}) {
        for (const auto& entry : fs::recursive_directory_iterator(fs::path(SOTB_SOURCE_DIR) / dir)) {
            const std::string extension = entry.path().extension().string();
            if (entry.is_regular_file() && (extension == ".cpp" || extension == ".h" || extension == ".c") &&
                entry.path().string().find("third_party") == std::string::npos) {
                const std::string text = ReadAll(entry.path());
                for (size_t at = text.find("\"skip_intro\""); at != std::string::npos; at = text.find("\"skip_intro\"", at + 1)) {
                    ++definitions;
                }
            }
        }
    }
    CHECK(definitions == 1);
    CHECK(Contains(launcher, "const bool saved = settings_.Save();") && Contains(launcher, "Launcher: settings %s, Skip Intro %s"));
}

void TestPageTexts() {
    const std::string launcher = ReadAll(fs::path(SOTB_SOURCE_DIR) / "launcher/launcher.cpp");
    for (const char* text : {"\"Check the game files and start playing.\"", "\"Window, monitor, scaling and presentation.\"",
                             "\"Resolution, anti-aliasing and texture filtering.\"", "\"Controller and keyboard.\"",
                             "\"Your trophies on this PC.\"", "\"This port, and where your files and settings live.\""}) {
        CHECK(Contains(launcher, text));
    }
    CHECK(Contains(launcher, "ui.Toggle(\"Show this launcher\", kShowLauncherHelp, kShowLauncherKey, true, \"Off\", \"On\")"));
    CHECK(Contains(std::string(kControllerRumbleHelp), "Applies at the next game start"));
    CHECK(std::string(kTextLanguageHelp) == "Language used by game menus and subtitles.");
    CHECK(std::string(kRendererChoices[0].name) == "Auto" && std::string(kRendererChoices[1].name) == "Vulkan" &&
          std::string(kRendererChoices[2].name) == "OpenGL (experimental)");
#ifdef _WIN32
    CHECK(std::string(kRendererChoices[3].name) == "Direct3D 11 (experimental)");
#endif
    CHECK(Contains(launcher, "Controller prompts") && Contains(launcher, "the game's own button art is not changed"));
}

void TestManagedSettings() {
    std::set<std::string_view> keys;
    for (const ManagedSetting& setting : ManagedSettings()) {
        CHECK(keys.insert(setting.key).second);
        CHECK(IsValidSetting(setting.key, setting.default_value) || setting.default_value.empty());
    }
    for (const char* key : {kDisplayModeKey, kRendererKey, kGraphicsCardKey, kVSyncKey, kPresentationKey, kUpscaleKey,
                            kSharpenKey, kSharpenStrengthKey, kAntiAliasingKey, kAnisotropyKey, kFrameRateKey,
                            kShowFpsTitleKey, kShowResolutionTitleKey, kShowFrameCounterKey, kControllerRumbleKey,
                            kDetailedLoggingKey, kWindowWidthKey, kWindowHeightKey, kMonitorKey, kShowLauncherKey}) {
        CHECK(keys.count(key) == 1);
    }
    CHECK(keys.count("overlay_corner") == 0 && keys.count("overlay_scale") == 0);
    CHECK(keys.count(kRenderResolutionKey) == 1 && keys.count("aspect_ratio") == 0 && keys.count("true_ultrawide") == 0);
    for (int action = 0; action < SOTB_KEY_COUNT; ++action) {
        CHECK(keys.count(sotb_key_actions[action].ini_key) == 1);
    }

    CHECK(IsValidSetting(kRendererKey, "auto") && IsValidSetting(kRendererKey, "vulkan") &&
          IsValidSetting(kRendererKey, "opengl") && IsValidSetting(kRendererKey, "d3d11") &&
          !IsValidSetting(kRendererKey, "metal"));
    CHECK(IsValidSetting(kDisplayModeKey, "borderless") && !IsValidSetting(kDisplayModeKey, "fullscreen"));
    CHECK(IsValidSetting(kPresentationKey, "stretch") && !IsValidSetting(kPresentationKey, "zoom"));
    CHECK(IsValidSetting(kAnisotropyKey, "16") && !IsValidSetting(kAnisotropyKey, "3"));
    CHECK(IsValidSetting(kFrameRateKey, "0") && IsValidSetting(kFrameRateKey, "240") && !IsValidSetting(kFrameRateKey, "75"));
    CHECK(IsValidSetting("cross", "space") || IsValidSetting(sotb_key_actions[SOTB_KEY_CROSS].ini_key, "space"));
    CHECK(!IsValidSetting(sotb_key_actions[SOTB_KEY_CROSS].ini_key, "nonsense-key"));
}

void TestSettingsFile(const fs::path& base) {
    const fs::path file = base / "sotb.ini";
    WriteAll(file,
             "# my comment\nrenderer = opengl\noverlay_corner = 2\nrender_resolution = 720p\nlauncher_x = 40\n"
             "frame_rate = 144\nunknown_future_key = keep\n");
    LauncherSettings settings(file);
    CHECK(settings.Get(kRendererKey, "vulkan") == "opengl");
    CHECK(settings.Get(kFrameRateKey, "60") == "144");
    CHECK(settings.Get("overlay_corner", "none") == "none");

    settings.SetInt(kFrameRateKey, 75);
    CHECK(settings.Get(kFrameRateKey, "60") == "60");
    settings.Set(kRendererKey, "d3d11");
    CHECK(settings.Save());
    std::string text = ReadAll(file);
    CHECK(Contains(text, "# my comment") && Contains(text, "overlay_corner = 2") &&
          Contains(text, "render_resolution = 720p") && Contains(text, "unknown_future_key = keep"));
    CHECK(Contains(text, "renderer = d3d11"));

    settings.SetBool(kControllerRumbleKey, false);
    settings.SetBool(kDetailedLoggingKey, false);
    settings.Set(kRendererKey, "auto");
    settings.Set(kGraphicsCardKey, "automatic");
    CHECK(settings.Save());
    settings.ResetToDefaults();
    CHECK(settings.Get(kRendererKey, "x") == kDefaultRenderer);
    CHECK(settings.GetBool(kControllerRumbleKey, false));
    CHECK(settings.GetBool(kDetailedLoggingKey, false));
    CHECK(settings.Get(kLauncherXKey, "") == "40");
    CHECK(settings.Save());
    text = ReadAll(file);
    CHECK(Contains(text, "launcher_x = 40"));
    CHECK(Contains(text, "overlay_corner = 2") && Contains(text, "unknown_future_key = keep"));
    CHECK(Contains(text, "renderer = vulkan"));

    LauncherSettings reloaded(file);
    CHECK(reloaded.Get(kRendererKey, "none") == "vulkan");
    CHECK(reloaded.GetBool(kControllerRumbleKey, false));
}

void TestDetailedLogging(const fs::path& base) {
    const fs::path file = base / "logging.ini";
    CHECK(sotb_detailed_logging(nullptr) == 1);
    CHECK(sotb_detailed_logging("") == 1);
    CHECK(sotb_detailed_logging((base / "missing.ini").string().c_str()) == 1);
    WriteAll(file, "detailed_logging = 0\n");
    CHECK(sotb_detailed_logging(file.string().c_str()) == 0);
    WriteAll(file, "detailed_logging = 1\n");
    CHECK(sotb_detailed_logging(file.string().c_str()) == 1);
    WriteAll(file, "detailed_logging = garbage\n");
    CHECK(sotb_detailed_logging(file.string().c_str()) == 1);
}

void TestRumble(const fs::path& base) {
    const fs::path file = base / "rumble.ini";
    SotbSettings::Values values;
    CHECK(values.controller_rumble);
    WriteAll(file, "controller_rumble = 0\n");
    CHECK(SotbSettings::Read(file.string(), values) && !values.controller_rumble);
    WriteAll(file, "controller_rumble = on\n");
    CHECK(SotbSettings::Read(file.string(), values) && values.controller_rumble);
    WriteAll(file, "controller_rumble = banana\n");
    values.controller_rumble = false;
    CHECK(SotbSettings::Read(file.string(), values) && !values.controller_rumble);
    CHECK(!SotbSettings::Read((base / "none.ini").string(), values));
}

void TestRendererSelection(const fs::path& base) {
    using namespace SotbRenderer;
    CHECK(ParseChoice("vulkan") == Vulkan && ParseChoice("auto") == Auto && ParseChoice("opengl") == OpenGL &&
          ParseChoice("d3d11", true) == Direct3D11);
    CHECK(ParseChoice("d3d11", false) == Vulkan);
    CHECK(ParseChoice("nonsense") == Vulkan && ParseChoice("") == Vulkan);

    CHECK(Resolve(Auto, true, "").backend == Backend::Vulkan && Resolve(Auto, true, "").fallback_reason.empty());
    const Resolution fallback = Resolve(Auto, false, "no Vulkan");
    CHECK(fallback.backend == Backend::OpenGL && fallback.fallback_reason == "no Vulkan");
    CHECK(Resolve(Vulkan, false, "no Vulkan").backend == Backend::Vulkan);
    CHECK(Resolve(OpenGL, true, "").backend == Backend::OpenGL);
    CHECK(Resolve(Direct3D11, false, "x").backend == Backend::Direct3D11);

    for (const Sotb::Choice& choice : kRendererChoices) {
        CHECK(IsValidSetting(kRendererKey, choice.key));
        CHECK(std::find(ChoiceKeys.begin(), ChoiceKeys.end(), choice.key) != ChoiceKeys.end());
    }
    CHECK(std::string(kDefaultRenderer) == "vulkan");

    const fs::path file = base / "renderer.ini";
    SotbSettings::Values values;
    CHECK(values.renderer == Vulkan);
    WriteAll(file, "renderer = auto\n");
    SotbSettings::Read(file.string(), values);
    CHECK(values.renderer == Auto);
    WriteAll(file, "renderer = opengl\n");
    SotbSettings::Read(file.string(), values);
    CHECK(values.renderer == OpenGL);
    WriteAll(file, "renderer = bogus\n");
    SotbSettings::Read(file.string(), values);
    CHECK(values.renderer == Vulkan);
}

void TestGraphicsCard(const fs::path& base) {
    using namespace SotbGraphics;
    const std::vector<std::string> none;
    const std::vector<std::string> one = {"GPU A"};
    const std::vector<std::string> two = {"GPU A", "", "GPU B"};
    CHECK(ResolveGpuByName(none, "") == -1 && ResolveGpuByName(none, "GPU A") == -1);
    CHECK(ResolveGpuByName(one, "") == 0 && ResolveGpuByName(one, "GPU A") == 0);
    CHECK(ResolveGpuByName(two, "") == 0);
    CHECK(ResolveGpuByName(two, "GPU B") == 2);
    CHECK(ResolveGpuByName(two, "Removed GPU") == 0);
    CHECK(FindGpuByName(two, "Removed GPU") == -1 && FindGpuByName(two, "") == -1);
    CHECK(GpuChoices(two).size() == 2);

    CHECK(GpuChoices(one) == one && GpuChoices(none).empty());
    for (const std::string& name : GpuChoices(two)) {
        CHECK(name != "Auto" && name != "Automatic" && name != "automatic");
    }
    CHECK(two[ResolveGpuByName(two, "RTX 4090")] == "GPU A");

    const fs::path file = base / "gpu.ini";
    SotbSettings::Values values;
    WriteAll(file, "graphics_card = automatic\n");
    SotbSettings::Read(file.string(), values);
    CHECK(values.graphics_card.empty());
    WriteAll(file, "graphics_card = GPU B\n");
    SotbSettings::Read(file.string(), values);
    CHECK(values.graphics_card == "GPU B");
}

void TestRenderResolutions(const fs::path& base) {
    const char* keys[] = {"native", "480p", "720p", "1440p", "1800p", "4K"};
    const int widths[] = {1920, 854, 1280, 2560, 3200, 3840};
    const int heights[] = {1080, 480, 720, 1440, 1800, 2160};
    const char* labels[] = {"Native \xE2\x80\x94 1920 \xC3\x97 1080", "480p \xE2\x80\x94 854 \xC3\x97 480",
                            "720p \xE2\x80\x94 1280 \xC3\x97 720",   "1440p \xE2\x80\x94 2560 \xC3\x97 1440",
                            "1800p \xE2\x80\x94 3200 \xC3\x97 1800", "4K \xE2\x80\x94 3840 \xC3\x97 2160"};
    for (int i = 0; i < SOTB_RESOLUTION_COUNT; ++i) {
        CHECK(sotb_resolution_index(keys[i]) == i);
        CHECK(sotb_resolutions[i].width == widths[i] && sotb_resolutions[i].height == heights[i]);
        CHECK(ResolutionLabel(sotb_resolutions[i]) == labels[i]);
        CHECK(IsValidSetting(kRenderResolutionKey, keys[i]));
        CHECK(sotb_resolutions[i].width * 9 == sotb_resolutions[i].height * 16 || i == 1);
    }
    CHECK(sotb_resolution_index("bogus") == 0 && sotb_resolution_index(nullptr) == 0);
    CHECK(!IsValidSetting(kRenderResolutionKey, "bogus"));
    CHECK(WindowSizes().size() == 5 && (WindowSizes()[0] == WindowSize{1280, 720}) && (WindowSizes()[4] == WindowSize{3840, 2160}));
    for (const WindowSize size : WindowSizes()) {
        CHECK(size.width * 9 == size.height * 16 || size.width == 1600);
    }

    const fs::path file = base / "old.ini";
    WriteAll(file, "aspect_ratio = 21:9\ntrue_ultrawide = 1\nrender_resolution = 720p\n");
    LauncherSettings settings(file);
    CHECK(settings.Get(kRenderResolutionKey, "native") == "720p");
    CHECK(sotb_resolution_index(settings.Get(kRenderResolutionKey, "native").c_str()) == 2);
    CHECK(SettingsSummary(settings, nullptr).find("aspect") == std::string::npos);
    CHECK(SettingsSummary(settings, nullptr).find("Render resolution 720p 1280x720") != std::string::npos);
    CHECK(sotb_render_resolution(file.string().c_str()) == 2);
    settings.ResetToDefaults();
    CHECK(settings.Save());
    const std::string text = ReadAll(file);
    CHECK(Contains(text, "aspect_ratio = 21:9") && Contains(text, "true_ultrawide = 1"));
    CHECK(settings.Get(kRenderResolutionKey, "x") == kNativeResolution);
}

std::vector<uint8_t> LoadBootImage() {
    std::ifstream in(SOTB_BOOT_IMAGE, std::ios::binary);
    if (!in) {
        return {};
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto u64 = [&](size_t offset) {
        uint64_t value = 0;
        std::memcpy(&value, data.data() + offset, 8);
        return value;
    };
    if (data.size() < 128 || std::memcmp(data.data(), "SOTBBOT1", 8)) {
        return {};
    }
    const uint64_t size = u64(8), segments = u64(24), relocations = u64(32), imports = u64(40);
    size_t offset = 8 + 6 * 8 + 8 + 4 * 8;
    const uint64_t modules = u64(offset);
    offset += 8 + modules * 56;
    offset += 8 + u64(offset) * 24;
    offset += segments * 24 + imports * 128 + relocations * 32;
    if (offset + size > data.size()) {
        return {};
    }
    return std::vector<uint8_t>(data.begin() + long(offset), data.begin() + long(offset + size));
}

void TestGuestPatches() {
    const std::vector<uint8_t> original = LoadBootImage();
    if (original.empty()) {
        std::printf("guest image not prepared: fingerprint checks skipped\n");
        return;
    }
    char error[200];
    std::vector<uint8_t> image = original;
    CHECK(guest_apply_display_size(image.data(), image.size(), 1280, 720, error, sizeof(error)));
    int changed = 0;
    for (size_t i = 0; i < image.size(); ++i) {
        changed += image[i] != original[i];
    }
    CHECK(changed > 0 && changed <= 6 * 4);
    uint32_t pair[2];
    std::memcpy(pair, image.data() + 0x6080b9, 8);
    CHECK(pair[0] == 1280 && pair[1] == 720);
    CHECK(!guest_apply_display_size(image.data(), image.size(), 2560, 1440, error, sizeof(error)));

    image = original;
    image[0x609292 + 4] ^= 1;
    const std::vector<uint8_t> damaged = image;
    CHECK(!guest_apply_display_size(image.data(), image.size(), 1280, 720, error, sizeof(error)));
    CHECK(image == damaged);
    CHECK(!guest_apply_display_size(image.data(), 0x1000, 1280, 720, error, sizeof(error)));
    std::vector<uint8_t> unknown(original.size(), 0);
    CHECK(!guest_apply_display_size(unknown.data(), unknown.size(), 1280, 720, error, sizeof(error)));
    CHECK(unknown == std::vector<uint8_t>(original.size(), 0));

    image = original;
    CHECK(guest_apply_display_size(image.data(), image.size(), 3200, 1800, error, sizeof(error)));
    for (const size_t site : {size_t(0x607f0c), size_t(0x6080b7), size_t(0x609011), size_t(0x609290)}) {
        std::memcpy(pair, image.data() + site + 2, 8);
        CHECK(pair[0] == 3200 && pair[1] == 1800);
    }
    std::memcpy(pair, image.data() + 0x60903d, 4);
    std::memcpy(pair + 1, image.data() + 0x609047, 4);
    CHECK(pair[0] == 3200 && pair[1] == 1800);
    for (size_t i = 0x1280000; i < 0x1280200; ++i) {
        CHECK(image[i] == original[i]);
    }
    for (size_t i = 0; i < image.size(); ++i) {
        if (image[i] != original[i]) {
            const bool in_site = (i >= 0x607f0c && i < 0x607f16) || (i >= 0x6080b7 && i < 0x6080c1) ||
                                 (i >= 0x609011 && i < 0x60901b) || (i >= 0x609037 && i < 0x609041) ||
                                 (i >= 0x609041 && i < 0x60904b) || (i >= 0x609290 && i < 0x60929a);
            CHECK(in_site);
        }
    }
}

void TestDisplayAndGraphicsParsing(const fs::path& base) {
    const fs::path file = base / "graphics.ini";
    SotbSettings::Values values;
    WriteAll(file,
             "display_mode = borderless\nvsync = off\npresentation = stretch\nfsr1 = on\nrcas = off\nrcas_strength = 30\n"
             "smaa = 1\nanisotropy = 8\nmonitor = 2\nwindow_width = 2560\nwindow_height = 1440\nshow_fps = on\n"
             "show_fps_title = yes\nshow_resolution_title = true\nframe_rate = 144\noverlay_corner = 3\n");
    CHECK(SotbSettings::Read(file.string(), values));
    CHECK(values.display_mode == SotbGraphics::Borderless && !values.vsync && values.presentation == SotbGraphics::Stretch);
    CHECK(values.fsr1 && !values.rcas && values.rcas_strength == 30 && values.smaa && values.anisotropy == 8);
    CHECK(values.monitor == 2 && values.requested_width == 2560 && values.requested_height == 1440);
    CHECK(values.show_fps && values.show_fps_title && values.show_resolution_title && values.frame_rate == 144);

    WriteAll(file, "anisotropy = 3\nrcas_strength = 500\nframe_rate = 75\npresentation = zoom\ndisplay_mode = fullscreen\n");
    SotbSettings::Values invalid;
    CHECK(SotbSettings::Read(file.string(), invalid));
    CHECK(invalid.anisotropy == 0 && invalid.rcas_strength == 75 && invalid.frame_rate == 60 &&
          invalid.presentation == SotbGraphics::Letterbox && invalid.display_mode == SotbGraphics::Windowed);

    for (const std::string_view level : kAnisotropyLevels) {
        CHECK(SotbGraphics::ParseAnisotropy(std::atoi(std::string(level).c_str())) == std::atoi(std::string(level).c_str()));
    }
    CHECK(SotbGraphics::FsrPassRuns(true, false, 1920, 1080, 2560, 1440));
    CHECK(!SotbGraphics::FsrPassRuns(false, false, 1920, 1080, 2560, 1440));
    CHECK(SotbGraphics::FsrPassRuns(false, true, 1920, 1080, 1920, 1080));
    CHECK(!SotbGraphics::FsrPassRuns(false, true, 1920, 1080, 2560, 1440));
    CHECK(!SotbGraphics::FsrPassRuns(true, true, 1920, 1080, 1280, 720));
}

void TestFrameRates() {
    CHECK(std::size(kFrameRates) == SotbFramePacing::kFrameRateModes.size());
    for (const std::string_view rate : kFrameRates) {
        CHECK(SotbFramePacing::IsFrameRateMode(std::atoi(std::string(rate).c_str())));
        CHECK(SotbFramePacing::ParseFrameRate(std::string(rate)) == std::atoi(std::string(rate).c_str()));
    }
    CHECK(SotbFramePacing::ParseFrameRate("75") == 60 && SotbFramePacing::ParseFrameRate("") == 60);
    CHECK(FrameRateLabel(0) == "Unlimited" && FrameRateLabel(144) == "144 FPS");
}

void TestSettingsSummary(const fs::path& base) {
    const fs::path file = base / "summary.ini";
    WriteAll(file, "renderer = auto\ngraphics_card = Removed GPU\nframe_rate = 120\ncontroller_rumble = 0\ndetailed_logging = 0\n");
    const LauncherSettings settings(file);
    const std::string text = SettingsSummary(settings, "CUSA03762 1.00");
    for (const char* part : {"Renderer auto", "graphics card Removed GPU", "Frame rate 120 FPS", "Controller rumble off",
                             "detailed logging off", "CUSA03762 1.00", "SMAA off", "anisotropy 0", "frame counter off"}) {
        CHECK(Contains(text, part));
    }
    CHECK(!Contains(text, "corner") && !Contains(text, "Dragon") && !Contains(text, "DCP"));
    const std::string defaults = SettingsSummary(LauncherSettings(base / "none.ini"), nullptr);
    CHECK(Contains(defaults, "Renderer vulkan") && Contains(defaults, "graphics card first available") &&
          Contains(defaults, "not a supported build"));
}

void TestKeys(const fs::path& base) {
    SotbKeyMap map;
    sotb_keys_defaults(&map);
    const fs::path file = base / "keys.ini";
    const char* key = sotb_key_actions[SOTB_KEY_CROSS].ini_key;
    WriteAll(file, std::string(key) + " = space\n");
    SotbKeyMap loaded;
    sotb_keys_load(file.string().c_str(), &loaded);
    CHECK(loaded.scancode[SOTB_KEY_CROSS] == sotb_key_from_name("space"));
    WriteAll(file, std::string(key) + " = notakey\n");
    sotb_keys_load(file.string().c_str(), &loaded);
    CHECK(loaded.scancode[SOTB_KEY_CROSS] == map.scancode[SOTB_KEY_CROSS]);
    CHECK(sotb_key_from_name("escape") == 0);
    char name[32];
    CHECK(sotb_key_name(sotb_key_actions[SOTB_KEY_CIRCLE].default_scancode, name, sizeof(name))[0] != 0);
    SotbKeyMap duplicate = map;
    CHECK(sotb_keys_apply(&duplicate, sotb_key_actions[SOTB_KEY_CROSS].ini_key, "space") &&
          sotb_keys_apply(&duplicate, sotb_key_actions[SOTB_KEY_CIRCLE].ini_key, "space") &&
          duplicate.scancode[SOTB_KEY_CROSS] == duplicate.scancode[SOTB_KEY_CIRCLE]);
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / ("sotb_launcher_tests_" + std::to_string(std::rand()));
    fs::create_directories(base);
    TestPages();
    TestXboxMapping();
    TestTextLanguages(base);
    TestPageIcons();
    TestSidebarIcons();
    TestAppIcon();
    TestPageTexts();
    TestSkipIntro(base);
    TestLauncherQuit();
    TestManagedSettings();
    TestSettingsFile(base);
    TestDetailedLogging(base);
    TestRumble(base);
    TestRendererSelection(base);
    TestGraphicsCard(base);
    TestRenderResolutions(base);
    TestGuestPatches();
    TestDisplayAndGraphicsParsing(base);
    TestFrameRates();
    TestSettingsSummary(base);
    TestKeys(base);
    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

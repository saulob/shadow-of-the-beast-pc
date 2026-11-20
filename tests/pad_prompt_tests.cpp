// Controller type and prompt tests: which controller family is connected, which button names the Auto, Xbox and
// PlayStation modes show, how the active controller is followed, how the mode is persisted, and that none of it changes
// the PS4 buttons the game receives. Synthetic device metadata only: no controller, no window, no game.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "options.h"
#include "settings.h"
#include "sotb_pad.h"
#include "sotb_pad_prompts.h"

namespace fs = std::filesystem;
using namespace Sotb;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

SotbPadKind Classify(SDL_GamepadType type, SDL_GamepadType real = SDL_GAMEPAD_TYPE_UNKNOWN, unsigned vendor = 0, unsigned product = 0) {
    return sotb_pad_classify(static_cast<int>(type), static_cast<int>(real), vendor, product);
}

constexpr unsigned kSony = 0x054c, kMicrosoft = 0x045e, kValve = 0x28de, kSteamVirtual = 0x11ff;

void TestDetection() {
    CHECK(Classify(SDL_GAMEPAD_TYPE_XBOX360) == SOTB_PAD_KIND_XBOX);
    CHECK(Classify(SDL_GAMEPAD_TYPE_XBOXONE) == SOTB_PAD_KIND_XBOX);
    CHECK(Classify(SDL_GAMEPAD_TYPE_PS4) == SOTB_PAD_KIND_DUALSHOCK4);
    CHECK(Classify(SDL_GAMEPAD_TYPE_PS5) == SOTB_PAD_KIND_DUALSENSE);
    CHECK(Classify(SDL_GAMEPAD_TYPE_PS3) == SOTB_PAD_KIND_DUALSHOCK3);
    CHECK(Classify(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO) == SOTB_PAD_KIND_GENERIC);
    CHECK(Classify(SDL_GAMEPAD_TYPE_GAMECUBE) == SOTB_PAD_KIND_GENERIC);
    // SDL knows no type: only device metadata decides, never the button order.
    CHECK(Classify(SDL_GAMEPAD_TYPE_UNKNOWN) == SOTB_PAD_KIND_UNKNOWN);
    CHECK(Classify(SDL_GAMEPAD_TYPE_STANDARD) == SOTB_PAD_KIND_UNKNOWN);
    CHECK(Classify(SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_TYPE_UNKNOWN, 0x1234, 0x5678) == SOTB_PAD_KIND_UNKNOWN);
    CHECK(Classify(SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_UNKNOWN, kMicrosoft, 0x02ea) == SOTB_PAD_KIND_XBOX);
    CHECK(Classify(SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_UNKNOWN, kSony, 0x05c4) == SOTB_PAD_KIND_DUALSHOCK4);
    CHECK(Classify(SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_UNKNOWN, kSony, 0x09cc) == SOTB_PAD_KIND_DUALSHOCK4);
    CHECK(Classify(SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_UNKNOWN, kSony, 0x0ce6) == SOTB_PAD_KIND_DUALSENSE);
    CHECK(Classify(SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_UNKNOWN, kSony, 0xffff) == SOTB_PAD_KIND_UNKNOWN);
    // Steam Input: the virtual device presents as an Xbox pad, but the real device behind it wins when SDL knows it.
    CHECK(sotb_pad_is_steam_virtual(kValve, kSteamVirtual) && !sotb_pad_is_steam_virtual(kSony, kSteamVirtual));
    CHECK(Classify(SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_UNKNOWN, kValve, kSteamVirtual) == SOTB_PAD_KIND_XBOX);
    CHECK(Classify(SDL_GAMEPAD_TYPE_XBOX360, SDL_GAMEPAD_TYPE_PS5, kValve, kSteamVirtual) == SOTB_PAD_KIND_DUALSENSE);
    CHECK(Classify(SDL_GAMEPAD_TYPE_XBOX360, SDL_GAMEPAD_TYPE_PS4, kValve, kSteamVirtual) == SOTB_PAD_KIND_DUALSHOCK4);
    CHECK(Classify(SDL_GAMEPAD_TYPE_XBOX360, SDL_GAMEPAD_TYPE_UNKNOWN, kValve, kSteamVirtual) == SOTB_PAD_KIND_XBOX);
    for (const SotbPadKind kind : {SOTB_PAD_KIND_UNKNOWN, SOTB_PAD_KIND_XBOX, SOTB_PAD_KIND_DUALSHOCK3, SOTB_PAD_KIND_DUALSHOCK4,
                                   SOTB_PAD_KIND_DUALSENSE, SOTB_PAD_KIND_GENERIC}) {
        CHECK(std::strlen(sotb_pad_kind_name(kind)) > 0);
    }
}

void TestFamilyPolicy() {
    // Auto follows the controller.
    CHECK(sotb_prompt_family(SOTB_PROMPT_AUTO, SOTB_PAD_KIND_XBOX) == SOTB_PROMPT_FAMILY_XBOX);
    CHECK(sotb_prompt_family(SOTB_PROMPT_AUTO, SOTB_PAD_KIND_DUALSHOCK4) == SOTB_PROMPT_FAMILY_PLAYSTATION);
    CHECK(sotb_prompt_family(SOTB_PROMPT_AUTO, SOTB_PAD_KIND_DUALSENSE) == SOTB_PROMPT_FAMILY_PLAYSTATION);
    CHECK(sotb_prompt_family(SOTB_PROMPT_AUTO, SOTB_PAD_KIND_DUALSHOCK3) == SOTB_PROMPT_FAMILY_PLAYSTATION);
    // Generic, unknown and no controller: the Xbox names, which are SDL's own layout and the launcher's existing default.
    CHECK(sotb_prompt_family(SOTB_PROMPT_AUTO, SOTB_PAD_KIND_GENERIC) == SOTB_PROMPT_FAMILY_XBOX);
    CHECK(sotb_prompt_family(SOTB_PROMPT_AUTO, SOTB_PAD_KIND_UNKNOWN) == SOTB_PROMPT_FAMILY_XBOX);
    // A manual mode wins over every controller.
    for (int kind = SOTB_PAD_KIND_UNKNOWN; kind <= SOTB_PAD_KIND_GENERIC; ++kind) {
        CHECK(sotb_prompt_family(SOTB_PROMPT_XBOX, static_cast<SotbPadKind>(kind)) == SOTB_PROMPT_FAMILY_XBOX);
        CHECK(sotb_prompt_family(SOTB_PROMPT_PLAYSTATION, static_cast<SotbPadKind>(kind)) == SOTB_PROMPT_FAMILY_PLAYSTATION);
    }
    CHECK(std::string(sotb_prompt_family_name(SOTB_PROMPT_FAMILY_XBOX)) == "Xbox");
    CHECK(std::string(sotb_prompt_family_name(SOTB_PROMPT_FAMILY_PLAYSTATION)) == "PlayStation");
}

void TestModeNames() {
    CHECK(sotb_prompt_mode_from_name("auto") == SOTB_PROMPT_AUTO && sotb_prompt_mode_from_name("xbox") == SOTB_PROMPT_XBOX);
    CHECK(sotb_prompt_mode_from_name("playstation") == SOTB_PROMPT_PLAYSTATION && sotb_prompt_mode_from_name("XBOX") == SOTB_PROMPT_XBOX);
    CHECK(sotb_prompt_mode_from_name(nullptr) == SOTB_PROMPT_AUTO && sotb_prompt_mode_from_name("") == SOTB_PROMPT_AUTO);
    CHECK(sotb_prompt_mode_from_name("nintendo") == SOTB_PROMPT_AUTO && sotb_prompt_mode_from_name("2") == SOTB_PROMPT_AUTO);
    for (const SotbPromptMode mode : {SOTB_PROMPT_AUTO, SOTB_PROMPT_XBOX, SOTB_PROMPT_PLAYSTATION}) {
        CHECK(sotb_prompt_mode_from_name(sotb_prompt_mode_name(mode)) == mode);
    }
    CHECK(std::string(PromptModeLabel(SOTB_PROMPT_AUTO)) == "Auto" && std::string(PromptModeLabel(SOTB_PROMPT_PLAYSTATION)) == "PlayStation");
}

void TestActiveControllerTracking() {
    const SotbPadDevice xbox{10, SOTB_PAD_KIND_XBOX}, dualsense{11, SOTB_PAD_KIND_DUALSENSE}, dualshock{12, SOTB_PAD_KIND_DUALSHOCK4};
    SotbActivePad pad;
    sotb_active_pad_init(&pad);
    CHECK(sotb_active_pad_update(&pad, nullptr, 0, 0) == SOTB_PAD_KIND_UNKNOWN);
    // The first controller listed is active until another one is used.
    std::vector<SotbPadDevice> devices{xbox, dualsense};
    CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 0) == SOTB_PAD_KIND_XBOX && pad.active_id == 10);
    // A stray press on the other controller does not flip the family...
    CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 11) == SOTB_PAD_KIND_XBOX);
    CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 10) == SOTB_PAD_KIND_XBOX);
    CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 11) == SOTB_PAD_KIND_XBOX);
    CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 11) == SOTB_PAD_KIND_XBOX);
    CHECK(pad.active_id == 10);
    // ...a controller that keeps being used does.
    CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 11) == SOTB_PAD_KIND_DUALSENSE);
    CHECK(pad.active_id == 11 && pad.candidate_hits == 0);
    for (int i = 0; i < 20; ++i) {
        CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 0) == SOTB_PAD_KIND_DUALSENSE); // idle: stays
    }
    // An inactive controller of the same family changes nothing visible.
    devices = {dualsense, dualshock};
    for (int i = 0; i < SOTB_ACTIVE_PAD_SWITCH_HITS; ++i) {
        CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 12) == (i + 1 < SOTB_ACTIVE_PAD_SWITCH_HITS ? SOTB_PAD_KIND_DUALSENSE : SOTB_PAD_KIND_DUALSHOCK4));
    }
    // The active controller is unplugged: the next one takes over; with none left the last family is kept.
    devices = {xbox};
    CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 0) == SOTB_PAD_KIND_XBOX && pad.active_id == 10);
    CHECK(sotb_active_pad_update(&pad, nullptr, 0, 0) == SOTB_PAD_KIND_XBOX && pad.active_id == 0);
    // An input id that is not connected is ignored.
    devices = {dualsense};
    for (int i = 0; i < 10; ++i) {
        sotb_active_pad_update(&pad, devices.data(), devices.size(), 999);
    }
    CHECK(pad.kind == SOTB_PAD_KIND_DUALSENSE && pad.active_id == 11);
    // Alternating presses between two controllers never reach the switch count: no flapping.
    devices = {xbox, dualsense};
    sotb_active_pad_init(&pad);
    sotb_active_pad_update(&pad, devices.data(), devices.size(), 0);
    const SotbPadKind first = pad.kind;
    for (int i = 0; i < 40; ++i) {
        sotb_active_pad_update(&pad, devices.data(), devices.size(), i % 2 ? 11 : 10);
    }
    CHECK(pad.kind == first);
    // A virtual duplicate (Steam Input) listed beside the real pad: the real pad's family is kept.
    const SotbPadDevice real{20, SOTB_PAD_KIND_DUALSENSE}, steam{21, SOTB_PAD_KIND_XBOX};
    devices = {real, steam};
    sotb_active_pad_init(&pad);
    CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 20) == SOTB_PAD_KIND_DUALSENSE);
    CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 21) == SOTB_PAD_KIND_DUALSENSE);
    CHECK(sotb_active_pad_update(&pad, devices.data(), devices.size(), 20) == SOTB_PAD_KIND_DUALSENSE);
}

void TestModeSurvivesControllerChanges() {
    SotbActivePad pad;
    sotb_active_pad_init(&pad);
    std::vector<SotbPadDevice> devices{{1, SOTB_PAD_KIND_XBOX}};
    for (const SotbPromptMode mode : {SOTB_PROMPT_XBOX, SOTB_PROMPT_PLAYSTATION}) {
        const SotbPromptFamily wanted = mode == SOTB_PROMPT_XBOX ? SOTB_PROMPT_FAMILY_XBOX : SOTB_PROMPT_FAMILY_PLAYSTATION;
        for (const SotbPadKind kind : {SOTB_PAD_KIND_XBOX, SOTB_PAD_KIND_DUALSENSE, SOTB_PAD_KIND_DUALSHOCK4, SOTB_PAD_KIND_GENERIC}) {
            devices = {{1, kind}};
            sotb_active_pad_init(&pad);
            CHECK(sotb_prompt_family(mode, sotb_active_pad_update(&pad, devices.data(), devices.size(), 0)) == wanted);
        }
    }
    // Auto follows a change of the active controller.
    sotb_active_pad_init(&pad);
    devices = {{1, SOTB_PAD_KIND_XBOX}};
    CHECK(sotb_prompt_family(SOTB_PROMPT_AUTO, sotb_active_pad_update(&pad, devices.data(), devices.size(), 0)) == SOTB_PROMPT_FAMILY_XBOX);
    devices = {{2, SOTB_PAD_KIND_DUALSENSE}};
    CHECK(sotb_prompt_family(SOTB_PROMPT_AUTO, sotb_active_pad_update(&pad, devices.data(), devices.size(), 0)) == SOTB_PROMPT_FAMILY_PLAYSTATION);
}

void TestPersistence(const fs::path& base) {
    const fs::path file = base / "prompts.ini";
    {
        LauncherSettings settings(file);
        CHECK(SavedPromptMode(settings) == SOTB_PROMPT_AUTO);
        settings.Set(kControllerPromptsKey, "playstation");
        CHECK(SavedPromptMode(settings) == SOTB_PROMPT_PLAYSTATION);
        CHECK(settings.Save());
    }
    CHECK(Contains(ReadAll(file), "controller_prompts = playstation"));
    {
        const LauncherSettings reloaded(file);
        CHECK(SavedPromptMode(reloaded) == SOTB_PROMPT_PLAYSTATION);
    }
    for (const char* bad : {"nintendo", "", "PLAYSTATION5", "1", "xbox one"}) {
        std::ofstream(file, std::ios::trunc) << "controller_prompts = " << bad << "\n";
        const LauncherSettings invalid(file);
        CHECK(SavedPromptMode(invalid) == SOTB_PROMPT_AUTO);
        CHECK(!IsValidSetting(kControllerPromptsKey, bad));
    }
    CHECK(IsValidSetting(kControllerPromptsKey, "auto") && IsValidSetting(kControllerPromptsKey, "xbox") &&
          IsValidSetting(kControllerPromptsKey, "playstation"));
    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        managed = managed || (std::string(setting.key) == kControllerPromptsKey && std::string(setting.default_value) == "auto");
    }
    CHECK(managed);
    // The runtime reads the same line the launcher wrote.
    CHECK(Contains(ReadAll(fs::path(SOTB_SOURCE_DIR) / "src/runtime_pad.c"), "\"controller_prompts\""));
    // A file that never had the setting is Auto, and a missing file too.
    std::ofstream(file, std::ios::trunc) << "renderer = auto\n";
    CHECK(SavedPromptMode(LauncherSettings(file)) == SOTB_PROMPT_AUTO);
    CHECK(SavedPromptMode(LauncherSettings(base / "none.ini")) == SOTB_PROMPT_AUTO);
}

SotbPadMap MapOf(const LauncherSettings& settings) {
    SotbPadMap map;
    for (int action = 0; action < SOTB_PAD_COUNT; ++action) {
        map.source[action] = SavedPadSource(settings, static_cast<SotbPadAction>(action));
    }
    return map;
}

void TestPromptsAndBindingsAreIndependent(const fs::path& base) {
    LauncherSettings settings(base / "independent.ini");
    SetPadSource(settings, SOTB_PAD_CROSS, SDL_GAMEPAD_BUTTON_EAST);
    SetPadSource(settings, SOTB_PAD_TRIANGLE, SOTB_PAD_SOURCE_LEFT_TRIGGER);
    const SotbPadMap before = MapOf(settings);
    SotbPadInput press{};
    press.buttons = 1u << SDL_GAMEPAD_BUTTON_EAST;
    SotbPadOutput expected;
    sotb_pad_translate(&before, &press, &expected);
    CHECK((expected.pressed & (1u << SOTB_PAD_CROSS)) != 0);
    // Changing the prompt mode never changes what the game receives.
    for (const char* mode : {"xbox", "playstation", "auto", "bogus"}) {
        settings.Set(kControllerPromptsKey, mode);
        const SotbPadMap map = MapOf(settings);
        CHECK(std::memcmp(&map, &before, sizeof(map)) == 0);
        SotbPadOutput out;
        sotb_pad_translate(&map, &press, &out);
        CHECK(out.pressed == expected.pressed && out.l2 == expected.l2 && out.r2 == expected.r2);
    }
    // Rebinding never changes the detected family or the mode.
    const SotbPadKind kind = sotb_pad_classify(SDL_GAMEPAD_TYPE_PS5, SDL_GAMEPAD_TYPE_UNKNOWN, 0, 0);
    settings.Set(kControllerPromptsKey, "auto");
    const SotbPromptFamily family = sotb_prompt_family(SavedPromptMode(settings), kind);
    for (const int source : {static_cast<int>(SDL_GAMEPAD_BUTTON_WEST), static_cast<int>(SDL_GAMEPAD_BUTTON_BACK), SOTB_PAD_SOURCE_RIGHT_TRIGGER}) {
        SetPadSource(settings, SOTB_PAD_SQUARE, source);
        CHECK(sotb_prompt_family(SavedPromptMode(settings), kind) == family && family == SOTB_PROMPT_FAMILY_PLAYSTATION);
    }
    ResetPads(settings);
    CHECK(SavedPromptMode(settings) == SOTB_PROMPT_AUTO);
}

void TestRegionalConfirmBack() {
    // The game asks the system for its enter button and gets Cross; no prompt family or mode changes that.
    CHECK(sotb_prompt_enter_button_assign() == 1);
    CHECK(sotb_prompt_confirm_action() == SOTB_PAD_CROSS && sotb_prompt_back_action() == SOTB_PAD_CIRCLE);
    CHECK(std::string(sotb_pad_actions[sotb_prompt_confirm_action()].name) == "cross");
    CHECK(std::string(sotb_pad_actions[sotb_prompt_back_action()].name) == "circle");
    // The defaults place Confirm on the bottom face button and Back on the right one, for every family.
    SotbPadMap map;
    sotb_pad_defaults(&map);
    CHECK(map.source[SOTB_PAD_CROSS] == SDL_GAMEPAD_BUTTON_SOUTH && map.source[SOTB_PAD_CIRCLE] == SDL_GAMEPAD_BUTTON_EAST);
    for (const SotbPromptFamily family : {SOTB_PROMPT_FAMILY_XBOX, SOTB_PROMPT_FAMILY_PLAYSTATION}) {
        CHECK(std::string(sotb_pad_source_label(map.source[SOTB_PAD_CROSS], family)) == (family == SOTB_PROMPT_FAMILY_XBOX ? "A" : "Cross"));
        CHECK(std::string(sotb_pad_source_label(map.source[SOTB_PAD_CIRCLE], family)) == (family == SOTB_PROMPT_FAMILY_XBOX ? "B" : "Circle"));
    }
    const std::string services = ReadAll(fs::path(SOTB_SOURCE_DIR) / "src/runtime_services.c");
    CHECK(Contains(services, "case 1000: *value=sotb_prompt_enter_button_assign()"));
    // The host menus keep Cross confirm and Circle back whatever the controller or prompt mode is.
    const std::string overlay = ReadAll(fs::path(SOTB_SOURCE_DIR) / "gpu/shim/sotb_overlay.cpp");
    CHECK(Contains(overlay, "Circle = 0x2000, Cross = 0x4000") && Contains(overlay, "Cross (A) = OK, Circle (B) = cancel"));
    CHECK(Contains(overlay, "(pressed & Cross) ? 1 : (pressed & Circle) ? 2 : 0"));
}

void TestLabels(const fs::path& base) {
    std::set<std::string> xbox, playstation;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
        if (!sotb_pad_source_bindable(button)) {
            continue;
        }
        const std::string x = sotb_pad_source_label(button, SOTB_PROMPT_FAMILY_XBOX);
        const std::string p = sotb_pad_source_label(button, SOTB_PROMPT_FAMILY_PLAYSTATION);
        CHECK(!x.empty() && !p.empty());
        CHECK(xbox.insert(x).second && playstation.insert(p).second);
    }
    for (const int trigger : {SOTB_PAD_SOURCE_LEFT_TRIGGER, SOTB_PAD_SOURCE_RIGHT_TRIGGER}) {
        CHECK(xbox.insert(sotb_pad_source_label(trigger, SOTB_PROMPT_FAMILY_XBOX)).second);
        CHECK(playstation.insert(sotb_pad_source_label(trigger, SOTB_PROMPT_FAMILY_PLAYSTATION)).second);
    }
    CHECK(std::string(sotb_pad_source_label(SDL_GAMEPAD_BUTTON_NORTH, SOTB_PROMPT_FAMILY_XBOX)) == "Y");
    CHECK(std::string(sotb_pad_source_label(SDL_GAMEPAD_BUTTON_NORTH, SOTB_PROMPT_FAMILY_PLAYSTATION)) == "Triangle");
    CHECK(std::string(sotb_pad_source_label(SOTB_PAD_SOURCE_LEFT_TRIGGER, SOTB_PROMPT_FAMILY_XBOX)) == "LT");
    CHECK(std::string(sotb_pad_source_label(SOTB_PAD_SOURCE_LEFT_TRIGGER, SOTB_PROMPT_FAMILY_PLAYSTATION)) == "L2");
    CHECK(std::string(sotb_pad_source_label(SDL_GAMEPAD_BUTTON_GUIDE, SOTB_PROMPT_FAMILY_XBOX)).empty());
    CHECK(std::string(sotb_pad_source_label(-1, SOTB_PROMPT_FAMILY_PLAYSTATION)).empty());

    // The launcher's row for a binding follows the family; the binding itself does not.
    LauncherSettings settings(base / "labels.ini");
    SetPadSource(settings, SOTB_PAD_CROSS, SDL_GAMEPAD_BUTTON_EAST);
    CHECK(PadBindingLabel(settings, SOTB_PAD_CROSS, SOTB_PROMPT_FAMILY_XBOX) == "B");
    CHECK(PadBindingLabel(settings, SOTB_PAD_CROSS, SOTB_PROMPT_FAMILY_PLAYSTATION) == "Circle");
    CHECK(PadBindingLabel(settings, SOTB_PAD_CIRCLE, SOTB_PROMPT_FAMILY_XBOX) == "B");
    CHECK(SavedPadSource(settings, SOTB_PAD_CROSS) == SDL_GAMEPAD_BUTTON_EAST);
    // Row names are the PS4 buttons the game receives, in every family.
    for (const SotbPadInfo& action : sotb_pad_actions) {
        CHECK(std::strlen(action.label) > 0);
    }
}

void TestLimitationIsDocumented() {
    // The game's button art is baked into its paks: the launcher says so and nothing pretends to replace it.
    const std::string launcher = ReadAll(fs::path(SOTB_SOURCE_DIR) / "launcher/launcher.cpp");
    CHECK(Contains(launcher, "the game's own button art is not changed"));
    const std::string header = ReadAll(fs::path(SOTB_SOURCE_DIR) / "src/sotb_pad_prompts.h");
    CHECK(Contains(header, "baked into its paks"));
    const std::string audit = ReadAll(fs::path(SOTB_SOURCE_DIR) / "docs/LAUNCHER_PARITY_AUDIT.md");
    CHECK(Contains(audit, "no prompt atlas or glyph textures"));
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / ("sotb-pad-prompt-tests-" + std::to_string(std::rand()));
    fs::create_directories(base);
    TestDetection();
    TestFamilyPolicy();
    TestModeNames();
    TestActiveControllerTracking();
    TestModeSurvivesControllerChanges();
    TestPersistence(base);
    TestPromptsAndBindingsAreIndependent(base);
    TestRegionalConfirmBack();
    TestLabels(base);
    TestLimitationIsDocumented();
    std::error_code ignored;
    fs::remove_all(base, ignored);
    std::printf("pad prompt tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

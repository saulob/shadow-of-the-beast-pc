// Controller rebinding tests: the physical-to-guest mapping, its persistence in the settings file, the Controls page's
// capture rules and the ways a mapping must survive (reset, hotplug, older settings files). Synthetic gamepad samples
// only: no controller, no window, no game, no audio.
#include <algorithm>
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
#include "pad_capture.h"
#include "settings.h"
#include "sotb_keys.h"
#include "sotb_pad.h"

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

void WriteAll(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string ReadSource(const std::string& relative) { return ReadAll(fs::path(SOTB_SOURCE_DIR) / relative); }

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

SotbPadInput Held(int source) {
    SotbPadInput in{};
    if (source == SOTB_PAD_SOURCE_LEFT_TRIGGER) {
        in.left_trigger = 255;
    } else if (source == SOTB_PAD_SOURCE_RIGHT_TRIGGER) {
        in.right_trigger = 255;
    } else if (source >= 0) {
        in.buttons = 1u << source;
    }
    return in;
}

SotbPadOutput Translate(const SotbPadMap& map, const SotbPadInput& in) {
    SotbPadOutput out;
    sotb_pad_translate(&map, &in, &out);
    return out;
}

SotbPadMap Defaults() {
    SotbPadMap map;
    sotb_pad_defaults(&map);
    return map;
}

std::vector<int> AllSources() {
    std::vector<int> sources;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
        if (sotb_pad_source_bindable(button)) {
            sources.push_back(button);
        }
    }
    sources.push_back(SOTB_PAD_SOURCE_LEFT_TRIGGER);
    sources.push_back(SOTB_PAD_SOURCE_RIGHT_TRIGGER);
    return sources;
}

uint32_t ActionBit(SotbPadAction action) { return 1u << action; }

void TestDefaultMapping() {
    const SotbPadMap map = Defaults();
    const struct { SotbPadAction action; int source; } expected[] = {
        {SOTB_PAD_CROSS, SDL_GAMEPAD_BUTTON_SOUTH},        {SOTB_PAD_CIRCLE, SDL_GAMEPAD_BUTTON_EAST},
        {SOTB_PAD_SQUARE, SDL_GAMEPAD_BUTTON_WEST},        {SOTB_PAD_TRIANGLE, SDL_GAMEPAD_BUTTON_NORTH},
        {SOTB_PAD_L1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},   {SOTB_PAD_R1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {SOTB_PAD_L2, SOTB_PAD_SOURCE_LEFT_TRIGGER},       {SOTB_PAD_R2, SOTB_PAD_SOURCE_RIGHT_TRIGGER},
        {SOTB_PAD_L3, SDL_GAMEPAD_BUTTON_LEFT_STICK},      {SOTB_PAD_R3, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {SOTB_PAD_OPTIONS, SDL_GAMEPAD_BUTTON_START},      {SOTB_PAD_TOUCHPAD, SDL_GAMEPAD_BUTTON_BACK},
        {SOTB_PAD_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},    {SOTB_PAD_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {SOTB_PAD_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {SOTB_PAD_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    CHECK(std::size(expected) == SOTB_PAD_COUNT);
    for (const auto& entry : expected) {
        CHECK(map.source[entry.action] == entry.source);
        CHECK(sotb_pad_actions[entry.action].default_source == entry.source);
        const SotbPadOutput out = Translate(map, Held(entry.source));
        CHECK((out.pressed & ActionBit(entry.action)) != 0);
    }
    CHECK(sotb_pad_changed(&map) == 0);
    const SotbPadOutput idle = Translate(map, SotbPadInput{});
    CHECK(idle.pressed == 0 && idle.l2 == 0 && idle.r2 == 0);
    // One physical button presses exactly its own PS4 button.
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_SOUTH)).pressed == ActionBit(SOTB_PAD_CROSS));
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_NORTH)).pressed == ActionBit(SOTB_PAD_TRIANGLE));
    // The sticks stay analog and the Guide button belongs to the system.
    CHECK(sotb_pad_source_from_name("leftx") == SOTB_PAD_SOURCE_NONE && sotb_pad_source_from_name("righty") == SOTB_PAD_SOURCE_NONE);
    CHECK(sotb_pad_source_from_name("guide") == SOTB_PAD_SOURCE_NONE);
}

void TestEveryActionEverySource() {
    const std::vector<int> sources = AllSources();
    CHECK(sources.size() >= 22);
    for (int action = 0; action < SOTB_PAD_COUNT; ++action) {
        for (const int source : sources) {
            SotbPadMap map = Defaults();
            map.source[action] = source;
            const SotbPadOutput out = Translate(map, Held(source));
            uint32_t expected = 0;
            for (int i = 0; i < SOTB_PAD_COUNT; ++i) {
                expected |= map.source[i] == source ? 1u << i : 0u;
            }
            if (source == SDL_GAMEPAD_BUTTON_TOUCHPAD && map.source[SOTB_PAD_TOUCHPAD] == SDL_GAMEPAD_BUTTON_BACK) {
                expected |= ActionBit(SOTB_PAD_TOUCHPAD);
            }
            CHECK(out.pressed == expected);
            CHECK((out.pressed & ActionBit(static_cast<SotbPadAction>(action))) != 0);
        }
    }
}

void TestNamesRoundTrip() {
    for (const int source : AllSources()) {
        char name[32];
        CHECK(sotb_pad_source_name(source, name, sizeof(name))[0] != 0);
        CHECK(sotb_pad_source_from_name(name) == source);
        std::string upper = name;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        CHECK(sotb_pad_source_from_name(upper.c_str()) == source);
    }
    char name[32];
    CHECK(sotb_pad_source_name(SDL_GAMEPAD_BUTTON_GUIDE, name, sizeof(name))[0] == 0);
    CHECK(sotb_pad_source_name(SOTB_PAD_SOURCE_NONE, name, sizeof(name))[0] == 0);
    CHECK(sotb_pad_source_name(0x7777, name, sizeof(name))[0] == 0);
    CHECK(sotb_pad_source_from_name(nullptr) == SOTB_PAD_SOURCE_NONE && sotb_pad_source_from_name("") == SOTB_PAD_SOURCE_NONE);
    CHECK(sotb_pad_source_from_name("banana") == SOTB_PAD_SOURCE_NONE);
    CHECK(sotb_pad_source_from_name(std::string(200, 'a').c_str()) == SOTB_PAD_SOURCE_NONE);
    CHECK(sotb_pad_source_from_name("lefttrigger") == SOTB_PAD_SOURCE_LEFT_TRIGGER);
    CHECK(sotb_pad_source_from_name("righttrigger") == SOTB_PAD_SOURCE_RIGHT_TRIGGER);
    // The saved name never depends on a label.
    std::set<std::string> keys;
    for (const SotbPadInfo& action : sotb_pad_actions) {
        CHECK(keys.insert(action.ini_key).second && std::string(action.ini_key).rfind("pad_", 0) == 0);
    }
}

void TestTriggersShouldersClicksMenu() {
    SotbPadMap map = Defaults();
    // Triggers pass their analog value through, and press above the threshold.
    SotbPadInput in{};
    in.left_trigger = 200;
    in.right_trigger = 20;
    SotbPadOutput out = Translate(map, in);
    CHECK(out.l2 == 200 && out.r2 == 20);
    CHECK((out.pressed & ActionBit(SOTB_PAD_L2)) != 0 && (out.pressed & ActionBit(SOTB_PAD_R2)) == 0);
    in.right_trigger = SOTB_PAD_TRIGGER_PRESS;
    CHECK((Translate(map, in).pressed & ActionBit(SOTB_PAD_R2)) == 0);
    in.right_trigger = SOTB_PAD_TRIGGER_PRESS + 1;
    CHECK((Translate(map, in).pressed & ActionBit(SOTB_PAD_R2)) != 0);

    // A button on a trigger action reads as a full pull; a trigger on a button action presses it.
    map.source[SOTB_PAD_L2] = SDL_GAMEPAD_BUTTON_SOUTH;
    out = Translate(map, Held(SDL_GAMEPAD_BUTTON_SOUTH));
    CHECK(out.l2 == 255 && (out.pressed & ActionBit(SOTB_PAD_L2)) != 0);
    CHECK(Translate(map, Held(SOTB_PAD_SOURCE_LEFT_TRIGGER)).pressed == 0);
    map = Defaults();
    map.source[SOTB_PAD_SQUARE] = SOTB_PAD_SOURCE_RIGHT_TRIGGER;
    out = Translate(map, Held(SOTB_PAD_SOURCE_RIGHT_TRIGGER));
    CHECK((out.pressed & ActionBit(SOTB_PAD_SQUARE)) != 0 && (out.pressed & ActionBit(SOTB_PAD_R2)) != 0 && out.r2 == 255);
    // Swapped triggers.
    map = Defaults();
    map.source[SOTB_PAD_L2] = SOTB_PAD_SOURCE_RIGHT_TRIGGER;
    map.source[SOTB_PAD_R2] = SOTB_PAD_SOURCE_LEFT_TRIGGER;
    in = SotbPadInput{};
    in.left_trigger = 100;
    out = Translate(map, in);
    CHECK(out.r2 == 100 && out.l2 == 0);

    // Shoulders.
    map = Defaults();
    map.source[SOTB_PAD_L1] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    map.source[SOTB_PAD_R1] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)).pressed == ActionBit(SOTB_PAD_R1));
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)).pressed == ActionBit(SOTB_PAD_L1));

    // Stick clicks.
    map = Defaults();
    map.source[SOTB_PAD_L3] = SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    map.source[SOTB_PAD_R3] = SDL_GAMEPAD_BUTTON_LEFT_STICK;
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_LEFT_STICK)).pressed == ActionBit(SOTB_PAD_R3));
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_RIGHT_STICK)).pressed == ActionBit(SOTB_PAD_L3));

    // Menu / Options.
    map = Defaults();
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_START)).pressed == ActionBit(SOTB_PAD_OPTIONS));
    map.source[SOTB_PAD_OPTIONS] = SDL_GAMEPAD_BUTTON_BACK;
    CHECK((Translate(map, Held(SDL_GAMEPAD_BUTTON_BACK)).pressed & ActionBit(SOTB_PAD_OPTIONS)) != 0);
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_START)).pressed == 0);
}

void TestTouchpadEquivalent() {
    SotbPadMap map = Defaults();
    // View/Back and the controller's own touchpad click both press Touchpad by default.
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_BACK)).pressed == ActionBit(SOTB_PAD_TOUCHPAD));
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_TOUCHPAD)).pressed == ActionBit(SOTB_PAD_TOUCHPAD));
    // A rebound Touchpad follows the chosen button only.
    map.source[SOTB_PAD_TOUCHPAD] = SDL_GAMEPAD_BUTTON_NORTH;
    CHECK((Translate(map, Held(SDL_GAMEPAD_BUTTON_NORTH)).pressed & ActionBit(SOTB_PAD_TOUCHPAD)) != 0);
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_BACK)).pressed == 0);
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_TOUCHPAD)).pressed == 0);
    // The touchpad click itself can be the source.
    map.source[SOTB_PAD_TOUCHPAD] = SDL_GAMEPAD_BUTTON_TOUCHPAD;
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_TOUCHPAD)).pressed == ActionBit(SOTB_PAD_TOUCHPAD));
}

void TestSpec() {
    SotbPadMap map = Defaults();
    CHECK(sotb_pad_apply_spec(&map, "cross=b, circle=a") == 2);
    CHECK(map.source[SOTB_PAD_CROSS] == SDL_GAMEPAD_BUTTON_EAST && map.source[SOTB_PAD_CIRCLE] == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(sotb_pad_apply_spec(&map, "nonsense,square=nothing,bogus=a,l2=y") == 1);
    CHECK(map.source[SOTB_PAD_SQUARE] == SDL_GAMEPAD_BUTTON_WEST && map.source[SOTB_PAD_L2] == SDL_GAMEPAD_BUTTON_NORTH);
    CHECK(sotb_pad_apply_spec(&map, nullptr) == 0 && sotb_pad_apply_spec(&map, "") == 0);
    CHECK(sotb_pad_changed(&map) == 3);
}

void TestSettingsPersistence(const fs::path& base) {
    const fs::path file = base / "Sotb.ini";
    {
        LauncherSettings settings(file);
        for (int action = 0; action < SOTB_PAD_COUNT; ++action) {
            CHECK(SavedPadSource(settings, static_cast<SotbPadAction>(action)) == sotb_pad_actions[action].default_source);
        }
        SetPadSource(settings, SOTB_PAD_CROSS, SDL_GAMEPAD_BUTTON_EAST);
        SetPadSource(settings, SOTB_PAD_L2, SOTB_PAD_SOURCE_RIGHT_TRIGGER);
        SetPadSource(settings, SOTB_PAD_OPTIONS, SDL_GAMEPAD_BUTTON_MISC1);
        CHECK(settings.Save());
    }
    const std::string text = ReadAll(file);
    CHECK(Contains(text, "pad_cross = b") && Contains(text, "pad_l2 = righttrigger") && Contains(text, "pad_options = misc1"));
    {
        const LauncherSettings loaded(file);
        CHECK(SavedPadSource(loaded, SOTB_PAD_CROSS) == SDL_GAMEPAD_BUTTON_EAST);
        CHECK(SavedPadSource(loaded, SOTB_PAD_L2) == SOTB_PAD_SOURCE_RIGHT_TRIGGER);
        CHECK(SavedPadSource(loaded, SOTB_PAD_OPTIONS) == SDL_GAMEPAD_BUTTON_MISC1);
        CHECK(SavedPadSource(loaded, SOTB_PAD_CIRCLE) == SDL_GAMEPAD_BUTTON_EAST);
        // The runtime reads the same file the launcher wrote.
        SotbPadMap runtime;
        sotb_pad_load(file.string().c_str(), &runtime);
        for (int action = 0; action < SOTB_PAD_COUNT; ++action) {
            CHECK(runtime.source[action] == SavedPadSource(loaded, static_cast<SotbPadAction>(action)));
        }
    }
    // Saving again without changes leaves the bytes alone.
    {
        LauncherSettings again(file);
        CHECK(again.Save());
    }
    CHECK(ReadAll(file) == text);
}

void TestDeterministicOutput(const fs::path& base) {
    std::string outputs[2];
    for (int run = 0; run < 2; ++run) {
        const fs::path file = base / ("deterministic" + std::to_string(run) + ".ini");
        LauncherSettings settings(file);
        for (int action = 0; action < SOTB_PAD_COUNT; ++action) {
            SetPadSource(settings, static_cast<SotbPadAction>(action),
                         AllSources()[static_cast<std::size_t>((action * 5 + 3) % static_cast<int>(AllSources().size()))]);
        }
        CHECK(settings.Save());
        outputs[run] = ReadAll(file);
    }
    CHECK(!outputs[0].empty() && outputs[0] == outputs[1]);
    std::size_t lines = 0;
    for (const SotbPadInfo& action : sotb_pad_actions) {
        lines += Contains(outputs[0], std::string(action.ini_key) + " = ") ? 1u : 0u;
    }
    CHECK(lines == SOTB_PAD_COUNT);
}

void TestMissingPartialAndInvalid(const fs::path& base) {
    const fs::path file = base / "partial.ini";
    WriteAll(file, "pad_cross = b\npad_circle = banana\npad_square = guide\npad_l1 =\npad_r1 = leftx\n"
                   "pad_triangle = " + std::string(200, 'x') + "\npad_l3 = RIGHTSTICK\n");
    const LauncherSettings settings(file);
    CHECK(SavedPadSource(settings, SOTB_PAD_CROSS) == SDL_GAMEPAD_BUTTON_EAST);
    for (const SotbPadAction action : {SOTB_PAD_CIRCLE, SOTB_PAD_SQUARE, SOTB_PAD_L1, SOTB_PAD_R1, SOTB_PAD_TRIANGLE}) {
        CHECK(SavedPadSource(settings, action) == sotb_pad_actions[action].default_source);
    }
    CHECK(SavedPadSource(settings, SOTB_PAD_L3) == SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    CHECK(SavedPadSource(settings, SOTB_PAD_R2) == SOTB_PAD_SOURCE_RIGHT_TRIGGER);
    SotbPadMap map = Defaults();
    CHECK(sotb_pad_apply(&map, "pad_circle", "banana") == 0 && map.source[SOTB_PAD_CIRCLE] == SDL_GAMEPAD_BUTTON_EAST);
    CHECK(sotb_pad_apply(&map, "key_cross", "b") == 0);
    SotbPadMap loaded;
    sotb_pad_load(file.string().c_str(), &loaded);
    CHECK(loaded.source[SOTB_PAD_CROSS] == SDL_GAMEPAD_BUTTON_EAST && loaded.source[SOTB_PAD_CIRCLE] == SDL_GAMEPAD_BUTTON_EAST);
    sotb_pad_load((base / "does-not-exist.ini").string().c_str(), &loaded);
    CHECK(sotb_pad_changed(&loaded) == 0);
    sotb_pad_load(nullptr, &loaded);
    CHECK(sotb_pad_changed(&loaded) == 0);
}

void TestDuplicatePolicy(const fs::path& base) {
    // The same physical input may press several PS4 buttons. Nothing is removed from another binding.
    LauncherSettings settings(base / "duplicates.ini");
    SetPadSource(settings, SOTB_PAD_CIRCLE, SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(SavedPadSource(settings, SOTB_PAD_CROSS) == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(SavedPadSource(settings, SOTB_PAD_CIRCLE) == SDL_GAMEPAD_BUTTON_SOUTH);
    SotbPadMap map;
    for (int action = 0; action < SOTB_PAD_COUNT; ++action) {
        map.source[action] = SavedPadSource(settings, static_cast<SotbPadAction>(action));
    }
    CHECK(sotb_pad_source_users(&map, SDL_GAMEPAD_BUTTON_SOUTH) == 2);
    CHECK(sotb_pad_source_users(&map, SDL_GAMEPAD_BUTTON_EAST) == 0);
    CHECK(Translate(map, Held(SDL_GAMEPAD_BUTTON_SOUTH)).pressed == (ActionBit(SOTB_PAD_CROSS) | ActionBit(SOTB_PAD_CIRCLE)));
    SetPadSource(settings, SOTB_PAD_SQUARE, SDL_GAMEPAD_BUTTON_SOUTH);
    map.source[SOTB_PAD_SQUARE] = SavedPadSource(settings, SOTB_PAD_SQUARE);
    CHECK(sotb_pad_source_users(&map, SDL_GAMEPAD_BUTTON_SOUTH) == 3);
    CHECK(SavedPadSource(settings, SOTB_PAD_CROSS) == SDL_GAMEPAD_BUTTON_SOUTH);
}

void TestResetKeepsEverythingElse(const fs::path& base) {
    const fs::path file = base / "reset.ini";
    LauncherSettings settings(file);
    SetKey(settings, SOTB_KEY_CROSS, SDL_SCANCODE_Q);
    SetKey(settings, SOTB_KEY_HOTKEY_WINDOW_MODE, SDL_SCANCODE_F9);
    settings.SetBool(kControllerRumbleKey, false);
    settings.Set(kControllerPromptsKey, "playstation");
    settings.Set(kVSyncKey, "0");
    settings.Set(kFrameRateKey, "120");
    settings.Set(kRendererKey, "auto");
    SetPadSource(settings, SOTB_PAD_CROSS, SDL_GAMEPAD_BUTTON_EAST);
    SetPadSource(settings, SOTB_PAD_L2, SDL_GAMEPAD_BUTTON_WEST);
    SetPadSource(settings, SOTB_PAD_TOUCHPAD, SDL_GAMEPAD_BUTTON_TOUCHPAD);
    ResetPads(settings);
    for (int action = 0; action < SOTB_PAD_COUNT; ++action) {
        CHECK(SavedPadSource(settings, static_cast<SotbPadAction>(action)) == sotb_pad_actions[action].default_source);
    }
    CHECK(SavedKey(settings, SOTB_KEY_CROSS) == SDL_SCANCODE_Q && SavedKey(settings, SOTB_KEY_HOTKEY_WINDOW_MODE) == SDL_SCANCODE_F9);
    CHECK(!settings.GetBool(kControllerRumbleKey, true));
    CHECK(SavedPromptMode(settings) == SOTB_PROMPT_PLAYSTATION);
    CHECK(settings.Get(kVSyncKey, "1") == "0" && settings.Get(kFrameRateKey, "60") == "120" && settings.Get(kRendererKey, "x") == "auto");
    // Resetting the keys does not touch the controller bindings either.
    SetPadSource(settings, SOTB_PAD_CROSS, SDL_GAMEPAD_BUTTON_EAST);
    ResetKeys(settings);
    CHECK(SavedPadSource(settings, SOTB_PAD_CROSS) == SDL_GAMEPAD_BUTTON_EAST);
    CHECK(SavedKey(settings, SOTB_KEY_CROSS) == sotb_key_actions[SOTB_KEY_CROSS].default_scancode);
    CHECK(settings.Save());
    const LauncherSettings reloaded(file);
    CHECK(SavedPadSource(reloaded, SOTB_PAD_CROSS) == SDL_GAMEPAD_BUTTON_EAST && !reloaded.GetBool(kControllerRumbleKey, true));
}

void TestOlderConfigStaysCompatible(const fs::path& base) {
    const fs::path file = base / "older.ini";
    WriteAll(file, "# Shadow of the Beast user settings\nrenderer = auto\nframe_rate = 144\ncontroller_rumble = 0\n"
                   "key_cross = q\nkey_circle = lshift\nlauncher_x = 40\nunknown_future_key = keep\n");
    LauncherSettings settings(file);
    CHECK(settings.Get(kRendererKey, "vulkan") == "auto" && settings.Get(kFrameRateKey, "60") == "144");
    CHECK(!settings.GetBool(kControllerRumbleKey, true));
    for (int action = 0; action < SOTB_PAD_COUNT; ++action) {
        CHECK(SavedPadSource(settings, static_cast<SotbPadAction>(action)) == sotb_pad_actions[action].default_source);
    }
    CHECK(SavedPromptMode(settings) == SOTB_PROMPT_AUTO);
    CHECK(SavedKey(settings, SOTB_KEY_CROSS) == SDL_SCANCODE_Q);
    SetPadSource(settings, SOTB_PAD_SQUARE, SDL_GAMEPAD_BUTTON_NORTH);
    CHECK(settings.Save());
    const std::string text = ReadAll(file);
    CHECK(Contains(text, "key_cross = q") && Contains(text, "unknown_future_key = keep") && Contains(text, "launcher_x = 40"));
    CHECK(Contains(text, "pad_square = y"));
    // A setting that was never written is not invented for the other controls.
    CHECK(!Contains(text, "pad_cross"));
    // Reset all settings brings the controller bindings back with everything else.
    settings.ResetToDefaults();
    CHECK(SavedPadSource(settings, SOTB_PAD_SQUARE) == SDL_GAMEPAD_BUTTON_WEST);
    // The defaults for the new keys are the canonical layout.
    bool managed_pad = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        if (std::string(setting.key) == "pad_cross") {
            managed_pad = setting.default_value == std::string("a");
        }
    }
    CHECK(managed_pad);
}

SotbPadInput Sample(std::initializer_list<int> held) {
    SotbPadInput in{};
    for (const int source : held) {
        const SotbPadInput one = Held(source);
        in.buttons |= one.buttons;
        in.left_trigger = std::max(in.left_trigger, one.left_trigger);
        in.right_trigger = std::max(in.right_trigger, one.right_trigger);
    }
    return in;
}

void TestCaptureAssignsAfterRelease() {
    PadCapture capture;
    CHECK(!capture.active());
    CHECK(!capture.Begin(SOTB_PAD_CROSS, 0) && !capture.active());
    CHECK(capture.Begin(SOTB_PAD_CROSS, 1) && capture.active() && capture.action() == SOTB_PAD_CROSS);

    // The button that opened the row is still held: ignored until everything is released.
    const SotbPadInput held_a = Sample({SDL_GAMEPAD_BUTTON_SOUTH});
    const SotbPadInput none = Sample({});
    for (int frame = 0; frame < 10; ++frame) {
        const SotbPadInput pads[] = {held_a};
        CHECK(capture.Update(pads, false).kind == PadCaptureResult::Kind::kWaiting);
    }
    CHECK(capture.active());
    const SotbPadInput released[] = {none};
    CHECK(capture.Update(released, false).kind == PadCaptureResult::Kind::kWaiting);

    // A real press is assigned once it is stable, and only once.
    const SotbPadInput press_b[] = {Sample({SDL_GAMEPAD_BUTTON_EAST})};
    CHECK(capture.Update(press_b, false).kind == PadCaptureResult::Kind::kWaiting);
    const PadCaptureResult result = capture.Update(press_b, false);
    CHECK(result.kind == PadCaptureResult::Kind::kAssigned && result.source == SDL_GAMEPAD_BUTTON_EAST);
    CHECK(!capture.active());
    CHECK(capture.Update(press_b, false).kind == PadCaptureResult::Kind::kWaiting);
}

void TestCaptureDebounce() {
    PadCapture capture;
    CHECK(capture.Begin(SOTB_PAD_SQUARE, 1));
    const SotbPadInput none[] = {Sample({})};
    CHECK(capture.Update(none, false).kind == PadCaptureResult::Kind::kWaiting);
    // A one-frame glitch assigns nothing.
    const SotbPadInput glitch[] = {Sample({SDL_GAMEPAD_BUTTON_NORTH})};
    CHECK(capture.Update(glitch, false).kind == PadCaptureResult::Kind::kWaiting);
    CHECK(capture.Update(none, false).kind == PadCaptureResult::Kind::kWaiting && capture.active());
    // A changing source restarts the count.
    const SotbPadInput a[] = {Sample({SDL_GAMEPAD_BUTTON_SOUTH})};
    const SotbPadInput x[] = {Sample({SDL_GAMEPAD_BUTTON_WEST})};
    CHECK(capture.Update(a, false).kind == PadCaptureResult::Kind::kWaiting);
    CHECK(capture.Update(x, false).kind == PadCaptureResult::Kind::kWaiting);
    CHECK(capture.Update(a, false).kind == PadCaptureResult::Kind::kWaiting);
    CHECK(capture.active());
    const PadCaptureResult done = capture.Update(a, false);
    CHECK(done.kind == PadCaptureResult::Kind::kAssigned && done.source == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(kPadCaptureStableUpdates >= 2);
}

void TestCaptureTriggers() {
    PadCapture capture;
    CHECK(capture.Begin(SOTB_PAD_R1, 1));
    const SotbPadInput none[] = {Sample({})};
    capture.Update(none, false);
    SotbPadInput light{};
    light.right_trigger = 100;
    const SotbPadInput lightly[] = {light};
    for (int frame = 0; frame < 5; ++frame) {
        CHECK(capture.Update(lightly, false).kind == PadCaptureResult::Kind::kWaiting); // not pulled far enough
    }
    const SotbPadInput pulled[] = {Sample({SOTB_PAD_SOURCE_RIGHT_TRIGGER})};
    capture.Update(pulled, false);
    const PadCaptureResult result = capture.Update(pulled, false);
    CHECK(result.kind == PadCaptureResult::Kind::kAssigned && result.source == SOTB_PAD_SOURCE_RIGHT_TRIGGER);
}

void TestCaptureCancelAndDisconnect() {
    PadCapture capture;
    CHECK(capture.Begin(SOTB_PAD_OPTIONS, 1));
    const SotbPadInput pads[] = {Sample({})};
    PadCaptureResult result = capture.Update(pads, true);
    CHECK(result.kind == PadCaptureResult::Kind::kCancelled && !capture.active());
    CHECK(capture.Update(pads, false).kind == PadCaptureResult::Kind::kWaiting);

    // Escape wins over a press in the same frame.
    CHECK(capture.Begin(SOTB_PAD_OPTIONS, 1));
    const SotbPadInput pressed[] = {Sample({SDL_GAMEPAD_BUTTON_SOUTH})};
    CHECK(capture.Update(pressed, true).kind == PadCaptureResult::Kind::kCancelled && !capture.active());

    // The controller disappears while waiting: the capture ends, nothing is assigned.
    CHECK(capture.Begin(SOTB_PAD_L3, 1));
    capture.Update(pads, false);
    result = capture.Update(std::span<const SotbPadInput>{}, false);
    CHECK(result.kind == PadCaptureResult::Kind::kNoController && result.source == SOTB_PAD_SOURCE_NONE && !capture.active());
    // With no controller a new capture does not start; after reconnecting it does and works.
    CHECK(!capture.Begin(SOTB_PAD_L3, 0));
    CHECK(capture.Begin(SOTB_PAD_L3, 1));
    capture.Update(pads, false);
    const SotbPadInput stick[] = {Sample({SDL_GAMEPAD_BUTTON_RIGHT_STICK})};
    capture.Update(stick, false);
    result = capture.Update(stick, false);
    CHECK(result.kind == PadCaptureResult::Kind::kAssigned && result.source == SDL_GAMEPAD_BUTTON_RIGHT_STICK);

    // Replaced by another controller mid-capture: the new one is read.
    CHECK(capture.Begin(SOTB_PAD_L3, 1));
    capture.Update(pads, false);
    const SotbPadInput replacement[] = {Sample({}), Sample({SDL_GAMEPAD_BUTTON_NORTH})};
    capture.Update(replacement, false);
    result = capture.Update(replacement, false);
    CHECK(result.kind == PadCaptureResult::Kind::kAssigned && result.source == SDL_GAMEPAD_BUTTON_NORTH);
}

void TestMappingAppliesAfterReconnect(const fs::path& base) {
    const fs::path file = base / "reconnect.ini";
    {
        LauncherSettings settings(file);
        SetPadSource(settings, SOTB_PAD_CROSS, SDL_GAMEPAD_BUTTON_EAST);
        SetPadSource(settings, SOTB_PAD_TRIANGLE, SOTB_PAD_SOURCE_LEFT_TRIGGER);
        CHECK(settings.Save());
    }
    SotbPadMap map;
    sotb_pad_load(file.string().c_str(), &map);
    const SotbPadInput first_pad = Sample({SDL_GAMEPAD_BUTTON_EAST, SOTB_PAD_SOURCE_LEFT_TRIGGER});
    const SotbPadOutput before = Translate(map, first_pad);
    CHECK((before.pressed & ActionBit(SOTB_PAD_CROSS)) != 0 && (before.pressed & ActionBit(SOTB_PAD_TRIANGLE)) != 0);
    // Disconnect: no input. Reconnect the same or another controller: the saved mapping is unchanged.
    CHECK(Translate(map, SotbPadInput{}).pressed == 0);
    const SotbPadOutput after = Translate(map, first_pad);
    CHECK(after.pressed == before.pressed && after.l2 == before.l2);
    SotbPadMap reloaded;
    sotb_pad_load(file.string().c_str(), &reloaded);
    CHECK(std::memcmp(&map, &reloaded, sizeof(map)) == 0);
}

void TestRuntimeStaysInSync() {
    const std::string runtime = ReadSource("src/runtime_pad.c");
    const std::string header = ReadSource("src/sotb_pad.h");
    // Every PS4 action in the order of the bindings maps to the guest button of the same name.
    const std::size_t table = runtime.find("ps_names[]");
    CHECK(table != std::string::npos);
    std::vector<std::string> names;
    std::size_t at = table;
    const std::size_t end = runtime.find("};", table);
    while ((at = runtime.find("{\"", at)) != std::string::npos && at < end) {
        const std::size_t close = runtime.find('"', at + 2);
        names.push_back(runtime.substr(at + 2, close - at - 2));
        at = close;
    }
    CHECK(names.size() == SOTB_PAD_COUNT);
    for (std::size_t i = 0; i < names.size() && i < SOTB_PAD_COUNT; ++i) {
        CHECK(names[i] == sotb_pad_actions[i].name);
    }
    const auto guest = [&](const std::string& token) -> long {
        const std::size_t position = runtime.find(token + "=0x");
        return position == std::string::npos ? -1 : std::strtol(runtime.c_str() + position + token.size() + 1, nullptr, 16);
    };
    CHECK(guest("BTN_CROSS") == 0x4000 && guest("BTN_CIRCLE") == 0x2000 && guest("BTN_SQUARE") == 0x8000 && guest("BTN_TRIANGLE") == 0x1000);
    CHECK(guest("BTN_L1") == 0x400 && guest("BTN_R1") == 0x800 && guest("BTN_L2") == 0x100 && guest("BTN_R2") == 0x200);
    CHECK(guest("BTN_L3") == 0x2 && guest("BTN_R3") == 0x4 && guest("BTN_OPTIONS") == 0x8 && guest("BTN_TOUCHPAD") == 0x100000);
    CHECK(guest("BTN_UP") == 0x10 && guest("BTN_RIGHT") == 0x20 && guest("BTN_DOWN") == 0x40 && guest("BTN_LEFT") == 0x80);
    // The runtime applies the bindings to every gamepad and keeps the L3 + R3 Options chord and rumble.
    CHECK(Contains(runtime, "sotb_pad_translate(&pad_map,&in,&out)") && Contains(runtime, "sotb_pad_load(keys_file,&pad_map)"));
    CHECK(Contains(runtime, "gamepad_buttons(g,&d->l2,&d->r2)") && Contains(runtime, "gamepad_buttons(g,NULL,NULL)"));
    CHECK(Contains(runtime, "SDL_GAMEPAD_BUTTON_LEFT_STICK") && Contains(runtime, "sticks"));
    CHECK(Contains(runtime, "sotbgpu_controller_rumble()") && Contains(runtime, "SDL_RumbleGamepad(g,"));
    CHECK(Contains(header, "SOTB_PAD_SOURCE_LEFT_TRIGGER"));
    // The sticks are read as analog axes, never through the bindings.
    CHECK(Contains(runtime, "d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX))"));
}

void TestLauncherUsesBindings() {
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    CHECK(Contains(launcher, "ResetPads(settings_)") && Contains(launcher, "PadRow(ui,") && Contains(launcher, "pad_capture_.Update("));
    CHECK(Contains(launcher, "kControllerBindingsHelp") && Contains(launcher, "Esc cancels"));
    const std::string settings = ReadSource("launcher/settings.cpp");
    CHECK(Contains(settings, "sotb_pad_actions"));
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / ("sotb-pad-binding-tests-" + std::to_string(std::rand()));
    fs::create_directories(base);
    TestDefaultMapping();
    TestEveryActionEverySource();
    TestNamesRoundTrip();
    TestTriggersShouldersClicksMenu();
    TestTouchpadEquivalent();
    TestSpec();
    TestSettingsPersistence(base);
    TestDeterministicOutput(base);
    TestMissingPartialAndInvalid(base);
    TestDuplicatePolicy(base);
    TestResetKeepsEverythingElse(base);
    TestOlderConfigStaysCompatible(base);
    TestCaptureAssignsAfterRelease();
    TestCaptureDebounce();
    TestCaptureTriggers();
    TestCaptureCancelAndDisconnect();
    TestMappingAppliesAfterReconnect(base);
    TestRuntimeStaysInSync();
    TestLauncherUsesBindings();
    std::error_code ignored;
    fs::remove_all(base, ignored);
    std::printf("pad binding tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

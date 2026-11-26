// Supported-build parity and the platform feature contract. The supported builds come from the launcher's build table;
// every build-keyed runtime table must know each of them and refuse anything else; every patch installer is applied
// together, in the loader's order and cave layout, to each build's real boot image (read only, copied in memory); and
// the public features are checked against the sources this platform's shipped programs compile and the code that wires
// them in. tools\test.ps1 compares the contract lines this prints on Windows and on Linux. No window, no game, no GPU.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include "guest_patches.h"
#include "isa_emulate.h"
}
#include "game_profile.h"
#include "guest_display.h"
#include "guest_hud.h"
#include "guest_ultrawide.h"
#include "lzcnt_compat.h"
#include "psn_notice.h"
#include "skip_intro.h"
#include "sotb_exit_game.h"
#include "sotb_exit_game_l10n.h"
#include "sotb_keys.h"
#include "sotb_menu_buttons.h"
#include "sotb_platform_contract.h"
#include "sotb_prompt_assets.h"
#include "sotb_renderer.h"
#include "sotb_version.h"
#include "test_progress.h"

// Menu Button Layout reads its setting through sotb_keys.c, which needs SDL; no setting is read here.
extern "C" void sotb_ini_read(const char*, SotbIniEntry, void*) {}

namespace fs = std::filesystem;

namespace {

const fs::path kRoot = SOTB_SOURCE_DIR;
long long g_checks = 0;

struct RequiredBuild {
    const char* title_id;
    const char* version;
    const char* region;
};

// The builds this release supports. The launcher's table must list exactly these.
constexpr RequiredBuild kRequiredBuilds[] = {
    {"CUSA03762", "1.00", "Korea"}, {"CUSA02871", "1.00", "USA"}, {"CUSA00951", "1.00", "Europe"}};

constexpr const char* kUnknownKeys[] = {"CUSA99999-1.00", "CUSA03762-1.01", "cusa03762-1.00", "CUSA03762",
                                        "CUSA03762 1.00", "-1.00", ""};

#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

// Code that must be there once this platform's conditional compilation is resolved.
struct Wiring {
    const char* file;
    const char* code;
};

struct Feature {
    const char* key;
    const char* title;
    bool per_build;
    std::vector<const char*> sources;
    std::vector<Wiring> wiring;
    std::vector<const char*> windows_sources = {};
    std::vector<const char*> linux_sources = {};
    std::vector<Wiring> windows_wiring = {};
    std::vector<Wiring> linux_wiring = {};
};

constexpr const char* kLoader = "src/loader.c";
constexpr const char* kRuntime = "src/runtime.c";
constexpr const char* kLauncher = "launcher/launcher.cpp";
constexpr const char* kPresenter = "gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp";
constexpr const char* kFrontend = "guest_ultrawide_apply_frontend(image, size";

const std::vector<Feature> kFeatures = {
    {"build_profile", "launcher recognition and install profile", true,
     {"launcher/game_profile.cpp", "launcher/game_files.cpp", "launcher/game_install.cpp", "launcher/build_identity.cpp"},
     {{kLauncher, "InstallGame(source, roots, hooks)"}, {"launcher/game_files.cpp", "IsBuildExecutable(build, sha)"}}},
    {"build_fingerprint", "build fingerprint", true, {"launcher/boot_image.cpp", "launcher/build_identity.cpp"},
     {{kLoader, "sotb_game_build(paths.image_dir, &game_build)"}, {kLoader, "sotb_game_build_problem("}},
     {"launcher/crypto_win.cpp"}, {"launcher/crypto_openssl.cpp"}},
    {"cpu_compatibility", "MOVBE, BMI1 and LZCNT compatibility", true, {"src/isa_emulate.c", "src/lzcnt_compat.c"},
     {{kLoader, "= isa_emulate((const uint8_t *)"}, {kLoader, "lzcnt_compat_install(image, size"}}},
    {"offline_policy", "offline Social Features policy", true, {"src/guest_patches.c"},
     {{kLoader, "guest_patches_apply(image, size, cave, cave_size)"}}},
    {"controller_prompts", "in-game controller prompts", true,
     {"src/sotb_game_prompts.c", "src/sotb_prompt_assets.c", "src/sotb_prompt_assets_data.c", "src/sotb_pad_prompts.c"},
     {{kLoader, "install_controller_prompts(settings_file, app0, &game_build)"}, {kLoader, "sotb_game_prompts_apply(&request)"}}},
    {"menu_button_layout", "Menu Button Layout", true, {"src/sotb_menu_buttons.c"},
     {{kLoader, "sotb_menu_buttons_apply(image, size, game_build.code_key"}}},
    {"exit_game", "Exit Game", true, {"src/sotb_exit_game.c", "src/sotb_exit_game_l10n.c", "gpu/shim/sotb_overlay.cpp"},
     {{kLoader, "sotb_exit_game_apply(image, size, game_build.code_key"}, {kLoader, "sotbgpu_exit_confirm_begin("}}},
    {"psn_notice", "PlayStation Network notice removal", true, {"src/psn_notice.c"},
     {{kLoader, "psn_notice_apply(image, size"}}},
    {"skip_intro", "Skip Intro", true, {"src/skip_intro.c"}, {{kLoader, "skip_intro_apply(image, size"}}},
    {"render_resolution", "render resolution", true, {"src/guest_display.c"},
     {{kLoader, "guest_apply_display_size(image, size, render_target.width, render_target.height"}}},
    {"aspect_ratio", "aspect ratio", true, {"src/guest_display.c"},
     {{kLoader, "guest_apply_display_size(image, size, render_target.width, render_target.height"},
      {kLoader, "sotb_render_target("}}},
    {"resolution_guard", "Resolution Guard", true, {"src/resolution_guard.c", "launcher/resolution_recovery.cpp"},
     {{kLoader, "sotb_resolution_guard_arm_target("}, {kLauncher, "RecoverFailedRenderResolution("}}},
    {"loading_hint", "loading hint layout", true, {"src/guest_display.c"},
     {{kLoader, "guest_apply_loading_screen_layout(image, size"}}},
    {"true_ultrawide", "True Ultrawide Hor+ projection", true, {"src/guest_ultrawide.c"},
     {{kLoader, "guest_ultrawide_apply(image, size"}}},
    {"frontend_safe_area", "frontend safe area", true, {"src/guest_ultrawide.c"},
     {{kLoader, "guest_ultrawide_apply_safe_area(image, size"}}},
    {"title_context", "title context", true, {"src/guest_ultrawide.c"},
     {{kLoader, kFrontend}, {"src/guest_ultrawide.c", "case SOTB_SCREEN_MAIN_MENU:"}}},
    {"map_context", "Map context", true, {"src/guest_ultrawide.c"},
     {{kLoader, kFrontend}, {"src/guest_ultrawide.c", "case SOTB_SCREEN_MAP:"}}},
    {"mana_overlay_context", "Mana overlay context", true, {"src/guest_ultrawide.c"},
     {{kLoader, kFrontend}, {"src/guest_ultrawide.c", "case SOTB_SCREEN_MANA_OVERLAY:"}}},
    {"options_context", "Options context", true, {"src/guest_ultrawide.c"},
     {{kLoader, kFrontend}, {"src/guest_ultrawide.c", "case SOTB_SCREEN_OPTIONS:"}}},
    {"wisdom_of_shadows_context", "Wisdom of Shadows context", true, {"src/guest_ultrawide.c"},
     {{kLoader, kFrontend}, {"src/guest_ultrawide.c", "case SOTB_SCREEN_SHOP_CONTAINER:"}}},
    {"hud_anchoring", "HUD component anchoring", true, {"src/guest_hud.c"}, {{kLoader, "guest_hud_apply(image, size"}}},
    {"cutscene_fade_fix", "full-screen cutscene fade", true, {"src/guest_hud.c"},
     {{kLoader, "guest_hud_apply(image, size"}, {"src/guest_hud.c", "fade_call_supported(image)"}}},
    {"localization", "localization", true, {"launcher/text_languages.cpp", "src/sotb_exit_game_l10n.c"},
     {{kLoader, "sotb_text_language(settings_file)"}, {kLoader, "sotb_exit_game_set_locale(g_exit_locale)"}}},
    {"launcher", "TRG Launcher", false, {"launcher/launcher.cpp", "launcher/host.cpp"},
     {{kLoader, "sotb_launcher_run(&launcher)"}, {"launcher/host.cpp", "SDL_CreateWindowWithProperties(props)"}}},
    {"launcher_settings", "launcher settings", false, {"launcher/settings.cpp", "gpu/shim/sotb_settings.cpp"},
     {{kLauncher, "settings_.Save()"}, {"gpu/shim/sotb_settings.cpp", "v.frame_rate = "}}},
    {"updater", "updater", false,
     {"launcher/update_config.cpp", "launcher/updater/update_controller.cpp", "launcher/updater/update_platform.cpp"},
     {{kLauncher, "UpdateController>(MakeUpdateConfig("}, {"launcher/updater/update_platform.cpp", "bool LaunchApplication("}},
     {"launcher/updater/helper_main.cpp", "launcher/updater/install_transaction.cpp"}, {},
     {{"launcher/update_config.cpp", "UpdatePackageKind::kZip"}},
     {{"launcher/update_config.cpp", "UpdatePackageKind::kAppImage"},
      {"launcher/updater/update_controller.cpp", "ApplyAppImageUpdate(download_path_"}}},
    {"controller_input", "controller input and rebinding", false,
     {"src/sotb_keys.c", "src/sotb_pad.c", "src/runtime_pad.c", "launcher/pad_capture.cpp"},
     {{kRuntime, "runtime_pad_resolve(name)"}, {"src/runtime_pad.c", "sotb_pad_load(keys_file"},
      {kLauncher, "pad_capture_.Update("}}},
    {"side_fill", "ultrawide side fill (title, Map, loading)", false, {"gpu/shim/sotbgpu.cpp", kPresenter},
     {{kLoader, "sotb_ultrawide_enable_side_fill("}, {kPresenter, "SotbSideFill::Current("}}},
    {"frame_pacing", "frame pacing", false, {"gpu/shim/sotb_settings.cpp", "gpu/shadps4/core/libraries/videoout/driver.cpp"},
     {{"gpu/shadps4/core/libraries/videoout/driver.cpp", "SotbFramePacing::PresentsFlip("}}},
    {"vulkan_presentation", "Vulkan presentation and device-lost handling", false,
     {kPresenter, "gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp"},
     {{kLoader, "sotbgpu_init(&gpu)"}, {kPresenter, "presented = swapchain.Present()"},
      {kPresenter, "DeviceLost::WaitWhileHealthy("}}},
    {"save_data", "save data", false, {"src/runtime_savedata.c"},
     {{kRuntime, "runtime_savedata_resolve(name)"}, {kLoader, "runtime_savedata_configure(id)"}}},
    {"trophies", "trophies", false, {"launcher/trophy_page.cpp", "src/trophies/trophy_runtime.cpp", "src/runtime_services.c"},
     {{kLoader, "trophy_runtime_configure(app0"}, {"src/runtime_services.c", "trophy_runtime_unlock(ctx"},
      {kLauncher, "make_unique<TrophyPage>"}}},
    {"bug_report", "Bug Report", false,
     {"launcher/incident_page.cpp", "launcher/incident_report.cpp", "launcher/http_post.cpp"},
     {{kLauncher, "make_unique<IncidentPage>"}, {"launcher/http_post.cpp", "HttpReply PostJsonHttps("}}},
};

const char* TitleOf(std::string_view key) {
    for (const Feature& feature : kFeatures) {
        if (key == feature.key) return feature.title;
    }
    return key == "frontend_contexts" ? "frontend screen hook" : "unknown feature";
}

enum class State { Pass, Fail, Blocked };

const char* Name(State state) { return state == State::Pass ? "PASS" : state == State::Fail ? "FAIL" : "BLOCKED"; }

struct Outcome {
    State state = State::Pass;
    std::string detail;
};

struct BuildRun {
    const Sotb::GameBuild* build = nullptr;
    std::string key;
    std::string name;
    bool real = false;
    std::string missing;
    std::map<std::string, Outcome> features;

    void Check(const std::string& feature, bool ok, const std::string& why) {
        ++g_checks;
        sotb_test_progress(g_checks);
        Outcome& outcome = features[feature];
        if (ok) return;
        outcome.detail += (outcome.state == State::Fail ? "; " : "") + why;
        outcome.state = State::Fail;
    }
    void Block(const std::string& feature) {
        Outcome& outcome = features[feature];
        if (outcome.state == State::Pass) outcome.state = State::Blocked;
    }
};

std::vector<std::string> g_problems;

void Problem(const std::string& group, bool ok, const std::string& why) {
    ++g_checks;
    sotb_test_progress(g_checks);
    if (!ok) g_problems.push_back(group + ": " + why);
}

std::string Hex(uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string JsonValue(const std::string& json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos) return {};
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return {};
    at = json.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos) return {};
    if (json[at] == '"') {
        const size_t end = json.find('"', at + 1);
        return end == std::string::npos ? std::string() : json.substr(at + 1, end - at - 1);
    }
    const size_t end = json.find_first_of(",}\r\n \t", at);
    return json.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

uint64_t Number(const std::string& text) { return text.empty() ? 0 : std::strtoull(text.c_str(), nullptr, 0); }

struct BootImage {
    std::vector<uint8_t> bytes;
    uint64_t entry = 0;
    std::vector<std::pair<uint64_t, uint64_t>> relocations;
};

// The boot image format the loader reads (SOTBBOT1): header, modules, bindings, segments, import names, relocations,
// then the memory image. Only relocations against the image itself are kept.
bool LoadBootImage(const fs::path& path, BootImage& out) {
    std::error_code error;
    const uintmax_t file_size = fs::file_size(path, error);
    if (error || file_size < 128) return false;
    std::vector<uint8_t> data(static_cast<size_t>(file_size));
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) return false;
    const auto u64 = [&](size_t offset) {
        uint64_t value = 0;
        if (offset + 8 <= data.size()) std::memcpy(&value, data.data() + offset, 8);
        return value;
    };
    if (std::memcmp(data.data(), "SOTBBOT1", 8)) return false;
    const uint64_t size = u64(8), segments = u64(24), relocs = u64(32), imports = u64(40);
    if (!size || segments > 64 || relocs > 1000000 || imports > 100000) return false;
    size_t offset = 96;
    const uint64_t modules = u64(offset);
    if (modules > 16) return false;
    offset += 8 + modules * 56;
    const uint64_t bindings = u64(offset);
    if (bindings > imports) return false;
    offset += 8 + bindings * 24;
    offset += segments * 24 + imports * 128;
    const size_t relocation_table = offset;
    offset += relocs * 32;
    if (offset > data.size() || size > data.size() - offset) return false;
    for (uint64_t i = 0; i < relocs; ++i) {
        if (u64(relocation_table + i * 32 + 8) == 0) {
            out.relocations.emplace_back(u64(relocation_table + i * 32), u64(relocation_table + i * 32 + 16));
        }
    }
    out.entry = u64(16);
    out.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                     data.begin() + static_cast<std::ptrdiff_t>(offset + size));
    return true;
}

constexpr size_t kCave = GUEST_PATCH_CAVE_SIZE;
constexpr size_t kShift = 16;

// The image, the shared cave right after it (so every rel32 reaches it) and the LZCNT stub area after the cave.
struct Workspace {
    size_t size = 0;
    std::vector<uint8_t> buffer;
    std::vector<uint8_t> shadow;

    Workspace(const std::vector<uint8_t>& image, size_t shift = 0)
        : size(image.size()), buffer(image.size() + kCave + lzcnt_compat_stub_bytes(), 0) {
        std::copy(image.begin(), image.end() - static_cast<std::ptrdiff_t>(shift),
                  buffer.begin() + static_cast<std::ptrdiff_t>(shift));
    }
    uint8_t* image() { return buffer.data(); }
    uint8_t* cave() { return buffer.data() + size; }
    uint8_t* stubs() { return buffer.data() + size + kCave; }
    size_t stubs_size() const { return buffer.size() - size - kCave; }
    void Relocate(const std::vector<std::pair<uint64_t, uint64_t>>& relocations) {
        for (const auto& [target, value] : relocations) {
            if (target + 8 > size) continue;
            const uint64_t pointer = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buffer.data())) + value;
            std::memcpy(buffer.data() + target, &pointer, sizeof(pointer));
        }
    }
    void Snapshot() { shadow = buffer; }
    std::vector<size_t> Take() {
        std::vector<size_t> changed;
        constexpr size_t kBlock = 4096;
        for (size_t at = 0; at < buffer.size(); at += kBlock) {
            const size_t length = std::min(kBlock, buffer.size() - at);
            if (!std::memcmp(buffer.data() + at, shadow.data() + at, length)) continue;
            for (size_t i = at; i < at + length; ++i) {
                if (buffer[i] != shadow[i]) {
                    changed.push_back(i);
                    shadow[i] = buffer[i];
                }
            }
        }
        return changed;
    }
};

struct Window {
    uint64_t begin, end;
};

Window At(uint64_t begin, uint64_t length) { return {begin, begin + length}; }

bool Inside(uint64_t offset, const std::vector<Window>& windows) {
    return std::any_of(windows.begin(), windows.end(), [&](const Window& w) { return offset >= w.begin && offset < w.end; });
}

struct Step {
    const char* feature;
    std::function<bool(Workspace&, std::string&)> apply;
    std::vector<Window> image_windows;  // empty: the installer's own fingerprints decide where it writes
    Window cave;                        // offsets in the shared cave this installer may write
    bool stubs = false;
};

std::string Error(const char* text) { return text && *text ? text : "refused"; }

// Every installer the loader runs, in its order, with everything on (21:9, True Ultrawide, Western, Skip Intro).
std::vector<Step> LoaderSteps(const std::string& key, uint64_t entry) {
    const SotbRenderTarget target = sotb_render_target(&sotb_resolutions[sotb_resolution_index("1440p")],
                                                       &sotb_aspect_ratios[sotb_aspect_index("21:9")]);
    return {
        {"cpu_compatibility",
         [entry](Workspace& w, std::string& why) {
             const LzcntBuild* known = nullptr;
             const LzcntCompatStatus status = lzcnt_compat_install(w.image(), w.size, entry, w.stubs(), w.stubs_size(), &known);
             why = lzcnt_compat_status_text(status);
             return status == LZCNT_COMPAT_OK;
         },
         {}, {0, 0}, true},
        {"offline_policy",
         [](Workspace& w, std::string& why) {
             const GuestPatchReport report = guest_patches_apply(w.image(), w.size, w.cave(), kCave);
             why = std::to_string(report.applied) + " applied, " + std::to_string(report.skipped) + " skipped";
             return report.applied > 0 && report.skipped == 0;
         },
         {}, {0, GUEST_CAVE_EXIT_GAME}},
        {"menu_button_layout",
         [key](Workspace& w, std::string& why) {
             why = "the Western layout was refused";
             return sotb_menu_buttons_apply(w.image(), w.size, key.c_str(), SOTB_MENU_BUTTON_LAYOUT_WESTERN) == 1;
         },
         {}, {0, 0}},
        {"exit_game",
         [key](Workspace& w, std::string& why) {
             char error[200] = "";
             const bool ok = sotb_exit_game_apply(w.image(), w.size, key.c_str(), w.cave() + GUEST_CAVE_EXIT_GAME,
                                                  kCave - GUEST_CAVE_EXIT_GAME, error, sizeof(error));
             why = Error(error);
             return ok;
         },
         {At(SOTB_EXIT_GAME_TICK_SITE, 5), At(SOTB_EXIT_GAME_SETUP_SITE, 6), At(SOTB_EXIT_GAME_REFRESH_SITE, 16)},
         {GUEST_CAVE_EXIT_GAME, GUEST_CAVE_PSN_NOTICE}},
        {"psn_notice",
         [](Workspace& w, std::string& why) {
             char error[200] = "";
             const bool ok = psn_notice_apply(w.image(), w.size, w.cave() + GUEST_CAVE_PSN_NOTICE, kCave - GUEST_CAVE_PSN_NOTICE,
                                              error, sizeof(error));
             why = Error(error);
             return ok;
         },
         {At(PSN_NOTICE_HOOK_SITE, PSN_NOTICE_HOOK_PATCH_SIZE)}, {GUEST_CAVE_PSN_NOTICE, GUEST_CAVE_ULTRAWIDE}},
        {"true_ultrawide",
         [](Workspace& w, std::string& why) {
             char error[200] = "";
             const bool ok = guest_ultrawide_apply(w.image(), w.size, w.cave() + GUEST_CAVE_ULTRAWIDE,
                                                   kCave - GUEST_CAVE_ULTRAWIDE, error, sizeof(error));
             why = Error(error);
             return ok;
         },
         {At(SOTB_ULTRAWIDE_SITE, SOTB_ULTRAWIDE_PATCH_SIZE)}, {GUEST_CAVE_ULTRAWIDE, GUEST_CAVE_HUD}},
        {"frontend_safe_area",
         [](Workspace& w, std::string& why) {
             char error[200] = "";
             const bool ok = guest_ultrawide_apply_safe_area(w.image(), w.size, error, sizeof(error));
             why = Error(error);
             return ok;
         },
         {At(SOTB_GAME_LAYER_VTABLE + SOTB_GAME_LAYER_ARRANGE_SLOT * 8u, 8)}, {0, 0}},
        {"hud_anchoring",
         [](Workspace& w, std::string& why) {
             char error[200] = "";
             const bool ok = guest_hud_apply(w.image(), w.size, w.cave() + GUEST_CAVE_HUD, kCave - GUEST_CAVE_HUD, error,
                                             sizeof(error));
             why = Error(error);
             return ok;
         },
         {}, {GUEST_CAVE_HUD, kCave}},
        {"frontend_contexts",
         [](Workspace& w, std::string& why) {
             char error[200] = "";
             const bool ok = guest_ultrawide_apply_frontend(w.image(), w.size, error, sizeof(error));
             why = Error(error);
             return ok;
         },
         {At(SOTB_FRONTEND_VTABLE + SOTB_FRONTEND_TICK_SLOT * 8u, 8)}, {0, 0}},
        {"skip_intro",
         [](Workspace& w, std::string& why) {
             char error[200] = "";
             const bool ok = skip_intro_apply(w.image(), w.size, error, sizeof(error));
             why = Error(error);
             return ok;
         },
         {At(SKIP_INTRO_SITE, SKIP_INTRO_PATCH_SIZE)}, {0, 0}},
        {"aspect_ratio",
         [target](Workspace& w, std::string& why) {
             char error[200] = "";
             const bool ok = guest_apply_display_size(w.image(), w.size, target.width, target.height, error, sizeof(error));
             why = Error(error);
             return ok;
         },
         {}, {0, 0}},
        {"loading_hint",
         [](Workspace& w, std::string& why) {
             char error[200] = "";
             const bool ok = guest_apply_loading_screen_layout(w.image(), w.size, error, sizeof(error));
             why = Error(error);
             return ok;
         },
         {}, {0, 0}},
    };
}

// The read-only check the unsupported-build diagnostics run in place of each installer, on the same image.
bool DiagnosticCheck(const std::string& feature, const Workspace& w, const std::string& key, uint64_t entry) {
    const uint8_t* p = w.buffer.data();
    const size_t n = w.size;
    char e[200];
    if (feature == "cpu_compatibility") {
        for (const LzcntBuild& known : lzcnt_builds) {
            if (known.image.size == n && known.image.entry == entry && lzcnt_compat_check(p, n, &known, nullptr) == LZCNT_COMPAT_OK) {
                return true;
            }
        }
        return false;
    }
    if (feature == "offline_policy") {
        const GuestPatchReport report = guest_patches_check(p, n);
        return report.applied > 0 && report.skipped == 0;
    }
    if (feature == "menu_button_layout") return sotb_menu_buttons_check(p, n, key.c_str(), e, sizeof(e)) == 1;
    if (feature == "exit_game") return sotb_exit_game_check(p, n, key.c_str(), e, sizeof(e)) == 1;
    if (feature == "psn_notice") return psn_notice_check(p, n, e, sizeof(e)) == 1;
    if (feature == "true_ultrawide") return guest_ultrawide_check(p, n, e, sizeof(e)) == 1;
    if (feature == "frontend_safe_area") return guest_ultrawide_check_safe_area(p, n, e, sizeof(e)) == 1;
    if (feature == "hud_anchoring") return guest_hud_check(p, n, e, sizeof(e)) == 1;
    if (feature == "frontend_contexts") return guest_ultrawide_check_frontend(p, n, e, sizeof(e)) == 1;
    if (feature == "skip_intro") return skip_intro_check(p, n, e, sizeof(e)) == 1;
    if (feature == "aspect_ratio") return guest_check_display_size(p, n, e, sizeof(e)) == 1;
    if (feature == "loading_hint") return guest_check_loading_screen_layout(p, n, e, sizeof(e)) == 1;
    return false;
}

constexpr const char* kContextFeatures[] = {"title_context", "map_context", "mana_overlay_context", "options_context",
                                            "wisdom_of_shadows_context"};

// Every installer together on the relocated image: each one installs, writes only its own bytes and cave slice, and no
// byte is written by two of them.
void ApplyTogether(BuildRun& run, const BootImage& boot, std::set<size_t>& patched) {
    Workspace w(boot.bytes);
    w.Relocate(boot.relocations);
    w.Snapshot();
    const std::string& key = run.key;

    run.Check("menu_button_layout", sotb_menu_buttons_apply(w.image(), w.size, key.c_str(), SOTB_MENU_BUTTON_LAYOUT_ORIGINAL) == 1,
              "the Original layout was refused");
    run.Check("menu_button_layout", w.Take().empty(), "the Original layout wrote to the image");

    std::map<size_t, std::string> owner;
    for (const Step& step : LoaderSteps(key, boot.entry)) {
        std::string why;
        const bool checked = DiagnosticCheck(step.feature, w, key, boot.entry);
        const bool ok = step.apply(w, why);
        run.Check(step.feature, ok, std::string(TitleOf(step.feature)) + " was refused: " + why);
        run.Check(step.feature, checked == ok, "the unsupported-build diagnostics check disagrees with the installer");
        const std::vector<size_t> changed = w.Take();
        if (!ok) {
            run.Check(step.feature, changed.empty(), "a refused installer wrote " + std::to_string(changed.size()) + " bytes");
            continue;
        }
        std::vector<size_t> own;
        size_t outside_image = 0, outside_cave = 0, overlaps = 0;
        std::string overlap_with;
        for (const size_t at : changed) {
            if (at < w.size) {
                own.push_back(at);
                patched.insert(at);
                if (!step.image_windows.empty() && !Inside(at, step.image_windows)) ++outside_image;
                const auto [it, fresh] = owner.emplace(at, step.feature);
                if (!fresh) {
                    ++overlaps;
                    overlap_with = it->second;
                }
            } else if (at < w.size + kCave) {
                const uint64_t in_cave = at - w.size;
                if (in_cave < step.cave.begin || in_cave >= step.cave.end) ++outside_cave;
            } else if (!step.stubs) {
                ++outside_cave;
            }
        }
        run.Check(step.feature, !own.empty(), "installed but wrote nothing");
        run.Check(step.feature, outside_image == 0,
                  std::to_string(outside_image) + " bytes written outside its patch destination");
        run.Check(step.feature, outside_cave == 0, std::to_string(outside_cave) + " cave bytes written outside its slice");
        run.Check(step.feature, overlaps == 0,
                  std::to_string(overlaps) + " bytes overlap the " + std::string(TitleOf(overlap_with)) + " patch");
        if (step.feature == std::string("menu_button_layout")) {
            const bool contiguous = own.size() <= 6 && (own.empty() || own.back() - own.front() < 6);
            run.Check(step.feature, contiguous, "the Western layout wrote more than its six-byte branch");
        }
        if (step.feature == std::string("hud_anchoring")) {
            const bool fade = std::any_of(own.begin(), own.end(),
                                          [](size_t at) { return at >= SOTB_HUD_FADE_CALL && at < SOTB_HUD_FADE_CALL + 5; });
            run.Check("cutscene_fade_fix", fade, "the cutscene skip fade call was not redirected");
        }
    }
    if (run.features["hud_anchoring"].state != State::Pass) {
        run.Check("cutscene_fade_fix", false, "HUD anchoring did not install");
    }
    const Outcome frontend = run.features["frontend_contexts"];
    for (const char* feature : kContextFeatures) {
        run.Check(feature, frontend.state == State::Pass, "the frontend screen hook did not install: " + frontend.detail);
    }
    run.features.erase("frontend_contexts");
    run.Check("frontend_safe_area", run.features["true_ultrawide"].state == State::Pass,
              "the Hor+ projection hook it depends on did not install");
}

// Nothing may install on an image whose code is not where the build's table says.
void RefusesShiftedImage(BuildRun& run, const BootImage& boot) {
    Workspace shifted(boot.bytes, kShift);
    shifted.Snapshot();
    const std::string& key = run.key;
    char error[200];
    const auto refused = [&](const char* feature, bool installed) {
        const bool untouched = shifted.Take().empty();
        run.Check(feature, !installed && untouched,
                  std::string("installed or wrote on an image shifted by ") + std::to_string(kShift) + " bytes");
    };
    const LzcntBuild* known = nullptr;
    refused("cpu_compatibility", lzcnt_compat_install(shifted.image(), shifted.size, boot.entry, shifted.stubs(),
                                                      shifted.stubs_size(), &known) == LZCNT_COMPAT_OK);
    refused("offline_policy", guest_patches_apply(shifted.image(), shifted.size, shifted.cave(), kCave).applied > 0);
    refused("menu_button_layout", sotb_menu_buttons_apply(shifted.image(), shifted.size, key.c_str(), SOTB_MENU_BUTTON_LAYOUT_WESTERN) != 0);
    refused("exit_game", sotb_exit_game_apply(shifted.image(), shifted.size, key.c_str(), shifted.cave() + GUEST_CAVE_EXIT_GAME,
                                              kCave - GUEST_CAVE_EXIT_GAME, error, sizeof(error)) != 0);
    refused("psn_notice", psn_notice_apply(shifted.image(), shifted.size, shifted.cave() + GUEST_CAVE_PSN_NOTICE,
                                           kCave - GUEST_CAVE_PSN_NOTICE, error, sizeof(error)) != 0);
    refused("true_ultrawide", guest_ultrawide_apply(shifted.image(), shifted.size, shifted.cave() + GUEST_CAVE_ULTRAWIDE,
                                                    kCave - GUEST_CAVE_ULTRAWIDE, error, sizeof(error)) != 0);
    refused("frontend_safe_area", guest_ultrawide_apply_safe_area(shifted.image(), shifted.size, error, sizeof(error)) != 0);
    refused("hud_anchoring", guest_hud_apply(shifted.image(), shifted.size, shifted.cave() + GUEST_CAVE_HUD,
                                             kCave - GUEST_CAVE_HUD, error, sizeof(error)) != 0);
    refused("title_context", guest_ultrawide_apply_frontend(shifted.image(), shifted.size, error, sizeof(error)) != 0);
    refused("skip_intro", skip_intro_apply(shifted.image(), shifted.size, error, sizeof(error)) != 0);
    refused("render_resolution", guest_apply_display_size(shifted.image(), shifted.size, 2560, 1440, error, sizeof(error)) != 0);
    refused("loading_hint", guest_apply_loading_screen_layout(shifted.image(), shifted.size, error, sizeof(error)) != 0);
}

void RefusesForeignKeys(BuildRun& run, const BootImage& boot) {
    Workspace right(boot.bytes);
    right.Snapshot();
    char error[200];
    for (const char* unknown : kUnknownKeys) {
        const bool menu = sotb_menu_buttons_apply(right.image(), right.size, unknown, SOTB_MENU_BUTTON_LAYOUT_WESTERN) != 0;
        run.Check("menu_button_layout", !menu && right.Take().empty(),
                  std::string("the unknown build key '") + unknown + "' installed on this image");
        const bool exit_game = sotb_exit_game_apply(right.image(), right.size, unknown, right.cave() + GUEST_CAVE_EXIT_GAME,
                                                    kCave - GUEST_CAVE_EXIT_GAME, error, sizeof(error)) != 0;
        run.Check("exit_game", !exit_game && right.Take().empty(),
                  std::string("the unknown build key '") + unknown + "' installed on this image");
    }
    const LzcntBuild* none = nullptr;
    run.Check("cpu_compatibility",
              lzcnt_compat_install(right.image(), right.size - 1, boot.entry, right.stubs(), right.stubs_size(), &none) ==
                      LZCNT_COMPAT_UNKNOWN_IMAGE && right.Take().empty(),
              "an image of another size was taken for this build");
}

// Every render target the launcher offers that differs from native, on a fresh copy each time: the six display size
// sites accept it, and the Resolution Guard's fallback for every shape is light and applies.
void DisplayTargets(BuildRun& run, const BootImage& boot) {
    Workspace w(boot.bytes);
    char error[200];
    const auto apply = [&](const char* feature, SotbRenderTarget target, const std::string& label) {
        if (target.width == 1920 && target.height == 1080) return;
        std::copy(boot.bytes.begin(), boot.bytes.end(), w.buffer.begin());
        error[0] = 0;
        const bool ok = guest_apply_display_size(w.image(), w.size, target.width, target.height, error, sizeof(error));
        run.Check(feature, ok, label + " (" + std::to_string(target.width) + "x" + std::to_string(target.height) +
                                   ") was refused: " + Error(error));
    };
    const SotbAspectRatio* standard = &sotb_aspect_ratios[0];
    for (const SotbResolution& resolution : sotb_resolutions) {
        apply("render_resolution", sotb_render_target(&resolution, standard), std::string(resolution.key) + " at 16:9");
    }
    for (const SotbAspectRatio& aspect : sotb_aspect_ratios) {
        apply("aspect_ratio", sotb_render_target(&sotb_resolutions[0], &aspect), std::string("native at ") + aspect.key);
        const int safe = sotb_safe_resolution_index(&aspect);
        const bool listed = safe >= 0 && safe < SOTB_RESOLUTION_COUNT;
        run.Check("resolution_guard", listed, std::string("no fallback resolution for ") + aspect.key);
        if (!listed) continue;
        const SotbRenderTarget fallback = sotb_render_target(&sotb_resolutions[safe], &aspect);
        run.Check("resolution_guard", !sotb_target_is_heavy(fallback),
                  std::string("the fallback for ") + aspect.key + " still needs the guard");
        apply("resolution_guard", fallback, std::string("the fallback ") + sotb_resolutions[safe].key + " at " + aspect.key);
    }
}

// What the launcher's table, the runtime tables and the generated prompt table say about one build, with no game file.
void TableChecks(BuildRun& run) {
    const Sotb::GameBuild& build = *run.build;
    const std::string title = build.title_id, version = build.version, region = build.region;
    const std::string sha = build.eboot_sha256;
    run.Check("build_profile", title.size() == 9 && title.rfind("CUSA", 0) == 0, "title id " + title + " is not a CUSA id");
    run.Check("build_profile", std::string(build.folder) == title + "-" + version, "folder " + std::string(build.folder));
    run.Check("build_profile", run.key == build.folder, "code build key " + run.key + " is not the folder name");
    run.Check("build_profile", std::string(build.label) == title + " " + version + " " + region, "label " + std::string(build.label));
    run.Check("build_profile", std::string(build.content_id).find(title) != std::string::npos, "content id does not name the title");
    run.Check("build_profile", sha.size() == 64 && sha.find_first_not_of("0123456789abcdef") == std::string::npos,
              "eboot SHA-256 is not 64 lowercase hex digits");
    run.Check("build_profile", Sotb::IsBuildExecutable(build, sha), "its own executable hash is not accepted");
    run.Check("build_profile", !Sotb::IsBuildExecutable(build, std::string(64, '0')), "an unknown executable hash is accepted");
    const std::string profiles = ReadText(kRoot / "scripts" / "game_profiles.py");
    for (const std::string& field : {title, std::string(build.content_id), sha}) {
        run.Check("build_profile", profiles.find(field) != std::string::npos,
                  "scripts/game_profiles.py does not list " + field + " (the package reader would not recognize it)");
    }

    const SotbPromptSet* prompts = sotb_prompt_set_for_build(run.key.c_str());
    run.Check("controller_prompts", prompts != nullptr, "no Xbox prompt set for " + run.key);
    if (prompts) {
        run.Check("controller_prompts", run.key == prompts->build, "the prompt set names another build");
        run.Check("controller_prompts", std::string(prompts->guest_path) == "/app0/" + std::string(build.regional_archive),
                  "the prompt set patches " + std::string(prompts->guest_path) + ", not the build's regional archive");
        run.Check("controller_prompts", prompts->asset_count > 0 && prompts->run_count > 0, "the prompt set is empty");
    }

    run.Check("menu_button_layout", sotb_menu_buttons_supported(run.key.c_str()), "no Menu Button Layout site table for " + run.key);
    run.Check("menu_button_layout",
              sotb_menu_buttons_effective_circle_confirm(run.key.c_str(), SOTB_MENU_BUTTON_LAYOUT_ORIGINAL) ==
                  (build.original_circle_confirm ? 1 : 0),
              "the Original confirm button differs between the launcher's and the runtime's tables");
    run.Check("menu_button_layout", !sotb_menu_buttons_effective_circle_confirm(run.key.c_str(), SOTB_MENU_BUTTON_LAYOUT_WESTERN),
              "the Western layout does not confirm with Cross/A");
    run.Check("exit_game", sotb_exit_game_supported(run.key.c_str()), "no Exit Game site table for " + run.key);

    const auto languages = Sotb::TextLanguagesOf(&build);
    run.Check("localization", !languages.empty(), "no text language");
    run.Check("localization", !languages.empty() && Sotb::ResolveTextLanguage(languages, "english").key == "english",
              "English text is not available");
    run.Check("localization", sotb_exit_game_strings("en_US") != nullptr, "no Exit Game text for en_US");
}

void ImageChecks(BuildRun& run, const BootImage& boot, const std::string& json) {
    const Sotb::GameBuild& build = *run.build;
    run.Check("build_fingerprint", JsonValue(json, "title_id") == build.title_id,
              "image.json says title " + JsonValue(json, "title_id"));
    run.Check("build_fingerprint", JsonValue(json, "app_ver") == Sotb::FinishedLayer(build).app_ver,
              "image.json says APP_VER " + JsonValue(json, "app_ver"));
    run.Check("build_fingerprint", Sotb::IsBuildExecutable(build, JsonValue(json, "eboot_sha256")),
              "the prepared executable hash is not this build's");
    run.Check("build_fingerprint", Number(JsonValue(json, "image_bytes")) == boot.bytes.size(),
              "boot.bin does not hold the image size image.json records");
    run.Check("build_fingerprint", Number(JsonValue(json, "entry")) == build.executable.entry && boot.entry == build.executable.entry,
              "the entry point is not the table's " + Hex(build.executable.entry));
    run.Check("build_fingerprint", !boot.relocations.empty(), "the boot image has no relocations");
}

// Builds that share an executable hash must have identical images; the bytes where builds differ must not be patched.
void ImageIdentity(const std::vector<BuildRun>& runs, const std::vector<BootImage>& images, const std::set<size_t>& written) {
    for (size_t i = 0; i < runs.size(); ++i) {
        if (!runs[i].real) continue;
        for (size_t j = 0; j < i; ++j) {
            if (!runs[j].real) continue;
            const std::vector<uint8_t>& a = images[j].bytes;
            const std::vector<uint8_t>& b = images[i].bytes;
            const bool same_hash = std::string(runs[i].build->eboot_sha256) == runs[j].build->eboot_sha256;
            size_t differ = 0, patched = 0, first = 0, last = 0;
            const size_t common = std::min(a.size(), b.size());
            for (size_t at = 0; at < common; ++at) {
                if (a[at] == b[at]) continue;
                if (!differ++) first = at;
                last = at;
                patched += written.count(at);
            }
            differ += std::max(a.size(), b.size()) - common;
            std::string text;
            if (!differ) {
                text = "identical to " + runs[j].name;
            } else {
                text = "differs from " + runs[j].name + " in " + std::to_string(differ) + " bytes (" + Hex(first) + ".." +
                       Hex(last) + "), " + std::to_string(patched) + " of them patched";
            }
            const bool good = same_hash ? differ == 0 : patched == 0 && a.size() == b.size() &&
                                                            images[i].relocations.size() == images[j].relocations.size();
            Problem("image-identity", good, runs[i].name + " " + text);
            std::printf("IMAGE_IDENTITY|%s|%s|%s|%s\n", runs[i].build->title_id, runs[i].build->version, good ? "PASS" : "FAIL",
                        text.c_str());
        }
    }
}

void SupportedBuildTable() {
    const auto builds = Sotb::SupportedBuilds();
    for (const RequiredBuild& required : kRequiredBuilds) {
        const auto found = std::find_if(builds.begin(), builds.end(), [&](const Sotb::GameBuild& b) {
            return std::string_view(b.title_id) == required.title_id && std::string_view(b.version) == required.version;
        });
        Problem("supported-builds", found != builds.end(),
                std::string(required.title_id) + " " + required.version + " " + required.region + " is not in the launcher's build table");
        if (found != builds.end()) {
            Problem("supported-builds", std::string_view(found->region) == required.region,
                    std::string(required.title_id) + " is listed for region " + found->region);
        }
    }
    std::set<std::string> identities, keys;
    for (const Sotb::GameBuild& build : builds) {
        std::printf("SUPPORTED_BUILD|%s|%s|%s\n", build.title_id, build.version, build.region);
        const bool required = std::any_of(std::begin(kRequiredBuilds), std::end(kRequiredBuilds), [&](const RequiredBuild& r) {
            return std::string_view(build.title_id) == r.title_id && std::string_view(build.version) == r.version;
        });
        Problem("supported-builds", required, std::string(build.label) + " is in the launcher's table but not a required build");
        Problem("supported-builds", identities.insert(std::string(build.title_id) + " " + build.version).second,
                std::string(build.label) + " is listed twice");
        Problem("supported-builds", keys.insert(Sotb::CodeBuildKey(build)).second,
                "two builds share the code build key " + Sotb::CodeBuildKey(build));
    }
    for (size_t i = 0; i < sotb_prompt_set_count; ++i) {
        Problem("supported-builds", keys.count(sotb_prompt_sets[i].build) == 1,
                std::string("the prompt table has a set for ") + sotb_prompt_sets[i].build + ", which is not a supported build");
    }
}

void UnknownBuilds() {
    for (const char* unknown : kUnknownKeys) {
        Problem("unknown-builds", !sotb_menu_buttons_supported(unknown), std::string("Menu Button Layout knows '") + unknown + "'");
        Problem("unknown-builds", !sotb_exit_game_supported(unknown), std::string("Exit Game knows '") + unknown + "'");
        Problem("unknown-builds", sotb_prompt_set_for_build(unknown) == nullptr,
                std::string("the prompt table has a set for '") + unknown + "'");
        Problem("unknown-builds", !sotb_menu_buttons_effective_circle_confirm(unknown, SOTB_MENU_BUTTON_LAYOUT_ORIGINAL),
                std::string("'") + unknown + "' inherits a confirm button");
    }
    Problem("unknown-builds", !sotb_menu_buttons_supported(nullptr) && !sotb_exit_game_supported(nullptr) &&
                                  sotb_prompt_set_for_build(nullptr) == nullptr,
            "a null build key is accepted");
    char error[200];
    Problem("unknown-builds",
            !sotb_exit_game_apply(nullptr, 0, "CUSA99999-1.00", nullptr, 0, error, sizeof(error)) &&
                std::string(error) == "not a supported build",
            "Exit Game does not refuse an unknown build before reading the image");
}

void CaveLayout() {
    Problem("patch-safety", GUEST_CAVE_EXIT_GAME < GUEST_CAVE_PSN_NOTICE && GUEST_CAVE_PSN_NOTICE < GUEST_CAVE_ULTRAWIDE &&
                                GUEST_CAVE_ULTRAWIDE < GUEST_CAVE_HUD && GUEST_CAVE_HUD < kCave,
            "the cave slices are out of order");
    Problem("patch-safety", GUEST_CAVE_PSN_NOTICE - GUEST_CAVE_EXIT_GAME >= SOTB_EXIT_GAME_MIN_CAVE_SIZE,
            "the Exit Game cave slice is smaller than its stubs");
    Problem("patch-safety", GUEST_CAVE_ULTRAWIDE - GUEST_CAVE_PSN_NOTICE >= PSN_NOTICE_MIN_CAVE_SIZE,
            "the PSN notice cave slice is smaller than its stub");
    Problem("patch-safety", GUEST_CAVE_HUD - GUEST_CAVE_ULTRAWIDE >= SOTB_ULTRAWIDE_MIN_CAVE_SIZE,
            "the Hor+ cave slice is smaller than its stub");
    Problem("patch-safety", kCave - GUEST_CAVE_HUD >= SOTB_HUD_FADE_STUB_SIZE, "the HUD cave slice is smaller than its stub");
    Problem("patch-safety", lzcnt_compat_stub_bytes() > 0, "the LZCNT rewrite needs no stub area");
}

void FrontendScreens() {
    Problem("frontend-contexts", sotb_frontend_context_of_screen(SOTB_SCREEN_MAIN_MENU) == SOTB_CONTEXT_TITLE, "the main menu is not the title");
    Problem("frontend-contexts", sotb_frontend_context_of_screen(SOTB_SCREEN_MAP) == SOTB_CONTEXT_MAP, "the Map is not the Map context");
    Problem("frontend-contexts", sotb_frontend_context_of_screen(SOTB_SCREEN_MANA_OVERLAY) == SOTB_CONTEXT_MAP,
            "the Mana overlay is not classified with the Map");
    Problem("frontend-contexts", sotb_frontend_context_of_screen(SOTB_SCREEN_OPTIONS) == SOTB_CONTEXT_MENU, "Options is not a menu");
    Problem("frontend-contexts", sotb_frontend_context_of_screen(SOTB_SCREEN_SHOP_CONTAINER) == SOTB_CONTEXT_MENU,
            "Wisdom of Shadows is not a menu");
}

std::string Join(const std::vector<std::string>& items, const char* separator) {
    std::string text;
    for (const std::string& item : items) text += (text.empty() ? "" : separator) + item;
    return text;
}

// 1 or 0 when a conditional compilation line asks whether this is Windows, -1 when it asks anything else.
int PlatformCondition(std::string line) {
    line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return c <= ' ' || c == '(' || c == ')'; }),
               line.end());
    if (line == "#ifdef_WIN32" || line == "#ifdefined_WIN32") return kWindows;
    if (line == "#ifndef_WIN32" || line == "#if!defined_WIN32") return !kWindows;
    if (line == "#ifdef__linux__" || line == "#ifdefined__linux__") return !kWindows;
    return -1;
}

// The source with the branches of other platforms and the comment lines left out.
std::string PlatformText(const std::string& text) {
    struct Branch {
        int condition;
        bool active;
    };
    std::vector<Branch> open;
    std::stringstream in(text);
    std::string out, line;
    while (std::getline(in, line)) {
        const size_t start = line.find_first_not_of(" \t");
        const std::string code = start == std::string::npos ? std::string() : line.substr(start);
        if (code.rfind("#if", 0) == 0) {
            const int condition = PlatformCondition(code);
            open.push_back({condition, condition != 0});
        } else if (!open.empty() && (code.rfind("#else", 0) == 0 || code.rfind("#elif", 0) == 0)) {
            open.back().active = open.back().condition != 1;
        } else if (!open.empty() && code.rfind("#endif", 0) == 0) {
            open.pop_back();
        } else if (code.rfind("//", 0) != 0 && code.rfind("* ", 0) != 0 &&
                   std::all_of(open.begin(), open.end(), [](const Branch& b) { return b.active; })) {
            out += line + "\n";
        }
    }
    return out;
}

// A public feature counts when this platform's shipped programs compile every source it needs and the code that wires it
// in survives this platform's conditional compilation.
bool PlatformFeatures() {
    std::set<std::string> compiled;
    for (const char* const* source = sotb_contract_sources; *source; ++source) compiled.insert(*source);
    const std::string shipped = SOTB_CONTRACT_SHIPPED_TARGETS;
    std::printf("PLATFORM|%s|shipped=%s|sources=%zu\n", SOTB_CONTRACT_PLATFORM, shipped.empty() ? "none" : shipped.c_str(),
                compiled.size());
    std::printf("PLATFORM_CAPABILITY|renderer_direct3d11|%d\n", SotbRenderer::Direct3D11Available ? 1 : 0);
    size_t available = 0;
    std::vector<std::string> problems;
    std::map<std::string, std::string> texts;
    for (const Feature& feature : kFeatures) {
        std::vector<std::string> missing, absent;
        std::vector<const char*> sources = feature.sources;
        std::vector<Wiring> wiring = feature.wiring;
        const auto& platform_sources = kWindows ? feature.windows_sources : feature.linux_sources;
        const auto& platform_wiring = kWindows ? feature.windows_wiring : feature.linux_wiring;
        sources.insert(sources.end(), platform_sources.begin(), platform_sources.end());
        wiring.insert(wiring.end(), platform_wiring.begin(), platform_wiring.end());
        for (const char* source : sources) {
            if (!fs::exists(kRoot / source)) absent.push_back(source);
            if (!compiled.count(source)) missing.push_back(source);
        }
        for (const Wiring& wire : wiring) {
            const auto [text, fresh] = texts.try_emplace(wire.file);
            if (fresh) text->second = PlatformText(ReadText(kRoot / wire.file));
            ++g_checks;
            if (!compiled.count(wire.file)) {
                missing.push_back(wire.file);
            } else if (text->second.find(wire.code) == std::string::npos) {
                missing.push_back(std::string(wire.file) + ":" + wire.code);
            }
        }
        ++g_checks;
        std::printf("FEATURE|%s|%d\n", feature.key, missing.empty() ? 1 : 0);
        if (missing.empty()) ++available;
        else std::printf("FEATURE_MISSING|%s|%s|%s\n", feature.key, SOTB_CONTRACT_PLATFORM, Join(missing, "; ").c_str());
        if (!absent.empty()) problems.push_back(std::string(feature.key) + " names missing files: " + Join(absent, " "));
    }
    if (shipped != SOTB_CONTRACT_EXPECTED_TARGETS) {
        problems.push_back("the shipped programs on " + std::string(SOTB_CONTRACT_PLATFORM) + " are '" + shipped + "', not '" +
                           SOTB_CONTRACT_EXPECTED_TARGETS + "'");
    }
    if (available != kFeatures.size()) {
        problems.push_back(std::to_string(kFeatures.size() - available) + " of " + std::to_string(kFeatures.size()) +
                           " public features are not built on " + SOTB_CONTRACT_PLATFORM);
    }
    for (const std::string& problem : problems) std::printf("FAIL platform-features: %s\n", problem.c_str());
    std::printf("GROUP|platform-features|%s|features=%zu/%zu%s%s\n", problems.empty() ? "PASS" : "FAIL", available,
                kFeatures.size(), problems.empty() ? "" : "|", Join(problems, "; ").c_str());
    return problems.empty();
}

} // namespace

int main() {
    std::printf("VERSION|%s\n", SOTB_VERSION_STRING);
    SupportedBuildTable();
    UnknownBuilds();
    CaveLayout();
    FrontendScreens();

    std::vector<BuildRun> runs;
    std::vector<BootImage> images;
    std::set<size_t> patched;
    for (const Sotb::GameBuild& build : Sotb::SupportedBuilds()) {
        BuildRun run;
        run.build = &build;
        run.key = Sotb::CodeBuildKey(build);
        run.name = std::string(build.title_id) + " " + build.version;
        for (const Feature& feature : kFeatures) {
            if (feature.per_build) run.features[feature.key];
        }
        TableChecks(run);
        const fs::path folder = fs::path("build") / "image" / build.folder;
        BootImage boot;
        const std::string json = ReadText(kRoot / folder / "image.json");
        if (json.empty() || !LoadBootImage(kRoot / folder / "boot.bin", boot)) {
            run.missing = "no prepared boot image at " + (folder / "boot.bin").generic_string() + " and " +
                          (folder / "image.json").generic_string() + " (install the build from the launcher)";
            for (const Feature& feature : kFeatures) {
                const std::string key = feature.key;
                if (feature.per_build && key != "build_profile" && key != "controller_prompts" && key != "localization") {
                    run.Block(key);
                }
            }
        } else {
            run.real = true;
            ImageChecks(run, boot, json);
            ApplyTogether(run, boot, patched);
            RefusesShiftedImage(run, boot);
            RefusesForeignKeys(run, boot);
            DisplayTargets(run, boot);
        }
        images.push_back(std::move(boot));
        runs.push_back(std::move(run));
    }
    ImageIdentity(runs, images, patched);
    images.clear();

    size_t builds_passed = 0, builds_blocked = 0, builds_failed = 0;
    for (const BuildRun& run : runs) {
        std::vector<std::string> failed;
        for (const auto& [key, outcome] : run.features) {
            const char* evidence = outcome.state == State::Blocked ? "none" : run.real ? "real" : "table";
            std::printf("BUILD_FEATURE|%s|%s|%s|%s|%s\n", run.build->title_id, run.build->version, key.c_str(),
                        Name(outcome.state), evidence);
            if (outcome.state == State::Fail) {
                failed.push_back(key);
                std::printf("FAIL %s %s: %s\n", run.name.c_str(), TitleOf(key), outcome.detail.c_str());
            }
        }
        const State state = !failed.empty() ? State::Fail : run.real ? State::Pass : State::Blocked;
        if (state == State::Pass) ++builds_passed;
        if (state == State::Blocked) ++builds_blocked;
        if (state == State::Fail) ++builds_failed;
        std::string detail = run.real ? "evidence=real" : "evidence=table|" + run.missing;
        if (!failed.empty()) detail += "|failed=" + Join(failed, ",");
        std::printf("BUILD_PARITY|%s|%s|%s|%s|%s\n", run.build->title_id, run.build->version, run.build->region, Name(state),
                    detail.c_str());
    }

    for (const Feature& feature : kFeatures) {
        if (!feature.per_build) continue;
        size_t pass = 0;
        std::vector<std::string> failing, blocked;
        for (const BuildRun& run : runs) {
            const auto it = run.features.find(feature.key);
            const State state = it == run.features.end() ? State::Fail : it->second.state;
            if (state == State::Pass) ++pass;
            else if (state == State::Fail) failing.push_back(run.name);
            else blocked.push_back(run.name);
        }
        const State state = !failing.empty() ? State::Fail : !blocked.empty() ? State::Blocked : State::Pass;
        std::string detail = "builds=" + std::to_string(pass) + "/" + std::to_string(runs.size());
        if (!failing.empty()) detail += "|missing on " + Join(failing, ", ");
        if (!blocked.empty()) detail += "|no real evidence for " + Join(blocked, ", ");
        std::printf("FEATURE_PARITY|%s|%s|%s\n", feature.key, Name(state), detail.c_str());
    }

    std::map<std::string, std::vector<std::string>> by_group;
    for (const std::string& problem : g_problems) {
        const size_t colon = problem.find(':');
        by_group[problem.substr(0, colon)].push_back(problem.substr(colon + 2));
        std::printf("FAIL %s\n", problem.c_str());
    }
    for (const char* group : {"supported-builds", "unknown-builds", "patch-safety", "frontend-contexts", "image-identity"}) {
        const auto it = by_group.find(group);
        const std::string detail = it == by_group.end() ? "" : "|" + Join(it->second, "; ");
        std::printf("GROUP|%s|%s%s\n", group, it == by_group.end() ? "PASS" : "FAIL", detail.c_str());
    }

    const char* parity = !g_problems.empty() || builds_failed ? "FAIL" : builds_blocked ? "BLOCKED" : "PASS";
    const bool platform_ok = PlatformFeatures();
    std::printf("GROUP|build-parity|%s|checks=%lld|builds=%zu/%zu real%s\n", parity, g_checks, builds_passed, runs.size(),
                builds_blocked ? "|a supported build has no prepared boot image" : "");
    std::printf("build parity tests: %lld checks, %zu failures\n", g_checks, g_problems.size() + builds_failed);
    return std::string(parity) == "PASS" && platform_ok ? 0 : 1;
}

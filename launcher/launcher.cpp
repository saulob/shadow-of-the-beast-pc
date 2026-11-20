#include "sotb_launcher.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <imgui.h>

#include "backends/imgui_impl_sdl3.h"
#include "game_files.h"
#include "game_install.h"
#include "game_profile.h"
#include "gameplay_rows.h"
#include "guest_display.h"
#include "header_art.h"
#include "incident_page.h"
#include "launcher_quit.h"
#include "host.h"
#include "options.h"
#include "page_icons.h"
#include "paths.h"
#include "placement.h"
#include "settings.h"
#include "startup_phase.h"
#include "text_languages.h"
#include "trophy_page.h"
#include "update_config.h"
#include "updater/update_platform.h"
#include "updater/update_ui.h"
#include "sotb_graphics.h"
#include "sotb_png.h"
#include "trg/launcher.h"
#include "trg/layout.h"
#include "trg/platform.h"
#include "trg/settings.h"
#include "utf8.h"

#ifndef SOTB_PORT_VERSION
#define SOTB_PORT_VERSION "unknown"
#endif

extern "C" int sotb_gpu_names(char (*names)[256], int max);

// Defined by the SDL3 ImGui backend; the header does not expose it.
ImGuiKey ImGui_ImplSDL3_KeyEventToImGuiKey(SDL_Keycode keycode, SDL_Scancode scancode);

namespace Sotb {
namespace {

constexpr char kPortTitle[] = "SHADOW OF THE BEAST";
constexpr char kShowLauncherHelp[] = "Off starts the game directly. Hold Shift while starting to bring it back.";
constexpr char kTrgLauncherVersion[] = "1.1.0";
const ImVec4 kAccent = ImVec4(0.88f, 0.58f, 0.20f, 1.0f);

trg::Theme LauncherTheme() {
    trg::Theme theme = trg::Theme::Midnight().WithAccent(kAccent);
    theme.warn = ImVec4(0.42f, 0.16f, 0.10f, 1.0f);
    theme.warn_dot = ImVec4(1.0f, 0.48f, 0.28f, 1.0f);
    return theme;
}

bool LoadLauncherImage(const char* file, SotbPng::Image& image) {
    const char* base = SDL_GetBasePath();
    if (!base) {
        return false;
    }
    const std::filesystem::path path = PathFromUtf8(base) / "assets" / "launcher" / file;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::vector<unsigned char> encoded((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return SotbPng::DecodeRgba(encoded.data(), encoded.size(), image);
}

ImTextureID LoadSponsorIcon(LauncherHost& host) {
    SotbPng::Image image;
    if (!LoadLauncherImage("sponsor-heart.png", image)) {
        return ImTextureID{};
    }
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        if (image.rgba[i + 3] == 0) {
            image.rgba[i] = image.rgba[i + 1] = image.rgba[i + 2] = 255;
        }
    }
    return host.CreateTexture(image.rgba.data(), image.width, image.height);
}

trg::HeaderPainter LauncherHeader(std::function<ImTextureID(int, int)> art, float aspect) {
    return [art = std::move(art), aspect](const trg::HeaderContext& c) {
        const ImVec4& bg = c.theme.panel;
        c.draw->AddRectFilledMultiColor(c.min, c.max, trg::Col(bg), trg::Col(bg),
                                        trg::Col(ImVec4(bg.x * 0.55f, bg.y * 0.55f, bg.z * 0.55f, 1.0f)),
                                        trg::Col(ImVec4(bg.x * 0.55f, bg.y * 0.55f, bg.z * 0.55f, 1.0f)));
        const HeaderArtRect rect = PlaceHeaderArt(c.min.y, c.max.x, c.max.y, aspect);
        const float framebuffer = std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
        const ImTextureID texture = art(static_cast<int>((rect.max_x - rect.min_x) * framebuffer + 0.5f),
                                        static_cast<int>((rect.max_y - rect.min_y) * framebuffer + 0.5f));
        if (texture != ImTextureID{}) {
            c.draw->PushClipRect(c.min, c.max, false);
            for (const HeaderArtStrip& strip : SplitHeaderArt(rect)) {
                c.draw->AddImage(ImTextureRef(texture), ImVec2(strip.min_x, rect.min_y), ImVec2(strip.max_x, rect.max_y),
                                 ImVec2(strip.min_u, 0.0f), ImVec2(strip.max_u, 1.0f));
            }
            c.draw->PopClipRect();
        }
        const ImU32 fade = IM_COL32(0, 0, 0, 0);
        c.draw->AddRectFilledMultiColor(ImVec2(c.min.x, c.max.y - (c.max.y - c.min.y) * 0.35f), c.max, fade, fade,
                                        trg::Col(bg), trg::Col(bg));
    };
}

const char* StatusTitle(GameFileState state) {
    switch (state) {
    case GameFileState::kValid: return "Ready to play";
    case GameFileState::kMissing: return "Game files needed";
    case GameFileState::kDifferentVersion: return "Unsupported Shadow of the Beast build";
    case GameFileState::kReadError: return "Could not read the game files";
    }
    return "";
}

std::string StatusDetail(GameFileState state) {
    switch (state) {
    case GameFileState::kValid: return "The installed build matches the port's fingerprint.";
    case GameFileState::kMissing: return "Install the game from your own Shadow of the Beast .pkg file.";
    case GameFileState::kDifferentVersion:
        return "This package is not the supported build. Supported: " + SupportedBuildsText() + ".";
    case GameFileState::kReadError: return "Check that the game folder is readable, then try again.";
    }
    return "";
}

ImGuiKey ImGuiKeyOf(int scancode) {
    const SDL_Keycode keycode = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode), SDL_KMOD_NONE, false);
    return ImGui_ImplSDL3_KeyEventToImGuiKey(keycode, static_cast<SDL_Scancode>(scancode));
}

int ScancodeOf(ImGuiKey key) {
    for (int scancode = 1; scancode < SDL_SCANCODE_COUNT; ++scancode) {
        if (sotb_key_bindable(scancode) && ImGuiKeyOf(scancode) == key) {
            return scancode;
        }
    }
    return 0;
}

std::string RendererName(const std::string& key) {
    if (key == "vulkan") return "Vulkan";
    if (key == "opengl") return "OpenGL";
    if (key == "d3d11") return "Direct3D 11";
    return "Auto";
}

class LauncherApp {
public:
    explicit LauncherApp(const SotbLauncherConfig& config)
        : config_(config), app0_(PathFromUtf8(config.app0 ? config.app0 : "")),
          image_dir_(PathFromUtf8(config.image_dir ? config.image_dir : "")),
          user_dir_(PathFromUtf8(config.user_dir ? config.user_dir : "")),
          logs_dir_(PathFromUtf8(config.logs_dir ? config.logs_dir : "")),
          settings_(user_dir_ / "sotb.ini") {
        roots_ = RootsOf(GamePaths{app0_, image_dir_});
        std::error_code ec;
        std::filesystem::create_directories(user_dir_, ec);
        std::filesystem::create_directories(logs_dir_, ec);
        Refresh();
        if (LoadLauncherImage("header-art.png", header_art_)) {
            header_aspect_ = float(header_art_.width) / float(header_art_.height);
        }
    }

    SotbLauncherResult Run() {
        const bool ready = state_ == GameFileState::kValid && !install_.running();
        if (config_.skip_once && !config_.force_show) {
            return ready ? SOTB_LAUNCHER_PLAY : Show(ready);
        }
        if (!trg::ShouldShowLauncher(settings_, kShowLauncherKey, ready, config_.force_show)) {
            return SOTB_LAUNCHER_PLAY;
        }
        return Show(ready);
    }

private:
    SotbLauncherResult Show(bool ready) {
        (void)ready;
        Sotb::HostOptions options;
        options.title = "Shadow of the Beast";
        options.width = 1280;
        options.height = 720;
        options.min_width = 980;
        options.min_height = 620;
        options.restore = ReadPlacement(settings_);
        sotb_startup_phase("launcher opening");
        if (!host_.Open(options)) {
            std::fprintf(stderr, "Launcher: could not open the launcher window\n");
            return SOTB_LAUNCHER_BLOCKED;
        }
        placement_ = PlacementTracker(host_.Restored());
        displays_ = ListDisplays();
        fonts_ = trg::LoadSystemFonts(trg::layout::kTextBody);
        sponsor_icon_ = LoadSponsorIcon(host_);
        char gpu_names[16][256] = {};
        const int gpu_count = sotb_gpu_names(gpu_names, 16);
        for (int i = 0; i < gpu_count; ++i) {
            if (gpu_names[i][0]) {
                gpu_names_.emplace_back(gpu_names[i]);
            }
        }

        trophy_page_ = std::make_unique<TrophyPage>(host_, user_dir_);
        incident_page_ = std::make_unique<IncidentPage>(IncidentPlaces());
        incident_page_->CheckReports();
        launcher_ = std::make_unique<trg::Launcher>(MakeConfig());
        updater_ = std::make_unique<trg::update::UpdateController>(MakeUpdateConfig(InstallDir()));
        updater_->StartCheck();
        host_.BeginFrame();
        host_.EndFrame(LauncherTheme().bg);
        host_.Show();
        sotb_startup_phase("launcher running");

        SotbLauncherResult outcome = SOTB_LAUNCHER_QUIT;
        bool first = true;
        while (true) {
            if (!host_.PumpEvents()) {
                outcome = SOTB_LAUNCHER_QUIT;
                break;
            }
            if (!first && host_.Minimized()) {
                SDL_Delay(30);
                continue;
            }
            first = false;
            PollInstall();
            updater_->Poll();
            const double now = double(SDL_GetTicksNS()) / 1e9;
            if (const std::optional<WindowState> state = host_.State()) {
                placement_.Observe(*state, host_.Fullscreen(), now);
                SavePlacement(placement_.TakeSettled(now));
            }
            host_.BeginFrame();
            NormalizeGraphicsCard();
            const trg::Result result = launcher_->Frame();
            const bool update_restart =
                result == trg::Result::kNone &&
                HandleUpdateAction(trg::update::DrawUpdateBanner(updater_->snapshot(), LauncherTheme(), fonts_));
            host_.EndFrame(LauncherTheme().bg);
            if (update_restart) {
                outcome = SOTB_LAUNCHER_QUIT;
                break;
            }
            if (result == trg::Result::kPlay) {
                outcome = SOTB_LAUNCHER_PLAY;
                break;
            }
            if (result == trg::Result::kQuit && LauncherQuitAllowed(ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
                outcome = SOTB_LAUNCHER_QUIT;
                break;
            }
        }
        sotb_startup_phase("launcher loop ended");
        updater_->Cancel();
        if (updater_->Busy()) {
            // A network call in flight is never joined on the UI thread; its worker finishes on its own.
            updater_.release();
        }
        incident_page_.reset();
        std::printf("Launcher: closed%s\n", outcome == SOTB_LAUNCHER_PLAY ? ", starting the game" : "");
        SavePlacement(placement_.Flush());
        const bool saved = settings_.Save();
        std::printf("Launcher: settings %s, Skip Intro %s\n", saved ? "saved" : "NOT SAVED",
                    settings_.GetBool(kSkipIntroKey, false) ? "on" : "off");
        launcher_.reset();
        trophy_page_.reset();
        host_.Close();
        return outcome;
    }

    std::filesystem::path InstallDir() const {
        const std::filesystem::path executable = trg::update::CurrentExecutablePath();
        return executable.has_parent_path() ? executable.parent_path() : std::filesystem::current_path();
    }

    bool HandleUpdateAction(trg::update::UpdateUiAction action) {
        switch (action) {
        case trg::update::UpdateUiAction::kDownload:
            updater_->StartDownload();
            break;
        case trg::update::UpdateUiAction::kRetry:
            if (!updater_->RetryNeedsInstall()) {
                updater_->Retry();
            } else if (PrepareUpdateRestart()) {
                return updater_->Retry();
            }
            break;
        case trg::update::UpdateUiAction::kRestart:
            if (PrepareUpdateRestart()) {
                return updater_->BeginInstall();
            }
            break;
        case trg::update::UpdateUiAction::kNone:
            break;
        }
        return false;
    }

    bool PrepareUpdateRestart() {
        if (install_.running()) {
            launcher_->SetStatus("Wait for the current install to finish.", 5.0);
            return false;
        }
        if (!launcher_->Save()) {
            return false;
        }
        SavePlacement(placement_.Flush());
        return true;
    }

    trg::LauncherConfig MakeConfig() {
        trg::LauncherConfig config;
        config.settings = &settings_;
        config.save_on_play = true;
        config.theme = LauncherTheme();
        config.fonts = fonts_;
        config.hints = "Enter  Play     Alt+F4  Quit     Ctrl+S  Save";
        config.note = "Settings are saved when you press Play.";
        config.branding.title = kPortTitle;
        config.branding.subtitle_segments = {
            {"PC PORT   \xC2\xB7   ", ""},
            {"v" SOTB_PORT_VERSION "   \xC2\xB7   ", ""},
            {"BY SAULOB", kAuthorUrl},
        };
        config.branding.background = LauncherHeader(
            [this](int width, int height) { return HeaderArtTexture(width, height); }, header_aspect_);
        config.sponsor = {"Sponsor on GitHub", kSponsorUrl, sponsor_icon_};
        config.restart_keys = {kWindowWidthKey, kWindowHeightKey, kMonitorKey, kRendererKey, kGraphicsCardKey,
                               kAnisotropyKey, kTextLanguageKey};
        for (const PageId page : kPageOrder) {
            config.pages.push_back(MakePage(page));
            config.pages.back().icon = [this, page](int pixels) { return PageIconTexture(page, pixels); };
        }
        config.can_play = [this]() -> trg::PlayCheck {
            if (install_.running()) {
                return {false, "The game is being installed.", 0};
            }
            if (state_ != GameFileState::kValid) {
                return {false, "The game files are not ready: see the Play page.", 0};
            }
            return {true, {}, 0};
        };
        return config;
    }

    trg::Page MakePage(PageId page) {
        const std::string name(PageName(page));
        switch (page) {
        case PageId::kPlay:
            return {name, "Check the game files and start playing.", [this](trg::Ui& ui) { DrawPlay(ui); },
                    [this] { return state_ != GameFileState::kValid; }, nullptr};
        case PageId::kDisplay:
            return {name, "Window, monitor, scaling and presentation.", [this](trg::Ui& ui) { DrawDisplay(ui); },
                    nullptr, nullptr};
        case PageId::kGraphics:
            return {name, "Resolution, anti-aliasing and texture filtering.", [this](trg::Ui& ui) { DrawGraphics(ui); },
                    nullptr, nullptr};
        case PageId::kGameplay:
            return {name, "Frame rate and readouts.", [this](trg::Ui& ui) { DrawGameplay(ui); }, nullptr, nullptr};
        case PageId::kControls:
            return {name, "Controller and keyboard.", [this](trg::Ui& ui) { DrawControls(ui); }, nullptr, nullptr};
        case PageId::kTrophies:
            return {name, "Your trophies on this PC.", [this](trg::Ui& ui) { DrawTrophies(ui); }, nullptr, nullptr};
        case PageId::kBugReport:
            return {name, "Report a problem and send it with one game log.", [this](trg::Ui& ui) { DrawBugReport(ui); },
                    [this] { return incident_page_ && incident_page_->HasReportUpdates(); }, nullptr};
        case PageId::kAbout:
            return {name, "This port, and where your files and settings live.",
                    [this](trg::Ui& ui) { DrawAbout(ui); }, nullptr, nullptr};
        }
        return {};
    }

    ImTextureID PageIconTexture(PageId page, int pixels) {
        const auto key = std::make_pair(static_cast<int>(page), pixels);
        const auto [icon, is_new] = page_icons_.try_emplace(key);
        if (is_new) {
            const std::vector<uint8_t> rgba = RenderPageIcon(page, pixels);
            if (!rgba.empty()) {
                icon->second = host_.CreateTexture(rgba.data(), pixels, pixels);
            }
        }
        return icon->second;
    }

    static std::string GameFilesHelp() {
        return "Select your own legally obtained Shadow of the Beast package (.pkg). Supported: " + SupportedBuildsText() +
               ". The game is unpacked into the game folder next to the program. The game itself is not included. "
               "Reinstalling keeps your saves and settings.";
    }

    void DrawPlay(trg::Ui& ui) {
        const bool ready = state_ == GameFileState::kValid;
        const std::string title = ready && build_ ? std::string(StatusTitle(state_)) + " \xC2\xB7 " + build_->label
                                                  : StatusTitle(state_);
        const std::string detail = ready && build_ ? BuildMetadataText(*build_) : StatusDetail(state_);
        ui.StatusCard(ready ? trg::Status::kReady : trg::Status::kAttention, title.c_str(), detail.c_str());
        if (!install_message_.empty()) {
            ui.Spacer(4);
            ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
            ImGui::TextUnformatted(install_message_.c_str());
            ImGui::PopStyleColor();
            ui.Spacer(4);
        }

        ui.Row("Game files");
        if (!ui.TaskProgress(install_)) {
            const bool clicked = ready ? ImGui::Button("Reinstall game...", ImVec2(-FLT_MIN, 0))
                                       : ui.AccentButton("Install game...");
            if (clicked) {
                BrowseAndInstall();
            }
        }
        ui.EndRows();
        ui.Help(GameFilesHelp().c_str());
        ui.Toggle("Show this launcher", kShowLauncherHelp, kShowLauncherKey, true, "Off", "On");
        ui.EndRows();

        ui.Spacer(6.0f);
        ui.Info("Build", build_ ? build_->label : "Not installed");
        if (build_) {
            ui.Info("Package", build_->content_id);
            ui.Info("Metadata", BuildMetadataText(*build_).c_str());
        }
        if (!eboot_sha256_.empty()) {
            ui.Info("Executable SHA-256", eboot_sha256_.c_str());
        }
        ui.Info("Game folder", Utf8(app0_).c_str());
    }

    void DrawDisplay(trg::Ui& ui) {
        ui.Choice("Display mode", kDisplayModeHelp, kDisplayModeKey, "windowed",
                  {{"windowed", "Windowed"}, {"borderless", "Borderless Fullscreen"}}, 2);
        ui.Row("Window size", kWindowSizeHelp);
        WindowSizeCombo();
        ui.Row("Monitor", kMonitorHelp);
        MonitorCombo();
        std::vector<trg::Option> renderers;
        for (const Choice& choice : kRendererChoices) {
            renderers.push_back({std::string(choice.key), std::string(choice.name)});
        }
        ui.Combo("Renderer", kRendererHelp, kRendererKey, kDefaultRenderer, renderers);
        ui.Row("Graphics card", kGraphicsCardHelp);
        GraphicsCardCombo();
        ui.Toggle("VSync", kVSyncHelp, kVSyncKey, true, "Off", "On");
        std::vector<trg::Option> presentations;
        for (const Choice& choice : kPresentationChoices) {
            presentations.push_back({std::string(choice.key), std::string(choice.name)});
        }
        ui.Choice("Presentation", kPresentationHelp, kPresentationKey, kDefaultPresentation, presentations, 2);
        ui.Heading("Scaling and sharpening");
        ui.Toggle("Upscaling", kUpscaleHelp, kUpscaleKey, false, "Off", "FSR 1");
        ui.Toggle("Sharpening", kSharpenHelp, kSharpenKey, true, "Off", "On");
        ImGui::BeginDisabled(!settings_.GetBool(kSharpenKey, true));
        ui.SliderInt("Sharpening strength", "How strong the sharpening is.", kSharpenStrengthKey, 75, 0, 100, "%d%%", 5);
        ImGui::EndDisabled();
        ui.EndRows();
    }

    void DrawGraphics(trg::Ui& ui) {
        std::vector<trg::Option> resolutions;
        for (const SotbResolution& resolution : sotb_resolutions) {
            resolutions.push_back({resolution.key, ResolutionLabel(resolution)});
        }
        ui.Combo("Render resolution", kRenderResolutionHelp, kRenderResolutionKey, kNativeResolution, resolutions);
        ui.Toggle("Anti-aliasing", kAntiAliasingHelp, kAntiAliasingKey, false, "Off", "SMAA");
        ui.Combo("Texture filtering", kAnisotropyHelp, kAnisotropyKey, "0",
                 {{"0", "Game"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}});
        ui.EndRows();
    }

    void DrawGameplay(trg::Ui& ui) {
        DrawGameplayStartRows(ui);
        ui.Combo("Frame rate", kFrameRateHelp, kFrameRateKey, "60",
                 {{"30", "30 FPS"}, {"60", "60 FPS (Original)"}, {"90", "90 FPS"}, {"120", "120 FPS"},
                  {"144", "144 FPS"}, {"165", "165 FPS"}, {"240", "240 FPS"}, {"0", "Unlimited"}});
        ui.Toggle("Frame rate in title", kShowFpsTitleHelp, kShowFpsTitleKey, false, "Off", "On");
        ui.Toggle("Render resolution in title", kShowResolutionTitleHelp, kShowResolutionTitleKey, false, "Off", "On");
        ui.Toggle("Frame counter", kFrameCounterHelp, kShowFrameCounterKey, false, "Hidden", "Shown");
        ui.EndRows();
    }

    void DrawControls(trg::Ui& ui) {
        ui.Info("Controller", ControllersText().c_str());
        ui.Info("Keyboard", "Always on, together with the controller.");
        ui.Toggle("Controller Rumble", kControllerRumbleHelp, kControllerRumbleKey, true, "Off", "On");
        ui.Help("A controller that is not recognized needs a line in gamecontrollerdb.txt in the user folder. The log "
                "names its GUID.");
        if (ui.ConfirmRow("Keyboard defaults", "Puts the keyboard controls and the port hotkeys back to their defaults.",
                          "Reset to defaults", "Put every key back to its default?", "Reset")) {
            ResetKeys(settings_);
            ui.SetStatus("Keys reset to defaults.");
        }
        if (ui.Section("Keyboard controls", true)) {
            ui.Help("Click a key, then press the new one. Esc cancels. A key can be used for more than one control.");
            for (int action = 0; action < SOTB_KEY_FIRST_HOTKEY; ++action) {
                KeyRow(ui, static_cast<SotbKeyAction>(action));
            }
        }
        if (ui.Section("Xbox controller mapping", false)) {
            ui.Help("What each Xbox input does in the game. LS and RS pressed together open the Options menu instead.");
            for (const PadMapping& mapping : XboxMapping()) {
                ui.Info(mapping.xbox, mapping.ps4);
            }
        }
        if (ui.Section("Port hotkeys", true)) {
            for (int action = SOTB_KEY_FIRST_HOTKEY; action < SOTB_KEY_COUNT; ++action) {
                KeyRow(ui, static_cast<SotbKeyAction>(action));
            }
        }
    }

    void DrawTrophies(trg::Ui& ui) {
        trophy_page_->SetGame(state_ == GameFileState::kValid ? app0_ : std::filesystem::path());
        trophy_page_->Draw(ui);
    }

    IncidentPage::Places IncidentPlaces() const {
        return {logs_dir_,
                config_.log_file ? PathFromUtf8(config_.log_file) : std::filesystem::path(),
                user_dir_,
                {app0_, user_dir_.parent_path(), logs_dir_.parent_path()},
                [](const std::string& line) { std::printf("%s\n", line.c_str()); }};
    }

    ReportFacts CurrentReportFacts() const {
        static const std::string operating_system = OperatingSystemText();
        const bool known = build_ && state_ == GameFileState::kValid;
        return {SOTB_PORT_VERSION,
                known ? build_->label : "Not installed",
                known ? build_->version : "",
                known ? build_->region : "",
                known ? build_->title_id : "",
                known ? build_->eboot_sha256 : "",
                operating_system};
    }

    void DrawBugReport(trg::Ui& ui) { incident_page_->Draw(ui, CurrentReportFacts()); }

    void DrawAbout(trg::Ui& ui) {
        ui.SponsorCard(
            "This open source PC port is maintained and developed in my free time. If you enjoy the project and would "
            "like to support its continued development, you can sponsor my work on GitHub.",
            "Support is completely optional and helps support ongoing improvements and future projects.");
        ui.Paragraph("Shadow of the Beast for PlayStation 4, running on this PC through a native runtime. No game files "
                     "are included; the game runs from the user's own copy.");
        ui.Spacer(4);
        std::error_code ec;
        if (std::filesystem::is_directory(app0_, ec)) {
            ui.FolderRow("Game files", "Where the installed game is read from.", "Open game folder", Utf8(app0_));
        }
        std::filesystem::create_directories(user_dir_, ec);
        ui.FolderRow("Saves and settings", "Settings, saves and caches.", "Open user folder", Utf8(user_dir_));
        if (ui.ButtonRow("Settings file", "Launcher choices stored as plain text.", "Show settings file") &&
            launcher_->Save()) {
            trg::OpenInFileManager(Utf8(std::filesystem::exists(settings_.file(), ec) ? settings_.file() : user_dir_));
        }
        if (ui.ConfirmRow("Reset settings", "Put the launcher and display settings back to their defaults.",
                          "Reset all settings", "Put every setting back to its default?", "Reset")) {
            settings_.ResetToDefaults();
            launcher_->SetStatus("Settings reset to defaults. Save or press Play to keep them.", 6.0);
        }
        ui.Heading("Diagnostics");
        ui.Toggle("Detailed logging", kDetailedLoggingHelp, kDetailedLoggingKey, true, "Off", "On");
        ui.EndRows();
        if (ui.ButtonRow("Logs folder", "Open the folder containing the session and crash logs", "Open logs folder")) {
            trg::OpenInFileManager(Utf8(logs_dir_));
        }
        const char* platform = "Windows x64";
        ui.InfoGrid({{"Port version", SOTB_PORT_VERSION},
                     {"Game", InstalledGameName(state_, build_)},
                     {"Build", build_ ? build_->label : SupportedBuildsText()},
                     {"Renderer", RendererName(settings_.Get(kRendererKey, "auto"))},
                     {"Platform", platform}});
        char libraries[96];
        std::snprintf(libraries, sizeof(libraries), "SDL %d.%d.%d  \xC2\xB7  Dear ImGui %s  \xC2\xB7  TRG Launcher %s",
                      SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                      SDL_VERSIONNUM_MICRO(SDL_GetVersion()), IMGUI_VERSION, kTrgLauncherVersion);
        ui.Info("Libraries", libraries);
        ui.Link(kPortRepository, std::string("https://") + kPortRepository);
    }

    void WindowSizeCombo() {
        int width = settings_.GetInt(kWindowWidthKey, kDefaultWindowWidth);
        int height = settings_.GetInt(kWindowHeightKey, kDefaultWindowHeight);
        if (width <= 0 || height <= 0) {
            width = kDefaultWindowWidth;
            height = kDefaultWindowHeight;
        }
        const WindowSize current{width, height};
        if (ImGui::BeginCombo("##window_size", WindowSizeLabel(current).c_str())) {
            for (const WindowSize size : WindowSizes()) {
                const bool selected = size == current;
                if (ImGui::Selectable(WindowSizeLabel(size).c_str(), selected) && !selected) {
                    settings_.SetInt(kWindowWidthKey, size.width);
                    settings_.SetInt(kWindowHeightKey, size.height);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    void MonitorCombo() {
        const int current = settings_.GetInt(kMonitorKey, 1);
        if (ImGui::BeginCombo("##monitor", MonitorLabel(current, displays_).c_str())) {
            for (int i = 1; i <= static_cast<int>(displays_.size()); ++i) {
                if (ImGui::Selectable(MonitorLabel(i, displays_).c_str(), i == current) && i != current) {
                    settings_.SetInt(kMonitorKey, i);
                }
            }
            ImGui::EndCombo();
        }
    }

    void GraphicsCardCombo() {
        if (gpu_names_.empty()) {
            ImGui::TextUnformatted("No graphics card found");
            return;
        }
        const std::string current = settings_.Get(kGraphicsCardKey);
        if (ImGui::BeginCombo("##graphics_card", current.c_str())) {
            for (const std::string& name : gpu_names_) {
                if (ImGui::Selectable(name.c_str(), name == current) && name != current) {
                    settings_.Set(kGraphicsCardKey, name);
                }
            }
            ImGui::EndCombo();
        }
    }

    void NormalizeGraphicsCard() {
        const std::string current = settings_.Get(kGraphicsCardKey);
        const int found = SotbGraphics::ResolveGpuByName(gpu_names_, current);
        if (found >= 0 && gpu_names_[found] != current) {
            settings_.Set(kGraphicsCardKey, gpu_names_[found]);
        }
    }

    void KeyRow(trg::Ui& ui, SotbKeyAction action) {
        const std::optional<ImGuiKey> key =
            ui.KeyBindRow(sotb_key_actions[action].label, nullptr, KeyDisplayName(SavedKey(settings_, action)));
        if (!key) {
            return;
        }
        if (const int scancode = ScancodeOf(*key)) {
            SetKey(settings_, action, scancode);
        } else {
            ui.SetStatus("That key cannot be used.");
        }
    }

    std::string ControllersText() const {
        int count = 0;
        SDL_JoystickID* ids = SDL_WasInit(SDL_INIT_GAMEPAD) ? SDL_GetGamepads(&count) : nullptr;
        std::string names;
        for (int i = 0; ids && i < count; ++i) {
            const char* name = SDL_GetGamepadNameForID(ids[i]);
            names += (names.empty() ? "" : ", ") + std::string(name ? name : "Controller");
        }
        SDL_free(ids);
        return names.empty() ? "None connected. Xbox controllers work without setup." : names;
    }

    ImTextureID HeaderArtTexture(int width, int height) {
        if (header_art_.rgba.empty() || width <= 0 || height <= 0) {
            return ImTextureID{};
        }
        if (width != art_width_ || height != art_height_) {
            art_width_ = width;
            art_height_ = height;
            host_.DestroyTexture(art_texture_);
            const std::vector<uint8_t> rgba =
                ScaleArtRgba(header_art_.rgba.data(), header_art_.width, header_art_.height, width, height);
            art_texture_ = host_.CreateTexture(rgba.data(), width, height);
        }
        return art_texture_;
    }

    void SavePlacement(const std::optional<WindowPlacement>& placement) {
        if (!placement) {
            return;
        }
        for (const auto& [key, value] : PlacementValues(*placement)) {
            settings_.Set(key, value);
        }
        settings_.Save();
    }

    void BrowseAndInstall() {
        const std::string picked = trg::BrowseForFile("Select your Shadow of the Beast package (.pkg)",
                                                      {{"PlayStation 4 packages", "*.pkg"}, {"All files", "*.*"}});
        if (picked.empty()) {
            return;
        }
        install_message_.clear();
        auto outcome = std::make_shared<InstallResult>();
        install_outcome_ = outcome;
        const GameRoots roots = roots_;
        const std::filesystem::path source = PathFromUtf8(picked);
        install_.Start("Installing...", [source, roots, outcome](trg::Task& task) {
            InstallHooks hooks;
            hooks.expected = PackageKind::kBase;
            hooks.phase = [&task](const char* phase) {
                task.SetLabel(phase);
                task.Progress(0, 0);
            };
            hooks.progress = [&task](uint64_t done, uint64_t total) {
                task.Progress(static_cast<long long>(done), static_cast<long long>(total));
                return !task.cancelled();
            };
            *outcome = InstallGame(source, roots, hooks);
            return outcome->ok() ? std::string() : DescribeInstall(*outcome).title;
        });
    }

    void PollInstall() {
        std::string error;
        const trg::Task::State finished = install_.Finish(&error);
        if (finished == trg::Task::State::kIdle) {
            return;
        }
        if (install_outcome_) {
            const InstallMessage message = DescribeInstall(*install_outcome_);
            if (install_outcome_->ok()) {
                install_message_.clear();
            } else {
                install_message_ = message.title + ": " + message.text;
            }
            if (!install_outcome_->detail.empty()) {
                std::printf("Install: %s\n", install_outcome_->detail.c_str());
            }
            install_outcome_.reset();
        } else if (finished == trg::Task::State::kFailed) {
            install_message_ = error;
        }
        image_rebuilt_ = false;
        Refresh();
        if (trophy_page_) {
            trophy_page_->Invalidate();
        }
    }

    void Refresh() {
        GameCheck check = CheckGameFiles(GamePaths{app0_, image_dir_});
        if (!image_rebuilt_ && check.state != GameFileState::kValid) {
            // A game tree from an older image format only needs its boot image rebuilt, not another install.
            if (CheckGameTree(app0_).state == GameFileState::kValid) {
                image_rebuilt_ = true;
                char error[512] = "";
                if (sotb_build_boot_image(Utf8(app0_).c_str(), Utf8(image_dir_).c_str(), error, sizeof(error))) {
                    std::printf("Boot image rebuild failed: %s\n", error);
                } else {
                    std::printf("Boot image rebuilt from the installed game\n");
                    check = CheckGameFiles(GamePaths{app0_, image_dir_});
                }
            }
        }
        state_ = check.state;
        build_ = check.build;
        eboot_sha256_ = check.eboot_sha256;
        if (!check.detail.empty()) {
            std::printf("Game files: %s (%s)\n", StateName(check.state), check.detail.c_str());
        }
    }

    const SotbLauncherConfig& config_;
    std::filesystem::path app0_, image_dir_, user_dir_, logs_dir_;
    GameRoots roots_;
    LauncherHost host_;
    LauncherSettings settings_;
    trg::Fonts fonts_;
    PlacementTracker placement_;
    std::vector<Display> displays_;
    std::vector<std::string> gpu_names_;
    std::unique_ptr<trg::Launcher> launcher_;
    std::unique_ptr<TrophyPage> trophy_page_;
    std::unique_ptr<IncidentPage> incident_page_;
    std::unique_ptr<trg::update::UpdateController> updater_;
    GameFileState state_ = GameFileState::kMissing;
    const GameBuild* build_ = nullptr;
    std::string eboot_sha256_;
    std::string install_message_;
    trg::Task install_;
    std::shared_ptr<InstallResult> install_outcome_;
    bool image_rebuilt_ = false;
    SotbPng::Image header_art_;
    ImTextureID art_texture_{};
    ImTextureID sponsor_icon_{};
    std::map<std::pair<int, int>, ImTextureID> page_icons_;
    int art_width_ = 0, art_height_ = 0;
    float header_aspect_ = kHeaderArtAspect;
};

} // namespace
} // namespace Sotb

extern "C" SotbLauncherResult sotb_launcher_run(const SotbLauncherConfig* config) {
    if (!config) {
        return SOTB_LAUNCHER_BLOCKED;
    }
    Sotb::LauncherApp app(*config);
    return app.Run();
}

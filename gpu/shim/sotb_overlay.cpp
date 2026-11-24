// SPDX-License-Identifier: GPL-2.0-or-later
#include "sotb_overlay.h"
#include "sotb_exit_confirm.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>
#include "sotb_accent.h"
#include "sotb_menu_input.h"
#include "sotb_options_menu.h"
#include "localization/ui_text.h"
#include "sotb_options_ui.h"
#include "sotb_settings.h"
#include "sotb_overlay_box.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_opengl3.h"
#ifdef _WIN32
#include "sotb_renderer.h"
#include "imgui_impl_gles3.h"
#endif
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global sotb_font_ttf\n"
    "sotb_font_ttf:\n"
    ".incbin \"" SOTB_FONT_PATH "\"\n"
    ".global sotb_font_ttf_end\n"
    "sotb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden sotb_font_ttf\n"
    ".global sotb_font_ttf\n"
    "sotb_font_ttf:\n"
    ".incbin \"" SOTB_FONT_PATH "\"\n"
    ".hidden sotb_font_ttf_end\n"
    ".global sotb_font_ttf_end\n"
    "sotb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char sotb_font_ttf[];
extern "C" const unsigned char sotb_font_ttf_end[];

namespace SotbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
// The Options menu (F1): open, or closing and still holding the game's input until what the menu received is released.
std::atomic<bool> options_open{false};
sotb::menu::CloseGate menu_close;
std::bitset<ImGuiKey_NamedKey_COUNT> menu_pressed;
std::atomic<float> display_scale{1.0f};
std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
// ShowNotice: text guarded by imgui_mutex, shown until notice_until (steady_clock ticks).
std::string notice_text;
std::atomic<std::chrono::steady_clock::rep> notice_until{0};
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<SotbPng::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
SotbExitConfirm::State exit_confirm{};
std::atomic<bool> exit_confirm_active{false};
int (*exit_save_active_cb)() = nullptr;
int (*exit_close_cb)() = nullptr;
std::chrono::steady_clock::time_point exit_save_wait_started{};
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

// The caller holds imgui_mutex.
void SetNotice(const std::string& text, std::chrono::seconds shown = std::chrono::seconds(2)) {
    notice_text = text;
    notice_until = (std::chrono::steady_clock::now() + shown).time_since_epoch().count();
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// The frame counter's text.
std::string OverlayText() {
    char text[64];
    std::snprintf(text, sizeof(text), "%.0f FPS  %.1f ms", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                  frame_ms_avg);
    return text;
}

struct OverlayPanel {
    SotbOverlayBox::Box box;
    float size;
};

// `after`: a panel already in this corner; the new one goes next to it.
OverlayPanel PlaceOverlayPanel(const char* text, int corner, const OverlayPanel* after = nullptr) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float size = ImGui::GetFontSize() * SotbSettings::Get().overlay_scale / 100.0f;
    const ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const SotbOverlayBox::Rect work{viewport->WorkPos.x, viewport->WorkPos.y, viewport->WorkPos.x + viewport->WorkSize.x,
                                   viewport->WorkPos.y + viewport->WorkSize.y};
    if (after) {
        return {SotbOverlayBox::StackAfter(after->box, corner, text_size.x, text_size.y, size, work), size};
    }
    return {SotbOverlayBox::Place(corner, text_size.x, text_size.y, size, work), size};
}

OverlayPanel FpsCounterBox(const char* text) {
    return PlaceOverlayPanel(text, SotbSettings::Get().overlay_corner);
}

void DrawOverlayPanel(const OverlayPanel& panel, const char* text) {
    constexpr ImU32 panel_color = IM_COL32(7, 10, 22, 190);
    constexpr ImU32 border_color = IM_COL32(sotb::kAccentRed, sotb::kAccentGreen, sotb::kAccentBlue, 255);
    constexpr ImU32 text_color = IM_COL32(255, 255, 255, 255);
    const SotbOverlayBox::Rect& r = panel.box.rect;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), panel_color, panel.box.rounding);
    draw->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), border_color, panel.box.rounding, 0,
                  SotbOverlayBox::kBorderThickness);
    draw->AddText(ImGui::GetFont(), panel.size, ImVec2(r.x0 + panel.box.pad_x, r.y0 + panel.box.pad_y), text_color,
                  text);
}

bool FpsCounterShown() {
    return SotbSettings::Get().show_fps && frame_ms_avg > 0.0f;
}

bool MenuOpen() {
    return options_open;
}

ImVec4 Accent() {
    return ImVec4(sotb::kAccentRed / 255.0f, sotb::kAccentGreen / 255.0f, sotb::kAccentBlue / 255.0f, 1.0f);
}

// The keys and buttons that went down while the menu was open, until they come up again.
void TrackMenuKeys() {
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) {
            continue;
        }
        const size_t index = size_t(key - ImGuiKey_NamedKey_BEGIN);
        if (ImGui::IsKeyPressed(ImGuiKey(key), false)) {
            menu_pressed.set(index);
        } else if (!ImGui::IsKeyDown(ImGuiKey(key))) {
            menu_pressed.reset(index);
        }
    }
}

void OptionsMenu() {
    TrackMenuKeys();
    if (menu_close.requested()) {
        if (menu_close.Step(menu_pressed.any(), ImGui::GetTime())) {
            options_open = false;
            menu_close = {};
            menu_pressed.reset();
            ImGuiIO& io = ImGui::GetIO();
            io.ClearInputKeys();
            io.MouseDrawCursor = false;
        }
        return;
    }
    if (SotbOptionsUi::Frame() == SotbOptionsUi::Result::Close) {
        menu_close.Request();
    }
}

// F1 and the L3 + R3 chord open and close the Options menu; it stays away from the game's own dialogs.
[[maybe_unused]] void PressOptionsMenuKey(bool repeat) {
    const bool busy = text_entry_active || choice_active || loading;
    switch (sotb::menu::TogglePress(repeat, options_open, busy)) {
    case sotb::menu::ToggleAction::Open:
        options_open = true;
        menu_close = {};
        menu_pressed.reset();
        SotbOptionsUi::Reset();
        ImGui::GetIO().MouseDrawCursor = true;
        break;
    case sotb::menu::ToggleAction::Close: menu_close.Request(); break;
    case sotb::menu::ToggleAction::None: break;
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("%s", sotb::Tr("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel"));
    ImGui::TextDisabled("%s", sotb::Tr("Controller: Cross (A) = OK, Circle (B) = cancel"));
    ImGui::End();
}

void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.36f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", choice_title.c_str());
    ImGui::Separator();
    // Save pictures are 228x128; rows without one keep the text aligned when others have one.
    const bool pictures = std::any_of(choice_textures.begin(), choice_textures.end(),
                                      [](ImTextureData* tex) { return tex != nullptr; });
    const ImVec2 picture_size(228.0f * base_scale, 128.0f * base_scale);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool selected = i == choice_index;
        ImGui::BeginGroup();
        ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr;
        if (tex) {
            ImGui::Image(tex->GetTexRef(), picture_size);
        } else if (pictures) {
            ImGui::Dummy(picture_size);
        }
        if (pictures) {
            ImGui::SameLine();
        }
        if (selected) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "> %s", choice_items[i].c_str());
        } else {
            ImGui::Text("  %s", choice_items[i].c_str());
        }
        ImGui::EndGroup();
        if (selected) {
            if (pictures) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float pad = 3.0f * base_scale;
                ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - pad, min.y - pad),
                                                    ImVec2(max.x + pad, max.y + pad),
                                                    IM_COL32(255, 230, 140, 255), 0.0f, 0,
                                                    2.0f * base_scale);
            }
            ImGui::SetScrollHereY();
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", sotb::Tr("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel"));
    ImGui::End();
}

void ExitConfirmBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const SotbExitConfirm::PopupMetrics m =
        SotbExitConfirm::ComputeMetrics(viewport->WorkSize.x, viewport->WorkSize.y, base_scale);
    const SotbExitGameStrings& text = exit_confirm.Text();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(m.min_w, 0.0f), ImVec2(m.max_w, m.max_h));
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m.pad_x, m.pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m.item_spacing_x, m.item_spacing_y));
    ImGui::Begin("##exit_confirm", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(m.title_scale);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text.title);
    ImGui::Dummy(ImVec2(0.0f, m.section_gap * 0.5f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, m.section_gap));
    ImGui::SetWindowFontScale(m.body_scale);
    if (exit_confirm.waiting_for_save) {
        ImGui::TextWrapped("%s", text.saving);
    } else {
        ImGui::TextWrapped("%s", text.question);
        ImGui::Dummy(ImVec2(0.0f, m.section_gap));
        const char* labels[2] = {text.confirm, text.cancel};
        for (int i = 0; i < 2; ++i) {
            if (i == exit_confirm.focus) {
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "> %s", labels[i]);
            } else {
                ImGui::Text("  %s", labels[i]);
            }
        }
    }
    ImGui::Dummy(ImVec2(0.0f, m.section_gap));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, m.section_gap * 0.5f));
    ImGui::SetWindowFontScale(m.hint_scale);
    const std::string hint = exit_confirm.HintText();
    ImGui::TextDisabled("%s", hint.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(sotb::Tr(total ? "Loading shader cache..." : "Preparing graphics..."));
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled("%s", sotb::Tr("The first start after an update takes longer."));
    ImGui::End();
}

void FpsCounter() {
    if (frame_ms_avg <= 0.0f) {
        return;
    }
    const std::string label = OverlayText();
    DrawOverlayPanel(FpsCounterBox(label.c_str()), label.c_str());
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("%s %u%%", sotb::Tr("Loading shader cache"), u32(u64(done) * 100 / total));
    ImGui::End();
}

bool NoticeShown() {
    return std::chrono::steady_clock::now().time_since_epoch().count() < notice_until.load();
}

void Notice() {
    const int corner = SotbOverlayBox::NoticeCorner(SotbSettings::Get().overlay_corner);
    DrawOverlayPanel(PlaceOverlayPanel(notice_text.c_str(), corner), notice_text.c_str());
}

} // namespace

// imgui_mutex held.
void CreateImGuiContext() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "sotb";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(sotb_font_ttf),
                                   int(sotb_font_ttf_end - sotb_font_ttf), 18.0f, &font_config);
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    CreateImGuiContext();

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
}

void InitOpenGL() {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    CreateImGuiContext();
#ifdef _WIN32
    const bool started = SotbRenderer::Active() == SotbRenderer::Backend::Direct3D11
                             ? InitImGuiGles3()
                             : ImGui_ImplOpenGL3_Init("#version 330 core");
#else
    const bool started = ImGui_ImplOpenGL3_Init("#version 330 core");
#endif
    if (!started) {
        std::printf("Overlay: ImGui OpenGL backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const SotbPng::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
// Moves or closes the list (lock held). finish: 1 accept, 2 cancel.
void ChoiceStep(int move, int finish) {
    if (!choice_active) {
        return;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
}

// The runtime's buttons for the list (gamepad and keyboard as mapped, scripted input, the left
// stick as up/down), polled every frame: the game may stop reading the pad during the dialog and
// raw window events depend on focus. Holding up or down repeats.
uint32_t (*choice_input)() = nullptr;

void PollChoiceInput() {
    static std::mutex poll_mutex; // the present thread and the game thread both poll
    std::scoped_lock poll_lock{poll_mutex};
    static uint32_t previous = 0;
    static bool open = false;
    static std::chrono::steady_clock::time_point repeat_at{};
    if (!choice_input) {
        return;
    }
    if (!choice_active) {
        open = false;
        return;
    }
    const uint32_t buttons = choice_input();
    const auto now = std::chrono::steady_clock::now();
    if (!open) { // buttons held when the list opened (the press that opened it) wait for release
        open = true;
        previous = buttons;
        return;
    }
    constexpr uint32_t Up = 0x10, Down = 0x40, Circle = 0x2000, Cross = 0x4000;
    const uint32_t pressed = buttons & ~previous;
    previous = buttons;
    int move = (pressed & Up) ? -1 : (pressed & Down) ? 1 : 0;
    if (move) {
        repeat_at = now + std::chrono::milliseconds(400);
    } else if ((buttons & (Up | Down)) && now >= repeat_at) {
        move = (buttons & Up) ? -1 : 1;
        repeat_at = now + std::chrono::milliseconds(120);
    }
    const int finish = (pressed & Cross) ? 1 : (pressed & Circle) ? 2 : 0;
    if (move || finish) {
        std::scoped_lock lock{imgui_mutex};
        ChoiceStep(move, finish);
    }
}

bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (choice_input) {
            return true; // PollChoiceInput reads the gamepad
        }
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    ChoiceStep(move, finish);
    return true;
}

void SetChoiceInput(uint32_t (*buttons)()) {
    choice_input = buttons;
}

// Caller holds imgui_mutex.
void FinishExitConfirm(SotbExitConfirm::Action action) {
    if (action == SotbExitConfirm::Action::Cancelled) {
        exit_confirm_active = false;
        return;
    }
    if (action != SotbExitConfirm::Action::Confirmed) {
        return;
    }
    if (exit_save_active_cb && exit_save_active_cb()) {
        exit_confirm.waiting_for_save = true;
        exit_save_wait_started = std::chrono::steady_clock::now();
        return;
    }
    exit_confirm.Close();
    exit_confirm_active = false;
    if (exit_close_cb) {
        exit_close_cb();
    }
}

void PollExitConfirmInput() {
    static std::mutex poll_mutex;
    std::scoped_lock poll_lock{poll_mutex};
    static uint32_t previous = 0;
    static bool open = false;
    if (!exit_confirm_active) {
        open = false;
        return;
    }
    {
        std::scoped_lock lock{imgui_mutex};
        if (exit_confirm.waiting_for_save) {
            const auto elapsed = std::chrono::steady_clock::now() - exit_save_wait_started;
            if (!exit_save_active_cb || !exit_save_active_cb() || elapsed >= std::chrono::seconds(5)) {
                exit_confirm.Close();
                exit_confirm_active = false;
                if (exit_close_cb) {
                    exit_close_cb();
                }
            }
            return;
        }
    }
    if (!choice_input) {
        return;
    }
    const uint32_t buttons = choice_input();
    if (!open) {
        open = true;
        previous = buttons;
        return;
    }
    const uint32_t pressed = buttons & ~previous;
    previous = buttons;
    if (!pressed) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    FinishExitConfirm(exit_confirm.StepPad(pressed));
}

bool HandleExitConfirmEvent(const SDL_Event& event) {
    int move = 0;
    bool confirm = false, cancel = false;
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_LEFT || key == SDLK_RIGHT ||
                key == SDLK_W || key == SDLK_S || key == SDLK_A || key == SDLK_D) ? 1 : 0;
        confirm = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE;
        cancel = key == SDLK_ESCAPE || key == SDLK_BACKSPACE;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (choice_input) {
            return true;
        }
        const u8 b = event.gbutton.button;
        move = (b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ||
                b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? 1 : 0;
        const bool south = b == SDL_GAMEPAD_BUTTON_SOUTH;
        const bool east = b == SDL_GAMEPAD_BUTTON_EAST;
        confirm = exit_confirm.original_layout ? east : south;
        cancel = exit_confirm.original_layout ? south : east;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    FinishExitConfirm(exit_confirm.Step(move, confirm, cancel));
    return true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (exit_confirm_active) {
        return HandleExitConfirmEvent(event);
    }
    if (choice_active) {
        return HandleChoiceEvent(event);
    }
    ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (event.key.scancode == SotbOptions::kKey) {
            if (down) {
                PressOptionsMenuKey(event.key.repeat);
            }
            return true;
        }
        if (MenuOpen() && event.key.key == SDLK_ESCAPE && down) {
            if (!event.key.repeat) {
                menu_close.Request();
            }
            return true;
        }
        if (!MenuOpen()) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        (void)down;
        if (!MenuOpen()) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(event.gbutton.button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
        SotbOptionsUi::ForgetPad(event.gdevice.which);
        return false;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (MenuOpen() && (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
            constexpr float kStickDeadZone = 0.25f;
            const float amount = event.gaxis.value / 32767.0f;
            const bool vertical = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY;
            const ImGuiKey negative = vertical ? ImGuiKey_GamepadLStickUp : ImGuiKey_GamepadLStickLeft;
            const ImGuiKey positive = vertical ? ImGuiKey_GamepadLStickDown : ImGuiKey_GamepadLStickRight;
            io.AddKeyAnalogEvent(negative, amount < -kStickDeadZone, std::max(-amount, 0.0f));
            io.AddKeyAnalogEvent(positive, amount > kStickDeadZone, std::max(amount, 0.0f));
            return true;
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT:
        return MenuOpen();
    case SDL_EVENT_MOUSE_MOTION: {
        if (!MenuOpen()) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!MenuOpen()) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!MenuOpen()) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}

void ShowNotice(const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    SetNotice(text);
}

bool Visible() {
    const auto& s = SotbSettings::Get();
    return initialized && (loading || background_total || options_open || text_entry_active || choice_active ||
                           choice_retiring || exit_confirm_active || s.show_fps || NoticeShown());
}

void SetDisplayScale(float scale) {
    display_scale = scale > 0.0f ? scale : 1.0f;
}

bool CapturesInput() {
    return options_open || text_entry_active || choice_active || exit_confirm_active;
}

bool BeginExitConfirm(bool original_layout, bool xbox_prompts, const char* locale,
                      int (*save_active)(), int (*request_close)()) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized || options_open || text_entry_active || choice_active || exit_confirm.active) {
        return false;
    }
    exit_save_active_cb = save_active;
    exit_close_cb = request_close;
    exit_confirm.Open(original_layout, xbox_prompts, locale);
    exit_confirm_active = true;
    return true;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<SotbPng::Image> pictures, int focus) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

std::atomic<u64> presented_frames{0};

u64 PresentedFrames() {
    return presented_frames.load(std::memory_order_relaxed);
}

void LogPresent() {
    presented_frames.fetch_add(1, std::memory_order_relaxed);
}

namespace {

float last_interval_ms = 0.0f;

// Everything that runs every presented frame; false when nothing is on screen.
bool BeginPresent() {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    last_interval_ms = ms;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    PollChoiceInput();
    PollExitConfirmInput();
    return Visible();
}

// imgui_mutex held. `new_frame` is the renderer backend's.
void BuildFrame(u32 width, u32 height, void (*new_frame)()) {
    const float ms = last_interval_ms;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(width), float(height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    new_frame();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }
    if (options_open) {
        OptionsMenu();
    }
    if (SotbSettings::Get().show_fps) {
        FpsCounter();
    }
    if (background_total && !loading) {
        BackgroundLoading();
    }
    if (NoticeShown()) {
        Notice();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active) {
        ChoiceBox();
    }
    if (exit_confirm.active) {
        ExitConfirmBox();
    }
    ImGui::Render();
}

} // namespace

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    if (!BeginPresent()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    BuildFrame(extent.width, extent.height, ImGui_ImplVulkan_NewFrame);

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

void RenderOpenGL(u32 width, u32 height) {
    if (!BeginPresent()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
#ifdef _WIN32
    if (SotbRenderer::Active() == SotbRenderer::Backend::Direct3D11) {
        BuildFrame(width, height, ImGuiGles3NewFrame);
        ImGuiGles3RenderDrawData(ImGui::GetDrawData());
        return;
    }
#endif
    BuildFrame(width, height, ImGui_ImplOpenGL3_NewFrame);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace SotbOverlay

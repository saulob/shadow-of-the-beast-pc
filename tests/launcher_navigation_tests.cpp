// Escape in the launcher, through TRG's real Launcher and Dear ImGui with synthetic input: a key capture, a confirmation
// dialog and a text field each take one Escape and nothing else does; a normal page never quits on Escape; the
// controller capture cancels without binding. No window, no renderer.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "launcher_quit.h"
#include "pad_capture.h"
#include "report_ui.h"
#include "settings.h"
#include "test_progress.h"
#include "trg/launcher.h"

namespace fs = std::filesystem;
using namespace Sotb;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(condition)) {                                                       \
            ++g_failures;                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                         \
        sotb_test_progress(g_checks);                                             \
    } while (0)

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

ImVec2 Center(ImVec2 min, ImVec2 max) { return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f); }

// What the page saw in the last frame.
struct Page {
    std::optional<ImGuiKey> bound;
    int captured = 0;
    int confirmed = 0;
    bool popup_open = false;
    ImVec2 capture_min, capture_max, confirm_min, confirm_max, draft_min, draft_max, plain_min, plain_max;
    char draft[256] = {};
    char plain[256] = {};
    ImGuiID active = 0;
};

struct Step {
    trg::Result result = trg::Result::kNone;
    bool escape = false;
    // What launcher.cpp does with the result: TRG's Quit only counts without an Escape press.
    bool quits() const { return result == trg::Result::kQuit && LauncherQuitAllowed(escape); }
};

struct Harness {
    trg::Launcher& launcher;
    ImVec2 mouse{-1, -1};

    Step Frame(bool down = false) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, down);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        Step step;
        step.result = launcher.Frame();
        step.escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        ImGui::Render();
        return step;
    }
    void Idle(int frames = 3) {
        for (int i = 0; i < frames; ++i) Frame();
    }
    void Click(ImVec2 at) {
        mouse = at;
        Frame();
        Frame(true);
        Frame(true);
        Frame();
        mouse = ImVec2(-1, -1);
        Frame();
    }
    // One press: the frame in which the key goes down, then its release.
    Step Press(ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true);
        const Step pressed = Frame();
        ImGui::GetIO().AddKeyEvent(key, false);
        Frame();
        return pressed;
    }
};

void TestLauncherQuitPolicy() {
    CHECK(!LauncherQuitAllowed(true));
    CHECK(LauncherQuitAllowed(false));
}

void TestPadCaptureEscape() {
    PadCapture capture;
    CHECK(!capture.Begin(SOTB_PAD_CROSS, 0));
    CHECK(!capture.active());
    CHECK(capture.Begin(SOTB_PAD_CROSS, 1));
    SotbPadInput idle{};
    SotbPadInput held{};
    held.buttons = 1u;
    const std::vector<SotbPadInput> released{idle};
    const std::vector<SotbPadInput> pressed{held};
    CHECK(capture.Update(released, false).kind == PadCaptureResult::Kind::kWaiting);
    // Escape wins over a button pressed in the same frame, and assigns nothing.
    const PadCaptureResult cancelled = capture.Update(pressed, true);
    CHECK(cancelled.kind == PadCaptureResult::Kind::kCancelled && cancelled.source == SOTB_PAD_SOURCE_NONE);
    CHECK(!capture.active());
    // Once cancelled, further input and Escape do nothing: one Escape is one action.
    CHECK(capture.Update(pressed, false).kind == PadCaptureResult::Kind::kWaiting);
    CHECK(capture.Update(pressed, false).kind == PadCaptureResult::Kind::kWaiting);
    CHECK(capture.Update(released, true).kind == PadCaptureResult::Kind::kWaiting);
    CHECK(!capture.active());
    // A fresh capture still binds normally.
    CHECK(capture.Begin(SOTB_PAD_CROSS, 1));
    capture.Update(released, false);
    capture.Update(pressed, false);
    CHECK(capture.Update(pressed, false).kind == PadCaptureResult::Kind::kAssigned);
}

void TestLauncherEscape() {
    const fs::path file = fs::temp_directory_path() / "sotb_navigation_test.ini";
    {
        std::ofstream(file, std::ios::trunc) << "\n";
    }
    LauncherSettings settings(file);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1728, 918);
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImFontConfig font_config;
    font_config.SizePixels = 20.0f;
    io.Fonts->AddFontDefault(&font_config);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    Page page;
    trg::LauncherConfig config;
    config.settings = &settings;
    config.pages.push_back({"Play", "Test.", [](trg::Ui& ui) { ui.Paragraph("Ready."); }, nullptr, nullptr});
    config.pages.push_back({"Controls", "Test.", [&](trg::Ui& ui) {
                                if (const std::optional<ImGuiKey> key = ui.KeyCapture("jump", page.bound ? "Bound" : "Space", 300.0f)) {
                                    page.bound = key;
                                    ++page.captured;
                                }
                                page.capture_min = ImGui::GetItemRectMin();
                                page.capture_max = ImGui::GetItemRectMax();
                                ui.EndRows();
                                if (ui.ConfirmRow("Reset", "Test.", "Reset all", "Reset everything?")) ++page.confirmed;
                                page.popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
                                if (!page.popup_open) {
                                    page.confirm_min = ImGui::GetItemRectMin();
                                    page.confirm_max = ImGui::GetItemRectMax();
                                }
                                ui.EndRows();
                                InputTextMultilineKeepOnEscape("##description", page.draft, sizeof(page.draft), ImVec2(500, 80), 0);
                                page.draft_min = ImGui::GetItemRectMin();
                                page.draft_max = ImGui::GetItemRectMax();
                                ImGui::InputTextMultiline("##plain", page.plain, sizeof(page.plain), ImVec2(500, 80));
                                page.plain_min = ImGui::GetItemRectMin();
                                page.plain_max = ImGui::GetItemRectMax();
                                page.active = ImGui::GetActiveID();
                            },
                            nullptr, nullptr});
    trg::Launcher launcher(config);
    Harness h{launcher};
    h.Frame();
    launcher.GoToPage(1);
    h.Idle(60);
    CHECK(launcher.page() == 1);

    // A normal page: Escape is not Quit, and repeating it changes nothing.
    for (int i = 0; i < 3; ++i) {
        const Step step = h.Press(ImGuiKey_Escape);
        CHECK(step.escape && !step.quits());
        CHECK(launcher.page() == 1 && !launcher.capturing_key() && !page.popup_open);
    }
    // The Quit button and Alt+F4 still quit: they come without Escape.
    launcher.RequestQuit();
    CHECK(h.Frame().quits());
    h.Idle();

    // Controller Back on a normal page does nothing either.
    const Step back = h.Press(ImGuiKey_GamepadFaceRight);
    CHECK(back.result == trg::Result::kNone && !back.quits() && launcher.page() == 1);

    // Key capture: Escape cancels it, binds nothing, keeps the old binding and does not also act on the launcher.
    CHECK(page.capture_max.x > page.capture_min.x);
    h.Click(Center(page.capture_min, page.capture_max));
    CHECK(launcher.capturing_key());
    h.Idle(2);
    const Step cancel = h.Press(ImGuiKey_Escape);
    CHECK(!launcher.capturing_key());
    CHECK(!page.bound && page.captured == 0);
    CHECK(cancel.escape && !cancel.quits() && launcher.page() == 1 && !page.popup_open);
    h.Press(ImGuiKey_A);
    CHECK(!page.bound && page.captured == 0);
    // A capture that gets a key binds it, and a later cancelled capture keeps it.
    h.Click(Center(page.capture_min, page.capture_max));
    h.Idle(2);
    h.Press(ImGuiKey_B);
    CHECK(page.bound && *page.bound == ImGuiKey_B && page.captured == 1 && !launcher.capturing_key());
    h.Click(Center(page.capture_min, page.capture_max));
    h.Idle(2);
    h.Press(ImGuiKey_Escape);
    CHECK(page.bound && *page.bound == ImGuiKey_B && page.captured == 1 && !launcher.capturing_key());

    // Confirmation dialog: Escape closes it as Cancel, never confirms, and nothing else happens in that press.
    h.Idle();
    CHECK(page.confirm_max.x > page.confirm_min.x);
    h.Click(Center(page.confirm_min, page.confirm_max));
    h.Idle();
    CHECK(page.popup_open);
    const Step dismiss = h.Press(ImGuiKey_Escape);
    h.Idle();
    CHECK(!page.popup_open && page.confirmed == 0);
    CHECK(dismiss.escape && !dismiss.quits() && launcher.page() == 1);
    const Step again = h.Press(ImGuiKey_Escape);
    CHECK(!again.quits() && !page.popup_open && page.confirmed == 0 && launcher.page() == 1);

    // A text field takes Escape while it is being edited: Play/Quit hotkeys stay off, and the draft is kept.
    h.Idle();
    h.Click(Center(page.draft_min, page.draft_max));
    io.AddInputCharactersUTF8("my long bug report draft");
    h.Idle();
    CHECK(std::strcmp(page.draft, "my long bug report draft") == 0 && page.active != 0);
    const Step leave = h.Press(ImGuiKey_Escape);
    h.Idle();
    CHECK(leave.result == trg::Result::kNone && !leave.quits());
    CHECK(std::strcmp(page.draft, "my long bug report draft") == 0);
    CHECK(page.active == 0 && launcher.page() == 1);

    // Without the helper ImGui reverts to the text the edit began with, which would lose the draft.
    h.Click(Center(page.plain_min, page.plain_max));
    io.AddInputCharactersUTF8("typed");
    h.Idle();
    CHECK(std::strcmp(page.plain, "typed") == 0);
    h.Press(ImGuiKey_Escape);
    h.Idle();
    CHECK(page.plain[0] == '\0');

    // Escape with nothing focused again: still no quit, still on the page.
    const Step last = h.Press(ImGuiKey_Escape);
    CHECK(!last.quits() && launcher.page() == 1 && !launcher.capturing_key() && !page.popup_open);

    ImGui::DestroyContext();
    std::error_code ec;
    fs::remove(file, ec);
}

void TestSourceWiring() {
    const fs::path root = SOTB_SOURCE_DIR;
    const std::string launcher = ReadAll(root / "launcher/launcher.cpp");
    const std::string incident = ReadAll(root / "launcher/incident_page.cpp");
    const std::string trophy = ReadAll(root / "launcher/trophy_page.cpp");
    const std::string reports = ReadAll(root / "launcher/my_reports_page.cpp");
    CHECK(!launcher.empty() && !incident.empty() && !trophy.empty() && !reports.empty());

    // The controller capture reads Escape, then clears the keys so the rest of the frame never sees that press.
    const size_t poll = launcher.find("void PollPadCapture()");
    const size_t escape = launcher.find("pad_capture_.Update(pads, ImGui::IsKeyPressed(ImGuiKey_Escape, false))", poll);
    const size_t clear = launcher.find("ImGui::GetIO().ClearInputKeys();", poll);
    CHECK(poll != std::string::npos && escape < clear);
    // It runs before TRG draws the frame, so the capture comes first.
    const size_t loop = launcher.find("host_.NextFrame()");
    CHECK(launcher.find("PollPadCapture();", loop) < launcher.find("launcher_->Frame();", loop));

    // Escape never quits the launcher; the hints name Alt+F4.
    CHECK(Contains(launcher, "result == trg::Result::kQuit && LauncherQuitAllowed(ImGui::IsKeyPressed(ImGuiKey_Escape, false))"));
    CHECK(Contains(launcher, "Alt+F4  Quit") && !Contains(launcher, "Esc  Quit"));

    // The Bug Report draft keeps its text on Escape, and the page has no Escape action of its own that could clear a result.
    CHECK(Contains(incident, "InputTextMultilineKeepOnEscape(\"##description\""));
    CHECK(!Contains(incident, "ImGuiKey_Escape"));
    // Dialogs close on Escape through their Cancel path only.
    CHECK(Contains(trophy, "if (ImGui::Button(Tr(\"Cancel\"), button) || escape) {"));
    CHECK(Contains(reports, "ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();"));

    // Escape and capture never touch the cursor: the launcher leaves cursor visibility to SDL and the game.
    CHECK(!Contains(launcher, "SDL_HideCursor") && !Contains(launcher, "SDL_ShowCursor"));
}

} // namespace

int main() {
    TestLauncherQuitPolicy();
    TestPadCaptureEscape();
    TestLauncherEscape();
    TestSourceWiring();
    std::printf("launcher navigation tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

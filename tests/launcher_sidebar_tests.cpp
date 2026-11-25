// The launcher's sidebar in a window too short for every page, through TRG's real Launcher and Dear ImGui with synthetic
// input at 100, 150 and 225 percent: scroll range and clamps, reveal of the current page, wheel ownership, clicks on
// scrolled rows, keyboard, Tab and D-pad focus in view, resize, and no scrollbar when every page fits. No window, no
// renderer.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "imgui.h"
#include "imgui_internal.h"
#include "options.h"
#include "test_progress.h"
#include "trg/launcher.h"
#include "trg/layout.h"

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

std::string Between(const std::string& text, const char* begin, const char* end) {
    const size_t from = text.find(begin);
    if (from == std::string::npos) return {};
    const size_t to = text.find(end, from);
    return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

struct LogicalWindow {
    float width, height;
};

constexpr LogicalWindow kLauncherWindows[] = {{900, 610}, {1126, 610}, {1280, 720}, {1920, 1080}};
constexpr LogicalWindow kShortWindows[] = {{900, 420}, {1126, 400}, {900, 360}, {900, 300}};
constexpr float kDisplayScales[] = {1.0f, 1.5f, 2.25f};

int SidebarIndex(PageId page) {
    return int(std::find(std::begin(kPageOrder), std::end(kPageOrder), page) - std::begin(kPageOrder));
}

float SidebarBody(const LogicalWindow& window) {
    using namespace trg::layout;
    return std::max(100.0f, window.height - HeaderHeight(window.height, 1.0f) - kBodyGap - kFooterHeight);
}

void TestSidebarScrollRange() {
    using namespace trg::layout;
    const int pages = int(std::size(kPageOrder));
    CHECK(pages == 8 && kPageOrder[0] == PageId::kPlay && kPageOrder[pages - 1] == PageId::kAbout);
    CHECK(SidebarIndex(PageId::kBugReport) == pages - 2);

    for (const LogicalWindow& window : kLauncherWindows) {
        const float body = SidebarBody(window), row = SidebarRowFor(pages, body);
        CHECK(SidebarScrollMax(pages, row, body) == 0.0f);
        for (int page = 0; page < pages; ++page) CHECK(SidebarScrollToShow(page, pages, row, body, 0.0f) == 0.0f);
    }

    for (const LogicalWindow& window : kShortWindows) {
        const float body = SidebarBody(window), row = SidebarRowFor(pages, body);
        const float max = SidebarScrollMax(pages, row, body);
        CHECK(row == kSidebarRowMin);
        CHECK(SidebarHeight(pages, row) > body && max == SidebarHeight(pages, row) - body);

        CHECK(SidebarScrollToShow(0, pages, row, body, -50.0f) == 0.0f);
        CHECK(SidebarScrollToShow(pages - 1, pages, row, body, max + 50.0f) == max);

        // Moving down reveals each next row at the bottom edge, moving up at the top edge.
        float scroll = 0.0f;
        for (int page = 0; page < pages; ++page) {
            const float next = SidebarScrollToShow(page, pages, row, body, scroll);
            const float top = SidebarRowTop(page, row);
            CHECK(next >= scroll && top - next >= 0.0f && top + row - next <= body);
            scroll = next;
        }
        CHECK(scroll == max);
        CHECK(SidebarRowTop(pages - 1, row) + row + kSidebarPadding - scroll == body);
        for (int page = pages - 1; page >= 0; --page) {
            const float next = SidebarScrollToShow(page, pages, row, body, scroll);
            const float top = SidebarRowTop(page, row);
            CHECK(next <= scroll && top - next >= 0.0f && top + row - next <= body);
            scroll = next;
        }
        CHECK(scroll == 0.0f);

        // A row already in view leaves the scroll where it is.
        const float middle = max * 0.5f;
        for (int page = 0; page < pages; ++page) {
            const float top = SidebarRowTop(page, row) - kSidebarPadding;
            if (top >= middle && top + row + kSidebarPadding * 2.0f <= middle + body) {
                CHECK(SidebarScrollToShow(page, pages, row, body, middle) == middle);
            }
        }
    }

    // The scrollbar takes the place of the right padding, and the row never gets wider than the sidebar allows.
    constexpr float kScrollbar = 10.0f;
    for (const float scale : kDisplayScales) {
        const float narrow = SidebarWidth(900.0f * scale, scale) / scale;
        CHECK(SidebarRowWidth(narrow, 0.0f) == narrow - kSidebarPadding * 2.0f);
        CHECK(SidebarRowWidth(narrow, kScrollbar) == narrow - kSidebarPadding - kScrollbar);
        CHECK(SidebarRowWidth(narrow, 2.0f) == SidebarRowWidth(narrow, 0.0f));
        CHECK(SidebarLabelX() < SidebarRowWidth(narrow, kScrollbar));
    }

    const fs::path root = SOTB_SOURCE_DIR;
    const std::string trg = ReadAll(root / "third_party" / "trg-launcher" / "src" / "launcher.cpp");
    const std::string sidebar = Between(trg, "void Launcher::DrawSidebar(", "void Launcher::DrawContent(");
    CHECK(!sidebar.empty());
    CHECK(sidebar.find("ImGuiWindowFlags_NoScrollbar") == std::string::npos);
    CHECK(sidebar.find("NoScrollWithMouse") == std::string::npos && sidebar.find("NavFlattened") == std::string::npos);
    const size_t content_size = sidebar.find("ImGui::SetNextWindowContentSize(ImVec2(0.0f, layout::SidebarHeight(pages, row) * s_));");
    CHECK(content_size != std::string::npos && content_size < sidebar.find("ImGui::BeginChild(\"##sidebar\""));
    CHECK(sidebar.find("SetScrollY") == std::string::npos && sidebar.find("SetNextWindowScroll") != std::string::npos);
    CHECK(sidebar.find("const ImVec2 item(item_w, item_h);") != std::string::npos);
    CHECK(sidebar.find("ImGuiButtonFlags_EnableNav") != std::string::npos);
    CHECK(Between(trg, "void Launcher::DrawContent(", "void Launcher::OutlinePanel(").find("sidebar_") == std::string::npos);
    CHECK(trg.find("const float row = layout::SidebarRowFor(pages, body_h);") != std::string::npos);
    const std::string patch = ReadAll(root / "gpu" / "patches" / "trg-launcher" / "0020-sidebar-scroll.patch");
    CHECK(patch.find("SidebarScrollToShow") != std::string::npos && patch.find('\r') == std::string::npos);
}

class MemorySettings final : public trg::SettingsStore {
public:
    std::string Get(std::string_view key, std::string_view def) const override {
        const auto found = values_.find(std::string(key));
        return found == values_.end() || found->second.empty() ? std::string(def) : found->second;
    }
    void Set(std::string_view key, std::string value) override { values_[std::string(key)] = std::move(value); }
    bool Save() override { return true; }

private:
    std::map<std::string, std::string> values_;
};

// Launcher::Frame() with every sidebar page, no window and no renderer. Positions are physical pixels.
class HeadlessLauncher {
public:
    HeadlessLauncher(const LogicalWindow& window, float scale, int start_page = 0) : scale_(scale) {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_HasGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.Fonts->AddFontDefault();
        ImGui::GetStyle().FontScaleDpi = scale;
        Resize(window);

        trg::LauncherConfig config;
        config.settings = &settings_;
        config.start_page = start_page;
        config.fonts.size = trg::layout::kTextBody;
        accent_ = trg::Col(config.theme.action);
        for (const PageId id : kPageOrder) {
            trg::Page page;
            page.name = PageName(id);
            if (id == PageId::kTrophies) page.badge = [] { return 3; };
            page.draw = [](trg::Ui&) {
                for (int line = 0; line < 120; ++line) ImGui::TextUnformatted("A page taller than the window");
            };
            config.pages.push_back(std::move(page));
        }
        launcher_ = std::make_unique<trg::Launcher>(std::move(config));
    }
    ~HeadlessLauncher() {
        launcher_.reset();
        ImGui::DestroyContext(context_);
    }
    HeadlessLauncher(const HeadlessLauncher&) = delete;
    HeadlessLauncher& operator=(const HeadlessLauncher&) = delete;

    void Resize(const LogicalWindow& window) { ImGui::GetIO().DisplaySize = ImVec2(window.width * scale_, window.height * scale_); }

    void Frames(int count = 1) {
        for (int i = 0; i < count; ++i) {
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            launcher_->Frame();
            ImGui::Render();
        }
    }

    void Key(ImGuiKey key, int times = 1) {
        for (int i = 0; i < times; ++i) {
            ImGui::GetIO().AddKeyEvent(key, true);
            Frames();
            ImGui::GetIO().AddKeyEvent(key, false);
            Frames(2);
        }
    }

    void MoveMouse(ImVec2 at) {
        ImGui::GetIO().AddMousePosEvent(at.x, at.y);
        Frames(2);
    }

    void Click(ImVec2 at) {
        MoveMouse(at);
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        Frames();
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        Frames(2);
    }

    void Wheel(ImVec2 at, float steps) {
        MoveMouse(at);
        ImGui::GetIO().AddMouseWheelEvent(0.0f, steps);
        Frames(2);
    }

    ImGuiWindow* Window(std::string_view name) const {
        for (ImGuiWindow* window : GImGui->Windows) {
            if (window->Active && std::string_view(window->Name).find(name) != std::string_view::npos) return window;
        }
        return nullptr;
    }
    ImGuiWindow* Sidebar() const { return Window("/##sidebar_"); }
    ImGuiWindow* PageBody() const { return Window("/##page_body_"); }

    float Scroll() const { return Sidebar()->Scroll.y; }
    float ScrollMax() const { return Sidebar()->ScrollMax.y; }
    float Row() const { return trg::layout::SidebarRowFor(int(std::size(kPageOrder)), Sidebar()->Size.y / scale_); }
    float RowTop(int page) const { return Sidebar()->Pos.y + trg::layout::SidebarRowTop(page, Row()) * scale_ - Scroll(); }
    ImVec2 RowCenter(int page) const {
        return ImVec2(Sidebar()->Pos.x + Sidebar()->Size.x * 0.4f, RowTop(page) + Row() * scale_ * 0.5f);
    }
    bool RowVisible(int page) const {
        const float top = RowTop(page), bottom = top + Row() * scale_, tolerance = 0.5f;
        return top >= Sidebar()->Pos.y - tolerance && bottom <= Sidebar()->Pos.y + Sidebar()->Size.y + tolerance;
    }
    ImVec2 SidebarPoint() const { return ImVec2(Sidebar()->Pos.x + 20.0f, Sidebar()->Pos.y + Sidebar()->Size.y * 0.5f); }

    bool Clipped() const {
        const ImGuiWindow* sidebar = Sidebar();
        const ImVec2 min = sidebar->Pos, max(sidebar->Pos.x + sidebar->Size.x, sidebar->Pos.y + sidebar->Size.y);
        for (const ImDrawCmd& cmd : sidebar->DrawList->CmdBuffer) {
            if (cmd.ElemCount == 0) continue;
            if (cmd.ClipRect.x < min.x - 0.5f || cmd.ClipRect.y < min.y - 0.5f || cmd.ClipRect.z > max.x + 0.5f ||
                cmd.ClipRect.w > max.y + 0.5f) {
                return false;
            }
        }
        return true;
    }

    // The middle of the action-coloured shapes of a page's row: its badge, while the row is not the selected one.
    std::optional<float> BadgeCenter(int page) const {
        const ImGuiWindow* sidebar = Sidebar();
        const float row_top = RowTop(page), row_bottom = row_top + Row() * scale_;
        float top = FLT_MAX, bottom = -FLT_MAX;
        for (const ImDrawVert& vertex : sidebar->DrawList->VtxBuffer) {
            if (vertex.col != accent_ || vertex.pos.y < row_top || vertex.pos.y > row_bottom) continue;
            top = std::min(top, vertex.pos.y);
            bottom = std::max(bottom, vertex.pos.y);
        }
        if (top > bottom) return std::nullopt;
        return (top + bottom) * 0.5f;
    }

    int page() const { return launcher_->page(); }
    bool SidebarBoxFocused() const { return GImGui->NavId == Sidebar()->ChildId && GImGui->NavCursorVisible; }

private:
    float scale_;
    ImGuiContext* context_ = nullptr;
    MemorySettings settings_;
    std::unique_ptr<trg::Launcher> launcher_;
    ImU32 accent_ = 0;
};

void TestSidebarScrollFrames() {
    const int pages = int(std::size(kPageOrder));
    const int play = SidebarIndex(PageId::kPlay), trophies = SidebarIndex(PageId::kTrophies);
    const int bug_report = SidebarIndex(PageId::kBugReport), about = SidebarIndex(PageId::kAbout);
    constexpr float kHalfPixel = 0.5f, kPixel = 1.0f;
    const float padding = trg::layout::kSidebarPadding;

    for (const float scale : kDisplayScales) {
        for (const LogicalWindow& window : kLauncherWindows) {
            HeadlessLauncher launcher(window, scale, about);
            launcher.Frames(3);
            CHECK(launcher.Sidebar() != nullptr);
            if (!launcher.Sidebar()) return;
            CHECK(launcher.ScrollMax() <= kHalfPixel && !launcher.Sidebar()->ScrollbarY && launcher.Scroll() == 0.0f);
            for (int page = 0; page < pages; ++page) CHECK(launcher.RowVisible(page));
            launcher.Wheel(launcher.SidebarPoint(), -3.0f);
            CHECK(launcher.Scroll() == 0.0f);
            launcher.Click(launcher.RowCenter(bug_report));
            CHECK(launcher.page() == bug_report && launcher.Scroll() == 0.0f);
        }

        for (const LogicalWindow& window : kShortWindows) {
            HeadlessLauncher launcher(window, scale);
            launcher.Frames(3);
            CHECK(launcher.page() == play && launcher.Scroll() == 0.0f);
            const float body = launcher.Sidebar()->Size.y / scale;
            const float max = trg::layout::SidebarScrollMax(pages, launcher.Row(), body) * scale;
            CHECK(std::abs(launcher.ScrollMax() - max) <= kPixel && max > 0.0f && launcher.Sidebar()->ScrollbarY);
            CHECK(!launcher.RowVisible(about) && launcher.Clipped());

            // Mouse wheel: to the bottom clamp and back to the top clamp, the page body untouched.
            const float body_scroll = launcher.PageBody()->Scroll.y;
            launcher.Wheel(launcher.SidebarPoint(), -1.0f);
            CHECK(launcher.Scroll() > 0.0f && launcher.Scroll() <= launcher.ScrollMax());
            launcher.Wheel(launcher.SidebarPoint(), -100.0f);
            CHECK(launcher.Scroll() == launcher.ScrollMax() && launcher.RowVisible(about) && launcher.RowVisible(bug_report));
            CHECK(launcher.PageBody()->Scroll.y == body_scroll && launcher.Clipped());
            if (launcher.RowVisible(trophies)) {
                const std::optional<float> badge = launcher.BadgeCenter(trophies);
                CHECK(badge && std::abs(*badge - launcher.RowCenter(trophies).y) <= 1.0f);
            }

            // Clicks hit the rows where they are drawn now, and choosing a page keeps the scroll.
            launcher.Click(launcher.RowCenter(bug_report));
            CHECK(launcher.page() == bug_report);
            const float kept = launcher.Scroll();
            launcher.Click(launcher.RowCenter(about));
            CHECK(launcher.page() == about && launcher.Scroll() == kept);
            launcher.Frames(10);
            CHECK(launcher.Scroll() == kept);

            launcher.Wheel(launcher.SidebarPoint(), 100.0f);
            CHECK(launcher.Scroll() == 0.0f && launcher.RowVisible(play));
            if (launcher.RowVisible(trophies)) {
                const std::optional<float> badge = launcher.BadgeCenter(trophies);
                CHECK(badge && std::abs(*badge - launcher.RowCenter(trophies).y) <= 1.0f);
            }

            // Wheel over the page scrolls the page, not the sidebar.
            const ImGuiWindow* page_body = launcher.PageBody();
            launcher.Wheel(ImVec2(page_body->Pos.x + page_body->Size.x * 0.5f, page_body->Pos.y + page_body->Size.y * 0.5f), -2.0f);
            CHECK(launcher.PageBody()->Scroll.y > 0.0f && launcher.Scroll() == 0.0f);

            // Keyboard: focus follows the arrows and the sidebar keeps it in view, a row margin from the edge.
            launcher.Click(launcher.RowCenter(play));
            CHECK(launcher.page() == play);
            for (int page = 1; page < pages; ++page) {
                launcher.Key(ImGuiKey_DownArrow);
                CHECK(launcher.RowVisible(page));
            }
            CHECK(launcher.Scroll() >= launcher.ScrollMax() - padding * scale && launcher.Scroll() <= launcher.ScrollMax());
            launcher.Key(ImGuiKey_Space);
            CHECK(launcher.page() == about);
            for (int page = pages - 2; page >= 0; --page) {
                launcher.Key(ImGuiKey_UpArrow);
                CHECK(launcher.RowVisible(page));
            }
            CHECK(launcher.Scroll() <= padding * scale);
            launcher.Key(ImGuiKey_Space);
            CHECK(launcher.page() == play);

            // Tab walks the rows too.
            launcher.Key(ImGuiKey_Tab, pages - 1);
            CHECK(launcher.RowVisible(about));
            launcher.Key(ImGuiKey_Space);
            CHECK(launcher.page() == about);
            CHECK(launcher.Clipped());
        }

        // Controller only: the D-pad reaches the sidebar, A enters it on the first page, then every row is reachable.
        HeadlessLauncher pad(kShortWindows[0], scale);
        pad.Frames(3);
        pad.Key(ImGuiKey_GamepadDpadDown);
        pad.Key(ImGuiKey_GamepadDpadUp);
        CHECK(pad.SidebarBoxFocused());
        pad.Key(ImGuiKey_GamepadFaceDown);
        for (int page = 1; page < pages; ++page) {
            pad.Key(ImGuiKey_GamepadDpadDown);
            CHECK(pad.RowVisible(page));
        }
        CHECK(pad.Scroll() >= pad.ScrollMax() - padding * scale);
        pad.Key(ImGuiKey_GamepadFaceDown);
        CHECK(pad.page() == about);
        pad.Key(ImGuiKey_GamepadDpadUp, pages - 2);
        CHECK(pad.RowVisible(1));
        pad.Key(ImGuiKey_GamepadDpadUp);
        CHECK(pad.RowVisible(play) && pad.Scroll() <= padding * scale);
        pad.Key(ImGuiKey_GamepadFaceDown);
        CHECK(pad.page() == play && pad.Clipped());

        // Opening on the last page shows it; maximize and restore keep the scroll valid and the page in view.
        HeadlessLauncher opened(kShortWindows[0], scale, about);
        opened.Frames(3);
        CHECK(opened.page() == about && opened.RowVisible(about) && std::abs(opened.Scroll() - opened.ScrollMax()) <= kHalfPixel);
        opened.Resize({1920, 1080});
        opened.Frames(3);
        CHECK(opened.ScrollMax() <= kHalfPixel && opened.Scroll() == 0.0f && !opened.Sidebar()->ScrollbarY);
        opened.Resize(kShortWindows[0]);
        opened.Frames(3);
        CHECK(opened.RowVisible(about) && opened.Scroll() <= opened.ScrollMax() + kHalfPixel);
    }
}

} // namespace

int main() {
    TestSidebarScrollRange();
    TestSidebarScrollFrames();
    std::printf("launcher sidebar tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

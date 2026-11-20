// Drives the real Gameplay rows through TRG's Launcher with ImGui and synthetic mouse events; no window, no renderer.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "gameplay_rows.h"
#include "imgui.h"
#include "settings.h"
#include "trg/launcher.h"

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


std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Frame(trg::Launcher& launcher, ImVec2 mouse, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, down);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    launcher.Frame();
    ImGui::Render();
}

} // namespace

int main() {
    const fs::path file = fs::temp_directory_path() / "sotb_ui_test.ini";
    {
        std::ofstream(file, std::ios::trunc) << "frame_rate = 90\n";
    }
    LauncherSettings settings(file);
    CHECK(!settings.GetBool(kSkipIntroKey, false));

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1728, 918);
    io.IniFilename = nullptr;
    ImFontConfig font_config;
    font_config.SizePixels = 27.0f;
    io.Fonts->AddFontDefault(&font_config);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    ImVec2 on_min{0, 0}, on_max{0, 0};
    trg::LauncherConfig config;
    config.settings = &settings;
    config.pages.push_back({"Play", "Test.", [](trg::Ui& ui) { ui.Toggle("Show this launcher", "x", "show_launcher", true); }, nullptr, nullptr});
    config.pages.push_back({"Gameplay", "Test.", [&](trg::Ui& ui) {
                                DrawGameplayStartRows(ui);
                                on_min = ImGui::GetItemRectMin();
                                on_max = ImGui::GetItemRectMax();
                            },
                            nullptr, nullptr});
    trg::Launcher launcher(config);

    Frame(launcher, ImVec2(-1, -1), false);
    launcher.GoToPage(1);
    for (int i = 0; i < 90; ++i) Frame(launcher, ImVec2(-1, -1), false);
    CHECK(on_max.x > on_min.x && on_max.y > on_min.y);
    const ImVec2 center((on_min.x + on_max.x) * 0.5f, (on_min.y + on_max.y) * 0.5f);
    CHECK(center.x > 0 && center.y > 0 && center.x < 1728 && center.y < 918);

    Frame(launcher, center, false);
    Frame(launcher, center, true);
    Frame(launcher, center, true);
    Frame(launcher, center, false);
    Frame(launcher, center, false);
    CHECK(settings.GetBool(kSkipIntroKey, false));
    CHECK(settings.Get(kSkipIntroKey) == "1");
    CHECK(settings.Save());
    CHECK(ReadAll(file).find("skip_intro = 1") != std::string::npos);

    LauncherSettings reopened(file);
    CHECK(reopened.GetBool(kSkipIntroKey, false));

    ImGui::DestroyContext();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

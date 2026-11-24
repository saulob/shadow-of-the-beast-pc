#include "sotb_exit_confirm.h"
#include "sotb_exit_game.h"
#include "sotb_exit_game_l10n.h"
#include "sotb_menu_buttons.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

static uint64_t g_test_now = 1000;
static int g_confirm_opened = 0;

static uint64_t test_clock(void) {
    return g_test_now;
}

static int test_confirm_handler(void) {
    ++g_confirm_opened;
    return 1;
}

static std::string read_text(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    /* Screen allowlist: Map and Pause only; never gameplay/cutscenes/overlays/options. */
    CHECK(sotb_exit_game_screen_allowed(SOTB_FRONTEND_SCREEN_MAP) == 1);
    CHECK(sotb_exit_game_screen_allowed(SOTB_FRONTEND_SCREEN_PAUSE) == 1);
    CHECK(sotb_exit_game_screen_allowed(0) == 0);
    CHECK(sotb_exit_game_screen_allowed(2) == 0);
    CHECK(sotb_exit_game_screen_allowed(4) == 0);
    CHECK(sotb_exit_game_screen_allowed(27) == 0);
    CHECK(sotb_exit_game_screen_allowed(30) == 0);
    CHECK(SOTB_INFOBAR_BUTTON_SQUARE == 8u);

    /* Append guard: requires allowed screen, bShowInfoBar == 1, free slot, and hotkey 0x8 unused. */
    const uint8_t map_existing[] = {0xcu, 0x7u, 0x5u};
    const uint8_t pause_existing[] = {0x5u, 0x6u};
    const uint8_t live_feed_existing[] = {0x8u, 0x7u, 0x5u};
    CHECK(sotb_exit_game_can_append_button(SOTB_FRONTEND_SCREEN_MAP, 1, 3, 5, map_existing) == 1);
    CHECK(sotb_exit_game_can_append_button(SOTB_FRONTEND_SCREEN_PAUSE, 1, 2, 5, pause_existing) == 1);
    CHECK(sotb_exit_game_can_append_button(SOTB_FRONTEND_SCREEN_MAP, 0, 3, 5, map_existing) == 0);
    CHECK(sotb_exit_game_can_append_button(0, 1, 0, 5, nullptr) == 0);
    CHECK(sotb_exit_game_can_append_button(SOTB_FRONTEND_SCREEN_MAP, 1, 3, 5, live_feed_existing) == 0);
    CHECK(sotb_exit_game_can_append_button(SOTB_FRONTEND_SCREEN_MAP, 1, 5, 5, map_existing) == 0);

    /* Input filter: inactive during gameplay/combat, active only in allowed menu context. */
    sotb_exit_game_reset();
    sotb_exit_game_set_clock(test_clock);
    sotb_exit_game_set_confirm_handler(test_confirm_handler);
    g_confirm_opened = 0;
    g_test_now = 1000;

    CHECK(sotb_exit_game_active() == 0);
    CHECK(sotb_exit_game_filter_square(1) == 0);
    CHECK(g_confirm_opened == 0);
    CHECK(sotb_exit_game_filter_square(0) == 0);

    sotb_exit_game_set_active_for_test(1);
    CHECK(sotb_exit_game_active() == 1);
    CHECK(sotb_exit_game_filter_square(1) == 1);
    CHECK(g_confirm_opened == 1);
    CHECK(sotb_exit_game_filter_square(1) == 1);
    CHECK(g_confirm_opened == 1);
    CHECK(sotb_exit_game_filter_square(0) == 0);

    g_test_now = 1500;
    CHECK(sotb_exit_game_active() == 0);
    CHECK(sotb_exit_game_filter_square(1) == 0);
    CHECK(g_confirm_opened == 1);
    CHECK(sotb_exit_game_filter_square(0) == 0);

    /* Localization: 100% coverage of all 19 Menu & Subtitles locales. */
    const char* canonical[] = {
        "en_US", "en_GB", "ja", "ko", "zh_Hant", "fr", "de", "it", "es_ES", "es_MX",
        "pt_BR", "pt_PT", "nl", "ru", "pl", "fi", "sv", "da", "nb",
    };
    size_t table_count = 0;
    const SotbExitGameStrings* table = sotb_exit_game_strings_table(&table_count);
    CHECK(table != nullptr && table_count == 19);

    const SotbExitGameStrings* en = sotb_exit_game_strings("en_US");
    for (const char* loc : canonical) {
        const SotbExitGameStrings* s = sotb_exit_game_strings(loc);
        CHECK(std::string(s->locale) == loc);
        CHECK(s->footer && *s->footer);
        CHECK(s->title && *s->title);
        CHECK(s->question && *s->question);
        CHECK(s->confirm && *s->confirm);
        CHECK(s->cancel && *s->cancel);
        CHECK(s->saving && *s->saving);
        if (std::string(loc) != "en_US" && std::string(loc) != "en_GB") {
            CHECK(std::string(s->footer) != en->footer);
            CHECK(std::string(s->title) != en->title);
            CHECK(std::string(s->question) != en->question);
            CHECK(std::string(s->confirm) != en->confirm);
            CHECK(std::string(s->cancel) != en->cancel);
        }
    }

    /* Unknown/empty locale falls back to en_US. */
    CHECK(std::string(sotb_exit_game_strings("xx_YY")->locale) == "en_US");
    CHECK(std::string(sotb_exit_game_strings("")->locale) == "en_US");
    CHECK(std::string(sotb_exit_game_strings(nullptr)->locale) == "en_US");

    /* pt_BR and pt_PT preserve their distinct official variants. */
    const SotbExitGameStrings* pt_br = sotb_exit_game_strings("pt_BR");
    const SotbExitGameStrings* pt_pt = sotb_exit_game_strings("pt_PT");
    CHECK(std::string(pt_br->footer) == "SAIR DO JOGO");
    CHECK(std::string(pt_br->title) == "Sair do jogo");
    CHECK(std::string(pt_br->confirm) == "Confirmar");
    CHECK(std::string(pt_br->cancel) == "Cancelar");
    CHECK(std::string(pt_br->question) != std::string(pt_pt->question));
    CHECK(std::string(pt_br->saving) != std::string(pt_pt->saving));

    sotb_exit_game_set_locale("pt_BR");
    CHECK(std::string(sotb_exit_game_locale()) == "pt_BR");

    /* Confirmation modal: Western/Original + Xbox/PlayStation + localized hints. */
    SotbExitConfirm::State modal{};
    modal.Open(false, true, "en_US");
    CHECK(modal.HintText() == "A Confirm     B Cancel");
    CHECK(modal.StepPad(SotbExitConfirm::kButtonCircle) == SotbExitConfirm::Action::Cancelled);
    CHECK(!modal.active);

    modal.Open(false, false, "en_US");
    CHECK(modal.HintText() == "Cross Confirm     Circle Cancel");
    CHECK(modal.StepPad(SotbExitConfirm::kButtonCross) == SotbExitConfirm::Action::Confirmed);

    modal.Open(false, true, "pt_BR");
    CHECK(std::string(modal.Text().title) == "Sair do jogo");
    CHECK(std::string(modal.Text().confirm) == "Confirmar");
    CHECK(std::string(modal.Text().cancel) == "Cancelar");
    CHECK(modal.HintText() == "A Confirmar     B Cancelar");
    CHECK(modal.StepPad(0x20u) == SotbExitConfirm::Action::None);
    CHECK(modal.focus == 1);
    CHECK(modal.StepPad(SotbExitConfirm::kButtonCross) == SotbExitConfirm::Action::Cancelled);

    modal.Open(true, true, "pt_BR");
    CHECK(modal.HintText() == "B Confirmar     A Cancelar");
    CHECK(modal.StepPad(SotbExitConfirm::kButtonCross) == SotbExitConfirm::Action::Cancelled);

    modal.Open(true, false, "pt_BR");
    CHECK(modal.HintText() == "Circle Confirmar     Cross Cancelar");
    CHECK(modal.StepPad(SotbExitConfirm::kButtonCircle) == SotbExitConfirm::Action::Confirmed);

    /* Popup layout bounds across common resolutions (720p, 1080p, 1440p, 2160p). */
    const float resolutions[4][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    for (const auto& res : resolutions) {
        const float scale = std::max(res[1] / 1080.0f, 0.75f);
        const auto m = SotbExitConfirm::ComputeMetrics(res[0], res[1], scale);
        CHECK(m.min_w > res[0] * 0.30f && m.min_w < m.max_w && m.max_w < res[0] * 0.75f);
        CHECK(m.max_h > 0.0f && m.max_h < res[1] * 0.65f);
        CHECK(m.pad_x >= 18.0f && m.pad_y >= 15.0f && m.section_gap > 0.0f);
    }

    /* Divergent build and fingerprint fail closed without touching image. */
    std::vector<uint8_t> dummy_img(SOTB_EXIT_GAME_CREATETEXT_SITE + 128, 0xCC);
    std::vector<uint8_t> backup = dummy_img;
    uint8_t cave[512] = {};
    char err[128] = {};
    CHECK(sotb_exit_game_apply(dummy_img.data(), dummy_img.size(), "CUSA99999-1.00", cave, sizeof(cave), err,
                               sizeof(err)) == 0);
    CHECK(dummy_img == backup);
    CHECK(sotb_exit_game_apply(dummy_img.data(), dummy_img.size(), "CUSA03762-1.00", cave, sizeof(cave), err,
                               sizeof(err)) == 0);
    CHECK(dummy_img == backup);

#ifdef SOTB_SOURCE_DIR
    const std::string src = read_text(std::string(SOTB_SOURCE_DIR) + "/src/sotb_exit_game.c");
    CHECK(!src.empty());
    CHECK(src.find("DEBUG_SAULO") == std::string::npos);
#endif

    std::puts("exit_game_tests: PASS");
    return 0;
}

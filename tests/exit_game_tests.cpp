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
    for (const char* build : {"CUSA03762-1.00", "CUSA02871-1.00", "CUSA00951-1.00"}) {
        CHECK(sotb_exit_game_apply(dummy_img.data(), dummy_img.size(), build, cave, sizeof(cave), err,
                                   sizeof(err)) == 0);
        CHECK(dummy_img == backup);
    }

    /* Valid fingerprints patch cleanly for all 3 supported builds without touching unrelated bytes. */
    static const uint8_t kTickBytes[] = {
        0x4c, 0x89, 0xe7, 0xe8, 0x40, 0x1b, 0x08, 0x00, 0x48, 0x89, 0xc7, 0xe8, 0x18, 0x42, 0xfb, 0xff,
        0x49, 0x89, 0xc6, 0x4d, 0x3b, 0xb4, 0x24, 0x60, 0x03, 0x00, 0x00, 0x74, 0x27, 0x4d, 0x89, 0xb4,
        0x24, 0x60, 0x03, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0xe8, 0x5b, 0xfc, 0xff, 0xff, 0x4d, 0x85, 0xf6,
        0x0f, 0x84, 0xe0, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x06, 0x4c, 0x89, 0xf7, 0xff, 0x90, 0x70, 0x04,
        0x00, 0x00, 0xeb, 0x09, 0x4d, 0x85, 0xf6, 0x0f, 0x84, 0xc9, 0x00, 0x00, 0x00,
    };
    static const uint8_t kAddBytes[] = {
        0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x2b, 0x87, 0x54, 0x03, 0x00,
        0x00, 0x48, 0x63, 0xc8, 0x4c, 0x63, 0x84, 0x8f, 0x48, 0x03, 0x00, 0x00, 0x48, 0x89, 0xc8, 0x48,
        0xc1, 0xe0, 0x04, 0x48, 0x8b, 0x84, 0x07, 0x28, 0x03, 0x00, 0x00, 0x4a, 0x8b, 0x1c, 0xc0, 0x41,
        0x8d, 0x40, 0x01, 0x89, 0x84, 0x8f, 0x48, 0x03, 0x00, 0x00, 0x40, 0x88, 0xb3, 0x71, 0x02, 0x00,
        0x00, 0x48, 0x8d, 0xbb, 0x78, 0x02, 0x00, 0x00, 0x48, 0x89, 0xd6, 0xe8, 0x90, 0x01, 0x3a, 0x00,
        0x31, 0xf6, 0x48, 0x89, 0xdf, 0xe8, 0xe6, 0xef, 0xa4, 0x00, 0x48, 0x89, 0xd8, 0x48, 0x83, 0xc4,
        0x08, 0x5b, 0x5d, 0xc3,
    };
    static const uint8_t kRefreshBytes[] = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x49, 0x89, 0xfe, 0xe8, 0x91, 0xdb, 0xff, 0xff, 0x48,
        0x8b, 0x98, 0xf8, 0x03, 0x00, 0x00, 0x48, 0x85, 0xdb, 0x74, 0x33, 0x48, 0x8d, 0x3d, 0x64, 0xf4,
        0xec, 0x02, 0xe8, 0x29, 0x82, 0x14, 0x00, 0x48, 0x89, 0xdf, 0x48, 0x89, 0xc6, 0xe8, 0x4e, 0x5f,
        0x4d, 0x00, 0x84, 0xc0, 0x74, 0x18, 0x48, 0x89, 0xdf, 0xe8, 0xa2, 0xbc, 0xf7, 0xff, 0x49, 0x8b,
        0x06, 0x4c, 0x89, 0xf7, 0x5b, 0x41, 0x5e, 0x5d, 0xff, 0xa0, 0x70, 0x04, 0x00, 0x00, 0x5b, 0x41,
        0x5e, 0x5d, 0xc3,
    };
    static const uint8_t kCreateBytes[] = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
        0x83, 0xec, 0x48, 0x49, 0x89, 0xcf, 0x49, 0x89, 0xd4, 0x48, 0x89, 0xf3, 0x49, 0x89,
    };
    std::vector<uint8_t> synth(SOTB_EXIT_GAME_CREATETEXT_SITE + 512 + 64, 0);
    auto reset_synth = [&]() {
        std::fill(synth.begin(), synth.end(), 0);
        std::memcpy(synth.data() + 0x1e50e8u, kTickBytes, sizeof(kTickBytes));
        std::memcpy(synth.data() + SOTB_EXIT_GAME_ADDBUTTON_SITE, kAddBytes, sizeof(kAddBytes));
        std::memcpy(synth.data() + 0x269090u, kRefreshBytes, sizeof(kRefreshBytes));
        std::memcpy(synth.data() + SOTB_EXIT_GAME_CREATETEXT_SITE, kCreateBytes, sizeof(kCreateBytes));
    };
    const size_t img_len = SOTB_EXIT_GAME_CREATETEXT_SITE + 64;
    for (const char* build : {"CUSA03762-1.00", "CUSA02871-1.00", "CUSA00951-1.00"}) {
        reset_synth();
        uint8_t* local_cave = synth.data() + img_len;
        CHECK(sotb_exit_game_apply(synth.data(), img_len, build, local_cave, 512, err, sizeof(err)) == 1);
        CHECK(synth[SOTB_EXIT_GAME_TICK_SITE] == 0xe8);
        CHECK(synth[SOTB_EXIT_GAME_SETUP_SITE] == 0xe8 && synth[SOTB_EXIT_GAME_SETUP_SITE + 5] == 0x90);
        CHECK(synth[SOTB_EXIT_GAME_REFRESH_SITE] == 0xe9);
    }

    /* Regional menu layout + Exit Game modal confirm/cancel parity across builds. */
    CHECK(sotb_menu_buttons_effective_circle_confirm("CUSA03762-1.00", SOTB_MENU_BUTTON_LAYOUT_ORIGINAL) == 1);
    CHECK(sotb_menu_buttons_effective_circle_confirm("CUSA02871-1.00", SOTB_MENU_BUTTON_LAYOUT_ORIGINAL) == 0);
    CHECK(sotb_menu_buttons_effective_circle_confirm("CUSA00951-1.00", SOTB_MENU_BUTTON_LAYOUT_ORIGINAL) == 0);
    modal.Open(sotb_menu_buttons_effective_circle_confirm("CUSA02871-1.00", SOTB_MENU_BUTTON_LAYOUT_ORIGINAL) != 0,
               true, "en_US");
    CHECK(modal.HintText() == "A Confirm     B Cancel");
    CHECK(modal.StepPad(SotbExitConfirm::kButtonCross) == SotbExitConfirm::Action::Confirmed);

    /* Non-overlap check across all native patch sites in the executable image. */
    struct PatchRange { uint32_t start, end; };
    const PatchRange all_patches[] = {
        {0x1659c4u, 0x1659cau}, /* menu buttons */
        {0x1993b0u, 0x1993b6u}, /* guest_patches show_screen */
        {0x1a32fdu, 0x1a32ffu}, /* guest_patches map */
        {0x1a46bau, 0x1a46c0u}, /* guest_patches popup */
        {0x1c9f59u, 0x1c9f5fu}, /* guest_patches save flow */
        {0x28d5c21u, 0x28d5c42u}, /* skip_intro boot sequence */
        {0x1e50f3u, 0x1e50f8u}, /* exit_game tick */
        {0x1e5124u, 0x1e512au}, /* exit_game setup */
        {0x1ed540u, 0x1ed545u}, /* guest_patches options */
        {0x1eede5u, 0x1eedebu}, /* guest_patches options social */
        {0x2690ceu, 0x2690deu}, /* exit_game refresh */
        {0x28b810u, 0x28b814u}, /* guest_patches login */
        {0x3408c4u, 0x3408d2u}, /* guest_patches save defaults */
        {0x340f1cu, 0x340f2cu}, /* guest_patches save reset */
        {0x346ba3u, 0x346ba8u}, /* guest_patches serialize 0 */
        {0x346bdbu, 0x346be0u}, /* guest_patches serialize 1 */
        {0x607f0cu, 0x607f16u}, /* guest_display 0 */
        {0x6080b7u, 0x6080c1u}, /* guest_display 1 */
        {0x609011u, 0x60901bu}, /* guest_display 2 */
        {0x609037u, 0x60904bu}, /* guest_display 4+5 */
        {0x609290u, 0x60929au}, /* guest_display 3 */
        {0x3d42610u, 0x3d42618u}, /* loading hint vtable */
    };
    for (size_t i = 0; i < sizeof(all_patches) / sizeof(*all_patches); ++i) {
        CHECK(all_patches[i].start < all_patches[i].end);
        for (size_t j = i + 1; j < sizeof(all_patches) / sizeof(*all_patches); ++j) {
            CHECK(all_patches[i].end <= all_patches[j].start || all_patches[j].end <= all_patches[i].start);
        }
    }

#ifdef SOTB_SOURCE_DIR
    const std::string src = read_text(std::string(SOTB_SOURCE_DIR) + "/src/sotb_exit_game.c");
    CHECK(!src.empty());
    CHECK(src.find("DEBUG_SAULO") == std::string::npos);
    const std::string loader = read_text(std::string(SOTB_SOURCE_DIR) + "/src/loader.c");
    CHECK(loader.find("sotb_menu_buttons_effective_circle_confirm(") != std::string::npos);
#endif

    std::puts("exit_game_tests: PASS");
    return 0;
}

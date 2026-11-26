#include "sotb_exit_game.h"
#include "sotb_exit_game_l10n.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define GUEST_ABI __attribute__((sysv_abi))

#define TICK_CONTEXT_OFFSET 0x1e50e8u
#define REFRESH_CONTEXT_OFFSET 0x269090u
#define GET_TOP_SCREEN_TARGET 0x199310u
#define ACTIVE_TTL_MS 200u
#define FM_STACK_PTR_OFFSET 0x280u
#define FM_STACK_COUNT_OFFSET 0x288u
#define FM_STACK_ENTRY_SIZE 16u
#define FM_STACK_MAX_SCAN 64
#define SCREEN_SHOW_BAR_OFFSET 0x25cu
#define INFOBAR_BUTTONS_OFFSET 0x328u
#define INFOBAR_COUNTS_OFFSET 0x348u
#define INFOBAR_ACTIVE_BUF_OFFSET 0x354u
#define HOTKEY_BUTTON_ENUM_OFFSET 0x271u

static const uint8_t kTickContext[] = {
    0x4c, 0x89, 0xe7, 0xe8, 0x40, 0x1b, 0x08, 0x00, 0x48, 0x89, 0xc7, 0xe8, 0x18, 0x42, 0xfb, 0xff,
    0x49, 0x89, 0xc6, 0x4d, 0x3b, 0xb4, 0x24, 0x60, 0x03, 0x00, 0x00, 0x74, 0x27, 0x4d, 0x89, 0xb4,
    0x24, 0x60, 0x03, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0xe8, 0x5b, 0xfc, 0xff, 0xff, 0x4d, 0x85, 0xf6,
    0x0f, 0x84, 0xe0, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x06, 0x4c, 0x89, 0xf7, 0xff, 0x90, 0x70, 0x04,
    0x00, 0x00, 0xeb, 0x09, 0x4d, 0x85, 0xf6, 0x0f, 0x84, 0xc9, 0x00, 0x00, 0x00,
};

static const uint8_t kAddButtonPrologue[] = {
    0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x2b, 0x87, 0x54, 0x03, 0x00,
    0x00, 0x48, 0x63, 0xc8, 0x4c, 0x63, 0x84, 0x8f, 0x48, 0x03, 0x00, 0x00, 0x48, 0x89, 0xc8, 0x48,
    0xc1, 0xe0, 0x04, 0x48, 0x8b, 0x84, 0x07, 0x28, 0x03, 0x00, 0x00, 0x4a, 0x8b, 0x1c, 0xc0, 0x41,
    0x8d, 0x40, 0x01, 0x89, 0x84, 0x8f, 0x48, 0x03, 0x00, 0x00, 0x40, 0x88, 0xb3, 0x71, 0x02, 0x00,
    0x00, 0x48, 0x8d, 0xbb, 0x78, 0x02, 0x00, 0x00, 0x48, 0x89, 0xd6, 0xe8, 0x90, 0x01, 0x3a, 0x00,
    0x31, 0xf6, 0x48, 0x89, 0xdf, 0xe8, 0xe6, 0xef, 0xa4, 0x00, 0x48, 0x89, 0xd8, 0x48, 0x83, 0xc4,
    0x08, 0x5b, 0x5d, 0xc3,
};

static const uint8_t kRefreshContext[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x49, 0x89, 0xfe, 0xe8, 0x91, 0xdb, 0xff, 0xff, 0x48,
    0x8b, 0x98, 0xf8, 0x03, 0x00, 0x00, 0x48, 0x85, 0xdb, 0x74, 0x33, 0x48, 0x8d, 0x3d, 0x64, 0xf4,
    0xec, 0x02, 0xe8, 0x29, 0x82, 0x14, 0x00, 0x48, 0x89, 0xdf, 0x48, 0x89, 0xc6, 0xe8, 0x4e, 0x5f,
    0x4d, 0x00, 0x84, 0xc0, 0x74, 0x18, 0x48, 0x89, 0xdf, 0xe8, 0xa2, 0xbc, 0xf7, 0xff, 0x49, 0x8b,
    0x06, 0x4c, 0x89, 0xf7, 0x5b, 0x41, 0x5e, 0x5d, 0xff, 0xa0, 0x70, 0x04, 0x00, 0x00, 0x5b, 0x41,
    0x5e, 0x5d, 0xc3,
};

static const uint8_t kCreateTextPrologue[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xec, 0x48, 0x49, 0x89, 0xcf, 0x49, 0x89, 0xd4, 0x48, 0x89, 0xf3, 0x49, 0x89,
};

typedef void(GUEST_ABI *CreateTextFn)(void *out_ftext, const uint16_t *literal, const uint16_t *ns,
                                      const uint16_t *key);
typedef void *(GUEST_ABI *AddButtonFn)(void *infobar_ctrl, uint32_t button, const void *ftext);

static const uint16_t kExitGameNamespace[] = {'E', 'x', 'i', 't', 'G', 'a', 'm', 'e', 0};
static const uint16_t kExitGameKey[] = {'B', 'u', 't', 't', 'o', 'n', 'E', 'x', 'i', 't', 'G', 'a', 'm', 'e', 0};

typedef struct {
    const char *build_key;
    uint32_t tick_context;
    uint32_t tick_site;
    uint32_t setup_site;
    uint32_t refresh_context;
    uint32_t refresh_site;
    uint32_t addbutton_site;
    uint32_t createtext_site;
    uint32_t get_top_screen_target;
} ExitGameBuild;

static const ExitGameBuild kExitGameBuilds[] = {
    {"CUSA03762-1.00", TICK_CONTEXT_OFFSET, SOTB_EXIT_GAME_TICK_SITE, SOTB_EXIT_GAME_SETUP_SITE,
     REFRESH_CONTEXT_OFFSET, SOTB_EXIT_GAME_REFRESH_SITE, SOTB_EXIT_GAME_ADDBUTTON_SITE,
     SOTB_EXIT_GAME_CREATETEXT_SITE, GET_TOP_SCREEN_TARGET},
    {"CUSA02871-1.00", TICK_CONTEXT_OFFSET, SOTB_EXIT_GAME_TICK_SITE, SOTB_EXIT_GAME_SETUP_SITE,
     REFRESH_CONTEXT_OFFSET, SOTB_EXIT_GAME_REFRESH_SITE, SOTB_EXIT_GAME_ADDBUTTON_SITE,
     SOTB_EXIT_GAME_CREATETEXT_SITE, GET_TOP_SCREEN_TARGET},
    {"CUSA00951-1.00", TICK_CONTEXT_OFFSET, SOTB_EXIT_GAME_TICK_SITE, SOTB_EXIT_GAME_SETUP_SITE,
     REFRESH_CONTEXT_OFFSET, SOTB_EXIT_GAME_REFRESH_SITE, SOTB_EXIT_GAME_ADDBUTTON_SITE,
     SOTB_EXIT_GAME_CREATETEXT_SITE, GET_TOP_SCREEN_TARGET},
};

static const ExitGameBuild *find_exit_build(const char *build_key) {
    if (!build_key) return NULL;
    for (size_t i = 0; i < sizeof(kExitGameBuilds) / sizeof(*kExitGameBuilds); ++i) {
        if (!strcmp(build_key, kExitGameBuilds[i].build_key)) return &kExitGameBuilds[i];
    }
    return NULL;
}

int sotb_exit_game_supported(const char *build_key) { return find_exit_build(build_key) != NULL; }

static uint8_t *g_image_base;
static const ExitGameBuild *g_active_build;
static SotbExitConfirmHandler g_confirm_handler;
static SotbExitGameClock g_clock;
static const char *g_locale = "en_US";

static atomic_int g_active;
static atomic_uint_least8_t g_last_screen;
static atomic_uint_least64_t g_last_tick_ms;
static atomic_int g_collision_screen;
static atomic_int g_prev_square;
static atomic_int g_swallow_square;

/* UE4 FText is 40 bytes; keep a 64-byte aligned buffer created once on the game thread. */
static _Alignas(16) uint8_t g_exit_ftext[64];
static int g_exit_ftext_ready;

static void utf8_to_utf16(const char *src, uint16_t *dst, size_t cap) {
    if (!dst || !cap) return;
    size_t out = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p && out + 1 < cap) {
        uint32_t cp;
        if (*p < 0x80u) {
            cp = *p++;
        } else if ((*p & 0xe0u) == 0xc0u && p[1]) {
            cp = ((uint32_t)(p[0] & 0x1fu) << 6) | (uint32_t)(p[1] & 0x3fu);
            p += 2;
        } else if ((*p & 0xf0u) == 0xe0u && p[1] && p[2]) {
            cp = ((uint32_t)(p[0] & 0x0fu) << 12) | ((uint32_t)(p[1] & 0x3fu) << 6) | (uint32_t)(p[2] & 0x3fu);
            p += 3;
        } else {
            ++p;
            continue;
        }
        dst[out++] = (uint16_t)cp;
    }
    dst[out] = 0;
}

static uint64_t default_clock_ms(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000);
}

static uint64_t now_ms(void) {
    return g_clock ? g_clock() : default_clock_ms();
}

int sotb_exit_game_screen_allowed(uint8_t screen_id) {
    return screen_id == SOTB_FRONTEND_SCREEN_MAP || screen_id == SOTB_FRONTEND_SCREEN_PAUSE;
}

int sotb_exit_game_can_append_button(uint8_t screen_id, uint8_t show_bar, int32_t count, int32_t capacity,
                                     const uint8_t *existing_hotkeys) {
    if (!sotb_exit_game_screen_allowed(screen_id) || !show_bar) return 0;
    if (count < 0 || count >= capacity || capacity > 8) return 0;
    if (existing_hotkeys) {
        for (int32_t i = 0; i < count; ++i) {
            if (existing_hotkeys[i] == SOTB_INFOBAR_BUTTON_SQUARE) return 0;
        }
    }
    return 1;
}

void sotb_exit_game_set_confirm_handler(SotbExitConfirmHandler handler) {
    g_confirm_handler = handler;
}

void sotb_exit_game_set_clock(SotbExitGameClock clock) {
    g_clock = clock;
}

void sotb_exit_game_set_locale(const char *locale) {
    g_locale = sotb_exit_game_strings(locale)->locale;
    g_exit_ftext_ready = 0;
}

const char *sotb_exit_game_locale(void) {
    return g_locale;
}

void sotb_exit_game_reset(void) {
    g_confirm_handler = NULL;
    g_clock = NULL;
    g_locale = "en_US";
    g_exit_ftext_ready = 0;
    atomic_store(&g_active, 0);
    atomic_store(&g_last_screen, 0);
    atomic_store(&g_last_tick_ms, 0);
    atomic_store(&g_collision_screen, 0);
    atomic_store(&g_prev_square, 0);
    atomic_store(&g_swallow_square, 0);
}

void sotb_exit_game_set_active_for_test(int active) {
    atomic_store(&g_collision_screen, 0);
    atomic_store(&g_active, active ? 1 : 0);
    atomic_store(&g_last_tick_ms, active ? now_ms() : 0);
}

uint8_t sotb_exit_game_last_screen(void) {
    return atomic_load(&g_last_screen);
}

int sotb_exit_game_active(void) {
    if (!atomic_load(&g_active)) return 0;
    const uint64_t last = atomic_load(&g_last_tick_ms);
    const uint64_t now = now_ms();
    if (now < last || now - last > ACTIVE_TTL_MS) return 0;
    return 1;
}

int sotb_exit_game_filter_square(int square_down) {
    const int down = square_down ? 1 : 0;
    const int prev = atomic_exchange(&g_prev_square, down);
    if (!down) {
        atomic_store(&g_swallow_square, 0);
        return 0;
    }
    if (atomic_load(&g_swallow_square)) return 1;
    if (!prev && sotb_exit_game_active()) {
        atomic_store(&g_swallow_square, 1);
        if (g_confirm_handler) g_confirm_handler();
        return 1;
    }
    return 0;
}

static uint8_t lookup_screen_id(const void *frontend_manager, const void *top_screen) {
    if (!frontend_manager || !top_screen) return 0;
    const uint8_t *fm = (const uint8_t *)frontend_manager;
    const uint8_t *stack = NULL;
    int32_t count = 0;
    memcpy(&stack, fm + FM_STACK_PTR_OFFSET, sizeof(stack));
    memcpy(&count, fm + FM_STACK_COUNT_OFFSET, sizeof(count));
    if (!stack || count <= 0 || count > FM_STACK_MAX_SCAN) return 0;
    for (int32_t i = count - 1; i >= 0; --i) {
        const uint8_t *entry = stack + (size_t)i * FM_STACK_ENTRY_SIZE;
        const void *ctrl = NULL;
        memcpy(&ctrl, entry + 8, sizeof(ctrl));
        if (ctrl == top_screen) return entry[0];
    }
    return 0;
}

static void GUEST_ABI on_infobar_tick(void *frontend_manager, void *top_screen) {
    if (!frontend_manager || !top_screen) {
        atomic_store(&g_last_screen, 0);
        atomic_store(&g_active, 0);
        atomic_store(&g_collision_screen, 0);
        return;
    }
    const uint8_t screen_id = lookup_screen_id(frontend_manager, top_screen);
    const uint8_t prev_screen = atomic_exchange(&g_last_screen, screen_id);
    if (screen_id != prev_screen) {
        atomic_store(&g_collision_screen, 0);
    }
    const uint8_t show_bar = *((const uint8_t *)top_screen + SCREEN_SHOW_BAR_OFFSET);
    const int allowed = sotb_exit_game_screen_allowed(screen_id) && show_bar && !atomic_load(&g_collision_screen);
    atomic_store(&g_active, allowed ? 1 : 0);
    atomic_store(&g_last_tick_ms, allowed ? now_ms() : 0);
}

static void on_infobar_setup_impl(void *infobar_ctrl, void *top_screen) {
    if (!infobar_ctrl || !top_screen || !g_image_base) return;
    const uint8_t screen_id = atomic_load(&g_last_screen);
    const uint8_t show_bar = *((const uint8_t *)top_screen + SCREEN_SHOW_BAR_OFFSET);
    if (!sotb_exit_game_screen_allowed(screen_id) || !show_bar) return;

    const uint8_t *ctrl = (const uint8_t *)infobar_ctrl;
    int32_t active_buf = 0;
    memcpy(&active_buf, ctrl + INFOBAR_ACTIVE_BUF_OFFSET, sizeof(active_buf));
    if (active_buf != 0 && active_buf != 1) return;
    const int32_t inactive_buf = 1 - active_buf;

    int32_t count = 0;
    int32_t capacity = 0;
    void **buttons = NULL;
    memcpy(&count, ctrl + INFOBAR_COUNTS_OFFSET + (size_t)inactive_buf * 4u, sizeof(count));
    memcpy(&buttons, ctrl + INFOBAR_BUTTONS_OFFSET + (size_t)inactive_buf * 16u, sizeof(buttons));
    memcpy(&capacity, ctrl + INFOBAR_BUTTONS_OFFSET + (size_t)inactive_buf * 16u + 8u, sizeof(capacity));
    if (!buttons || count < 0 || count >= capacity || capacity > 8) return;

    uint8_t existing[8] = {0};
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t *btn = (const uint8_t *)buttons[i];
        existing[i] = btn ? btn[HOTKEY_BUTTON_ENUM_OFFSET] : 0xffu;
    }
    if (!sotb_exit_game_can_append_button(screen_id, show_bar, count, capacity, existing)) {
        atomic_store(&g_collision_screen, 1);
        atomic_store(&g_active, 0);
        return;
    }

    if (!g_exit_ftext_ready) {
        uint16_t footer_u16[64];
        utf8_to_utf16(sotb_exit_game_strings(g_locale)->footer, footer_u16, sizeof(footer_u16) / sizeof(footer_u16[0]));
        const uint32_t create_site = g_active_build ? g_active_build->createtext_site : SOTB_EXIT_GAME_CREATETEXT_SITE;
        CreateTextFn create_text = (CreateTextFn)(uintptr_t)(g_image_base + create_site);
        memset(g_exit_ftext, 0, sizeof(g_exit_ftext));
        create_text(g_exit_ftext, footer_u16, kExitGameNamespace, kExitGameKey);
        g_exit_ftext_ready = 1;
    }

    const uint32_t add_site = g_active_build ? g_active_build->addbutton_site : SOTB_EXIT_GAME_ADDBUTTON_SITE;
    AddButtonFn add_button = (AddButtonFn)(uintptr_t)(g_image_base + add_site);
    add_button(infobar_ctrl, SOTB_INFOBAR_BUTTON_SQUARE, g_exit_ftext);
}

static void GUEST_ABI on_infobar_setup(void *infobar_ctrl, void *top_screen) {
    on_infobar_setup_impl(infobar_ctrl, top_screen);
}

static void GUEST_ABI on_infobar_refresh(void *infobar_ctrl, void *top_screen) {
    on_infobar_setup_impl(infobar_ctrl, top_screen);
}

static size_t emit_tick_stub(uint8_t *dst, const uint8_t *orig_get_top) {
    static const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x41, 0x54, 0x48, 0x89, 0xfb};
    static const uint8_t mid[] = {0x49, 0x89, 0xc4, 0x48, 0x89, 0xdf, 0x4c, 0x89, 0xe6};
    static const uint8_t epilogue[] = {0x4c, 0x89, 0xe0, 0x41, 0x5c, 0x5b, 0x5d, 0xc3};
    const uint64_t target1 = (uint64_t)(uintptr_t)orig_get_top;
    const uint64_t target2 = (uint64_t)(uintptr_t)on_infobar_tick;
    uint8_t *p = dst;
    memcpy(p, prologue, sizeof(prologue));
    p += sizeof(prologue);
    *p++ = 0x48;
    *p++ = 0xb8;
    memcpy(p, &target1, 8);
    p += 8;
    *p++ = 0xff;
    *p++ = 0xd0;
    memcpy(p, mid, sizeof(mid));
    p += sizeof(mid);
    *p++ = 0x48;
    *p++ = 0xb8;
    memcpy(p, &target2, 8);
    p += 8;
    *p++ = 0xff;
    *p++ = 0xd0;
    memcpy(p, epilogue, sizeof(epilogue));
    p += sizeof(epilogue);
    return (size_t)(p - dst);
}

static size_t emit_setup_stub(uint8_t *dst) {
    static const uint8_t prologue[] = {
        0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x08, 0x48, 0x89, 0xfb,
        0xff, 0x90, 0x70, 0x04, 0x00, 0x00,
        0x4c, 0x89, 0xe7, 0x48, 0x89, 0xde,
    };
    static const uint8_t epilogue[] = {0x48, 0x83, 0xc4, 0x08, 0x5b, 0x5d, 0xc3};
    const uint64_t target = (uint64_t)(uintptr_t)on_infobar_setup;
    uint8_t *p = dst;
    memcpy(p, prologue, sizeof(prologue));
    p += sizeof(prologue);
    *p++ = 0x48;
    *p++ = 0xb8;
    memcpy(p, &target, 8);
    p += 8;
    *p++ = 0xff;
    *p++ = 0xd0;
    memcpy(p, epilogue, sizeof(epilogue));
    p += sizeof(epilogue);
    return (size_t)(p - dst);
}

static size_t emit_refresh_stub(uint8_t *dst) {
    /* Entered via jmp from RefreshInfoBar (0x2690ce) with rbx=InfoBarCtrl, r14=Screen, rsp 16-byte aligned. */
    static const uint8_t prologue[] = {
        0x49, 0x8b, 0x06, 0x4c, 0x89, 0xf7,
        0xff, 0x90, 0x70, 0x04, 0x00, 0x00,
        0x48, 0x89, 0xdf, 0x4c, 0x89, 0xf6,
    };
    static const uint8_t epilogue[] = {0x5b, 0x41, 0x5e, 0x5d, 0xc3};
    const uint64_t target = (uint64_t)(uintptr_t)on_infobar_refresh;
    uint8_t *p = dst;
    memcpy(p, prologue, sizeof(prologue));
    p += sizeof(prologue);
    *p++ = 0x48;
    *p++ = 0xb8;
    memcpy(p, &target, 8);
    p += 8;
    *p++ = 0xff;
    *p++ = 0xd0;
    memcpy(p, epilogue, sizeof(epilogue));
    p += sizeof(epilogue);
    return (size_t)(p - dst);
}

static int fail(char *error, size_t size, const char *reason) {
    if (error && size) snprintf(error, size, "%s", reason);
    return 0;
}

static int rel32_fits(intptr_t delta) {
    return delta >= -0x7fffffffLL && delta <= 0x7fffffffLL;
}

static int build_matches(const ExitGameBuild *build, const uint8_t *image, size_t image_size, char *error,
                         size_t error_size) {
    if (image_size < build->createtext_site + sizeof(kCreateTextPrologue))
        return fail(error, error_size, "image too short");
    if (memcmp(image + build->tick_context, kTickContext, sizeof(kTickContext)))
        return fail(error, error_size, "MenuInfoBar::Tick bytes do not match");
    if (memcmp(image + build->refresh_context, kRefreshContext, sizeof(kRefreshContext)))
        return fail(error, error_size, "RefreshInfoBar bytes do not match");
    if (memcmp(image + build->addbutton_site, kAddButtonPrologue, sizeof(kAddButtonPrologue)))
        return fail(error, error_size, "MenuInfoBar::AddButton bytes do not match");
    if (memcmp(image + build->createtext_site, kCreateTextPrologue, sizeof(kCreateTextPrologue)))
        return fail(error, error_size, "FText::CreateText bytes do not match");
    return 1;
}

int sotb_exit_game_check(const uint8_t *image, size_t image_size, const char *build_key, char *error, size_t error_size) {
    const ExitGameBuild *build = find_exit_build(build_key);
    if (!build) return fail(error, error_size, "not a supported build");
    if (!image) return fail(error, error_size, "missing image");
    return build_matches(build, image, image_size, error, error_size);
}

size_t sotb_exit_game_sites(const char *build_key, SotbPatchSite *out, size_t capacity) {
    const ExitGameBuild *build = find_exit_build(build_key);
    if (!build || capacity < 4) return 0;
    out[0] = (SotbPatchSite){"MenuInfoBar::Tick", build->tick_context, kTickContext, sizeof(kTickContext)};
    out[1] = (SotbPatchSite){"RefreshInfoBar", build->refresh_context, kRefreshContext, sizeof(kRefreshContext)};
    out[2] = (SotbPatchSite){"MenuInfoBar::AddButton", build->addbutton_site, kAddButtonPrologue, sizeof(kAddButtonPrologue)};
    out[3] = (SotbPatchSite){"FText::CreateText", build->createtext_site, kCreateTextPrologue, sizeof(kCreateTextPrologue)};
    return 4;
}

int sotb_exit_game_apply(uint8_t *image, size_t image_size, const char *build_key, uint8_t *cave, size_t cave_size,
                         char *error, size_t error_size) {
    const ExitGameBuild *build = find_exit_build(build_key);
    if (!build) return fail(error, error_size, "not a supported build");
    if (!image || !cave) return fail(error, error_size, "missing image or cave");
    if (cave_size < SOTB_EXIT_GAME_MIN_CAVE_SIZE) return fail(error, error_size, "cave too small");
    if (!build_matches(build, image, image_size, error, error_size)) return 0;

    uint8_t *tick_stub = cave;
    uint8_t *setup_stub = cave + 128;
    uint8_t *refresh_stub = cave + 256;
    const intptr_t tick_rel = tick_stub - (image + build->tick_site + 5);
    const intptr_t setup_rel = setup_stub - (image + build->setup_site + 5);
    const intptr_t refresh_rel = refresh_stub - (image + build->refresh_site + 5);
    if (!rel32_fits(tick_rel) || !rel32_fits(setup_rel) || !rel32_fits(refresh_rel))
        return fail(error, error_size, "cave out of 32-bit call range");

    g_image_base = image;
    g_active_build = build;
    g_exit_ftext_ready = 0;
    emit_tick_stub(tick_stub, image + build->get_top_screen_target);
    emit_setup_stub(setup_stub);
    emit_refresh_stub(refresh_stub);

    const int32_t tick_disp = (int32_t)tick_rel;
    image[build->tick_site] = 0xe8;
    memcpy(image + build->tick_site + 1, &tick_disp, sizeof(tick_disp));

    const int32_t setup_disp = (int32_t)setup_rel;
    image[build->setup_site] = 0xe8;
    memcpy(image + build->setup_site + 1, &setup_disp, sizeof(setup_disp));
    image[build->setup_site + 5] = 0x90;

    const int32_t refresh_disp = (int32_t)refresh_rel;
    image[build->refresh_site] = 0xe9;
    memcpy(image + build->refresh_site + 1, &refresh_disp, sizeof(refresh_disp));
    memset(image + build->refresh_site + 5, 0x90, 11);

    return 1;
}

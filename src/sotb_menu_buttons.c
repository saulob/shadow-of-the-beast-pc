#include "sotb_menu_buttons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sotb_keys.h"

/* CUSA03762-1.00 (KR_BD): region branch falls through to Circle-confirm keys; Western forces the Cross-confirm branch. */
#define MENU_BUTTONS_CONTEXT 0x1659b0
#define MENU_BUTTONS_SITE 0x1659c4
static const uint8_t kContext[] = {
    0xe8, 0x9b, 0x2d, 0x23, 0x00, 0x04, 0xfb, 0x0f, 0xb6, 0xc0, 0x48, 0x8d, 0x35,
    0xc4, 0x71, 0xf7, 0x02, 0x83, 0xf8, 0x03, 0x0f, 0x87, 0x1e, 0x01, 0x00, 0x00,
};
static const uint8_t kOriginalFirstKey[] = {0x48, 0x8d, 0x0d, 0x76, 0x64, 0x03, 0x04};
static const uint8_t kOriginalSecondKey[] = {0x48, 0x8d, 0x0d, 0xae, 0x63, 0x03, 0x04};
static const uint8_t kWesternFirstKey[] = {0x48, 0x8d, 0x0d, 0x3d, 0x63, 0x03, 0x04};
static const uint8_t kWesternSecondKey[] = {0x48, 0x8d, 0x0d, 0x8d, 0x62, 0x03, 0x04};
enum {
    kContextLength = sizeof(kContext) / sizeof(*kContext),
    kOriginalFirstKeyOffset = 0x1f,
    kOriginalSecondKeyOffset = 0xcf,
    kWesternFirstKeyOffset = 0x140,
    kWesternSecondKeyOffset = 0x208,
    kPatchLength = 6,
};
static const uint8_t kWesternBranch[kPatchLength] = {0xe9, 0x1f, 0x01, 0x00, 0x00, 0x90};

static int ascii_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static SotbMenuButtonLayout layout_by_name(const char *value, SotbMenuButtonLayout fallback) {
    if (!value || !*value) return fallback;
    if (ascii_eq_ci(value, "western")) return SOTB_MENU_BUTTON_LAYOUT_WESTERN;
    if (ascii_eq_ci(value, "original")) return SOTB_MENU_BUTTON_LAYOUT_ORIGINAL;
    return fallback;
}

static void read_setting(const char *key, const char *value, void *user) {
    if (!strcmp(key, SOTB_MENU_BUTTON_LAYOUT_KEY))
        *(SotbMenuButtonLayout *)user = layout_by_name(value, SOTB_MENU_BUTTON_LAYOUT_WESTERN);
}

SotbMenuButtonLayout sotb_menu_button_layout(const char *settings_file) {
    SotbMenuButtonLayout saved = SOTB_MENU_BUTTON_LAYOUT_WESTERN;
    sotb_ini_read(settings_file, read_setting, &saved);
    return layout_by_name(getenv(SOTB_MENU_BUTTON_LAYOUT_ENV), saved);
}

static const char *layout_label(SotbMenuButtonLayout layout) {
    return layout == SOTB_MENU_BUTTON_LAYOUT_ORIGINAL ? "Original" : "Western";
}

static int32_t load_rel32(const uint8_t *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static uint64_t lea_target(const uint8_t *image, size_t size, size_t at) {
    if (at > size || size - at < 7 || image[at] != 0x48 || image[at + 1] != 0x8d || image[at + 2] != 0x0d) return 0;
    return (uint64_t)((int64_t)at + 7 + load_rel32(image + at + 3));
}

static int site_count(const uint8_t *image, size_t size) {
    int count = 0;
    if (size < kContextLength) return 0;
    for (size_t at = 0; at + kContextLength <= size; ++at) {
        if (image[at] != kContext[0] || memcmp(image + at, kContext, kContextLength)) continue;
        if (++count > 1) return count;
    }
    return count;
}

static int unavailable(const char *build, SotbMenuButtonLayout layout, const char *reason) {
    printf("Menu button layout: %s requested but unavailable for %s: %s\n", layout_label(layout),
           build ? build : "an unknown build", reason);
    return 0;
}

int sotb_menu_buttons_apply(uint8_t *image, size_t image_size, const char *build_key, SotbMenuButtonLayout layout) {
    if (!build_key || strcmp(build_key, "CUSA03762-1.00")) return unavailable(build_key, layout, "not a supported build");
    if (!image || image_size < MENU_BUTTONS_SITE + kWesternSecondKeyOffset + sizeof(kWesternSecondKey))
        return unavailable(build_key, layout, "the image is too short");
    if (memcmp(image + MENU_BUTTONS_CONTEXT, kContext, kContextLength))
        return unavailable(build_key, layout, "the menu layout code does not match");
    if (site_count(image, image_size) != 1) return unavailable(build_key, layout, "the menu layout code is not unique");

    if (memcmp(image + MENU_BUTTONS_SITE + kOriginalFirstKeyOffset, kOriginalFirstKey, sizeof(kOriginalFirstKey)) ||
        memcmp(image + MENU_BUTTONS_SITE + kOriginalSecondKeyOffset, kOriginalSecondKey, sizeof(kOriginalSecondKey)) ||
        memcmp(image + MENU_BUTTONS_SITE + kWesternFirstKeyOffset, kWesternFirstKey, sizeof(kWesternFirstKey)) ||
        memcmp(image + MENU_BUTTONS_SITE + kWesternSecondKeyOffset, kWesternSecondKey, sizeof(kWesternSecondKey)))
        return unavailable(build_key, layout, "the menu key sites do not match");

    const uint64_t original_first = lea_target(image, image_size, MENU_BUTTONS_SITE + kOriginalFirstKeyOffset);
    const uint64_t original_second = lea_target(image, image_size, MENU_BUTTONS_SITE + kOriginalSecondKeyOffset);
    const uint64_t western_first = lea_target(image, image_size, MENU_BUTTONS_SITE + kWesternFirstKeyOffset);
    const uint64_t western_second = lea_target(image, image_size, MENU_BUTTONS_SITE + kWesternSecondKeyOffset);
    if (!original_first || !original_second || original_first != western_second || original_second != western_first)
        return unavailable(build_key, layout, "the menu key sites do not match");

    if (layout == SOTB_MENU_BUTTON_LAYOUT_ORIGINAL) {
        puts("Menu button layout: Original, Circle/B confirms and Cross/A cancels");
        return 1;
    }
    memcpy(image + MENU_BUTTONS_SITE, kWesternBranch, sizeof(kWesternBranch));
    puts("Menu button layout: Western, Cross/A confirms and Circle/B cancels");
    return 1;
}

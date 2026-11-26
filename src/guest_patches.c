#define _GNU_SOURCE
#include "guest_patches.h"
#include <stdio.h>
#include <string.h>

#define SCREEN_MAP_LIVE_FEED 0x0e
#define SCREEN_SOCIAL_FEATURES_TOGGLE 0x25
#define MAX_PATCH 8

#define NAVIGATION_REMOVE_OFFSET 0x268b80
#define WIDGET_SET_VISIBILITY_OFFSET 0xc345a0
#define OPTIONS_LIVE_FEED_BUTTON 0x2e8
#define VISIBILITY_COLLAPSED 1

typedef struct {
    const char *name;
    uint32_t offset;
    uint8_t length;
    uint8_t expected[MAX_PATCH];
    uint8_t replacement[MAX_PATCH];
} BytePatch;

/* Save-game defaults and the two first-run triggers of the Social Features popup. */
static const BytePatch byte_patches[] = {
    {"save default: social off", 0x3408c4, 7, {0xc6, 0x87, 0xfd, 0, 0, 0, 1}, {0xc6, 0x87, 0xfd, 0, 0, 0, 0}},
    {"save default: popup done", 0x3408cb, 7, {0xc6, 0x87, 0xfe, 0, 0, 0, 0}, {0xc6, 0x87, 0xfe, 0, 0, 0, 1}},
    {"save reset: social off", 0x340f1c, 8, {0x41, 0xc6, 0x87, 0xfd, 0, 0, 0, 1}, {0x41, 0xc6, 0x87, 0xfd, 0, 0, 0, 0}},
    {"save reset: popup done", 0x340f24, 8, {0x41, 0xc6, 0x87, 0xfe, 0, 0, 0, 0}, {0x41, 0xc6, 0x87, 0xfe, 0, 0, 0, 1}},
    {"map: no social popup", 0x1a32fd, 2, {0x75, 0x26}, {0xeb, 0x26}},
    {"save flow: no social popup", 0x1c9f59, 6, {0x0f, 0x85, 0xd3, 0, 0, 0}, {0x90, 0xe9, 0xd3, 0, 0, 0}},
    {"login does not enable social", 0x28b810, 4, {0x48, 0x8b, 0x47, 0x70}, {0xc3, 0x8b, 0x47, 0x70}},
    {"popup does not write social", 0x1a46ba, 6, {0x88, 0x81, 0xfd, 0, 0, 0}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {"options do not write social", 0x1eede5, 6, {0x88, 0x83, 0xfd, 0, 0, 0}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
};

static const uint32_t SERIALIZE_SITES[] = {0x346ba3, 0x346bdb};
#define SERIALIZE_SITE SERIALIZE_SITES[0]
static const uint8_t SERIALIZE_EXPECTED[] = {0x41, 0x80, 0x7e, 0x28, 0x00};
static const uint32_t SHOW_SCREEN_SITE = 0x1993b0;
static const uint8_t SHOW_SCREEN_EXPECTED[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
static const uint32_t OPTIONS_SITE = 0x1ed540;
static const uint8_t OPTIONS_EXPECTED[] = {0xe8, 0xfb, 0x2f, 0x00, 0x00};

static uint8_t *image_base;
static GuestOptionsHook options_hook;

static int matches(const uint8_t *at, const uint8_t *expected, size_t length) { return !memcmp(at, expected, length); }

static int32_t displacement(const uint8_t *from_end, const uint8_t *target) { return (int32_t)(target - from_end); }

static int reachable(const uint8_t *image, size_t image_size, const uint8_t *cave, size_t cave_size) {
    const intptr_t after = (cave + cave_size) - image, before = (image + image_size) - cave;
    return after < 0x7ff00000 && before < 0x7ff00000 && after > -0x7ff00000 && before > -0x7ff00000;
}

static void put_jump(uint8_t *at, uint8_t opcode, const uint8_t *target) {
    const int32_t rel = displacement(at + 5, target);
    at[0] = opcode;
    memcpy(at + 1, &rel, 4);
}

/* Normalizes Social Features whenever a save object is read or written; the original compare and its flags are kept. */
static size_t emit_serialize_cave(uint8_t *cave) {
    static const uint8_t code[] = {
        0xc6, 0x83, 0xfd, 0, 0, 0, 0x00,
        0xc6, 0x83, 0xfe, 0, 0, 0, 0x01,
        0x41, 0x80, 0x7e, 0x28, 0x00,
        0xc3,
    };
    memcpy(cave, code, sizeof(code));
    return sizeof(code);
}

/* Entry of the show-screen function: the social screens are refused, every other screen runs the original code. */
static size_t emit_show_screen_cave(uint8_t *cave, const uint8_t *original_after_prologue) {
    uint8_t *p = cave;
    uint8_t *skip_toggle, *skip_feed;
    *p++ = 0x83; *p++ = 0xfe; *p++ = SCREEN_SOCIAL_FEATURES_TOGGLE;
    *p++ = 0x74; skip_toggle = p++;
    *p++ = 0x83; *p++ = 0xfe; *p++ = SCREEN_MAP_LIVE_FEED;
    *p++ = 0x74; skip_feed = p++;
    static const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    memcpy(p, prologue, sizeof(prologue));
    p += sizeof(prologue);
    put_jump(p, 0xe9, original_after_prologue);
    p += 5;
    uint8_t *refuse = p;
    *p++ = 0x31; *p++ = 0xc0;
    *p++ = 0xc3;
    *skip_toggle = (uint8_t)(refuse - (skip_toggle + 1));
    *skip_feed = (uint8_t)(refuse - (skip_feed + 1));
    return (size_t)(p - cave);
}

/* Replaces the call that ends the Options screen setup: hook(controller), then the original callee. */
static size_t emit_options_cave(uint8_t *cave, const uint8_t *original_callee, GuestOptionsHook hook) {
    uint8_t *p = cave;
    *p++ = 0x57;
    *p++ = 0x48; *p++ = 0xb8;
    const uint64_t address = (uint64_t)(uintptr_t)hook;
    memcpy(p, &address, 8);
    p += 8;
    *p++ = 0xff; *p++ = 0xd0;
    *p++ = 0x5f;
    put_jump(p, 0xe9, original_callee);
    p += 5;
    return (size_t)(p - cave);
}

static void remove_from_navigation(void *controller, void *button) {
    ((GUEST_ABI void (*)(void *, void *))(image_base + NAVIGATION_REMOVE_OFFSET))(controller, button);
}

static void collapse_widget(void *widget) {
    ((GUEST_ABI void (*)(void *, int))(image_base + WIDGET_SET_VISIBILITY_OFFSET))(widget, VISIBILITY_COLLAPSED);
}

int guest_options_hide_social(void *controller, const GuestUiCalls *calls) {
    void *button = *(void **)((uint8_t *)controller + OPTIONS_LIVE_FEED_BUTTON);
    if (!button) return 0;
    calls->remove_from_navigation(controller, button);
    calls->collapse(button);
    return 1;
}

static GUEST_ABI void options_hide_social_hook(void *controller) {
    const GuestUiCalls calls = {
        .remove_from_navigation = remove_from_navigation,
        .collapse = collapse_widget,
    };
    guest_options_hide_social(controller, &calls);
}

void guest_patches_set_options_hook(GuestOptionsHook hook) { options_hook = hook; }

static int count(int ok, GuestPatchReport *report) {
    if (ok) ++report->applied;
    else ++report->skipped;
    return ok;
}

static int byte_patch_matches(const uint8_t *image, size_t image_size, const BytePatch *patch) {
    return patch->offset + patch->length <= image_size && matches(image + patch->offset, patch->expected, patch->length);
}

static int hook_sites_fit(size_t image_size) {
    return image_size > OPTIONS_SITE + 8 && image_size > SHOW_SCREEN_SITE + 8 && image_size > SERIALIZE_SITES[1] + 8;
}

GuestPatchReport guest_patches_check(const uint8_t *image, size_t image_size) {
    GuestPatchReport report = {0, 0};
    for (size_t i = 0; i < sizeof(byte_patches) / sizeof(*byte_patches); ++i) {
        count(image && byte_patch_matches(image, image_size, &byte_patches[i]), &report);
    }
    const int fits = image && hook_sites_fit(image_size);
    for (size_t i = 0; i < sizeof(SERIALIZE_SITES) / sizeof(*SERIALIZE_SITES); ++i) {
        count(fits && matches(image + SERIALIZE_SITES[i], SERIALIZE_EXPECTED, sizeof(SERIALIZE_EXPECTED)), &report);
    }
    count(fits && matches(image + SHOW_SCREEN_SITE, SHOW_SCREEN_EXPECTED, sizeof(SHOW_SCREEN_EXPECTED)), &report);
    count(fits && matches(image + OPTIONS_SITE, OPTIONS_EXPECTED, sizeof(OPTIONS_EXPECTED)), &report);
    return report;
}

GuestPatchReport guest_patches_apply(uint8_t *image, size_t image_size, uint8_t *cave, size_t cave_size) {
    GuestPatchReport report = {0, 0};
    image_base = image;
    uint8_t *next_cave = cave;
    if (!options_hook) options_hook = options_hide_social_hook;

    for (size_t i = 0; i < sizeof(byte_patches) / sizeof(*byte_patches); ++i) {
        const BytePatch *patch = &byte_patches[i];
        if (count(byte_patch_matches(image, image_size, patch), &report)) memcpy(image + patch->offset, patch->replacement, patch->length);
    }

    const int fits = hook_sites_fit(image_size);
    if (!fits || cave_size < 128 || !reachable(image, image_size, cave, cave_size)) {
        report.skipped += 4;
        return report;
    }

    uint8_t *site;
    uint8_t *serialize_cave = NULL;
    for (size_t i = 0; i < sizeof(SERIALIZE_SITES) / sizeof(*SERIALIZE_SITES); ++i) {
        site = image + SERIALIZE_SITES[i];
        if (!count(matches(site, SERIALIZE_EXPECTED, sizeof(SERIALIZE_EXPECTED)), &report)) continue;
        if (!serialize_cave) {
            serialize_cave = next_cave;
            next_cave += (emit_serialize_cave(next_cave) + 15) & ~(size_t)15;
        }
        put_jump(site, 0xe8, serialize_cave);
    }

    site = image + SHOW_SCREEN_SITE;
    if (count(matches(site, SHOW_SCREEN_EXPECTED, sizeof(SHOW_SCREEN_EXPECTED)), &report)) {
        const size_t used = emit_show_screen_cave(next_cave, site + sizeof(SHOW_SCREEN_EXPECTED));
        put_jump(site, 0xe9, next_cave);
        site[5] = 0x90;
        next_cave += (used + 15) & ~(size_t)15;
    }

    site = image + OPTIONS_SITE;
    if (count(matches(site, OPTIONS_EXPECTED, sizeof(OPTIONS_EXPECTED)), &report)) {
        int32_t original;
        memcpy(&original, site + 1, 4);
        emit_options_cave(next_cave, site + 5 + original, options_hook);
        put_jump(site, 0xe8, next_cave);
    }
    return report;
}

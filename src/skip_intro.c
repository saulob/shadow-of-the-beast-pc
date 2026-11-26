#include "skip_intro.h"
#include <stdio.h>
#include <string.h>

#define BOOT_TIMER_SITE 0x28d5c21u
#define BOOT_THROBBER_SITE 0x28d5c2cu
#define BOOT_FADE_SITE 0x28d5c37u
#define BOOT_FINISHED 0x40e00000u /* 7.0 s, past the 6.75 s sequence */
#define BOOT_VISIBLE 0x3f800000u  /* 1.0 */

_Static_assert(BOOT_TIMER_SITE == SKIP_INTRO_SITE, "skip intro site");
_Static_assert(BOOT_THROBBER_SITE == BOOT_TIMER_SITE + 0xb, "throbber timer site");
_Static_assert(BOOT_FADE_SITE == BOOT_TIMER_SITE + 0x16, "throbber fade site");

/* mov dword [r15+0x274], 0; mov dword [r15+0x258], 0; mov dword [r15+0x270], 0 */
static const uint8_t expected[SKIP_INTRO_PATCH_SIZE] = {
    0x41, 0xc7, 0x87, 0x74, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x41, 0xc7, 0x87, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x41, 0xc7, 0x87, 0x70, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static int fail(char *error, size_t size, const char *reason) {
    if (error && size) snprintf(error, size, "%s", reason);
    return 0;
}

static void put32(uint8_t *at, uint32_t value) { memcpy(at, &value, sizeof(value)); }

static const SotbPatchSite sites[] = {{"boot screen sequence", BOOT_TIMER_SITE, expected, sizeof(expected)}};

size_t skip_intro_sites(const SotbPatchSite **out) {
    *out = sites;
    return sizeof(sites) / sizeof(*sites);
}

int skip_intro_check(const uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if (!image || image_size < BOOT_TIMER_SITE + SKIP_INTRO_PATCH_SIZE) {
        return fail(error, error_size, "image too short");
    }
    if (memcmp(image + BOOT_TIMER_SITE, expected, sizeof(expected))) {
        return fail(error, error_size, "boot screen sequence does not match a supported build");
    }
    return 1;
}

int skip_intro_apply(uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if (!skip_intro_check(image, image_size, error, error_size)) return 0;
    uint8_t *site = image + BOOT_TIMER_SITE;

    uint8_t patched[sizeof(expected)];
    memcpy(patched, expected, sizeof(patched));
    put32(patched + 7, BOOT_FINISHED);
    put32(patched + (BOOT_THROBBER_SITE - BOOT_TIMER_SITE) + 7, BOOT_VISIBLE);
    put32(patched + (BOOT_FADE_SITE - BOOT_TIMER_SITE) + 7, BOOT_VISIBLE);
    memcpy(site, patched, sizeof(patched));
    return 1;
}

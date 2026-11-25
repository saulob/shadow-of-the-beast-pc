#include "psn_notice.h"
#include <stdio.h>
#include <string.h>

#define HOOK_RESUME (PSN_NOTICE_HOOK_SITE + PSN_NOTICE_HOOK_PATCH_SIZE)

typedef struct {
    uint32_t namespace_lea;
    uint32_t key_lea;
    const char16_t *ns;
    size_t ns_bytes;
} NoticeSite;

static const char16_t sign_in_popup[] = u"SignInPopup";
static const char16_t main_menu_screen[] = u"MainMenuScreen";
static const char16_t connection_success[] = u"ConnectionSuccess";
static const char16_t blank_key_text[] = u"ConnectionSuccessBlank";
static const char16_t blank_source_text[] = u" ";

static const NoticeSite sites[] = {
    {0x1a34ba, 0x1a34c1, sign_in_popup, sizeof(sign_in_popup)},
    {0x1caa8c, 0x1caa93, main_menu_screen, sizeof(main_menu_screen)},
};

static PsnNoticeString blank_key = {blank_key_text, sizeof(blank_key_text) / 2, sizeof(blank_key_text) / 2};
static PsnNoticeString blank_source = {blank_source_text, sizeof(blank_source_text) / 2, sizeof(blank_source_text) / 2};

/* FTextLocalizationManager::GetDisplayString(out, manager, namespace, key, source): push rbp; mov rbp,rsp; push r15;
 * ... A found key returns its locres entry without comparing the source; a missing key returns the source. */
static const uint8_t hook_expected[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xe4, 0xe0, 0xff,
    0xff, 0xff, 0x48, 0x81, 0xec, 0x40, 0x01, 0x00, 0x00, 0x4c, 0x89, 0xc3, 0x49, 0x89, 0xcd, 0x49, 0x89, 0xd7,
    0x49, 0x89, 0xf4, 0x48, 0x89, 0x7c, 0x24, 0x48, 0x48, 0x8b, 0x05, 0x3d, 0x52, 0x8f, 0x03, 0x48, 0x8b, 0x00,
    0x48, 0x89, 0x84, 0x24, 0x20, 0x01, 0x00, 0x00, 0x4d, 0x8d, 0x74, 0x24, 0x08, 0x4c, 0x89, 0xf7,
};

static int fail(char *error, size_t size, const char *reason) {
    if (error && size) snprintf(error, size, "%s", reason);
    return 0;
}

static int string_is(const PsnNoticeString *s, const char16_t *text, size_t bytes) {
    return s && s->data && (size_t)s->num * 2 == bytes && !memcmp(s->data, text, bytes);
}

int GUEST_ABI psn_notice_matches(const PsnNoticeString *ns, const PsnNoticeString *key) {
    if (!string_is(key, connection_success, sizeof(connection_success))) return 0;
    for (size_t i = 0; i < sizeof(sites) / sizeof(*sites); ++i) {
        if (string_is(ns, sites[i].ns, sites[i].ns_bytes)) return 1;
    }
    return 0;
}

static const uint8_t *lea_target(const uint8_t *image, size_t image_size, uint32_t at, uint8_t modrm) {
    if (at + 7 > image_size || image[at] != 0x48 || image[at + 1] != 0x8d || image[at + 2] != modrm) return NULL;
    int32_t rel;
    memcpy(&rel, image + at + 3, sizeof(rel));
    const int64_t target = (int64_t)at + 7 + rel;
    return target >= 0 && (uint64_t)target < image_size ? image + target : NULL;
}

static int site_matches(const uint8_t *image, size_t image_size, const NoticeSite *site) {
    const uint8_t *ns = lea_target(image, image_size, site->namespace_lea, 0x15);
    const uint8_t *key = lea_target(image, image_size, site->key_lea, 0x0d);
    return ns && key && (size_t)(ns - image) + site->ns_bytes <= image_size &&
           (size_t)(key - image) + sizeof(connection_success) <= image_size &&
           !memcmp(ns, site->ns, site->ns_bytes) && !memcmp(key, connection_success, sizeof(connection_success));
}

static void put64(uint8_t **at, uint64_t value) {
    memcpy(*at, &value, sizeof(value));
    *at += sizeof(value);
}

static void put_bytes(uint8_t **at, const void *bytes, size_t size) {
    memcpy(*at, bytes, size);
    *at += size;
}

static void emit_stub(uint8_t *cave, uint8_t *resume) {
    static const uint8_t save[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x48, 0x89, 0xd7, 0x48, 0x89, 0xce, 0x48, 0xb8};
    static const uint8_t test_and_restore[] = {0xff, 0xd0, 0x85, 0xc0, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f, 0x74, 0x14};
    static const uint8_t blank[] = {0x48, 0xb9};
    static const uint8_t blank_source_load[] = {0x49, 0xb8};
    static const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x49, 0xba};
    static const uint8_t jump[] = {0x41, 0xff, 0xe2};
    uint8_t *p = cave;
    put_bytes(&p, save, sizeof(save));
    put64(&p, (uint64_t)(uintptr_t)psn_notice_matches);
    put_bytes(&p, test_and_restore, sizeof(test_and_restore));
    put_bytes(&p, blank, sizeof(blank));
    put64(&p, (uint64_t)(uintptr_t)&blank_key);
    put_bytes(&p, blank_source_load, sizeof(blank_source_load));
    put64(&p, (uint64_t)(uintptr_t)&blank_source);
    put_bytes(&p, prologue, sizeof(prologue));
    put64(&p, (uint64_t)(uintptr_t)resume);
    put_bytes(&p, jump, sizeof(jump));
}

int psn_notice_apply(uint8_t *image, size_t image_size, uint8_t *cave, size_t cave_size, char *error,
                     size_t error_size) {
    if (!image || !cave) return fail(error, error_size, "missing image or cave");
    if (cave_size < PSN_NOTICE_MIN_CAVE_SIZE) return fail(error, error_size, "cave too small");
    if (image_size < HOOK_RESUME) return fail(error, error_size, "image too short");
    if (memcmp(image + PSN_NOTICE_HOOK_SITE, hook_expected, sizeof(hook_expected))) {
        return fail(error, error_size, "text lookup does not match a supported build");
    }
    for (size_t i = 0; i < sizeof(sites) / sizeof(*sites); ++i) {
        if (!site_matches(image, image_size, &sites[i])) {
            return fail(error, error_size, "connection success text does not match a supported build");
        }
    }
    const intptr_t rel = cave - (image + PSN_NOTICE_HOOK_SITE + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return fail(error, error_size, "cave out of 32-bit jump range");

    emit_stub(cave, image + HOOK_RESUME);
    const int32_t disp = (int32_t)rel;
    image[PSN_NOTICE_HOOK_SITE] = 0xe9;
    memcpy(image + PSN_NOTICE_HOOK_SITE + 1, &disp, sizeof(disp));
    image[PSN_NOTICE_HOOK_SITE + 5] = 0x90;
    return 1;
}

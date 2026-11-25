#include "../src/psn_notice.c"
#include "../src/skip_intro.h"
#include <stdlib.h>
#include <windows.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define IMAGE_SIZE 0x3c00000u
#define CAVE_SIZE 0x1000u
#define HOOK_BYTES 6
#define OLD_FLOW_SITE 0x1a34ac
#define CASES_OFFSET 0x1a3cbc
#define NS_SIGN_IN_AT 0x30eda3a
#define NS_MAIN_MENU_AT 0x30f29c2
#define KEY_A_AT 0x30edad6
#define KEY_B_AT 0x30f2afa

static const char *const builds[] = {"CUSA03762-1.00", "CUSA02871-1.00", "CUSA00951-1.00"};
static const uint32_t lea_ns[] = {0x1a34ba, 0x1caa8c};
static const uint32_t lea_key[] = {0x1a34c1, 0x1caa93};
static const size_t untouched[] = {
    0x1a34ac, 0x1a34b3, 0x1a370d, 0x1a37e0, 0x1a38bb, 0x1a336f, 0x1a34d2, 0x1caa85, 0x1caaa4, 0x30ed9c8, 0x30eda78,
    0x30f2a9c, 0x30eabd0, 0x30eac44, 0x30eacd8, 0x30ead50, 0x30ead9a, 0x30eae1e, 0x30eae64, 0x316b70a, 0x317a4cc,
    0x30edb4e, 0x30edbc2, 0x30f2b72, 0x30f2a1c, 0x1dcce90, 0x585e70, 0x5861e6,
};
static uint8_t *block;
static uint8_t *cave;

static uint8_t *fresh(size_t size) {
    if (block) VirtualFree(block, 0, MEM_RELEASE);
    block = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    return block;
}

static uint64_t read64(const uint8_t *data, size_t offset) {
    uint64_t value;
    memcpy(&value, data + offset, 8);
    return value;
}

static char *read_file(const char *path, long *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    *length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = malloc((size_t)*length + 1);
    if (!data || fread(data, 1, (size_t)*length, file) != (size_t)*length) {
        fclose(file);
        free(data);
        return NULL;
    }
    fclose(file);
    data[*length] = 0;
    return data;
}

static uint8_t *load_boot_image(const char *build, size_t *size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/boot.bin", SOTB_IMAGE_ROOT, build);
    long length = 0;
    uint8_t *data = (uint8_t *)read_file(path, &length);
    if (!data) return NULL;
    if (length < 8 || memcmp(data, "SOTBBOT1", 8)) {
        free(data);
        return NULL;
    }
    size_t offset = 8;
    const uint64_t image_size = read64(data, offset), segments = read64(data, offset + 16),
                   relocations = read64(data, offset + 24), imports = read64(data, offset + 32);
    offset += 6 * 8 + 8 + 4 * 8;
    offset += 8 + read64(data, offset) * 56;
    offset += 8 + read64(data, offset) * 24;
    offset += segments * 24 + imports * 128 + relocations * 32;
    uint8_t *image = malloc((size_t)image_size);
    memcpy(image, data + offset, image_size);
    free(data);
    *size = (size_t)image_size;
    return image;
}

static void synthetic_image(uint8_t *image) {
    memset(image, 0x11, IMAGE_SIZE);
    memcpy(image + PSN_NOTICE_HOOK_SITE, hook_expected, sizeof(hook_expected));
    memcpy(image + NS_SIGN_IN_AT, sign_in_popup, sizeof(sign_in_popup));
    memcpy(image + NS_MAIN_MENU_AT, main_menu_screen, sizeof(main_menu_screen));
    memcpy(image + KEY_A_AT, connection_success, sizeof(connection_success));
    memcpy(image + KEY_B_AT, connection_success, sizeof(connection_success));
    const uint32_t ns_at[] = {NS_SIGN_IN_AT, NS_MAIN_MENU_AT}, key_at[] = {KEY_A_AT, KEY_B_AT};
    for (size_t i = 0; i < 2; ++i) {
        const uint8_t ns_lea[3] = {0x48, 0x8d, 0x15}, key_lea[3] = {0x48, 0x8d, 0x0d};
        int32_t rel = (int32_t)(ns_at[i] - (lea_ns[i] + 7));
        memcpy(image + lea_ns[i], ns_lea, 3);
        memcpy(image + lea_ns[i] + 3, &rel, 4);
        rel = (int32_t)(key_at[i] - (lea_key[i] + 7));
        memcpy(image + lea_key[i], key_lea, 3);
        memcpy(image + lea_key[i] + 3, &rel, 4);
    }
}

static uint8_t *fresh_image(void) {
    uint8_t *image = fresh(IMAGE_SIZE + CAVE_SIZE);
    cave = image + IMAGE_SIZE;
    memset(cave, 0xcc, CAVE_SIZE);
    synthetic_image(image);
    return image;
}

static int identical(const uint8_t *a, const uint8_t *b, size_t size) { return !memcmp(a, b, size); }

static size_t count_changed(const uint8_t *a, const uint8_t *b, size_t size) {
    size_t changed = 0;
    for (size_t i = 0; i < size; ++i) changed += a[i] != b[i];
    return changed;
}

static PsnNoticeString make_string(const char16_t *text, size_t bytes) {
    PsnNoticeString s = {text, (int32_t)(bytes / 2), (int32_t)(bytes / 2)};
    return s;
}

#define STRING_OF(array) make_string(array, sizeof(array))

static void test_matcher(void) {
    static const char16_t no_connection[] = u"ConnectionNoConnection";
    static const char16_t progress[] = u"ConnectionProgress";
    static const char16_t age[] = u"ConnectionAgeRestriction";
    static const char16_t toast[] = u"NetworkErrorToast";
    static const char16_t exit_game[] = u"ExitGame";
    static const char16_t shorter[] = u"ConnectionSucces";
    static const char16_t longer[] = u"ConnectionSuccessBlank";
    static const char16_t lower[] = u"connectionsuccess";
    static const char16_t other_ns[] = u"SignInPopu";
    const PsnNoticeString sign = STRING_OF(sign_in_popup), menu = STRING_OF(main_menu_screen),
                          key = STRING_OF(connection_success);
    CHECK(psn_notice_matches(&sign, &key));
    CHECK(psn_notice_matches(&menu, &key));
    const char16_t *const wrong_keys[] = {no_connection, progress, age, toast, exit_game, shorter, longer, lower};
    const size_t wrong_bytes[] = {sizeof(no_connection), sizeof(progress), sizeof(age), sizeof(toast),
                                  sizeof(exit_game), sizeof(shorter), sizeof(longer), sizeof(lower)};
    for (size_t i = 0; i < 8; ++i) {
        const PsnNoticeString wrong = make_string(wrong_keys[i], wrong_bytes[i]);
        CHECK(!psn_notice_matches(&sign, &wrong));
        CHECK(!psn_notice_matches(&menu, &wrong));
    }
    const PsnNoticeString wrong_ns[] = {STRING_OF(exit_game), STRING_OF(other_ns), STRING_OF(connection_success)};
    for (size_t i = 0; i < 3; ++i) CHECK(!psn_notice_matches(&wrong_ns[i], &key));
    PsnNoticeString empty = {NULL, 0, 0}, no_terminator = sign;
    no_terminator.num -= 1;
    CHECK(!psn_notice_matches(&empty, &key) && !psn_notice_matches(&sign, &empty));
    CHECK(!psn_notice_matches(&no_terminator, &key));
    CHECK(!psn_notice_matches(NULL, &key) && !psn_notice_matches(&sign, NULL) && !psn_notice_matches(NULL, NULL));
}

static void test_blank_text(void) {
    CHECK(blank_source.num == 2 && blank_source.data[0] == 0x20 && blank_source.data[1] == 0);
    CHECK(blank_key.num == (int32_t)(sizeof(blank_key_text) / 2) && blank_key.data[blank_key.num - 1] == 0);
    const PsnNoticeString sign = STRING_OF(sign_in_popup), menu = STRING_OF(main_menu_screen);
    CHECK(!psn_notice_matches(&sign, &blank_key) && !psn_notice_matches(&menu, &blank_key));
}

typedef void(GUEST_ABI *LookupFn)(void *, void *, const PsnNoticeString *, const PsnNoticeString *,
                                  const PsnNoticeString *);

static uint64_t recorded[4];

static void install_probe(uint8_t *image) {
    uint8_t *p = image + PSN_NOTICE_HOOK_SITE + HOOK_BYTES;
    static const uint8_t first[] = {0x48, 0xb8};
    static const uint8_t rest[] = {0x48, 0x89, 0x10, 0x48, 0x89, 0x48, 0x08, 0x4c, 0x89, 0x40, 0x10,
                                   0x48, 0x89, 0x70, 0x18, 0x41, 0x5f, 0x5d, 0xc3};
    const uint64_t where = (uint64_t)(uintptr_t)recorded;
    memcpy(p, first, sizeof(first));
    memcpy(p + 2, &where, 8);
    memcpy(p + 10, rest, sizeof(rest));
}

static void lookup(uint8_t *image, const PsnNoticeString *ns, const PsnNoticeString *key, const PsnNoticeString *src) {
    memset(recorded, 0, sizeof(recorded));
    ((LookupFn)(uintptr_t)(image + PSN_NOTICE_HOOK_SITE))((void *)0x1000, (void *)0x2000, ns, key, src);
}

static void test_stub_execution(void) {
    uint8_t *image = fresh_image();
    char error[160];
    CHECK(psn_notice_apply(image, IMAGE_SIZE, cave, CAVE_SIZE, error, sizeof(error)));
    install_probe(image);
    static const char16_t source_text[] = u"Successfully connected";
    static const char16_t no_connection[] = u"ConnectionNoConnection";
    const PsnNoticeString sign = STRING_OF(sign_in_popup), menu = STRING_OF(main_menu_screen),
                          key = STRING_OF(connection_success), other = STRING_OF(no_connection),
                          src = STRING_OF(source_text);
    const PsnNoticeString *const blocked[] = {&sign, &menu};
    for (size_t i = 0; i < 2; ++i) {
        lookup(image, blocked[i], &key, &src);
        CHECK(recorded[0] == (uint64_t)(uintptr_t)blocked[i]);
        CHECK(recorded[1] == (uint64_t)(uintptr_t)&blank_key);
        CHECK(recorded[2] == (uint64_t)(uintptr_t)&blank_source);
        CHECK(recorded[3] == 0x2000);
    }
    lookup(image, &sign, &other, &src);
    CHECK(recorded[0] == (uint64_t)(uintptr_t)&sign && recorded[1] == (uint64_t)(uintptr_t)&other &&
          recorded[2] == (uint64_t)(uintptr_t)&src && recorded[3] == 0x2000);
    lookup(image, &other, &key, &src);
    CHECK(recorded[1] == (uint64_t)(uintptr_t)&key && recorded[2] == (uint64_t)(uintptr_t)&src);
}

static void check_patched(const uint8_t *image, const uint8_t *original, size_t size, const uint8_t *cave_after) {
    CHECK(count_changed(image, original, size) <= HOOK_BYTES);
    CHECK(image[PSN_NOTICE_HOOK_SITE] == 0xe9 && image[PSN_NOTICE_HOOK_SITE + 5] == 0x90);
    int32_t rel;
    memcpy(&rel, image + PSN_NOTICE_HOOK_SITE + 1, 4);
    CHECK(image + PSN_NOTICE_HOOK_SITE + 5 + rel == cave_after);
    CHECK(identical(image + PSN_NOTICE_HOOK_SITE + HOOK_BYTES, original + PSN_NOTICE_HOOK_SITE + HOOK_BYTES,
                    sizeof(hook_expected) - HOOK_BYTES));
    for (size_t i = 0; i < sizeof(untouched) / sizeof(*untouched); ++i) {
        CHECK(identical(image + untouched[i], original + untouched[i], 16));
    }
}

static void test_synthetic_apply(void) {
    uint8_t *image = fresh_image();
    uint8_t *original = malloc(IMAGE_SIZE);
    memcpy(original, image, IMAGE_SIZE);
    char error[160];
    CHECK(psn_notice_apply(image, IMAGE_SIZE, cave, CAVE_SIZE, error, sizeof(error)));
    check_patched(image, original, IMAGE_SIZE, cave);
    CHECK(cave[0] == 0x57 && cave[0x48] != 0xcc && cave[0x49] == 0xcc);
    free(original);
}

static void test_failures_write_nothing(void) {
    char error[160];
    uint8_t *original = malloc(IMAGE_SIZE + CAVE_SIZE);
    size_t damage[sizeof(hook_expected) + 12];
    size_t count = 0;
    for (size_t i = 0; i < sizeof(hook_expected); ++i) damage[count++] = PSN_NOTICE_HOOK_SITE + i;
    for (size_t i = 0; i < 2; ++i) {
        damage[count++] = lea_ns[i];
        damage[count++] = lea_ns[i] + 5;
        damage[count++] = lea_key[i] + 2;
        damage[count++] = lea_key[i] + 6;
    }
    damage[count++] = NS_SIGN_IN_AT + 4;
    damage[count++] = NS_MAIN_MENU_AT + 26;
    for (size_t i = 0; i < count; ++i) {
        uint8_t *image = fresh_image();
        image[damage[i]] ^= 0x5a;
        memcpy(original, image, IMAGE_SIZE + CAVE_SIZE);
        error[0] = 0;
        CHECK(!psn_notice_apply(image, IMAGE_SIZE, cave, CAVE_SIZE, error, sizeof(error)) && error[0]);
        CHECK(identical(image, original, IMAGE_SIZE + CAVE_SIZE));
    }
    uint8_t *image = fresh_image();
    memcpy(original, image, IMAGE_SIZE + CAVE_SIZE);
    CHECK(!psn_notice_apply(image, IMAGE_SIZE, cave, PSN_NOTICE_MIN_CAVE_SIZE - 1, error, sizeof(error)));
    CHECK(!psn_notice_apply(image, KEY_B_AT + 4, cave, CAVE_SIZE, error, sizeof(error)));
    CHECK(!psn_notice_apply(image, 0x100, cave, CAVE_SIZE, error, sizeof(error)));
    CHECK(!psn_notice_apply(NULL, 0, cave, CAVE_SIZE, error, sizeof(error)));
    CHECK(!psn_notice_apply(image, IMAGE_SIZE, NULL, 0, error, sizeof(error)));
    CHECK(identical(image, original, IMAGE_SIZE + CAVE_SIZE));
    uint8_t *far_cave = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (far_cave && (uintptr_t)far_cave > (uintptr_t)image + 0x7fff0000u) {
        CHECK(!psn_notice_apply(image, IMAGE_SIZE, far_cave, 0x1000, error, sizeof(error)));
    }
    if (far_cave) VirtualFree(far_cave, 0, MEM_RELEASE);
    CHECK(psn_notice_apply(image, IMAGE_SIZE, cave, CAVE_SIZE, NULL, 0));
    free(original);
}

static void test_real_images(void) {
    uint8_t *first = NULL;
    size_t first_size = 0;
    for (size_t b = 0; b < sizeof(builds) / sizeof(*builds); ++b) {
        size_t size = 0;
        uint8_t *loaded = load_boot_image(builds[b], &size);
        if (!loaded) {
            printf("psn notice: %s boot image not available, real image checks skipped\n", builds[b]);
            continue;
        }
        uint8_t *image = fresh(size + CAVE_SIZE);
        memcpy(image, loaded, size);
        free(loaded);
        cave = image + size;
        memset(cave, 0xcc, CAVE_SIZE);
        char error[160];
        CHECK(!memcmp(image + PSN_NOTICE_HOOK_SITE, hook_expected, sizeof(hook_expected)));
        CHECK(!memcmp(image + NS_SIGN_IN_AT, sign_in_popup, sizeof(sign_in_popup)));
        CHECK(!memcmp(image + NS_MAIN_MENU_AT, main_menu_screen, sizeof(main_menu_screen)));
        CHECK(!memcmp(image + KEY_A_AT, connection_success, sizeof(connection_success)));
        CHECK(!memcmp(image + KEY_B_AT, connection_success, sizeof(connection_success)));
        int32_t rel;
        memcpy(&rel, image + CASES_OFFSET, 4);
        CHECK((size_t)(CASES_OFFSET + rel) == OLD_FLOW_SITE);
        CHECK(image[OLD_FLOW_SITE] == 0x49 && image[OLD_FLOW_SITE + 1] == 0x8d && image[OLD_FLOW_SITE + 2] == 0x9e);
        memcpy(&rel, image + 0x585fdd + 1, 4);
        CHECK(image[0x585fdd] == 0xe8 && 0x585fdd + 5 + rel == (int32_t)PSN_NOTICE_HOOK_SITE);
        uint8_t *original = malloc(size);
        memcpy(original, image, size);
        CHECK(psn_notice_apply(image, size, cave, CAVE_SIZE, error, sizeof(error)));
        check_patched(image, original, size, cave);
        if (!first) {
            first = original;
            first_size = size;
        } else {
            CHECK(size == first_size);
            CHECK(identical(original + 0x585e00, first + 0x585e00, 0x500) && identical(original + 0x1a3400, first + 0x1a3400, 0x200) && identical(original + 0x30ed9c0, first + 0x30ed9c0, 0x200) && count_changed(original, first, size) <= 16);
            free(original);
        }
    }
    free(first);
}

static void test_no_overlap(void) {
    static const size_t other_sites[] = {0x1659c4, 0x1e50f3, 0x1e5124, 0x2690ce, 0x1e5840, 0x1dcce90};
    const size_t skip_begin = SKIP_INTRO_SITE, skip_end = SKIP_INTRO_SITE + SKIP_INTRO_PATCH_SIZE;
    const size_t begin = PSN_NOTICE_HOOK_SITE, end = PSN_NOTICE_HOOK_SITE + HOOK_BYTES;
    CHECK(end <= skip_begin || skip_end <= begin);
    for (size_t j = 0; j < sizeof(other_sites) / sizeof(*other_sites); ++j) {
        CHECK(end <= other_sites[j] || other_sites[j] + 16 <= begin);
    }
    CHECK(PSN_NOTICE_HOOK_PATCH_SIZE == HOOK_BYTES);
}

static char *read_source(const char *relative) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", SOTB_SOURCE_DIR, relative);
    long length = 0;
    return read_file(path, &length);
}

static int count_of(const char *text, const char *part) {
    int count = 0;
    for (const char *at = strstr(text, part); at; at = strstr(at + 1, part)) ++count;
    return count;
}

static void test_static_structure(void) {
    char *psn = read_source("src/psn_notice.c");
    CHECK(psn != NULL);
    static const char *const forbidden[] = {"0x30eda78", "0x30f2a9c", "0x1a34ac", "0x1a3c27", "runtime_np", "sceNp",
                                            "trophy", "NetworkErrorToast", "ConnectionNoConnection"};
    for (size_t i = 0; psn && i < sizeof(forbidden) / sizeof(*forbidden); ++i) CHECK(!strstr(psn, forbidden[i]));
    free(psn);
    char *header = read_source("src/psn_notice.h");
    CHECK(header && !strstr(header, "PSN_NOTICE_TEXT") && !strstr(header, "PSN_NOTICE_SITE"));
    free(header);
    char *loader = read_source("src/loader.c");
    CHECK(loader && count_of(loader, "psn_notice_apply(") == 1);
    CHECK(loader && strstr(loader, "PlayStation Network: ConnectionSuccess text overridden with blank text"));
    CHECK(loader && strstr(loader, "PlayStation Network: ConnectionSuccess blank text unavailable: %s"));
    CHECK(loader && !strstr(loader, "text hidden") && !strstr(loader, "notification suppressed"));
    free(loader);
    char *np = read_source("src/runtime_np_local.c");
    CHECK(np && strstr(np, "LOCAL_GATE_NP_STATE") && strstr(np, "LOCAL_GATE_NP_REGISTER") &&
          strstr(np, "LOCAL_GATE_NP_CHECK_CALLBACK") && strstr(np, "LOCAL_GATE_NP_ID"));
    CHECK(np && strstr(np, "sceNpScoreCreateNpTitleCtx") && strstr(np, "sceNpTusCreateNpTitleCtx"));
    free(np);
    char *driver = read_source("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(driver && !strstr(driver, "psn_notice") && !strstr(driver, "skip_intro"));
    free(driver);
}

int main(void) {
    test_matcher();
    test_blank_text();
    test_stub_execution();
    test_synthetic_apply();
    test_failures_write_nothing();
    test_real_images();
    test_no_overlap();
    test_static_structure();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

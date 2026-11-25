#include "../src/skip_intro.c"
#include <stdlib.h>
#include "test_platform.h"

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define IMAGE_SIZE 0x3c00000u
#define CHANGED_BYTES 6

static const char *const builds[] = {"CUSA03762-1.00", "CUSA02871-1.00", "CUSA00951-1.00"};
static uint8_t *block;

static uint8_t *fresh(size_t size) {
    static size_t block_size;
    test_release(block, block_size);
    block = test_allocate(size + 0x2000, 1);
    block_size = size + 0x2000;
    return block;
}

static uint64_t read64(const uint8_t *data, size_t offset) {
    uint64_t value;
    memcpy(&value, data + offset, 8);
    return value;
}

static int32_t read32(const uint8_t *data) {
    int32_t value;
    memcpy(&value, data, 4);
    return value;
}

static uint32_t read_u32(const uint8_t *data, size_t offset) {
    uint32_t value;
    memcpy(&value, data + offset, 4);
    return value;
}

static float read_float(const uint8_t *data, size_t offset) {
    float value;
    memcpy(&value, data + offset, 4);
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
    uint8_t *image = fresh(image_size);
    memcpy(image, data + offset, image_size);
    free(data);
    *size = (size_t)image_size;
    return image;
}

static int count_bytes(const uint8_t *data, size_t size, const uint8_t *needle, size_t needle_size) {
    int count = 0;
    for (size_t i = 0; i + needle_size <= size; ++i) {
        if (!memcmp(data + i, needle, needle_size)) ++count;
    }
    return count;
}

static void synthetic_image(uint8_t *image) {
    memset(image, 0, IMAGE_SIZE);
    memcpy(image + SKIP_INTRO_SITE, expected, sizeof(expected));
}

static void test_patch_shape(void) {
    CHECK(sizeof(expected) == SKIP_INTRO_PATCH_SIZE);
    CHECK(SKIP_INTRO_PATCH_SIZE == 0x21);
    CHECK(expected[0] == 0x41 && expected[2] == 0x87 && expected[3] == 0x74);
    CHECK(expected[11] == 0x41 && expected[14] == 0x58);
    CHECK(expected[22] == 0x41 && expected[25] == 0x70);
    CHECK(read_u32(expected, 7) == 0);
    CHECK(read_u32(expected, 0x12) == 0);
    CHECK(read_u32(expected, 0x1d) == 0);
    CHECK(BOOT_FINISHED > 0x40d80000u); /* above the game's 6.75 s completion threshold */
    CHECK(BOOT_VISIBLE == 0x3f800000u);
}

static void test_synthetic_apply(void) {
    uint8_t *image = fresh(IMAGE_SIZE);
    uint8_t *original = malloc(IMAGE_SIZE);
    uint8_t *patched = malloc(IMAGE_SIZE);
    char error[160];
    synthetic_image(image);
    memcpy(original, image, IMAGE_SIZE);
    CHECK(skip_intro_apply(image, IMAGE_SIZE, error, sizeof(error)));
    size_t changed = 0;
    for (size_t i = 0; i < IMAGE_SIZE; ++i) changed += image[i] != original[i];
    CHECK(changed == CHANGED_BYTES);
    CHECK(read_u32(image, SKIP_INTRO_SITE + 7) == BOOT_FINISHED);
    CHECK(read_u32(image, SKIP_INTRO_SITE + 0x12) == BOOT_VISIBLE);
    CHECK(read_u32(image, SKIP_INTRO_SITE + 0x1d) == BOOT_VISIBLE);
    CHECK(!memcmp(image + SKIP_INTRO_SITE, expected, 7));
    CHECK(!memcmp(image + SKIP_INTRO_SITE + 11, expected + 11, 7));
    CHECK(!memcmp(image + SKIP_INTRO_SITE + 22, expected + 22, 7));
    memcpy(patched, image, IMAGE_SIZE);
    CHECK(!skip_intro_apply(image, IMAGE_SIZE, error, sizeof(error)));
    CHECK(!memcmp(image, patched, IMAGE_SIZE));
    free(patched);
    free(original);
}

static void test_failures_write_nothing(void) {
    uint8_t *image = fresh(IMAGE_SIZE);
    uint8_t *original = malloc(IMAGE_SIZE);
    char error[160];
    for (size_t i = 0; i < sizeof(expected); ++i) {
        synthetic_image(image);
        image[SKIP_INTRO_SITE + i] ^= 0x5a;
        memcpy(original, image, IMAGE_SIZE);
        CHECK(!skip_intro_apply(image, IMAGE_SIZE, error, sizeof(error)) && error[0]);
        CHECK(!memcmp(image, original, IMAGE_SIZE));
    }
    synthetic_image(image);
    CHECK(!skip_intro_apply(image, SKIP_INTRO_SITE + sizeof(expected) - 1, error, sizeof(error)));
    CHECK(!skip_intro_apply(NULL, IMAGE_SIZE, error, sizeof(error)));
    memset(image, 0, IMAGE_SIZE);
    CHECK(!skip_intro_apply(image, IMAGE_SIZE, NULL, 0));
    free(original);
}

static void test_tick_handoff(const uint8_t *image, size_t size) {
    const size_t site = SKIP_INTRO_SITE;
    CHECK(size > site + 0x30);
    static const uint8_t lea[] = {0x48, 0x8d, 0x15, 0x77, 0x08, 0x00, 0x00};
    CHECK(!memcmp(image + site + 0x21, lea, sizeof(lea)));
    const int32_t displacement = read32(image + site + 0x24);
    CHECK((size_t)(site + 0x28 + displacement) == 0x28d64c0);
    CHECK(read_float(image, 0x3351ff0) == 4.0f);
    CHECK(read_float(image, 0x3351ff4) == -3.25f);
    CHECK(read_float(image, 0x3351ff8) == -6.5f);
    CHECK(read_float(image, 0x3351ffc) == 1.0f);
    CHECK(read_float(image, 0x3352000) == 6.75f);
}

static void test_real_images(void) {
    for (size_t b = 0; b < sizeof(builds) / sizeof(*builds); ++b) {
        size_t size = 0;
        uint8_t *image = load_boot_image(builds[b], &size);
        if (!image) {
            printf("skip intro: %s boot image not available, real image checks skipped\n", builds[b]);
            continue;
        }
        char error[160];
        CHECK(!memcmp(image + SKIP_INTRO_SITE, expected, sizeof(expected)));
        CHECK(count_bytes(image, size, expected, sizeof(expected)) == 1);
        test_tick_handoff(image, size);
        uint8_t *original = malloc(size);
        memcpy(original, image, size);
        CHECK(skip_intro_apply(image, size, error, sizeof(error)));
        size_t changed = 0;
        for (size_t i = 0; i < size; ++i) {
            if (image[i] != original[i]) {
                ++changed;
                CHECK(i >= SKIP_INTRO_SITE && i < SKIP_INTRO_SITE + sizeof(expected));
            }
        }
        CHECK(changed == CHANGED_BYTES);
        CHECK(read_u32(image, SKIP_INTRO_SITE + 7) == BOOT_FINISHED);
        CHECK(read_u32(image, SKIP_INTRO_SITE + 0x12) == BOOT_VISIBLE);
        CHECK(read_u32(image, SKIP_INTRO_SITE + 0x1d) == BOOT_VISIBLE);
        CHECK(image[0x1c99e1] == original[0x1c99e1]);
        CHECK(image[0x1ca55c] == original[0x1ca55c]);
        uint8_t *patched = malloc(size);
        memcpy(patched, image, size);
        CHECK(!skip_intro_apply(image, size, error, sizeof(error)));
        CHECK(!memcmp(image, patched, size));
        free(patched);
        free(original);
    }
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
    char *skip = read_source("src/skip_intro.c");
    CHECK(skip != NULL);
    static const char *const forbidden[] = {"DEBUG", "SAULO", "snapshot", "F5", "splash", "Splash", "Press Start",
                                            "PrepareBlankFrame", "SDL", "gamepad", "controller", "runtime_pad"};
    for (size_t i = 0; skip && i < sizeof(forbidden) / sizeof(*forbidden); ++i) CHECK(!strstr(skip, forbidden[i]));
    CHECK(skip && !strstr(skip, "0x1c99e1") && !strstr(skip, "0x1ca55c"));
    free(skip);

    char *driver = read_source("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(driver != NULL);
    CHECK(driver && !strstr(driver, "skip_intro"));
    CHECK(driver && count_of(driver, "PrepareBlankFrame(true)") == 1);
    free(driver);

    char *loader = read_source("src/loader.c");
    CHECK(loader != NULL);
    CHECK(loader && strstr(loader, "Skip Intro: off"));
    CHECK(loader && strstr(loader, "Skip Intro: on, publisher and developer boot screens bypassed"));
    CHECK(loader && strstr(loader, "Skip Intro: requested but unavailable: %s"));
    CHECK(loader && count_of(loader, "skip_intro_apply(") == 1);
    CHECK(loader && !strstr(loader, "skip_intro_debug") && !strstr(loader, "DEBUG_SAULO"));
    free(loader);

    char *help = read_source("launcher/settings.h");
    CHECK(help && strstr(help, "Skip startup logos") && strstr(help, "Press Start"));
    free(help);
}

int main(void) {
    test_patch_shape();
    test_synthetic_apply();
    test_failures_write_nothing();
    test_real_images();
    test_static_structure();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

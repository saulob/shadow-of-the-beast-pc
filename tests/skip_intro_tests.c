#include "../src/skip_intro.c"
#include <stdlib.h>
#include <windows.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define IMAGE_SIZE 0x3c00000u
#define CAVE_OFFSET 0x3c01000u
#define CAVE_SIZE 0x1000u
#define CAVE_BASE 1024u

typedef uint64_t (GUEST_ABI *Call)(uint64_t, uint64_t, uint64_t, uint64_t);

static uint8_t *block;

static uint8_t *fresh(size_t size) {
    if (block) VirtualFree(block, 0, MEM_RELEASE);
    block = VirtualAlloc(NULL, size + CAVE_SIZE + 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    return block;
}

static uint64_t read64(const uint8_t *data, size_t offset) {
    uint64_t value;
    memcpy(&value, data + offset, 8);
    return value;
}

static uint8_t *load_boot_image(size_t *size) {
    FILE *file = fopen(SOTB_BOOT_IMAGE, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length || memcmp(data, "SOTBBOT1", 8)) {
        fclose(file);
        free(data);
        return NULL;
    }
    fclose(file);
    size_t offset = 8;
    const uint64_t image_size = read64(data, offset), segments = read64(data, offset + 16), relocations = read64(data, offset + 24),
                   imports = read64(data, offset + 32);
    offset += 6 * 8 + 8 + 4 * 8;
    offset += 8 + read64(data, offset) * 56;
    offset += 8 + read64(data, offset) * 24;
    offset += segments * 24 + imports * 128 + relocations * 32;
    uint8_t *image = fresh(image_size + 0x1000);
    memcpy(image, data + offset, image_size);
    free(data);
    *size = (size_t)image_size;
    return image;
}

static void synthetic_image(uint8_t *image) {
    memset(image, 0, IMAGE_SIZE + CAVE_SIZE);
    memcpy(image + SKIP_INTRO_SITE, expected, sizeof(expected));
    memcpy(image + DELAY_CONSTANT, one_float, sizeof(one_float));
    memcpy(image + REVEAL_CONTEXT_OFFSET, reveal_context, sizeof(reveal_context));
    static const uint8_t original[] = {0x48, 0x8d, 0x04, 0x37, 0x48, 0x01, 0xd0, 0x48, 0x01, 0xc8, 0xba, 0x34, 0x12, 0x00, 0x00, 0xc3};
    memcpy(image + SHOW_START_BUTTON_TARGET, original, sizeof(original));
}

static int identical(const uint8_t *a, const uint8_t *b, size_t size) { return !memcmp(a, b, size); }

static void test_patch_shape(void) {
    CHECK(sizeof(expected) == 42 && sizeof(reveal_context) == 27);
    CHECK(SKIP_INTRO_SITE + SKIP_INTRO_PATCH_SIZE <= SKIP_INTRO_STATE_WRITE);
    CHECK(expected[SKIP_INTRO_STATE_WRITE - SKIP_INTRO_SITE + 7] == 0x04);
    CHECK(!memcmp(zero_delay, "\xc5\xf8\x57\xc0\x90\x90\x90\x90", 8));
    CHECK(REVEAL_CONTEXT_OFFSET + 15 == SKIP_INTRO_REVEAL_CALL && reveal_context[15] == 0xe8);
}

static void test_off_and_default(void) {
    atomic_store(&hidden, 0);
    CHECK(!skip_intro_hide_presentation());
}

static void test_synthetic_apply(void) {
    uint8_t *image = fresh(IMAGE_SIZE);
    uint8_t *cave = image + CAVE_OFFSET;
    uint8_t *original = malloc(IMAGE_SIZE + CAVE_SIZE);
    char error[160];
    synthetic_image(image);
    memcpy(original, image, IMAGE_SIZE + CAVE_SIZE);
    CHECK(skip_intro_apply(image, IMAGE_SIZE, cave + CAVE_BASE, CAVE_SIZE - CAVE_BASE, error, sizeof(error)));
    CHECK(skip_intro_hide_presentation());
    size_t changed = 0;
    for (size_t i = 0; i < IMAGE_SIZE; ++i) changed += image[i] != original[i];
    CHECK(changed <= SKIP_INTRO_PATCH_SIZE + 4 && changed > 0);
    CHECK(!memcmp(image + SKIP_INTRO_SITE, zero_delay, sizeof(zero_delay)));
    CHECK(identical(image + SKIP_INTRO_STATE_WRITE, original + SKIP_INTRO_STATE_WRITE, 19));
    CHECK(identical(image + SKIP_INTRO_SITE + 8, original + SKIP_INTRO_SITE + 8, sizeof(expected) - 8));
    CHECK(identical(image + SHOW_START_BUTTON_TARGET, original + SHOW_START_BUTTON_TARGET, 16));
    CHECK(identical(image + REVEAL_CONTEXT_OFFSET, original + REVEAL_CONTEXT_OFFSET, 16) &&
          identical(image + SKIP_INTRO_REVEAL_CALL + 5, original + SKIP_INTRO_REVEAL_CALL + 5, 7));

    const uint8_t *call = image + SKIP_INTRO_REVEAL_CALL;
    int32_t rel;
    memcpy(&rel, call + 1, 4);
    CHECK(call[0] == 0xe8 && call + 5 + rel == cave + CAVE_BASE);

    const Call stub = (Call)(cave + CAVE_BASE);
    const uint64_t result = stub(1000, 200, 30, 4);
    CHECK(result == 1234);
    CHECK(!skip_intro_hide_presentation());
    CHECK(stub(1, 2, 3, 4) == 10);
    CHECK(!skip_intro_hide_presentation());
    free(original);
}

static void test_failures_arm_nothing(void) {
    uint8_t *image = fresh(IMAGE_SIZE);
    uint8_t *cave = image + CAVE_OFFSET;
    uint8_t *original = malloc(IMAGE_SIZE + CAVE_SIZE);
    char error[160];
    const struct {
        size_t offset;
        uint8_t mask;
    } damage[] = {{SKIP_INTRO_SITE + 20, 0x01}, {SKIP_INTRO_STATE_WRITE + 7, 0x07}, {DELAY_CONSTANT + 3, 0x40},
                  {REVEAL_CONTEXT_OFFSET + 3, 0x01}, {SKIP_INTRO_REVEAL_CALL + 2, 0x01}, {SKIP_INTRO_REVEAL_CALL + 5, 0x01}};
    for (size_t i = 0; i < sizeof(damage) / sizeof(*damage); ++i) {
        synthetic_image(image);
        image[damage[i].offset] ^= damage[i].mask;
        memcpy(original, image, IMAGE_SIZE + CAVE_SIZE);
        CHECK(!skip_intro_apply(image, IMAGE_SIZE, cave + CAVE_BASE, CAVE_SIZE - CAVE_BASE, error, sizeof(error)) && error[0]);
        CHECK(identical(original, image, IMAGE_SIZE + CAVE_SIZE));
        CHECK(!skip_intro_hide_presentation());
    }
    synthetic_image(image);
    memset(image, 0, IMAGE_SIZE);
    CHECK(!skip_intro_apply(image, IMAGE_SIZE, cave + CAVE_BASE, CAVE_SIZE - CAVE_BASE, error, sizeof(error)));
    CHECK(image[SKIP_INTRO_SITE] == 0 && !skip_intro_hide_presentation());
    synthetic_image(image);
    CHECK(!skip_intro_apply(image, 0x100000, cave + CAVE_BASE, CAVE_SIZE - CAVE_BASE, error, sizeof(error)));
    CHECK(!skip_intro_apply(image, IMAGE_SIZE, cave + CAVE_BASE, 64, error, sizeof(error)));
    CHECK(!skip_intro_apply(NULL, 0, cave, CAVE_SIZE, error, sizeof(error)));
    CHECK(!skip_intro_apply(image, IMAGE_SIZE, NULL, 0, error, sizeof(error)));
    CHECK(!skip_intro_hide_presentation());
    free(original);
}

static void test_reveal_and_limit(void) {
    atomic_store(&hidden, 1);
    atomic_store(&armed_at_ms, now_ms());
    CHECK(skip_intro_hide_presentation());
    reveal();
    CHECK(!skip_intro_hide_presentation());
    reveal();
    CHECK(!skip_intro_hide_presentation());
    atomic_store(&hidden, 1);
    atomic_store(&armed_at_ms, now_ms() - SKIP_INTRO_HIDE_LIMIT_MS - 1000);
    CHECK(!skip_intro_hide_presentation());
    CHECK(!skip_intro_hide_presentation());
}

static void test_real_image(void) {
    size_t size = 0;
    uint8_t *image = load_boot_image(&size);
    if (!image) {
        printf("skip intro: boot image not available, real image checks skipped\n");
        return;
    }
    char error[160];
    uint8_t *cave = image + ((size + 0xfff) & ~(size_t)0xfff);
    CHECK(!memcmp(image + SKIP_INTRO_SITE, expected, sizeof(expected)));
    CHECK(!memcmp(image + DELAY_CONSTANT, one_float, sizeof(one_float)));
    CHECK(!memcmp(image + REVEAL_CONTEXT_OFFSET, reveal_context, sizeof(reveal_context)));
    uint8_t *original = malloc(size);
    memcpy(original, image, size);
    CHECK(skip_intro_apply(image, size, cave + CAVE_BASE, CAVE_SIZE - CAVE_BASE, error, sizeof(error)));
    CHECK(skip_intro_hide_presentation());
    size_t changed = 0;
    for (size_t i = 0; i < size; ++i) changed += image[i] != original[i];
    CHECK(changed > 0 && changed <= SKIP_INTRO_PATCH_SIZE + 4);
    CHECK(identical(image + SKIP_INTRO_STATE_WRITE, original + SKIP_INTRO_STATE_WRITE, 8));
    CHECK(image[SKIP_INTRO_STATE_WRITE + 7] == 0x04);
    CHECK(identical(image + SHOW_START_BUTTON_TARGET, original + SHOW_START_BUTTON_TARGET, 8));
    atomic_store(&hidden, 0);
    free(original);
}

static char *read_source(const char *relative) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", SOTB_SOURCE_DIR, relative);
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = malloc((size_t)length + 1);
    text[fread(text, 1, (size_t)length, file)] = 0;
    fclose(file);
    return text;
}

static int count_of(const char *text, const char *part) {
    int count = 0;
    for (const char *at = strstr(text, part); at; at = strstr(at + 1, part)) ++count;
    return count;
}

static void test_static_structure(void) {
    char *skip = read_source("src/skip_intro.c");
    CHECK(skip != NULL);
    static const char *const forbidden[] = {"Cross", "inject", "isDown", "wasPressed", "controller", "gamepad", "SDL", "runtime_pad"};
    for (size_t i = 0; skip && i < sizeof(forbidden) / sizeof(*forbidden); ++i) CHECK(!strstr(skip, forbidden[i]));
    free(skip);

    char *driver = read_source("gpu/shadps4/core/libraries/videoout/driver.cpp");
    CHECK(driver != NULL);
    CHECK(driver && count_of(driver, "skip_intro_hide_presentation()") == 1);
    CHECK(driver && strstr(driver, "presenter->Present(presenter->PrepareBlankFrame(true), false, false)"));
    free(driver);
    static const char *const renderers[] = {"gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp",
                                            "gpu/shadps4/video_core/renderer_opengl/gl_presenter.cpp",
                                            "gpu/shadps4/video_core/renderer_opengl/gl_angle.cpp",
                                            "gpu/shadps4/video_core/renderer_factory.cpp"};
    for (size_t i = 0; i < sizeof(renderers) / sizeof(*renderers); ++i) {
        char *text = read_source(renderers[i]);
        CHECK(text && !strstr(text, "skip_intro"));
        free(text);
    }
    char *loader = read_source("src/loader.c");
    CHECK(loader && strstr(loader, "Skip Intro: off") && strstr(loader, "Skip Intro: on, startup presentation hidden") &&
          strstr(loader, "Skip Intro: requested but unavailable: %s"));
    free(loader);
}

int main(void) {
    test_patch_shape();
    test_off_and_default();
    test_synthetic_apply();
    test_failures_arm_nothing();
    test_reveal_and_limit();
    test_real_image();
    test_static_structure();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

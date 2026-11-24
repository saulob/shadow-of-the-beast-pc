#include "../src/sotb_menu_buttons.c"

#include <stdlib.h>
#include <windows.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define IMAGE_SIZE 0x166000u

static void synthetic_image(uint8_t *image) {
    memset(image, 0, IMAGE_SIZE);
    memcpy(image + MENU_BUTTONS_CONTEXT, kContext, sizeof(kContext));
    memcpy(image + MENU_BUTTONS_SITE + kOriginalFirstKeyOffset, kOriginalFirstKey, sizeof(kOriginalFirstKey));
    memcpy(image + MENU_BUTTONS_SITE + kOriginalSecondKeyOffset, kOriginalSecondKey, sizeof(kOriginalSecondKey));
    memcpy(image + MENU_BUTTONS_SITE + kWesternFirstKeyOffset, kWesternFirstKey, sizeof(kWesternFirstKey));
    memcpy(image + MENU_BUTTONS_SITE + kWesternSecondKeyOffset, kWesternSecondKey, sizeof(kWesternSecondKey));
}

static uint64_t read64(const uint8_t *data, size_t offset) {
    uint64_t value;
    memcpy(&value, data + offset, sizeof(value));
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
    const uint64_t image_size = read64(data, offset), segments = read64(data, offset + 16),
                   relocations = read64(data, offset + 24), imports = read64(data, offset + 32);
    offset += 6 * 8 + 8 + 4 * 8;
    offset += 8 + read64(data, offset) * 56;
    offset += 8 + read64(data, offset) * 24;
    offset += segments * 24 + imports * 128 + relocations * 32;
    uint8_t *image = malloc((size_t)image_size);
    if (image) memcpy(image, data + offset, (size_t)image_size);
    free(data);
    *size = (size_t)image_size;
    return image;
}

static void test_patch_shape(void) {
    CHECK(sizeof(kContext) == kContextLength);
    CHECK(MENU_BUTTONS_CONTEXT + 0x14 == MENU_BUTTONS_SITE);
    CHECK(kPatchLength == 6);
    CHECK(kWesternBranch[0] == 0xe9 && load_rel32(kWesternBranch + 1) == 0x11f && kWesternBranch[5] == 0x90);
    CHECK(kOriginalFirstKeyOffset < kOriginalSecondKeyOffset && kOriginalSecondKeyOffset < kWesternFirstKeyOffset &&
          kWesternFirstKeyOffset < kWesternSecondKeyOffset);
}

static void test_western_changes_only_the_branch(void) {
    uint8_t *image = malloc(IMAGE_SIZE);
    uint8_t *original = malloc(IMAGE_SIZE);
    synthetic_image(image);
    memcpy(original, image, IMAGE_SIZE);
    CHECK(sotb_menu_buttons_apply(image, IMAGE_SIZE, "CUSA03762-1.00", SOTB_MENU_BUTTON_LAYOUT_WESTERN));
    size_t changed = 0;
    for (size_t i = 0; i < IMAGE_SIZE; ++i) changed += image[i] != original[i];
    CHECK(changed > 0 && changed <= kPatchLength);
    CHECK(!memcmp(image, original, MENU_BUTTONS_SITE));
    CHECK(!memcmp(image + MENU_BUTTONS_SITE + kPatchLength, original + MENU_BUTTONS_SITE + kPatchLength,
                  IMAGE_SIZE - MENU_BUTTONS_SITE - kPatchLength));
    CHECK(!memcmp(image + MENU_BUTTONS_SITE, kWesternBranch, sizeof(kWesternBranch)));
    const uint8_t *jump = image + MENU_BUTTONS_SITE;
    CHECK(jump + 5 + load_rel32(jump + 1) == image + MENU_BUTTONS_SITE + 0x124);
    free(original);
    free(image);
}

static void test_original_leaves_the_image_alone(void) {
    uint8_t *image = malloc(IMAGE_SIZE);
    uint8_t *original = malloc(IMAGE_SIZE);
    synthetic_image(image);
    memcpy(original, image, IMAGE_SIZE);
    CHECK(sotb_menu_buttons_apply(image, IMAGE_SIZE, "CUSA03762-1.00", SOTB_MENU_BUTTON_LAYOUT_ORIGINAL));
    CHECK(!memcmp(image, original, IMAGE_SIZE));
    free(original);
    free(image);
}

static void test_failures_write_nothing(void) {
    uint8_t *image = malloc(IMAGE_SIZE);
    uint8_t *original = malloc(IMAGE_SIZE);
    synthetic_image(image);

    memcpy(original, image, IMAGE_SIZE);
    CHECK(!sotb_menu_buttons_apply(image, IMAGE_SIZE, "CUSA99999-1.00", SOTB_MENU_BUTTON_LAYOUT_WESTERN));
    CHECK(!memcmp(image, original, IMAGE_SIZE));

    synthetic_image(image);
    image[MENU_BUTTONS_CONTEXT + 3] ^= 0x40;
    memcpy(original, image, IMAGE_SIZE);
    CHECK(!sotb_menu_buttons_apply(image, IMAGE_SIZE, "CUSA03762-1.00", SOTB_MENU_BUTTON_LAYOUT_WESTERN));
    CHECK(!memcmp(image, original, IMAGE_SIZE));

    synthetic_image(image);
    memcpy(image + 0x1000, kContext, sizeof(kContext));
    memcpy(original, image, IMAGE_SIZE);
    CHECK(!sotb_menu_buttons_apply(image, IMAGE_SIZE, "CUSA03762-1.00", SOTB_MENU_BUTTON_LAYOUT_WESTERN));
    CHECK(!memcmp(image, original, IMAGE_SIZE));

    synthetic_image(image);
    image[MENU_BUTTONS_SITE + kWesternFirstKeyOffset + 3] ^= 0x01;
    memcpy(original, image, IMAGE_SIZE);
    CHECK(!sotb_menu_buttons_apply(image, IMAGE_SIZE, "CUSA03762-1.00", SOTB_MENU_BUTTON_LAYOUT_WESTERN));
    CHECK(!memcmp(image, original, IMAGE_SIZE));

    synthetic_image(image);
    memcpy(original, image, IMAGE_SIZE);
    CHECK(!sotb_menu_buttons_apply(image, MENU_BUTTONS_CONTEXT, "CUSA03762-1.00", SOTB_MENU_BUTTON_LAYOUT_WESTERN));
    CHECK(!memcmp(image, original, IMAGE_SIZE));
    CHECK(!sotb_menu_buttons_apply(NULL, IMAGE_SIZE, "CUSA03762-1.00", SOTB_MENU_BUTTON_LAYOUT_WESTERN));
    free(original);
    free(image);
}

static void test_setting_names(void) {
    CHECK(layout_by_name(NULL, SOTB_MENU_BUTTON_LAYOUT_WESTERN) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);
    CHECK(layout_by_name("", SOTB_MENU_BUTTON_LAYOUT_WESTERN) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);
    CHECK(layout_by_name("western", SOTB_MENU_BUTTON_LAYOUT_ORIGINAL) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);
    CHECK(layout_by_name("Western", SOTB_MENU_BUTTON_LAYOUT_ORIGINAL) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);
    CHECK(layout_by_name("original", SOTB_MENU_BUTTON_LAYOUT_WESTERN) == SOTB_MENU_BUTTON_LAYOUT_ORIGINAL);
    CHECK(layout_by_name("Original", SOTB_MENU_BUTTON_LAYOUT_WESTERN) == SOTB_MENU_BUTTON_LAYOUT_ORIGINAL);
    CHECK(layout_by_name("bogus", SOTB_MENU_BUTTON_LAYOUT_WESTERN) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);
    CHECK(layout_by_name("bogus", SOTB_MENU_BUTTON_LAYOUT_ORIGINAL) == SOTB_MENU_BUTTON_LAYOUT_ORIGINAL);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (file) {
        fputs(text, file);
        fclose(file);
    }
}

static void test_setting_file(void) {
    char folder[MAX_PATH], path[MAX_PATH];
    GetTempPathA(MAX_PATH, folder);
    GetTempFileNameA(folder, "sotb", 0, path);
    _putenv_s(SOTB_MENU_BUTTON_LAYOUT_ENV, "");

    CHECK(sotb_menu_button_layout(path) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);
    write_text(path, "renderer = auto\n");
    CHECK(sotb_menu_button_layout(path) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);
    write_text(path, "menu_button_layout = original\n");
    CHECK(sotb_menu_button_layout(path) == SOTB_MENU_BUTTON_LAYOUT_ORIGINAL);
    write_text(path, "menu_button_layout = Original\n");
    CHECK(sotb_menu_button_layout(path) == SOTB_MENU_BUTTON_LAYOUT_ORIGINAL);
    write_text(path, "menu_button_layout = western\n");
    CHECK(sotb_menu_button_layout(path) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);
    write_text(path, "menu_button_layout = bogus\n");
    CHECK(sotb_menu_button_layout(path) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);
    write_text(path, "menu_button_layout = original\n");
    CHECK(sotb_menu_button_layout(NULL) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);

    _putenv_s(SOTB_MENU_BUTTON_LAYOUT_ENV, "original");
    CHECK(sotb_menu_button_layout(path) == SOTB_MENU_BUTTON_LAYOUT_ORIGINAL);
    _putenv_s(SOTB_MENU_BUTTON_LAYOUT_ENV, "western");
    CHECK(sotb_menu_button_layout(path) == SOTB_MENU_BUTTON_LAYOUT_WESTERN);
    _putenv_s(SOTB_MENU_BUTTON_LAYOUT_ENV, "");
    DeleteFileA(path);
}

static void test_real_image(void) {
    size_t size = 0;
    uint8_t *image = load_boot_image(&size);
    if (!image) {
        printf("menu buttons: boot image not available, real image checks skipped\n");
        return;
    }
    CHECK(size > MENU_BUTTONS_CONTEXT + kContextLength);
    CHECK(!memcmp(image + MENU_BUTTONS_CONTEXT, kContext, sizeof(kContext)));
    CHECK(site_count(image, size) == 1);
    CHECK(!memcmp(image + MENU_BUTTONS_SITE + kOriginalFirstKeyOffset, kOriginalFirstKey, sizeof(kOriginalFirstKey)));
    CHECK(!memcmp(image + MENU_BUTTONS_SITE + kOriginalSecondKeyOffset, kOriginalSecondKey, sizeof(kOriginalSecondKey)));
    CHECK(!memcmp(image + MENU_BUTTONS_SITE + kWesternFirstKeyOffset, kWesternFirstKey, sizeof(kWesternFirstKey)));
    CHECK(!memcmp(image + MENU_BUTTONS_SITE + kWesternSecondKeyOffset, kWesternSecondKey, sizeof(kWesternSecondKey)));
    const uint64_t original_first = lea_target(image, size, MENU_BUTTONS_SITE + kOriginalFirstKeyOffset);
    const uint64_t original_second = lea_target(image, size, MENU_BUTTONS_SITE + kOriginalSecondKeyOffset);
    const uint64_t western_first = lea_target(image, size, MENU_BUTTONS_SITE + kWesternFirstKeyOffset);
    const uint64_t western_second = lea_target(image, size, MENU_BUTTONS_SITE + kWesternSecondKeyOffset);
    /* EKeys::Gamepad_FaceButton_Right (Circle/B) and Bottom (Cross/A). */
    CHECK(original_first == 0x419be60 && original_second == 0x419be48);
    CHECK(western_first == 0x419be48 && western_second == 0x419be60);

    uint8_t *original = malloc(size);
    memcpy(original, image, size);
    CHECK(sotb_menu_buttons_apply(image, size, "CUSA03762-1.00", SOTB_MENU_BUTTON_LAYOUT_WESTERN));
    size_t changed = 0;
    for (size_t i = 0; i < size; ++i) changed += image[i] != original[i];
    CHECK(changed > 0 && changed <= kPatchLength);
    CHECK(!memcmp(image, original, MENU_BUTTONS_SITE));
    CHECK(!memcmp(image + MENU_BUTTONS_SITE + kPatchLength, original + MENU_BUTTONS_SITE + kPatchLength,
                  size - MENU_BUTTONS_SITE - kPatchLength));
    CHECK(!memcmp(image + MENU_BUTTONS_SITE, kWesternBranch, sizeof(kWesternBranch)));
    free(original);
    free(image);

    size_t orig_size = 0;
    uint8_t *orig_image = load_boot_image(&orig_size);
    CHECK(orig_image != NULL);
    if (orig_image) {
        uint8_t *orig_copy = malloc(orig_size);
        memcpy(orig_copy, orig_image, orig_size);
        CHECK(sotb_menu_buttons_apply(orig_image, orig_size, "CUSA03762-1.00", SOTB_MENU_BUTTON_LAYOUT_ORIGINAL));
        CHECK(!memcmp(orig_image, orig_copy, orig_size));
        free(orig_copy);
        free(orig_image);
    }
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
    if (text) text[fread(text, 1, (size_t)length, file)] = 0;
    fclose(file);
    return text;
}

static void test_static_structure(void) {
    char *menu = read_source("src/sotb_menu_buttons.c");
    CHECK(menu != NULL);
    static const char *const forbidden[] = {"SDL", "runtime_pad", "sotb_pad", "Combat", "Dodge", "Attack", "Jump"};
    for (size_t i = 0; menu && i < sizeof(forbidden) / sizeof(*forbidden); ++i) CHECK(!strstr(menu, forbidden[i]));
    free(menu);

    char *loader = read_source("src/loader.c");
    CHECK(loader != NULL);
    const char *patch = loader ? strstr(loader, "sotb_menu_buttons_apply(image, size, game_build.code_key") : NULL;
    const char *entry = loader ? strstr(loader, "sotb_startup_phase(\"guest entry\")") : NULL;
    const char *guest = loader ? strstr(loader, "guest_patches_apply(image, size, cave, cave_size)") : NULL;
    CHECK(patch && guest && entry && guest < patch && patch < entry);
    free(loader);

    char *launcher = read_source("launcher/launcher.cpp");
    CHECK(launcher != NULL);
    const char *prompts = launcher ? strstr(launcher, "ui.Combo(\"Controller prompts\"") : NULL;
    const char *choice = launcher ? strstr(launcher, "ui.Choice(\"Menu Button Layout\"") : NULL;
    const char *defaults = launcher ? strstr(launcher, "ui.ConfirmRow(\"Controller defaults\"") : NULL;
    CHECK(prompts && choice && defaults && prompts < choice && choice < defaults);
    free(launcher);

    char *settings = read_source("launcher/settings.h");
    CHECK(settings != NULL);
    CHECK(settings && strstr(settings, "\"menu_button_layout\""));
    CHECK(settings && strstr(settings, "Combat controls are unchanged"));
    free(settings);
}

int main(void) {
    test_patch_shape();
    test_western_changes_only_the_branch();
    test_original_leaves_the_image_alone();
    test_failures_write_nothing();
    test_setting_names();
    test_setting_file();
    test_real_image();
    test_static_structure();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

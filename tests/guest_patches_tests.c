#include "../src/guest_patches.c"
#include <windows.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define IMAGE_SIZE 0x400000u
#define SOCIAL_SETTING 0xfd
#define POPUP_DONE 0xfe
#define ARCHIVE_FLAG 0x28

typedef uintptr_t (GUEST_ABI *ShowScreen)(void *manager, int screen);
typedef uintptr_t (GUEST_ABI *CaveCall)(void *argument);

static uint8_t *image, *cave;
static int hook_calls;
static void *hook_argument;

static GUEST_ABI void counting_hook(void *controller) {
    ++hook_calls;
    hook_argument = controller;
}

static void fresh_image(void) {
    if (!image) image = VirtualAlloc(NULL, IMAGE_SIZE + 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    cave = image + IMAGE_SIZE;
    memset(image, 0, IMAGE_SIZE + 0x10000);
    for (size_t i = 0; i < sizeof(byte_patches) / sizeof(*byte_patches); ++i)
        memcpy(image + byte_patches[i].offset, byte_patches[i].expected, byte_patches[i].length);
    for (size_t i = 0; i < 2; ++i) memcpy(image + SERIALIZE_SITES[i], SERIALIZE_EXPECTED, sizeof(SERIALIZE_EXPECTED));
    memcpy(image + SHOW_SCREEN_SITE, SHOW_SCREEN_EXPECTED, sizeof(SHOW_SCREEN_EXPECTED));
    memcpy(image + OPTIONS_SITE, OPTIONS_EXPECTED, sizeof(OPTIONS_EXPECTED));
    static const uint8_t after_prologue[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0x41, 0x5f, 0x5d, 0xc3};
    memcpy(image + SHOW_SCREEN_SITE + sizeof(SHOW_SCREEN_EXPECTED), after_prologue, sizeof(after_prologue));
    static const uint8_t return_argument[] = {0x48, 0x89, 0xf8, 0xc3};
    memcpy(image + 0x1f0540, return_argument, sizeof(return_argument));
    hook_calls = 0;
    hook_argument = NULL;
    guest_patches_set_options_hook(counting_hook);
}

static void test_byte_patches(void) {
    fresh_image();
    GuestPatchReport report = guest_patches_apply(image, IMAGE_SIZE, cave, GUEST_PATCH_CAVE_SIZE);
    CHECK(report.applied == 13 && report.skipped == 0);
    for (size_t i = 0; i < sizeof(byte_patches) / sizeof(*byte_patches); ++i)
        CHECK(!memcmp(image + byte_patches[i].offset, byte_patches[i].replacement, byte_patches[i].length));
    CHECK(image[0x1a32fd] == 0xeb);
    CHECK(image[0x3408c4 + 6] == 0 && image[0x3408cb + 6] == 1);
}

static void test_code_that_differs_is_left_alone(void) {
    fresh_image();
    image[0x1a32fd] = 0x74;
    image[SHOW_SCREEN_SITE + 1] ^= 0xff;
    GuestPatchReport report = guest_patches_apply(image, IMAGE_SIZE, cave, GUEST_PATCH_CAVE_SIZE);
    CHECK(report.skipped == 2 && report.applied == 11);
    CHECK(image[0x1a32fd] == 0x74);
    CHECK(image[SHOW_SCREEN_SITE] == 0x55);

    fresh_image();
    report = guest_patches_apply(image, IMAGE_SIZE, image + 0x90000000ull, GUEST_PATCH_CAVE_SIZE);
    CHECK(report.applied == 9 && report.skipped == 4);
    CHECK(!memcmp(image + SERIALIZE_SITE, SERIALIZE_EXPECTED, sizeof(SERIALIZE_EXPECTED)));
}

static void test_social_is_normalized_on_save_access(void) {
    fresh_image();
    guest_patches_apply(image, IMAGE_SIZE, cave, GUEST_PATCH_CAVE_SIZE);
    int32_t rel;
    memcpy(&rel, image + SERIALIZE_SITE + 1, 4);
    uint8_t *code = image + SERIALIZE_SITE + 5 + rel;
    CHECK(image[SERIALIZE_SITE] == 0xe8 && code == cave);
    int32_t second;
    memcpy(&second, image + SERIALIZE_SITES[1] + 1, 4);
    CHECK(image[SERIALIZE_SITES[1]] == 0xe8 && image + SERIALIZE_SITES[1] + 5 + second == code);

    for (int loading = 0; loading <= 1; ++loading) {
        uint8_t object[0x200] = {0}, archive[0x40] = {0};
        object[SOCIAL_SETTING] = 1;
        archive[ARCHIVE_FLAG] = (uint8_t)loading;
        uint8_t zero_flag;
        __asm__ volatile("movq %[object], %%rbx\n\tmovq %[archive], %%r14\n\tcallq *%[code]\n\tsete %[zf]"
                         : [zf] "=r"(zero_flag)
                         : [object] "r"(object), [archive] "r"(archive), [code] "r"(code)
                         : "rbx", "r14", "cc", "memory", "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11");
        CHECK(object[SOCIAL_SETTING] == 0 && object[POPUP_DONE] == 1);
        CHECK(zero_flag == (loading ? 0 : 1));
    }
}

static void test_social_screens_are_refused(void) {
    fresh_image();
    guest_patches_apply(image, IMAGE_SIZE, cave, GUEST_PATCH_CAVE_SIZE);
    const ShowScreen show = (ShowScreen)(void *)(image + SHOW_SCREEN_SITE);
    CHECK(show(NULL, 0x25) == 0);
    CHECK(show(NULL, 0x0e) == 0);
    CHECK(show(NULL, 0x24) == 42);
    CHECK(show(NULL, 0x03) == 42);
    CHECK(show(NULL, 0x00) == 42);
}

static void test_options_hook_runs_before_the_original_call(void) {
    fresh_image();
    guest_patches_apply(image, IMAGE_SIZE, cave, GUEST_PATCH_CAVE_SIZE);
    int32_t rel;
    memcpy(&rel, image + OPTIONS_SITE + 1, 4);
    const CaveCall call = (CaveCall)(void *)(image + OPTIONS_SITE + 5 + rel);
    int marker;
    CHECK(image[OPTIONS_SITE] == 0xe8);
    CHECK(call(&marker) == (uintptr_t)&marker);
    CHECK(hook_calls == 1 && hook_argument == &marker);
}

static int removed, collapsed;
static void *removed_button, *collapsed_button;
static void fake_remove(void *controller, void *button) { (void)controller; ++removed; removed_button = button; }
static void fake_collapse(void *widget) { ++collapsed; collapsed_button = widget; }

static void test_options_row_is_taken_out_of_the_list(void) {
    const GuestUiCalls calls = {fake_remove, fake_collapse};
    uint8_t controller[0x400] = {0};
    int button;
    CHECK(guest_options_hide_social(controller, &calls) == 0 && removed == 0 && collapsed == 0);
    *(void **)(controller + OPTIONS_LIVE_FEED_BUTTON) = &button;
    CHECK(guest_options_hide_social(controller, &calls) == 1);
    CHECK(removed == 1 && collapsed == 1 && removed_button == &button && collapsed_button == &button);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_byte_patches();
    test_code_that_differs_is_left_alone();
    test_social_is_normalized_on_save_access();
    test_social_screens_are_refused();
    test_options_hook_runs_before_the_original_call();
    test_options_row_is_taken_out_of_the_list();
    printf("guest patches: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

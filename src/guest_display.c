#include "guest_display.h"
#include <stdio.h>
#include <string.h>

const SotbResolution sotb_resolutions[SOTB_RESOLUTION_COUNT] = {
    {"native", "Native", 1920, 1080}, {"480p", "480p", 854, 480},     {"720p", "720p", 1280, 720},
    {"1440p", "1440p", 2560, 1440},           {"1800p", "1800p", 3200, 1800}, {"4K", "4K", 3840, 2160},
};

int sotb_resolution_index(const char *key) {
    for (int i = 0; key && i < SOTB_RESOLUTION_COUNT; ++i)
        if (!strcmp(key, sotb_resolutions[i].key)) return i;
    return 0;
}

const SotbAspectRatio sotb_aspect_ratios[SOTB_ASPECT_COUNT] = {
    {"16:9", "16:9 Original", 16, 9}, {"16:10", "16:10", 16, 10},          {"21:9", "21:9 Ultrawide", 21, 9},
    {"32:9", "32:9 Super Ultrawide", 32, 9}, {"4:3", "4:3", 4, 3}, {"5:4", "5:4", 5, 4},
};

int sotb_aspect_index(const char *key) {
    for (int i = 0; key && i < SOTB_ASPECT_COUNT; ++i)
        if (!strcmp(key, sotb_aspect_ratios[i].key)) return i;
    return 0;
}

static int aligned_width(int height, const SotbAspectRatio *aspect) {
    const int64_t width = ((int64_t)height * aspect->width * 2 + aspect->height) / (2 * aspect->height);
    return (int)((width + SOTB_TARGET_ALIGNMENT / 2) / SOTB_TARGET_ALIGNMENT * SOTB_TARGET_ALIGNMENT);
}

SotbRenderTarget sotb_render_target(const SotbResolution *resolution, const SotbAspectRatio *aspect) {
    SotbRenderTarget target = {resolution->width, resolution->height};
    if (!aspect || (aspect->width == 16 && aspect->height == 9)) return target;
    int height = resolution->height;
    while (height > SOTB_TARGET_ALIGNMENT && (int64_t)aligned_width(height, aspect) * height > SOTB_TARGET_PIXEL_BUDGET)
        height -= SOTB_TARGET_ALIGNMENT;
    target.width = aligned_width(height, aspect);
    target.height = height;
    return target;
}

int sotb_target_is_heavy(SotbRenderTarget target) {
    return (int64_t)target.width * target.height > SOTB_SAFE_TARGET_PIXELS;
}

int sotb_safe_resolution_index(const SotbAspectRatio *aspect) {
    static const char *const candidates[] = {"1440p", "native", "720p", "480p"};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(*candidates); ++i) {
        const int index = sotb_resolution_index(candidates[i]);
        if (!sotb_target_is_heavy(sotb_render_target(&sotb_resolutions[index], aspect))) return index;
    }
    return sotb_resolution_index("480p");
}

typedef struct {
    uint32_t offset;
    uint8_t length;
    uint8_t original[10];
    uint8_t immediate;
    uint8_t is_height;
} SizeSite;

static const SizeSite size_sites[] = {
    {0x607f0c, 10, {0x48, 0xb8, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, 0},
    {0x6080b7, 10, {0x48, 0xb8, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, 0},
    {0x609011, 10, {0x48, 0xb8, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, 0},
    {0x609290, 10, {0x48, 0xba, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, 0},
    {0x609037, 10, {0xc7, 0x85, 0x98, 0xdd, 0xff, 0xff, 0x80, 0x07, 0, 0}, 6, 0},
    {0x609041, 10, {0xc7, 0x85, 0x9c, 0xdd, 0xff, 0xff, 0x38, 0x04, 0, 0}, 6, 1},
};

static int fail(char *error, size_t size, const char *what, uint32_t offset) {
    if (error && size) snprintf(error, size, "%s at image offset 0x%x", what, (unsigned)offset);
    return 0;
}

static void put32(uint8_t *at, uint32_t value) { memcpy(at, &value, 4); }

#define SIZE_SITE_COUNT (sizeof(size_sites) / sizeof(*size_sites))

static int size_sites_match(const uint8_t *image, size_t image_size, char *error, size_t error_size) {
    for (size_t i = 0; i < SIZE_SITE_COUNT; ++i) {
        const SizeSite *site = &size_sites[i];
        if ((uint64_t)site->offset + site->length > image_size)
            return fail(error, error_size, "site outside the image", site->offset);
        if (memcmp(image + site->offset, site->original, site->length))
            return fail(error, error_size, "unexpected display size instruction", site->offset);
    }
    return 1;
}

int guest_check_display_size(const uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if (!image) return fail(error, error_size, "missing image", 0);
    return size_sites_match(image, image_size, error, error_size);
}

int guest_apply_display_size(uint8_t *image, size_t image_size, int width, int height, char *error, size_t error_size) {
    if (width < 8 || height < 8) return fail(error, error_size, "invalid render size", 0);
    const size_t count = SIZE_SITE_COUNT;
    if (!size_sites_match(image, image_size, error, error_size)) return 0;
    for (size_t i = 0; i < count; ++i) {
        const SizeSite *site = &size_sites[i];
        uint8_t *at = image + site->offset + site->immediate;
        if (site->immediate == 2) {
            put32(at, (uint32_t)width);
            put32(at + 4, (uint32_t)height);
        } else {
            put32(at, (uint32_t)(site->is_height ? height : width));
        }
    }
    return 1;
}

_Static_assert(sizeof(GuestSlateGeometry) == 0x38, "UE4.8 FGeometry size mismatch");

#define GUEST_ABI __attribute__((sysv_abi))
#define LOADING_VTABLE_ARRANGE_OFFSET 0x3d42610u
#define COMPOUND_ARRANGE_OFFSET 0x808a50u
#define HINT_TEXT_POS_OFFSET 0x1e0963au

static const uint8_t compound_arrange_bytes[] = {
    0x48, 0x8d, 0x87, 0xf0, 0x00, 0x00, 0x00,
    0x48, 0x8d, 0x8f, 0x38, 0x01, 0x00, 0x00,
    0x48, 0x89, 0xf7,
};

static const uint8_t hint_text_pos_bytes[] = {
    0x48, 0xb8, 0x00, 0x00, 0x48, 0x43, 0x00, 0x00, 0x57, 0x44,
};

typedef void (GUEST_ABI *GuestArrangeChildrenFn)(void *self, const GuestSlateGeometry *allotted, void *arranged);
static GuestArrangeChildrenFn original_loading_arrange;

static float absf(float x) { return x < 0.0f ? -x : x; }

GuestSlateGeometry guest_scale_loading_geometry(const GuestSlateGeometry *allotted) {
    GuestSlateGeometry out = *allotted;
    if (allotted->size_x <= 0.0f || allotted->size_y <= 0.0f) return out;
    const float scale_x = allotted->size_x / SOTB_LOADING_DESIGN_WIDTH;
    const float scale_y = allotted->size_y / SOTB_LOADING_DESIGN_HEIGHT;
    const float scale = scale_x < scale_y ? scale_x : scale_y;
    const float offset_x = (allotted->size_x - SOTB_LOADING_DESIGN_WIDTH * scale) * 0.5f;
    const float offset_y = (allotted->size_y - SOTB_LOADING_DESIGN_HEIGHT * scale) * 0.5f;
    if (absf(scale - 1.0f) < 1e-5f && absf(offset_x) < 1e-3f && absf(offset_y) < 1e-3f) return out;
    out.size_x = SOTB_LOADING_DESIGN_WIDTH;
    out.size_y = SOTB_LOADING_DESIGN_HEIGHT;
    out.scale = allotted->scale * scale;
    out.abs_pos_x = allotted->abs_pos_x + offset_x * allotted->scale;
    out.abs_pos_y = allotted->abs_pos_y + offset_y * allotted->scale;
    out.position_x = offset_x;
    out.position_y = offset_y;
    out.render_m00 = allotted->render_m00 * scale;
    out.render_m01 = allotted->render_m01 * scale;
    out.render_m10 = allotted->render_m10 * scale;
    out.render_m11 = allotted->render_m11 * scale;
    out.render_tx = allotted->render_tx + offset_x * allotted->render_m00 + offset_y * allotted->render_m10;
    out.render_ty = allotted->render_ty + offset_x * allotted->render_m01 + offset_y * allotted->render_m11;
    return out;
}

GuestSlateGeometry guest_safe_area_geometry(const GuestSlateGeometry *allotted) {
    GuestSlateGeometry out = *allotted;
    if (allotted->size_x <= 0.0f || allotted->size_y <= 0.0f) return out;
    const float safe_width = allotted->size_y * (16.0f / 9.0f);
    if (allotted->size_x <= safe_width * 1.01f) return out;
    const float offset = (allotted->size_x - safe_width) * 0.5f;
    out.size_x = safe_width;
    out.position_x = allotted->position_x + offset;
    out.abs_pos_x = allotted->abs_pos_x + offset * allotted->scale;
    out.render_tx = allotted->render_tx + offset * allotted->render_m00;
    out.render_ty = allotted->render_ty + offset * allotted->render_m01;
    return out;
}

static void (*loading_observer)(const GuestSlateGeometry *allotted);

void guest_loading_screen_set_observer(void (*observer)(const GuestSlateGeometry *allotted)) {
    loading_observer = observer;
}

static GUEST_ABI void loading_screen_arrange_children(void *self, const GuestSlateGeometry *allotted, void *arranged) {
    if (loading_observer) loading_observer(allotted);
    const GuestSlateGeometry scaled = guest_scale_loading_geometry(allotted);
    original_loading_arrange(self, &scaled, arranged);
}

int guest_check_loading_screen_layout(const uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if (!image || (uint64_t)LOADING_VTABLE_ARRANGE_OFFSET + sizeof(uint64_t) > image_size)
        return fail(error, error_size, "loading screen vtable outside the image", LOADING_VTABLE_ARRANGE_OFFSET);
    if (memcmp(image + COMPOUND_ARRANGE_OFFSET, compound_arrange_bytes, sizeof(compound_arrange_bytes)))
        return fail(error, error_size, "unexpected SCompoundWidget::OnArrangeChildren bytes", COMPOUND_ARRANGE_OFFSET);
    if (memcmp(image + HINT_TEXT_POS_OFFSET, hint_text_pos_bytes, sizeof(hint_text_pos_bytes)))
        return fail(error, error_size, "unexpected loading hint position bytes", HINT_TEXT_POS_OFFSET);
    uint64_t current = 0;
    memcpy(&current, image + LOADING_VTABLE_ARRANGE_OFFSET, sizeof(current));
    if (current != (uint64_t)(uintptr_t)(image + COMPOUND_ARRANGE_OFFSET))
        return fail(error, error_size, "unexpected SBeastLoadingScreen vtable entry", LOADING_VTABLE_ARRANGE_OFFSET);
    return 1;
}

static SotbPatchSite display_sites[SIZE_SITE_COUNT + 2];

size_t guest_display_sites(const SotbPatchSite **out) {
    static const char *const names[] = {"display size 1", "display size 2", "display size 3",
                                        "display size 4", "display size 5", "display size 6"};
    _Static_assert(sizeof(names) / sizeof(*names) == SIZE_SITE_COUNT, "one name per display size site");
    for (size_t i = 0; i < SIZE_SITE_COUNT; ++i) {
        display_sites[i] = (SotbPatchSite){names[i], size_sites[i].offset, size_sites[i].original, size_sites[i].length};
    }
    display_sites[SIZE_SITE_COUNT] = (SotbPatchSite){"SCompoundWidget::OnArrangeChildren", COMPOUND_ARRANGE_OFFSET,
                                                     compound_arrange_bytes, sizeof(compound_arrange_bytes)};
    display_sites[SIZE_SITE_COUNT + 1] =
        (SotbPatchSite){"loading hint position", HINT_TEXT_POS_OFFSET, hint_text_pos_bytes, sizeof(hint_text_pos_bytes)};
    *out = display_sites;
    return SIZE_SITE_COUNT + 2;
}

int guest_apply_loading_screen_layout(uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if (!guest_check_loading_screen_layout(image, image_size, error, error_size)) return 0;
    const uint64_t expected = (uint64_t)(uintptr_t)(image + COMPOUND_ARRANGE_OFFSET);
    original_loading_arrange = (GuestArrangeChildrenFn)(uintptr_t)expected;
    const uint64_t hook = (uint64_t)(uintptr_t)loading_screen_arrange_children;
    memcpy(image + LOADING_VTABLE_ARRANGE_OFFSET, &hook, sizeof(hook));
    return 1;
}

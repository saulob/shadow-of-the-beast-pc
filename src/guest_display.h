#ifndef GUEST_DISPLAY_H
#define GUEST_DISPLAY_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    const char *name;
    int width, height;
} SotbResolution;

#define SOTB_RESOLUTION_COUNT 6
extern const SotbResolution sotb_resolutions[SOTB_RESOLUTION_COUNT];

int sotb_resolution_index(const char *key);

typedef struct {
    const char *key;  /* aspect_ratio in sotb.ini */
    const char *name; /* launcher label */
    int width, height; /* the ratio's terms */
} SotbAspectRatio;

#define SOTB_ASPECT_COUNT 6
extern const SotbAspectRatio sotb_aspect_ratios[SOTB_ASPECT_COUNT];

/* Index of the key, 0 (16:9 Original) when it is not listed. */
int sotb_aspect_index(const char *key);

typedef struct {
    int width, height;
} SotbRenderTarget;

#define SOTB_TARGET_ALIGNMENT 8
#define SOTB_TARGET_PIXEL_BUDGET ((int64_t)3840 * 2160)
#define SOTB_SAFE_TARGET_PIXELS ((int64_t)2560 * 1440)

/* The render target of a resolution in an aspect ratio. 16:9 returns the resolution unchanged. Any other ratio keeps the
 * resolution's height, takes the width that ratio gives it aligned to SOTB_TARGET_ALIGNMENT, and loses height in steps of
 * that alignment until it fits SOTB_TARGET_PIXEL_BUDGET. */
SotbRenderTarget sotb_render_target(const SotbResolution *resolution, const SotbAspectRatio *aspect);

/* A target bigger than 2560 x 1440 pixels is the kind that needs the startup guard. */
int sotb_target_is_heavy(SotbRenderTarget target);

/* The resolution the guard falls back to for the ratio: 1440p when its target is not heavy, else the next largest. */
int sotb_safe_resolution_index(const SotbAspectRatio *aspect);

#define SOTB_LOADING_DESIGN_WIDTH 1920.0f
#define SOTB_LOADING_DESIGN_HEIGHT 1080.0f

typedef struct {
    float size_x, size_y;
    float scale;
    float abs_pos_x, abs_pos_y;
    float position_x, position_y;
    float render_m00, render_m01, render_m10, render_m11;
    float render_tx, render_ty;
    uint8_t has_render_xform;
    uint8_t pad[3];
} GuestSlateGeometry;

/* Every site is checked first; nothing is written unless all of them hold the original bytes of a supported build. */
int guest_apply_display_size(uint8_t *image, size_t image_size, int width, int height, char *error, size_t error_size);

GuestSlateGeometry guest_scale_loading_geometry(const GuestSlateGeometry *allotted);
int guest_apply_loading_screen_layout(uint8_t *image, size_t image_size, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif

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

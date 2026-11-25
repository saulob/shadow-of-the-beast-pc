#ifndef GUEST_HUD_H
#define GUEST_HUD_H
#include "guest_patches.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABeastHUD draws its components with Draw(component, hud, canvas), vtable slot 103; the entry sits this far after the
 * start of the class's vtable object. */
#define SOTB_HUD_DRAW_SLOT_OFFSET 0x348u
#define SOTB_HUD_POST_RENDER 0x292290u
#define SOTB_HUD_POST_RENDER_SIZE 0xa90u
#define SOTB_HUD_PUSH_TRANSFORM 0x1476f90u
#define SOTB_HUD_PUSH_TRANSFORM_SIZE 0x40u
#define SOTB_HUD_POP_TRANSFORM 0x1477380u
#define SOTB_HUD_POP_TRANSFORM_SIZE 0x50u
/* CutsceneSkip's fade: a black tile the size of the canvas, drawn by this call to UCanvas::DrawItem. */
#define SOTB_HUD_FADE_BLOCK 0x2e3700u
#define SOTB_HUD_FADE_CALL 0x2e37c0u
#define SOTB_HUD_DRAW_ITEM 0x147bf20u
#define SOTB_HUD_DRAW_ITEM_SIZE 0x10u
#define SOTB_HUD_FADE_STUB_SIZE 12u
#define SOTB_HUD_TILE_POSITION 0x08u
#define SOTB_HUD_TILE_SIZE 0x38u
#define SOTB_HUD_DESIGN_WIDTH 1920
#define SOTB_HUD_DESIGN_HEIGHT 1080

typedef enum { SOTB_HUD_LEFT = 0, SOTB_HUD_CENTRE = 1, SOTB_HUD_RIGHT = 2, SOTB_HUD_SIDE_GLOW = 3 } SotbHudAnchor;

typedef struct {
    int active;        /* false on 16:9 and narrower: the component is drawn as the game draws it */
    int override_size; /* the component sees a canvas of virtual_width and the scale below */
    float scale;
    int virtual_width;
    float shift_x, shift_y;
} SotbHudPlacement;

/* Where a component of the anchor group is drawn on a canvas of the given size: laid out as a 16:9 screen of the same
 * height and moved to the left edge, the centre or the right edge of the real one. */
SotbHudPlacement sotb_hud_placement(int anchor, int size_x, int size_y);

/* A full-canvas tile drawn by a component under `placement` on a canvas `real_width` wide: the position and size that make
 * it cover the real canvas once the placement's translation is applied. */
void sotb_hud_full_canvas_tile(const SotbHudPlacement *placement, int real_width, float position[2], float size[2]);

/* Needs the image after its relocations and a cave within 2 GB of it. Every function, call site and vtable slot involved
 * is checked first; nothing is written unless all of them are the supported ones. */
int guest_hud_apply(uint8_t *image, size_t image_size, uint8_t *cave, size_t cave_size, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif

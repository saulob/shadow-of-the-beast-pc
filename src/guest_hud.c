#include "guest_hud.h"
#include <stdio.h>
#include <string.h>

#define HUD_ANCHOR_LEFT SOTB_HUD_LEFT
#define HUD_ANCHOR_CENTRE SOTB_HUD_CENTRE
#define HUD_ANCHOR_RIGHT SOTB_HUD_RIGHT
#define HUD_ANCHOR_SIDE_GLOW SOTB_HUD_SIDE_GLOW
#define DRAW_CHECKED_SIZE 0x80u
#define HUD_CANVAS 0x340u
#define HUD_SCALE 0x518u
#define CANVAS_SIZE_X 0x40u
#define CANVAS_SIZE_Y 0x44u
#define CANVAS_FCANVAS 0x90u
#define POST_RENDER_FNV64 0x6c88052279a3ed43ull
#define PUSH_TRANSFORM_FNV64 0x85934f14a3d61d39ull
#define POP_TRANSFORM_FNV64 0x873180fa765d2804ull
#define FADE_BLOCK_FNV64 0xba922ed3c5adf52bull
#define DRAW_ITEM_FNV64 0xeda14952b9cfff89ull

typedef void (GUEST_ABI *DrawFn)(void *component, void *hud, void *canvas);
typedef void (GUEST_ABI *PushTransformFn)(void *fcanvas, const float *matrix);
typedef void (GUEST_ABI *PopTransformFn)(void *fcanvas);
typedef void (GUEST_ABI *DrawItemFn)(void *canvas, void *item);

typedef struct {
    uint32_t vtable_object, draw;
    int anchor;
    uint64_t fnv;
    DrawFn original;
} HudDraw;

static HudDraw hud_draws[] = {
    {0x3c26a60u, 0x2e7830u, HUD_ANCHOR_LEFT, 0xf448687e01e5b6c3ull, NULL},
    {0x3c285b0u, 0x2f4070u, HUD_ANCHOR_LEFT, 0x680783859e1bfb6cull, NULL},
    {0x3c2ab00u, 0x303880u, HUD_ANCHOR_LEFT, 0xfd39277b11286bdeull, NULL},
    {0x3c28d30u, 0x2f83f0u, HUD_ANCHOR_RIGHT, 0xdd44994aa875612cull, NULL},
    {0x3c276e0u, 0x2ed800u, HUD_ANCHOR_RIGHT, 0x5cf3aed56b3c4917ull, NULL},
    {0x3c29840u, 0x2fb900u, HUD_ANCHOR_RIGHT, 0x717a61f0ac7a377dull, NULL},
    {0x3c29c00u, 0x2fde50u, HUD_ANCHOR_RIGHT, 0xc5ed6137caabcde5ull, NULL},
    {0x3c28970u, 0x2f7550u, HUD_ANCHOR_RIGHT, 0x902ec2996aaf7288ull, NULL},
    {0x3c2ba00u, 0x307e40u, HUD_ANCHOR_RIGHT, 0xd5284174aaf45893ull, NULL},
    {0x3c29480u, 0x2fab50u, HUD_ANCHOR_RIGHT, 0x8b5b0054ca920d92ull, NULL},
    {0x3c2aec0u, 0x304410u, HUD_ANCHOR_RIGHT, 0x153a8d22750b25e2ull, NULL},
    {0x3c25b60u, 0x2e3680u, HUD_ANCHOR_RIGHT, 0x354172703e7a0803ull, NULL},
    {0x3c27e60u, 0x2f0600u, HUD_ANCHOR_RIGHT, 0xb369386a20fed303ull, NULL},
    {0x3c2b640u, 0x306ed0u, HUD_ANCHOR_CENTRE, 0xa179179cf969baceull, NULL},
    {0x3c29fc0u, 0x2ffc30u, HUD_ANCHOR_CENTRE, 0x289f12e514a497c5ull, NULL},
    {0x3c2b280u, 0x3059b0u, HUD_ANCHOR_CENTRE, 0x54ff131375f909caull, NULL},
    {0x3c262e0u, 0x2e5010u, HUD_ANCHOR_CENTRE, 0x29fcad93404afe56ull, NULL},
    {0x3c27aa0u, 0x2eeb80u, HUD_ANCHOR_CENTRE, 0xe938a9d5babb18c4ull, NULL},
    {0x3c24670u, 0x28f360u, HUD_ANCHOR_CENTRE, 0x7db673bc648fc439ull, NULL},
    {0x3c2c540u, 0x30afd0u, HUD_ANCHOR_CENTRE, 0x7f89924eb6fb9635ull, NULL},
    {0x3c24a30u, 0x2902a0u, HUD_ANCHOR_CENTRE, 0xdd5ac20a75b0c0cdull, NULL},
    {0x3c2bdc0u, 0x308e70u, HUD_ANCHOR_CENTRE, 0x1db317bcd74edd00ull, NULL},
    {0x3c2c180u, 0x309fc0u, HUD_ANCHOR_CENTRE, 0x1df3951d2d854548ull, NULL},
    {0x3c2a740u, 0x301bb0u, HUD_ANCHOR_SIDE_GLOW, 0x34a86389745aab58ull, NULL},
};

#define HUD_DRAW_COUNT (sizeof(hud_draws) / sizeof(*hud_draws))

static uint8_t *hud_image;

typedef struct {
    SotbHudPlacement placement;
    const uint8_t *canvases[2];
    int real_widths[2];
} ActivePlacement;

static ActivePlacement active_placement;

static int fail(char *error, size_t size, const char *reason) {
    if (error && size) snprintf(error, size, "%s", reason);
    return 0;
}

static uint64_t fnv1a64(const uint8_t *data, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

static int virtual_width(int size_y) {
    return (size_y * 16 + 4) / 9;
}

SotbHudPlacement sotb_hud_placement(int anchor, int size_x, int size_y) {
    SotbHudPlacement placement = {0, 0, 1.0f, size_x, 0.0f, 0.0f};
    if (size_x <= 0 || size_y <= 0) return placement;
    const int width = virtual_width(size_y);
    if ((int64_t)size_x * 100 <= (int64_t)width * 101) return placement;
    placement.active = 1;
    if (anchor == SOTB_HUD_SIDE_GLOW) {
        placement.shift_y = (float)size_y * 0.5f - (float)SOTB_HUD_DESIGN_HEIGHT * 0.5f * (float)size_x / SOTB_HUD_DESIGN_WIDTH;
        return placement;
    }
    placement.override_size = 1;
    placement.scale = (float)size_y / SOTB_HUD_DESIGN_HEIGHT;
    placement.virtual_width = width;
    if (anchor == SOTB_HUD_CENTRE) placement.shift_x = (float)(size_x - width) * 0.5f;
    if (anchor == SOTB_HUD_RIGHT) placement.shift_x = (float)(size_x - width);
    return placement;
}

void sotb_hud_full_canvas_tile(const SotbHudPlacement *placement, int real_width, float position[2], float size[2]) {
    if (!placement->active || !placement->override_size || placement->virtual_width <= 0) return;
    const float stretch = (float)real_width / (float)placement->virtual_width;
    position[0] = position[0] * stretch - placement->shift_x;
    position[1] -= placement->shift_y;
    size[0] *= stretch;
}

static GUEST_ABI void hud_fade_draw(void *canvas, uint8_t *item) {
    const ActivePlacement *active = &active_placement;
    for (int i = 0; item && i < 2; ++i) {
        if (!active->placement.active || active->canvases[i] != canvas) continue;
        float position[2], size[2];
        memcpy(position, item + SOTB_HUD_TILE_POSITION, sizeof(position));
        memcpy(size, item + SOTB_HUD_TILE_SIZE, sizeof(size));
        sotb_hud_full_canvas_tile(&active->placement, active->real_widths[i], position, size);
        memcpy(item + SOTB_HUD_TILE_POSITION, position, sizeof(position));
        memcpy(item + SOTB_HUD_TILE_SIZE, size, sizeof(size));
        break;
    }
    ((DrawItemFn)(uintptr_t)(hud_image + SOTB_HUD_DRAW_ITEM))(canvas, item);
}

static const HudDraw *find_draw(const void *component) {
    uint64_t vtable = 0;
    if (!component || !hud_image) return NULL;
    memcpy(&vtable, component, sizeof(vtable));
    for (size_t i = 0; i < HUD_DRAW_COUNT; ++i) {
        if (vtable == (uint64_t)(uintptr_t)(hud_image + hud_draws[i].vtable_object + 0x10u)) return &hud_draws[i];
    }
    return NULL;
}

typedef struct {
    uint8_t *canvas;
    void *fcanvas;
    int size_x;
    int pushed;
} CanvasState;

static void enter_canvas(CanvasState *state, uint8_t *canvas, const SotbHudPlacement *placement, const float *matrix, int shifted) {
    memcpy(&state->size_x, canvas + CANVAS_SIZE_X, sizeof(int));
    memcpy(&state->fcanvas, canvas + CANVAS_FCANVAS, sizeof(void *));
    state->canvas = canvas;
    state->pushed = 0;
    if (placement->override_size) memcpy(canvas + CANVAS_SIZE_X, &placement->virtual_width, sizeof(int));
    if (shifted && state->fcanvas) {
        ((PushTransformFn)(uintptr_t)(hud_image + SOTB_HUD_PUSH_TRANSFORM))(state->fcanvas, matrix);
        state->pushed = 1;
    }
}

static void leave_canvas(const CanvasState *state) {
    if (state->pushed) ((PopTransformFn)(uintptr_t)(hud_image + SOTB_HUD_POP_TRANSFORM))(state->fcanvas);
    memcpy(state->canvas + CANVAS_SIZE_X, &state->size_x, sizeof(int));
}

static GUEST_ABI void hud_component_draw(void *component, void *hud, void *canvas) {
    const HudDraw *draw = find_draw(component);
    if (!draw) return;
    uint8_t *const hud_bytes = (uint8_t *)hud;
    uint8_t *main_canvas = NULL, *second_canvas = (uint8_t *)canvas;
    if (hud_bytes) memcpy(&main_canvas, hud_bytes + HUD_CANVAS, sizeof(main_canvas));
    SotbHudPlacement placement = {0};
    if (main_canvas) {
        int size_x = 0, size_y = 0;
        memcpy(&size_x, main_canvas + CANVAS_SIZE_X, sizeof(int));
        memcpy(&size_y, main_canvas + CANVAS_SIZE_Y, sizeof(int));
        placement = sotb_hud_placement(draw->anchor, size_x, size_y);
    }
    if (!placement.active) {
        draw->original(component, hud, canvas);
        return;
    }
    _Alignas(16) float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, placement.shift_x, placement.shift_y, 0, 1};
    const int shifted = placement.shift_x != 0.0f || placement.shift_y != 0.0f;
    float saved_scale = 0.0f;
    memcpy(&saved_scale, hud_bytes + HUD_SCALE, sizeof(saved_scale));
    if (placement.override_size) memcpy(hud_bytes + HUD_SCALE, &placement.scale, sizeof(placement.scale));

    CanvasState states[2];
    int count = 0;
    enter_canvas(&states[count++], main_canvas, &placement, matrix, shifted);
    if (second_canvas && second_canvas != main_canvas) enter_canvas(&states[count++], second_canvas, &placement, matrix, shifted);
    active_placement = (ActivePlacement){placement, {states[0].canvas, count > 1 ? states[1].canvas : NULL},
                                         {states[0].size_x, count > 1 ? states[1].size_x : 0}};
    draw->original(component, hud, canvas);
    active_placement = (ActivePlacement){0};
    while (count > 0) leave_canvas(&states[--count]);
    memcpy(hud_bytes + HUD_SCALE, &saved_scale, sizeof(saved_scale));
}

static int fade_call_supported(const uint8_t *image) {
    static const uint8_t arguments[] = {0x4c, 0x89, 0xff, 0x48, 0x89, 0xde, 0xe8};
    const uint8_t *call = image + SOTB_HUD_FADE_CALL;
    int32_t rel = 0;
    memcpy(&rel, call + 1, sizeof(rel));
    return !memcmp(call - 6, arguments, sizeof(arguments)) &&
           (int64_t)SOTB_HUD_FADE_CALL + 5 + rel == (int64_t)SOTB_HUD_DRAW_ITEM &&
           fnv1a64(image + SOTB_HUD_FADE_BLOCK, SOTB_HUD_FADE_CALL + 5 - SOTB_HUD_FADE_BLOCK) == FADE_BLOCK_FNV64 &&
           fnv1a64(image + SOTB_HUD_DRAW_ITEM, SOTB_HUD_DRAW_ITEM_SIZE) == DRAW_ITEM_FNV64;
}

int guest_hud_apply(uint8_t *image, size_t image_size, uint8_t *cave, size_t cave_size, char *error, size_t error_size) {
    if (!image || !cave) return fail(error, error_size, "missing image or cave");
    if (cave_size < SOTB_HUD_FADE_STUB_SIZE) return fail(error, error_size, "cave too small");
    if ((uint64_t)SOTB_HUD_POST_RENDER + SOTB_HUD_POST_RENDER_SIZE > image_size ||
        (uint64_t)SOTB_HUD_DRAW_ITEM + SOTB_HUD_DRAW_ITEM_SIZE > image_size || (uint64_t)SOTB_HUD_FADE_CALL + 5 > image_size ||
        (uint64_t)SOTB_HUD_PUSH_TRANSFORM + SOTB_HUD_PUSH_TRANSFORM_SIZE > image_size ||
        (uint64_t)SOTB_HUD_POP_TRANSFORM + SOTB_HUD_POP_TRANSFORM_SIZE > image_size) {
        return fail(error, error_size, "image too short");
    }
    if (fnv1a64(image + SOTB_HUD_POST_RENDER, SOTB_HUD_POST_RENDER_SIZE) != POST_RENDER_FNV64 ||
        fnv1a64(image + SOTB_HUD_PUSH_TRANSFORM, SOTB_HUD_PUSH_TRANSFORM_SIZE) != PUSH_TRANSFORM_FNV64 ||
        fnv1a64(image + SOTB_HUD_POP_TRANSFORM, SOTB_HUD_POP_TRANSFORM_SIZE) != POP_TRANSFORM_FNV64) {
        return fail(error, error_size, "HUD canvas code does not match a supported build");
    }
    if (!fade_call_supported(image)) return fail(error, error_size, "cutscene fade does not match a supported build");
    const intptr_t fade_rel = cave - (image + SOTB_HUD_FADE_CALL + 5);
    if (fade_rel < INT32_MIN || fade_rel > INT32_MAX) return fail(error, error_size, "cave out of 32-bit call range");
    for (size_t i = 0; i < HUD_DRAW_COUNT; ++i) {
        const HudDraw *draw = &hud_draws[i];
        const uint64_t slot_at = (uint64_t)draw->vtable_object + SOTB_HUD_DRAW_SLOT_OFFSET;
        if (slot_at + sizeof(uint64_t) > image_size || (uint64_t)draw->draw + DRAW_CHECKED_SIZE > image_size) {
            return fail(error, error_size, "image too short");
        }
        uint64_t entry = 0;
        memcpy(&entry, image + slot_at, sizeof(entry));
        if (entry != (uint64_t)(uintptr_t)(image + draw->draw) || fnv1a64(image + draw->draw, DRAW_CHECKED_SIZE) != draw->fnv) {
            return fail(error, error_size, "HUD component does not match a supported build");
        }
    }
    hud_image = image;
    const uint64_t handler = (uint64_t)(uintptr_t)hud_fade_draw;
    cave[0] = 0x48;
    cave[1] = 0xb8;
    memcpy(cave + 2, &handler, sizeof(handler));
    cave[10] = 0xff;
    cave[11] = 0xe0;
    const int32_t fade_call = (int32_t)fade_rel;
    memcpy(image + SOTB_HUD_FADE_CALL + 1, &fade_call, sizeof(fade_call));
    const uint64_t hook = (uint64_t)(uintptr_t)hud_component_draw;
    for (size_t i = 0; i < HUD_DRAW_COUNT; ++i) {
        hud_draws[i].original = (DrawFn)(uintptr_t)(image + hud_draws[i].draw);
        memcpy(image + hud_draws[i].vtable_object + SOTB_HUD_DRAW_SLOT_OFFSET, &hook, sizeof(hook));
    }
    return 1;
}

#ifndef GUEST_ULTRAWIDE_H
#define GUEST_ULTRAWIDE_H
#include "guest_patches.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FMinimalViewInfo::CalculateProjectionMatrixGivenView(view, axis constraint, viewport, projection data). */
#define SOTB_ULTRAWIDE_SITE 0x127fd80u
#define SOTB_ULTRAWIDE_SIZE 1200u
#define SOTB_ULTRAWIDE_PATCH_SIZE 6u
#define SOTB_ULTRAWIDE_MIN_CAVE_SIZE 64u

#define SOTB_VIEW_FOV 0x18u
#define SOTB_VIEW_ASPECT_RATIO 0x28u
#define SOTB_VIEW_CONSTRAIN 0x2cu
#define SOTB_VIEW_PROJECTION_MODE 0x2du
#define SOTB_PROJECTION_VIEW_RECT 0x90u

/* SGameLayerManager's vtable: every widget the game puts on screen is arranged inside it. Slot 62 is
 * SCompoundWidget::OnArrangeChildren, which it inherits. */
#define SOTB_GAME_LAYER_CONSTRUCTOR 0x145b290u
#define SOTB_GAME_LAYER_VTABLE 0x3d136a0u
#define SOTB_GAME_LAYER_ARRANGE_SLOT 62u

/* How long a widened view keeps the safe area on after the last one. Only gameplay and what shares its world use it. */
#define SOTB_SAFE_AREA_HOLD_MS 750u

/* UFrontendManager: the game's own record of which frontend screens are open. Its screen stack is a TArray of 16-byte
 * entries (the FrontendScreen byte, then the widget). Slot 80 of its vtable runs every frame. */
#define SOTB_FRONTEND_CONSTRUCTOR 0x190d50u
#define SOTB_FRONTEND_CONSTRUCTOR_SIZE 0x152u
#define SOTB_FRONTEND_TOP_WIDGET 0x196c60u
#define SOTB_FRONTEND_TOP_WIDGET_SIZE 0x56u
#define SOTB_FRONTEND_TICK 0x196cc0u
#define SOTB_FRONTEND_TICK_SIZE 0x400u
#define SOTB_FRONTEND_VTABLE 0x3c0a290u
#define SOTB_FRONTEND_TICK_SLOT 80u
#define SOTB_FRONTEND_STACK_DATA 0x280u
#define SOTB_FRONTEND_STACK_NUM 0x288u
#define SOTB_FRONTEND_STACK_ENTRY 16u

/* FrontendScreen values, in the order the game registers them. */
#define SOTB_SCREEN_MAIN_MENU 2
#define SOTB_SCREEN_MAP 3
#define SOTB_SCREEN_OPTIONS 4
#define SOTB_SCREEN_MAP_LIVE_FEED 14
#define SOTB_SCREEN_MAP_CONFIRMATION 27
#define SOTB_SCREEN_MAP_INFO 29
#define SOTB_SCREEN_TIMELINE_INFO 28
#define SOTB_SCREEN_SHOP_CONTAINER 30 /* Wisdom of Shadows */
#define SOTB_SCREEN_MANA_OVERLAY 32

typedef enum {
    SOTB_CONTEXT_OTHER = 0, /* gameplay, pause and everything not named below */
    SOTB_CONTEXT_TITLE = 1, /* the main menu screen, which carries the title and START */
    SOTB_CONTEXT_MAP = 2,   /* the map and the screens that open over it */
    SOTB_CONTEXT_MENU = 3   /* options and Wisdom of Shadows: centred content, the scene behind them untouched */
} SotbFrontendContext;

/* Hor+ is for the two ultrawide shapes only. */
int sotb_ultrawide_wanted(const char *aspect_key);

/* The two shapes with the setting on. */
int sotb_ultrawide_enabled(const char *aspect_key, int true_ultrawide);

/* The horizontal field of view, in degrees, that keeps the vertical one of `fov_degrees` at `original_aspect` when the
 * view is `target_aspect` wide. */
double sotb_ultrawide_fov(double fov_degrees, double original_aspect, double target_aspect);

/* Runs at the entry of the projection function: a constrained perspective view gets the viewport's own aspect ratio
 * and the matching horizontal field of view. Everything else is left as it is. */
void GUEST_ABI sotb_ultrawide_adjust_view(uint8_t *view_info, const uint8_t *projection_data);

/* The frontend context of a FrontendScreen value (the top of the screen stack). */
int sotb_frontend_context_of_screen(int screen);

/* The context the last frame of the frontend manager reported; SOTB_CONTEXT_OTHER until it has ticked, and again
 * after the loading screen. */
int sotb_frontend_context(void);

/* Whether the game's widgets lay out in the centered 16:9 safe area: always on the title, the map and the menus, and
 * on everything else while a widened view was drawn within the hold time (gameplay). */
int sotb_ultrawide_use_safe_area(int context, int widened_recently);

/* A widened view was drawn within the hold time. */
int sotb_ultrawide_safe_area_active(void);

/* The clock the hold time reads, in milliseconds; null restores the system's. */
void sotb_ultrawide_set_clock(uint64_t (*clock_ms)(void));

/* Turns on the side fills: `mark_loading` is called each time the loading screen is arranged wider than 16:9, and
 * `set_frontend` is told the SotbFrontendContext whenever it changes. Null turns them off. */
void sotb_ultrawide_enable_side_fill(void (*mark_loading)(void), void (*set_frontend)(int context));

/* Every byte of the function is checked first; nothing is written unless it is the supported one. */
int guest_ultrawide_apply(uint8_t *image, size_t image_size, uint8_t *cave, size_t cave_size, char *error,
                          size_t error_size);

/* Needs the image after its relocations: the constructor and the vtable are checked before the slot is replaced. */
int guest_ultrawide_apply_safe_area(uint8_t *image, size_t image_size, char *error, size_t error_size);

/* Needs the image after its relocations: the manager's constructor, its stack reader and its per-frame function are
 * checked before the vtable slot is replaced. */
int guest_ultrawide_apply_frontend(uint8_t *image, size_t image_size, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif

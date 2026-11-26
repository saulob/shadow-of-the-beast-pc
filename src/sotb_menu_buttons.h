#ifndef SOTB_MENU_BUTTONS_H
#define SOTB_MENU_BUTTONS_H

#include <stddef.h>
#include <stdint.h>

#include "patch_sites.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOTB_MENU_BUTTON_LAYOUT_KEY "menu_button_layout"
#define SOTB_MENU_BUTTON_LAYOUT_ENV "SOTB_MENU_BUTTON_LAYOUT"

typedef enum SotbMenuButtonLayout {
    SOTB_MENU_BUTTON_LAYOUT_WESTERN = 0,
    SOTB_MENU_BUTTON_LAYOUT_ORIGINAL = 1,
} SotbMenuButtonLayout;

SotbMenuButtonLayout sotb_menu_button_layout(const char *settings_file);

int sotb_menu_buttons_supported(const char *build_key);

/* Patches the region branch for Western; leaves the image untouched for Original. */
int sotb_menu_buttons_apply(uint8_t *image, size_t image_size, const char *build_key, SotbMenuButtonLayout layout);

/* The image checks of sotb_menu_buttons_apply for the build's table, without writing or printing. */
int sotb_menu_buttons_check(const uint8_t *image, size_t image_size, const char *build_key, char *error, size_t error_size);
/* The layout code, then the four key sites; 0 for an unknown build or fewer than 5 slots. */
size_t sotb_menu_buttons_sites(const char *build_key, SotbPatchSite *sites, size_t capacity);

/* True when the active build + layout uses Circle/B to confirm (only Korea Original). */
int sotb_menu_buttons_effective_circle_confirm(const char *build_key, SotbMenuButtonLayout layout);

#ifdef __cplusplus
}
#endif

#endif

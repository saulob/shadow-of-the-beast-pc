#ifndef SOTB_MENU_BUTTONS_H
#define SOTB_MENU_BUTTONS_H

#include <stddef.h>
#include <stdint.h>

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

/* Patches the CUSA03762-1.00 region branch for Western; leaves the image untouched for Original. */
int sotb_menu_buttons_apply(uint8_t *image, size_t image_size, const char *build_key, SotbMenuButtonLayout layout);

#ifdef __cplusplus
}
#endif

#endif

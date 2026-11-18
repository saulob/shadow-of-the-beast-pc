/* Keyboard bindings: the actions, their defaults and the saved key names, shared by the launcher, the runtime pad
 * and the video core. Keys are SDL scancodes, saved as the shadPS4 names (space, lshift, f10, pgup...) or SDL's own
 * lowercase names for the rest. */
#ifndef SOTB_KEYS_H
#define SOTB_KEYS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOTB_KEY_LEFT_STICK_UP, SOTB_KEY_LEFT_STICK_DOWN, SOTB_KEY_LEFT_STICK_LEFT, SOTB_KEY_LEFT_STICK_RIGHT,
    SOTB_KEY_RIGHT_STICK_UP, SOTB_KEY_RIGHT_STICK_DOWN, SOTB_KEY_RIGHT_STICK_LEFT, SOTB_KEY_RIGHT_STICK_RIGHT,
    SOTB_KEY_CROSS, SOTB_KEY_CIRCLE, SOTB_KEY_SQUARE, SOTB_KEY_TRIANGLE,
    SOTB_KEY_L1, SOTB_KEY_R1, SOTB_KEY_L2, SOTB_KEY_R2, SOTB_KEY_L3, SOTB_KEY_R3,
    SOTB_KEY_DPAD_UP, SOTB_KEY_DPAD_DOWN, SOTB_KEY_DPAD_LEFT, SOTB_KEY_DPAD_RIGHT,
    SOTB_KEY_OPTIONS, SOTB_KEY_TOUCHPAD_LEFT, SOTB_KEY_TOUCHPAD_RIGHT,
    SOTB_KEY_HOTKEY_WINDOW_MODE,
    SOTB_KEY_COUNT
} SotbKeyAction;

#define SOTB_KEY_FIRST_HOTKEY SOTB_KEY_HOTKEY_WINDOW_MODE

typedef struct {
    const char *ini_key; /* the line in Sotb.ini */
    const char *label;   /* the launcher's row */
    int default_scancode;
} SotbKeyInfo;

extern const SotbKeyInfo sotb_key_actions[SOTB_KEY_COUNT];

typedef struct { int scancode[SOTB_KEY_COUNT]; } SotbKeyMap;

void sotb_keys_defaults(SotbKeyMap *map);

/* The scancode a saved name stands for (case-insensitive), or 0 when it names no bindable key. */
int sotb_key_from_name(const char *name);
/* False for no key and for Escape, which cancels a capture. */
int sotb_key_bindable(int scancode);
/* The name saved for a scancode, written to `buffer`; empty when the key is not bindable. */
const char *sotb_key_name(int scancode, char *buffer, size_t size);

/* Applies one saved line to the map; false when the key is not an action or the value names no bindable key. */
int sotb_keys_apply(SotbKeyMap *map, const char *ini_key, const char *value);
/* Defaults, then every valid binding in the Sotb.ini at `path`; a missing file or line keeps the default. */
void sotb_keys_load(const char *path, SotbKeyMap *map);

#ifdef __cplusplus
}
#endif

#endif

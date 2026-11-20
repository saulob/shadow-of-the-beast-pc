/* Controller bindings: which physical gamepad button or trigger presses each PS4 button the game reads. Shared by
 * the launcher, which edits them, and the runtime pad, which applies them to every connected gamepad. A source is
 * saved as SDL's gamepad name (a, leftshoulder, dpup, lefttrigger...). Sticks stay analog and are not rebindable.
 * One physical source may press several PS4 buttons; nothing is removed from another binding. */
#ifndef SOTB_PAD_H
#define SOTB_PAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The order is the PS4 button order of the launcher's rows. */
typedef enum {
    SOTB_PAD_CROSS, SOTB_PAD_CIRCLE, SOTB_PAD_SQUARE, SOTB_PAD_TRIANGLE,
    SOTB_PAD_L1, SOTB_PAD_R1, SOTB_PAD_L2, SOTB_PAD_R2, SOTB_PAD_L3, SOTB_PAD_R3,
    SOTB_PAD_OPTIONS, SOTB_PAD_TOUCHPAD,
    SOTB_PAD_DPAD_UP, SOTB_PAD_DPAD_DOWN, SOTB_PAD_DPAD_LEFT, SOTB_PAD_DPAD_RIGHT,
    SOTB_PAD_COUNT
} SotbPadAction;

/* A source below 32 is an SDL_GamepadButton; the triggers are axes in SDL and have their own values. */
#define SOTB_PAD_SOURCE_NONE (-1)
#define SOTB_PAD_SOURCE_LEFT_TRIGGER 0x100
#define SOTB_PAD_SOURCE_RIGHT_TRIGGER 0x101
/* A trigger counts as pressed above this value (0..255). */
#define SOTB_PAD_TRIGGER_PRESS 30

typedef struct {
    const char *ini_key;  /* the line in Sotb.ini */
    const char *label;    /* the launcher's row */
    const char *name;     /* the PS4 button in SOTB_PAD_MAP */
    int default_source;
} SotbPadInfo;

extern const SotbPadInfo sotb_pad_actions[SOTB_PAD_COUNT];

typedef struct { int source[SOTB_PAD_COUNT]; } SotbPadMap;

void sotb_pad_defaults(SotbPadMap *map);

/* The source a saved name stands for (case-insensitive), or SOTB_PAD_SOURCE_NONE. The Guide button belongs to the
 * system and is not bindable. */
int sotb_pad_source_from_name(const char *name);
int sotb_pad_source_bindable(int source);
/* The name saved for a source; empty when it is not bindable. */
const char *sotb_pad_source_name(int source, char *buffer, size_t size);

/* Applies one saved line; false when the key is not a controller binding or the value names no bindable source. */
int sotb_pad_apply(SotbPadMap *map, const char *ini_key, const char *value);
/* Defaults, then every valid binding in the Sotb.ini at `path`; a missing file or line keeps the default. */
void sotb_pad_load(const char *path, SotbPadMap *map);
/* Applies a SOTB_PAD_MAP value ("cross=b,circle=a") on top of the map; returns the entries used. Bad entries are
 * logged and skipped. */
int sotb_pad_apply_spec(SotbPadMap *map, const char *spec);
/* How many actions are bound to something other than their default. */
int sotb_pad_changed(const SotbPadMap *map);
/* How many actions the source presses. More than one is allowed. */
int sotb_pad_source_users(const SotbPadMap *map, int source);

/* One sample of a gamepad: a bit per pressed SDL_GamepadButton and the trigger values (0..255). */
typedef struct { uint32_t buttons; uint8_t left_trigger, right_trigger; } SotbPadInput;
/* The PS4 side: a bit per pressed action, and the L2/R2 values the game reads. */
typedef struct { uint32_t pressed; uint8_t l2, r2; } SotbPadOutput;
void sotb_pad_translate(const SotbPadMap *map, const SotbPadInput *in, SotbPadOutput *out);
/* The bindable source held in a sample, or SOTB_PAD_SOURCE_NONE; a trigger counts when pulled about halfway. */
int sotb_pad_first_pressed(const SotbPadInput *in);

#ifdef __cplusplus
}
#endif

#endif

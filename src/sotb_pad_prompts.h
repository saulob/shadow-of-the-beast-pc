/* Which controller is connected and which button prompts suit it. Detection and the prompt mode (Auto, Xbox or
 * PlayStation) are host-side: they pick the launcher's button names and are logged by the runtime. The game's own
 * prompt art is baked into its paks and is not changed here, and the PS4 buttons the game receives never depend on
 * the prompt family. */
#ifndef SOTB_PAD_PROMPTS_H
#define SOTB_PAD_PROMPTS_H

#include <stddef.h>
#include <stdint.h>

#include "sotb_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOTB_PAD_KIND_UNKNOWN, SOTB_PAD_KIND_XBOX, SOTB_PAD_KIND_DUALSHOCK3, SOTB_PAD_KIND_DUALSHOCK4,
    SOTB_PAD_KIND_DUALSENSE, SOTB_PAD_KIND_GENERIC
} SotbPadKind;

typedef enum { SOTB_PROMPT_AUTO, SOTB_PROMPT_XBOX, SOTB_PROMPT_PLAYSTATION } SotbPromptMode;
typedef enum { SOTB_PROMPT_FAMILY_XBOX, SOTB_PROMPT_FAMILY_PLAYSTATION } SotbPromptFamily;

/* `type` and `real_type` are SDL_GamepadType values; the real one is the device behind Steam Input or another
 * virtual layer and wins when it is known. Vendor and product only decide when SDL knows no type. */
SotbPadKind sotb_pad_classify(int type, int real_type, unsigned vendor, unsigned product);
int sotb_pad_is_steam_virtual(unsigned vendor, unsigned product);
const char *sotb_pad_kind_name(SotbPadKind kind);

/* A saved prompt mode; anything unknown is Auto. */
SotbPromptMode sotb_prompt_mode_from_name(const char *name);
const char *sotb_prompt_mode_name(SotbPromptMode mode);
/* A manual mode always wins. Auto follows the controller: PlayStation pads show PlayStation prompts, everything
 * else, an unknown or generic pad and no pad at all included, shows Xbox prompts (the layout SDL names). */
SotbPromptFamily sotb_prompt_family(SotbPromptMode mode, SotbPadKind kind);
const char *sotb_prompt_family_name(SotbPromptFamily family);

/* The launcher's text for a bindable source in a family, e.g. "A" or "Cross", "LB" or "L1". */
const char *sotb_pad_source_label(int source, SotbPromptFamily family);

/* The button the game treats as Confirm and Back. The game asks the system for its enter button and gets Cross,
 * the western assignment; the prompt family never changes it. */
int sotb_prompt_enter_button_assign(void);
SotbPadAction sotb_prompt_confirm_action(void);
SotbPadAction sotb_prompt_back_action(void);

/* The controller prompts follow: the active one, not the first listed. A different controller becomes active only
 * after it was the one pressed for SOTB_ACTIVE_PAD_SWITCH_HITS updates in a row, so a stray duplicate or virtual
 * device does not make the family flap. The last kind is kept while nothing is connected. */
#define SOTB_ACTIVE_PAD_SWITCH_HITS 3
typedef struct { uint32_t id; SotbPadKind kind; } SotbPadDevice;
typedef struct {
    uint32_t active_id;
    SotbPadKind kind;
    uint32_t candidate_id;
    int candidate_hits;
} SotbActivePad;

void sotb_active_pad_init(SotbActivePad *pad);
/* One update with the connected devices and the id of the device that had input since the last one (0: none).
 * Returns the kind of the active controller. */
SotbPadKind sotb_active_pad_update(SotbActivePad *pad, const SotbPadDevice *devices, size_t count, uint32_t input_id);

#ifdef __cplusplus
}
#endif

#endif

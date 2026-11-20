#include "sotb_pad_prompts.h"

#include <SDL3/SDL.h>
#include <string.h>

#define VENDOR_SONY 0x054c
#define VENDOR_MICROSOFT 0x045e
#define VENDOR_VALVE 0x28de
#define PRODUCT_STEAM_VIRTUAL 0x11ff

static SotbPadKind kind_from_type(int type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return SOTB_PAD_KIND_XBOX;
    case SDL_GAMEPAD_TYPE_PS3: return SOTB_PAD_KIND_DUALSHOCK3;
    case SDL_GAMEPAD_TYPE_PS4: return SOTB_PAD_KIND_DUALSHOCK4;
    case SDL_GAMEPAD_TYPE_PS5: return SOTB_PAD_KIND_DUALSENSE;
    case SDL_GAMEPAD_TYPE_STANDARD: case SDL_GAMEPAD_TYPE_UNKNOWN: return SOTB_PAD_KIND_UNKNOWN;
    default: return SOTB_PAD_KIND_GENERIC;
    }
}

int sotb_pad_is_steam_virtual(unsigned vendor, unsigned product) {
    return vendor==VENDOR_VALVE && product==PRODUCT_STEAM_VIRTUAL;
}

SotbPadKind sotb_pad_classify(int type, int real_type, unsigned vendor, unsigned product) {
    SotbPadKind kind=kind_from_type(real_type);
    if (kind!=SOTB_PAD_KIND_UNKNOWN) return kind;
    kind=kind_from_type(type);
    if (kind!=SOTB_PAD_KIND_UNKNOWN) return kind;
    if (sotb_pad_is_steam_virtual(vendor,product) || vendor==VENDOR_MICROSOFT) return SOTB_PAD_KIND_XBOX;
    if (vendor==VENDOR_SONY) {
        switch (product) {
        case 0x0268: return SOTB_PAD_KIND_DUALSHOCK3;
        case 0x05c4: case 0x09cc: case 0x0ba0: return SOTB_PAD_KIND_DUALSHOCK4;
        case 0x0ce6: case 0x0df2: return SOTB_PAD_KIND_DUALSENSE;
        default: break;
        }
    }
    return SOTB_PAD_KIND_UNKNOWN;
}

const char *sotb_pad_kind_name(SotbPadKind kind) {
    switch (kind) {
    case SOTB_PAD_KIND_XBOX: return "Xbox";
    case SOTB_PAD_KIND_DUALSHOCK3: return "DualShock 3";
    case SOTB_PAD_KIND_DUALSHOCK4: return "DualShock 4";
    case SOTB_PAD_KIND_DUALSENSE: return "DualSense";
    case SOTB_PAD_KIND_GENERIC: return "generic";
    default: return "unknown";
    }
}

SotbPromptMode sotb_prompt_mode_from_name(const char *name) {
    if (name && !SDL_strcasecmp(name,"xbox")) return SOTB_PROMPT_XBOX;
    if (name && !SDL_strcasecmp(name,"playstation")) return SOTB_PROMPT_PLAYSTATION;
    return SOTB_PROMPT_AUTO;
}

const char *sotb_prompt_mode_name(SotbPromptMode mode) {
    return mode==SOTB_PROMPT_XBOX ? "xbox" : mode==SOTB_PROMPT_PLAYSTATION ? "playstation" : "auto";
}

SotbPromptFamily sotb_prompt_family(SotbPromptMode mode, SotbPadKind kind) {
    if (mode==SOTB_PROMPT_XBOX) return SOTB_PROMPT_FAMILY_XBOX;
    if (mode==SOTB_PROMPT_PLAYSTATION) return SOTB_PROMPT_FAMILY_PLAYSTATION;
    return kind==SOTB_PAD_KIND_DUALSHOCK3 || kind==SOTB_PAD_KIND_DUALSHOCK4 || kind==SOTB_PAD_KIND_DUALSENSE
        ? SOTB_PROMPT_FAMILY_PLAYSTATION : SOTB_PROMPT_FAMILY_XBOX;
}

const char *sotb_prompt_family_name(SotbPromptFamily family) {
    return family==SOTB_PROMPT_FAMILY_PLAYSTATION ? "PlayStation" : "Xbox";
}

static const struct { int source; const char *xbox, *playstation; } labels[]={
    {SDL_GAMEPAD_BUTTON_SOUTH,"A","Cross"}, {SDL_GAMEPAD_BUTTON_EAST,"B","Circle"},
    {SDL_GAMEPAD_BUTTON_WEST,"X","Square"}, {SDL_GAMEPAD_BUTTON_NORTH,"Y","Triangle"},
    {SDL_GAMEPAD_BUTTON_BACK,"View","Create / Share"}, {SDL_GAMEPAD_BUTTON_START,"Menu","Options"},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,"Left stick click","L3"}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,"Right stick click","R3"},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,"LB","L1"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,"RB","R1"},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,"D-pad up","D-pad up"}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,"D-pad down","D-pad down"},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,"D-pad left","D-pad left"}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,"D-pad right","D-pad right"},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,"Touchpad click","Touchpad click"},
    {SDL_GAMEPAD_BUTTON_MISC1,"Share / Extra 1","Mute / Extra 1"},
    {SDL_GAMEPAD_BUTTON_MISC2,"Extra button 2","Extra button 2"}, {SDL_GAMEPAD_BUTTON_MISC3,"Extra button 3","Extra button 3"},
    {SDL_GAMEPAD_BUTTON_MISC4,"Extra button 4","Extra button 4"}, {SDL_GAMEPAD_BUTTON_MISC5,"Extra button 5","Extra button 5"},
    {SDL_GAMEPAD_BUTTON_MISC6,"Extra button 6","Extra button 6"},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,"Right paddle 1","Right paddle 1"}, {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,"Left paddle 1","Left paddle 1"},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,"Right paddle 2","Right paddle 2"}, {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,"Left paddle 2","Left paddle 2"},
    {SOTB_PAD_SOURCE_LEFT_TRIGGER,"LT","L2"}, {SOTB_PAD_SOURCE_RIGHT_TRIGGER,"RT","R2"},
};

const char *sotb_pad_source_label(int source, SotbPromptFamily family) {
    for (size_t i=0;i<sizeof(labels)/sizeof(*labels);++i)
        if (labels[i].source==source) return family==SOTB_PROMPT_FAMILY_PLAYSTATION ? labels[i].playstation : labels[i].xbox;
    return "";
}

int sotb_prompt_enter_button_assign(void) { return 1; }
SotbPadAction sotb_prompt_confirm_action(void) { return SOTB_PAD_CROSS; }
SotbPadAction sotb_prompt_back_action(void) { return SOTB_PAD_CIRCLE; }

void sotb_active_pad_init(SotbActivePad *pad) { memset(pad,0,sizeof(*pad)); }

static const SotbPadDevice *find_device(const SotbPadDevice *devices, size_t count, uint32_t id) {
    for (size_t i=0;id && i<count;++i) if (devices[i].id==id) return &devices[i];
    return NULL;
}

SotbPadKind sotb_active_pad_update(SotbActivePad *pad, const SotbPadDevice *devices, size_t count, uint32_t input_id) {
    if (!count) {
        pad->active_id=0;
        pad->candidate_id=0; pad->candidate_hits=0;
        return pad->kind;
    }
    const SotbPadDevice *active=find_device(devices,count,pad->active_id);
    if (!active) {
        active=&devices[0];
        pad->active_id=active->id;
        pad->candidate_id=0; pad->candidate_hits=0;
    }
    const SotbPadDevice *input=input_id!=pad->active_id ? find_device(devices,count,input_id) : NULL;
    if (input) {
        pad->candidate_hits=pad->candidate_id==input_id ? pad->candidate_hits+1 : 1;
        pad->candidate_id=input_id;
        if (pad->candidate_hits>=SOTB_ACTIVE_PAD_SWITCH_HITS) {
            active=input;
            pad->active_id=input_id;
            pad->candidate_id=0; pad->candidate_hits=0;
        }
    } else if (input_id) {
        pad->candidate_id=0; pad->candidate_hits=0;
    }
    pad->kind=active->kind;
    return pad->kind;
}

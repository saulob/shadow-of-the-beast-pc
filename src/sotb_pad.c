#include "sotb_pad.h"
#include "sotb_keys.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

_Static_assert(SDL_GAMEPAD_BUTTON_COUNT<=32,"a sample holds one bit per gamepad button");

const SotbPadInfo sotb_pad_actions[SOTB_PAD_COUNT] = {
    {"pad_cross", "Cross", "cross", SDL_GAMEPAD_BUTTON_SOUTH},
    {"pad_circle", "Circle", "circle", SDL_GAMEPAD_BUTTON_EAST},
    {"pad_square", "Square", "square", SDL_GAMEPAD_BUTTON_WEST},
    {"pad_triangle", "Triangle", "triangle", SDL_GAMEPAD_BUTTON_NORTH},
    {"pad_l1", "L1", "l1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"pad_r1", "R1", "r1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"pad_l2", "L2", "l2", SOTB_PAD_SOURCE_LEFT_TRIGGER},
    {"pad_r2", "R2", "r2", SOTB_PAD_SOURCE_RIGHT_TRIGGER},
    {"pad_l3", "L3 (left stick click)", "l3", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"pad_r3", "R3 (right stick click)", "r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"pad_options", "Options", "options", SDL_GAMEPAD_BUTTON_START},
    {"pad_touchpad", "Touchpad", "touchpad", SDL_GAMEPAD_BUTTON_BACK},
    {"pad_dpad_up", "D-pad up", "up", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"pad_dpad_down", "D-pad down", "down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"pad_dpad_left", "D-pad left", "left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"pad_dpad_right", "D-pad right", "right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

void sotb_pad_defaults(SotbPadMap *map) {
    for (int i=0;i<SOTB_PAD_COUNT;++i) map->source[i]=sotb_pad_actions[i].default_source;
}

int sotb_pad_source_bindable(int source) {
    if (source==SOTB_PAD_SOURCE_LEFT_TRIGGER || source==SOTB_PAD_SOURCE_RIGHT_TRIGGER) return 1;
    return source>SDL_GAMEPAD_BUTTON_INVALID && source<SDL_GAMEPAD_BUTTON_COUNT && source!=SDL_GAMEPAD_BUTTON_GUIDE;
}

int sotb_pad_source_from_name(const char *name) {
    if (!name || !*name || strlen(name)>31) return SOTB_PAD_SOURCE_NONE;
    char lower[32];
    size_t n=0;
    for (;name[n];++n) lower[n]=(char)(name[n]>='A' && name[n]<='Z' ? name[n]-'A'+'a' : name[n]);
    lower[n]=0;
    if (!strcmp(lower,SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER))) return SOTB_PAD_SOURCE_LEFT_TRIGGER;
    if (!strcmp(lower,SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) return SOTB_PAD_SOURCE_RIGHT_TRIGGER;
    const int button=(int)SDL_GetGamepadButtonFromString(lower);
    return sotb_pad_source_bindable(button) ? button : SOTB_PAD_SOURCE_NONE;
}

const char *sotb_pad_source_name(int source, char *buffer, size_t size) {
    if (size) buffer[0]=0;
    if (!size || !sotb_pad_source_bindable(source)) return buffer;
    const char *name=source==SOTB_PAD_SOURCE_LEFT_TRIGGER ? SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
                      : source==SOTB_PAD_SOURCE_RIGHT_TRIGGER ? SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                      : SDL_GetGamepadStringForButton((SDL_GamepadButton)source);
    if (name) SDL_strlcpy(buffer,name,size);
    return buffer;
}

int sotb_pad_apply(SotbPadMap *map, const char *ini_key, const char *value) {
    for (int i=0;i<SOTB_PAD_COUNT;++i) {
        if (strcmp(ini_key,sotb_pad_actions[i].ini_key)) continue;
        const int source=sotb_pad_source_from_name(value);
        if (source==SOTB_PAD_SOURCE_NONE) return 0;
        map->source[i]=source;
        return 1;
    }
    return 0;
}

static void apply_pad_entry(const char *key, const char *value, void *map) {
    sotb_pad_apply((SotbPadMap *)map,key,value);
}

void sotb_pad_load(const char *path, SotbPadMap *map) {
    sotb_pad_defaults(map);
    sotb_ini_read(path,apply_pad_entry,map);
}

int sotb_pad_apply_spec(SotbPadMap *map, const char *spec) {
    if (!spec || !*spec) return 0;
    char copy[1024];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    char *entry=copy;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*sotb_text_trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",sotb_text_trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        const char *button=sotb_text_trim(entry), *name=sotb_text_trim(eq+1);
        entry=next;
        int action=-1;
        for (int i=0;i<SOTB_PAD_COUNT;++i) if (!SDL_strcasecmp(button,sotb_pad_actions[i].name)) action=i;
        const int source=sotb_pad_source_from_name(name);
        if (action<0 || source==SOTB_PAD_SOURCE_NONE) {
            printf("Runtime: pad map: ignored '%s=%s'\n",button,name);
            continue;
        }
        map->source[action]=source;
        ++used;
    }
    return used;
}

int sotb_pad_changed(const SotbPadMap *map) {
    int changed=0;
    for (int i=0;i<SOTB_PAD_COUNT;++i) changed+=map->source[i]!=sotb_pad_actions[i].default_source;
    return changed;
}

int sotb_pad_source_users(const SotbPadMap *map, int source) {
    int users=0;
    for (int i=0;i<SOTB_PAD_COUNT;++i) users+=map->source[i]==source;
    return users;
}

static uint8_t source_value(int source, const SotbPadInput *in) {
    if (source==SOTB_PAD_SOURCE_LEFT_TRIGGER) return in->left_trigger;
    if (source==SOTB_PAD_SOURCE_RIGHT_TRIGGER) return in->right_trigger;
    return source>=0 && source<32 && ((in->buttons>>source) & 1u) ? 255 : 0;
}

int sotb_pad_first_pressed(const SotbPadInput *in) {
    for (int button=0;button<SDL_GAMEPAD_BUTTON_COUNT;++button)
        if (((in->buttons>>button) & 1u) && sotb_pad_source_bindable(button)) return button;
    if (in->left_trigger>=128) return SOTB_PAD_SOURCE_LEFT_TRIGGER;
    if (in->right_trigger>=128) return SOTB_PAD_SOURCE_RIGHT_TRIGGER;
    return SOTB_PAD_SOURCE_NONE;
}

void sotb_pad_translate(const SotbPadMap *map, const SotbPadInput *in, SotbPadOutput *out) {
    memset(out,0,sizeof(*out));
    for (int i=0;i<SOTB_PAD_COUNT;++i) {
        const int source=map->source[i];
        const uint8_t value=source_value(source,in);
        /* The default Touchpad source, Back, also stands for the controller's own touchpad click. */
        const int pressed=value>SOTB_PAD_TRIGGER_PRESS ||
            (i==SOTB_PAD_TOUCHPAD && source==SDL_GAMEPAD_BUTTON_BACK && ((in->buttons>>SDL_GAMEPAD_BUTTON_TOUCHPAD) & 1u));
        if (pressed) out->pressed|=1u<<i;
        if (i==SOTB_PAD_L2) out->l2=value;
        if (i==SOTB_PAD_R2) out->r2=value;
    }
}

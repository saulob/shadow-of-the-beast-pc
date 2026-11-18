#pragma once
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef SOTB_SDL_COMPAT_POLICY_ONLY
#include <SDL3/SDL.h>
#endif

typedef enum { SOTB_HIDAPI_LEAVE, SOTB_HIDAPI_DISABLE, SOTB_HIDAPI_PRESERVE } SotbHidapiAction;

static inline SotbHidapiAction sotb_hidapi_action(int under_wine, const char *configured) {
    if (configured && configured[0]) return SOTB_HIDAPI_PRESERVE;
    return under_wine ? SOTB_HIDAPI_DISABLE : SOTB_HIDAPI_LEAVE;
}

static inline int sotb_running_under_wine(void) {
#ifdef _WIN32
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;
#else
    return 0;
#endif
}

#ifndef SOTB_SDL_COMPAT_POLICY_ONLY
/* Before the first SDL_INIT_GAMEPAD, which reads the hint; later calls see "0" and do nothing. */
static inline void sotb_prepare_sdl_gamepad(void) {
    if (sotb_hidapi_action(sotb_running_under_wine(), SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI)) != SOTB_HIDAPI_DISABLE) return;
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    printf("Runtime: Proton/Wine detected; SDL HIDAPI gamepads disabled for compatibility\n");
}
#endif

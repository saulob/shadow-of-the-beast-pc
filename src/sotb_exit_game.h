#ifndef SOTB_EXIT_GAME_H
#define SOTB_EXIT_GAME_H

#include <stddef.h>
#include <stdint.h>

#include "patch_sites.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOTB_EXIT_GAME_TICK_SITE 0x1e50f3u
#define SOTB_EXIT_GAME_SETUP_SITE 0x1e5124u
#define SOTB_EXIT_GAME_REFRESH_SITE 0x2690ceu
#define SOTB_EXIT_GAME_ADDBUTTON_SITE 0x1e5840u
#define SOTB_EXIT_GAME_CREATETEXT_SITE 0x1dcce90u
#define SOTB_EXIT_GAME_MIN_CAVE_SIZE 384u

#define SOTB_FRONTEND_SCREEN_MAP 3u
#define SOTB_FRONTEND_SCREEN_PAUSE 11u
#define SOTB_INFOBAR_BUTTON_SQUARE 8u

typedef int (*SotbExitConfirmHandler)(void);
typedef uint64_t (*SotbExitGameClock)(void);

int sotb_exit_game_supported(const char *build_key);
int sotb_exit_game_screen_allowed(uint8_t screen_id);
int sotb_exit_game_can_append_button(uint8_t screen_id, uint8_t show_bar, int32_t count, int32_t capacity,
                                     const uint8_t *existing_hotkeys);
int sotb_exit_game_apply(uint8_t *image, size_t image_size, const char *build_key, uint8_t *cave, size_t cave_size,
                         char *error, size_t error_size);
/* The image checks of sotb_exit_game_apply for the build's table, without writing. */
int sotb_exit_game_check(const uint8_t *image, size_t image_size, const char *build_key, char *error, size_t error_size);
/* The build's four checked sites; 0 for an unknown build or fewer than 4 slots. */
size_t sotb_exit_game_sites(const char *build_key, SotbPatchSite *sites, size_t capacity);

void sotb_exit_game_set_confirm_handler(SotbExitConfirmHandler handler);
void sotb_exit_game_set_clock(SotbExitGameClock clock);
void sotb_exit_game_set_locale(const char *locale);
const char *sotb_exit_game_locale(void);
void sotb_exit_game_reset(void);

int sotb_exit_game_active(void);
int sotb_exit_game_filter_square(int square_down);

/* Test helpers. */
void sotb_exit_game_set_active_for_test(int active);
uint8_t sotb_exit_game_last_screen(void);

#ifdef __cplusplus
}
#endif

#endif

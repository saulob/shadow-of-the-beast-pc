#ifndef SOTB_EXIT_GAME_L10N_H
#define SOTB_EXIT_GAME_L10N_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SotbExitGameStrings {
    const char *locale;
    const char *footer;
    const char *title;
    const char *question;
    const char *confirm;
    const char *cancel;
    const char *saving;
} SotbExitGameStrings;

const SotbExitGameStrings *sotb_exit_game_strings(const char *locale);
const SotbExitGameStrings *sotb_exit_game_strings_table(size_t *count);

#ifdef __cplusplus
}
#endif

#endif

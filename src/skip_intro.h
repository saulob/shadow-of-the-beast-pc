#ifndef SKIP_INTRO_H
#define SKIP_INTRO_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SKIP_INTRO_SITE 0x1ca55c
#define SKIP_INTRO_PATCH_SIZE 8
#define SKIP_INTRO_STATE_WRITE (SKIP_INTRO_SITE + 23)
#define SKIP_INTRO_REVEAL_CALL 0x1ca6b6
#define SKIP_INTRO_HIDE_LIMIT_MS 90000

/* Shortens the Press Start prompt delay of CUSA03762 1.00 and arms the startup blackout. Everything is checked first;
 * on false the image is untouched, nothing is armed and `error` says why. `cave` is writable code within 2 GB of `image`. */
int skip_intro_apply(uint8_t *image, size_t image_size, uint8_t *cave, size_t cave_size, char *error, size_t error_size);

/* True while the startup presentation must be shown to the user as black. */
int skip_intro_hide_presentation(void);

#ifdef __cplusplus
}
#endif

#endif

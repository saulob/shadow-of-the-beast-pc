#ifndef SKIP_INTRO_H
#define SKIP_INTRO_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The loading-screen constructor starts its publisher/developer sequence at zero. Starting it finished makes the
 * game's own tick clear the three boot images and run the same completion path on the first frame. */
#define SKIP_INTRO_SITE 0x28d5c21u
#define SKIP_INTRO_PATCH_SIZE 0x21u

int skip_intro_apply(uint8_t *image, size_t image_size, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif

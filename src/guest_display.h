#ifndef GUEST_DISPLAY_H
#define GUEST_DISPLAY_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    const char *name;
    int width, height;
} SotbResolution;

#define SOTB_RESOLUTION_COUNT 6
extern const SotbResolution sotb_resolutions[SOTB_RESOLUTION_COUNT];

int sotb_resolution_index(const char *key);

/* Every site is checked first; nothing is written unless all of them hold the original bytes of CUSA03762 1.00. */
int guest_apply_display_size(uint8_t *image, size_t image_size, int width, int height, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif

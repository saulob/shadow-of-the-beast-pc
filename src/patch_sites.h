#ifndef SOTB_PATCH_SITES_H
#define SOTB_PATCH_SITES_H
#include <stddef.h>
#include <stdint.h>

/* The exact bytes a native patch checks at a fixed image offset of the supported builds. */
typedef struct {
    const char *name;
    uint32_t offset;
    const uint8_t *bytes;
    uint32_t length;
} SotbPatchSite;

#endif

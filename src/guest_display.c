#include "guest_display.h"
#include <stdio.h>
#include <string.h>

const SotbResolution sotb_resolutions[SOTB_RESOLUTION_COUNT] = {
    {"native", "Native", 1920, 1080}, {"480p", "480p", 854, 480},     {"720p", "720p", 1280, 720},
    {"1440p", "1440p", 2560, 1440},           {"1800p", "1800p", 3200, 1800}, {"4K", "4K", 3840, 2160},
};

int sotb_resolution_index(const char *key) {
    for (int i = 0; key && i < SOTB_RESOLUTION_COUNT; ++i)
        if (!strcmp(key, sotb_resolutions[i].key)) return i;
    return 0;
}

typedef struct {
    uint32_t offset;
    uint8_t length;
    uint8_t original[10];
    uint8_t immediate;
    uint8_t is_height;
} SizeSite;

static const SizeSite size_sites[] = {
    {0x607f0c, 10, {0x48, 0xb8, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, 0},
    {0x6080b7, 10, {0x48, 0xb8, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, 0},
    {0x609011, 10, {0x48, 0xb8, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, 0},
    {0x609290, 10, {0x48, 0xba, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, 0},
    {0x609037, 10, {0xc7, 0x85, 0x98, 0xdd, 0xff, 0xff, 0x80, 0x07, 0, 0}, 6, 0},
    {0x609041, 10, {0xc7, 0x85, 0x9c, 0xdd, 0xff, 0xff, 0x38, 0x04, 0, 0}, 6, 1},
};

static int fail(char *error, size_t size, const char *what, uint32_t offset) {
    if (error && size) snprintf(error, size, "%s at image offset 0x%x", what, (unsigned)offset);
    return 0;
}

static void put32(uint8_t *at, uint32_t value) { memcpy(at, &value, 4); }

int guest_apply_display_size(uint8_t *image, size_t image_size, int width, int height, char *error, size_t error_size) {
    if (width < 8 || height < 8) return fail(error, error_size, "invalid render size", 0);
    const size_t count = sizeof(size_sites) / sizeof(*size_sites);
    for (size_t i = 0; i < count; ++i) {
        const SizeSite *site = &size_sites[i];
        if ((uint64_t)site->offset + site->length > image_size)
            return fail(error, error_size, "site outside the image", site->offset);
        if (memcmp(image + site->offset, site->original, site->length))
            return fail(error, error_size, "unexpected display size instruction", site->offset);
    }
    for (size_t i = 0; i < count; ++i) {
        const SizeSite *site = &size_sites[i];
        uint8_t *at = image + site->offset + site->immediate;
        if (site->immediate == 2) {
            put32(at, (uint32_t)width);
            put32(at + 4, (uint32_t)height);
        } else {
            put32(at, (uint32_t)(site->is_height ? height : width));
        }
    }
    return 1;
}

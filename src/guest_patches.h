#ifndef GUEST_PATCHES_H
#define GUEST_PATCHES_H
#include <stddef.h>
#include <stdint.h>

#define GUEST_PATCH_CAVE_SIZE 4096
#define GUEST_ABI __attribute__((sysv_abi))

typedef struct {
    int applied, skipped;
} GuestPatchReport;

/* Offline policy for the shipped eboot: Social Features stay off and their UI is not offered. Every patch checks the
 * bytes it replaces and is skipped when they differ. `cave` is writable code memory within 2 GB of `image`. */
GuestPatchReport guest_patches_apply(uint8_t *image, size_t image_size, uint8_t *cave, size_t cave_size);

typedef void (*GuestRemoveFromNavigation)(void *controller, void *button);
typedef void (*GuestSetCollapsed)(void *widget);

typedef struct {
    GuestRemoveFromNavigation remove_from_navigation;
    GuestSetCollapsed collapse;
} GuestUiCalls;

/* Called from the Options screen once its widgets are bound; tests replace it before patching. */
typedef void (GUEST_ABI *GuestOptionsHook)(void *controller);
void guest_patches_set_options_hook(GuestOptionsHook hook);

/* Takes the Social Features row out of the Options list; returns 0 when it was already gone. */
int guest_options_hide_social(void *controller, const GuestUiCalls *calls);

#endif

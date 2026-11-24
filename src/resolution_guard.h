#ifndef SOTB_RESOLUTION_GUARD_H
#define SOTB_RESOLUTION_GUARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOTB_RESOLUTION_GUARD_FILE "render_resolution.pending"
#define SOTB_RESOLUTION_GUARD_SAFE_KEY "1440p"

void sotb_resolution_guard_init(const char *user_dir);
int sotb_resolution_guard_arm(const char *user_dir, const char *resolution_key);
int sotb_resolution_guard_pending(const char *user_dir, char *resolution_key, size_t size);
void sotb_resolution_guard_clear(const char *user_dir);
void sotb_resolution_guard_loading_screen_finished(void);

#ifdef __cplusplus
}
#endif

#endif

#include "resolution_guard.h"

#include <stdio.h>
#include <string.h>

#include "guest_display.h"
#include "startup_phase.h"

static char pending_path[4200];
static int gameplay_reached;

static int make_path(const char *user_dir, char *out, size_t size) {
    if (!user_dir || !*user_dir) return 0;
    const int written = snprintf(out, size, "%s/%s", user_dir, SOTB_RESOLUTION_GUARD_FILE);
    return written > 0 && written < (int)size;
}

static int high_resolution(const char *key) {
    return key && sotb_resolution_index(key) > sotb_resolution_index(SOTB_RESOLUTION_GUARD_SAFE_KEY);
}

void sotb_resolution_guard_init(const char *user_dir) {
    if (!make_path(user_dir, pending_path, sizeof(pending_path))) pending_path[0] = 0;
}

int sotb_resolution_guard_arm(const char *user_dir, const char *resolution_key) {
    char path[4200];
    if (!make_path(user_dir, path, sizeof(path))) return 0;
    if (!high_resolution(resolution_key)) {
        remove(path);
        return 0;
    }
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    const int written = fputs(resolution_key, file) >= 0 && fputc('\n', file) != EOF;
    fclose(file);
    if (!written) remove(path);
    return written;
}

int sotb_resolution_guard_pending(const char *user_dir, char *resolution_key, size_t size) {
    char path[4200];
    if (!size || !make_path(user_dir, path, sizeof(path))) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    const size_t got = fread(resolution_key, 1, size - 1, file);
    fclose(file);
    resolution_key[got] = 0;
    size_t length = strlen(resolution_key);
    while (length && (resolution_key[length - 1] == '\n' || resolution_key[length - 1] == '\r' ||
                      resolution_key[length - 1] == ' ' || resolution_key[length - 1] == '\t'))
        resolution_key[--length] = 0;
    return length && high_resolution(resolution_key);
}

void sotb_resolution_guard_clear(const char *user_dir) {
    char path[4200];
    if (make_path(user_dir, path, sizeof(path))) remove(path);
}

void sotb_resolution_guard_loading_screen_finished(void) {
    if (__atomic_exchange_n(&gameplay_reached, 1, __ATOMIC_ACQ_REL)) return;
    sotb_startup_phase("gameplay started");
    if (pending_path[0]) remove(pending_path);
}

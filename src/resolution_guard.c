#include "resolution_guard.h"

#include <stdio.h>
#include <string.h>

#include "guest_display.h"
#include "startup_phase.h"

#define GUARD_ASPECT_DEFAULT "16:9"

static char pending_path[4200];
static int gameplay_reached;

static int make_path(const char *user_dir, char *out, size_t size) {
    if (!user_dir || !*user_dir) return 0;
    const int written = snprintf(out, size, "%s/%s", user_dir, SOTB_RESOLUTION_GUARD_FILE);
    return written > 0 && written < (int)size;
}

static int heavy_attempt(const char *resolution_key, const char *aspect_key) {
    if (!resolution_key) return 0;
    const int index = sotb_resolution_index(resolution_key);
    if (strcmp(sotb_resolutions[index].key, resolution_key)) return 0;
    const SotbAspectRatio *aspect = &sotb_aspect_ratios[sotb_aspect_index(aspect_key)];
    return sotb_target_is_heavy(sotb_render_target(&sotb_resolutions[index], aspect));
}

static void trim_line_end(char *text) {
    size_t length = strlen(text);
    while (length && (text[length - 1] == '\r' || text[length - 1] == ' ' || text[length - 1] == '\t'))
        text[--length] = 0;
}

static void copy_line(const char *from, size_t length, char *out, size_t size) {
    if (!out || !size) return;
    if (length > size - 1) length = size - 1;
    memcpy(out, from, length);
    out[length] = 0;
    trim_line_end(out);
}

void sotb_resolution_guard_init(const char *user_dir) {
    if (!make_path(user_dir, pending_path, sizeof(pending_path))) pending_path[0] = 0;
}

int sotb_resolution_guard_arm_target(const char *user_dir, const char *resolution_key, const char *aspect_key) {
    char path[4200];
    if (!make_path(user_dir, path, sizeof(path))) return 0;
    if (!heavy_attempt(resolution_key, aspect_key)) {
        remove(path);
        return 0;
    }
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    const char *aspect = sotb_aspect_ratios[sotb_aspect_index(aspect_key)].key;
    const int written = fprintf(file, "%s\n%s\n", resolution_key, aspect) > 0;
    fclose(file);
    if (!written) remove(path);
    return written;
}

int sotb_resolution_guard_pending_target(const char *user_dir, char *resolution_key, size_t size, char *aspect_key,
                                         size_t aspect_size) {
    char path[4200];
    char text[256];
    if (!size || !make_path(user_dir, path, sizeof(path))) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    const size_t got = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[got] = 0;
    const char *second = strchr(text, '\n');
    const size_t first_length = second ? (size_t)(second - text) : got;
    copy_line(text, first_length, resolution_key, size);
    char aspect[32];
    if (second) {
        const char *line = second + 1;
        const char *end = strchr(line, '\n');
        copy_line(line, end ? (size_t)(end - line) : strlen(line), aspect, sizeof(aspect));
    } else {
        aspect[0] = 0;
    }
    if (!aspect[0]) snprintf(aspect, sizeof(aspect), "%s", GUARD_ASPECT_DEFAULT);
    if (aspect_key && aspect_size) snprintf(aspect_key, aspect_size, "%s", sotb_aspect_ratios[sotb_aspect_index(aspect)].key);
    return resolution_key[0] && heavy_attempt(resolution_key, aspect);
}

int sotb_resolution_guard_arm(const char *user_dir, const char *resolution_key) {
    return sotb_resolution_guard_arm_target(user_dir, resolution_key, GUARD_ASPECT_DEFAULT);
}

int sotb_resolution_guard_pending(const char *user_dir, char *resolution_key, size_t size) {
    return sotb_resolution_guard_pending_target(user_dir, resolution_key, size, NULL, 0);
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

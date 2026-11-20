#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SOTB_MAPPING_FILE_MAX (4 * 1024 * 1024)
#define SOTB_MAPPING_COUNT_MAX 99999

typedef struct {
    int64_t (*file_size)(const char *path, void *user); /* negative: no such file */
    int (*add_file)(const char *path, void *user);      /* mappings added or updated, negative on failure */
    const char *(*last_error)(void *user);
    void *user;
} SotbMappingLoader;

typedef enum { SOTB_MAPPING_ABSENT, SOTB_MAPPING_LOADED, SOTB_MAPPING_FAILED } SotbMappingResult;

static inline void sotb_clean_text(const char *text, char *out, size_t size) {
    size_t k = 0;
    if (!out || !size) return;
    for (const char *c = text; c && *c && k + 1 < size; ++c) out[k++] = (unsigned char)*c < 0x20 || *c == 0x7f ? ' ' : *c;
    out[k] = 0;
}

/* A missing file is silent; otherwise `line` holds the one summary line. The file is only ever the one beside the settings file. */
static inline SotbMappingResult sotb_load_user_mappings(const char *path, const SotbMappingLoader *loader, char *line, size_t size) {
    if (line && size) line[0] = 0;
    if (!path || !*path || !loader || !loader->file_size || !loader->add_file || !line || !size) return SOTB_MAPPING_ABSENT;
    const int64_t bytes = loader->file_size(path, loader->user);
    if (bytes < 0) return SOTB_MAPPING_ABSENT;
    if (bytes > SOTB_MAPPING_FILE_MAX) {
        snprintf(line, size, "Runtime: gamepad mappings not read from user/gamecontrollerdb.txt: larger than %d bytes", SOTB_MAPPING_FILE_MAX);
        return SOTB_MAPPING_FAILED;
    }
    const int added = loader->add_file(path, loader->user);
    if (added < 0) {
        char reason[96];
        sotb_clean_text(loader->last_error ? loader->last_error(loader->user) : NULL, reason, sizeof(reason));
        snprintf(line, size, "Runtime: gamepad mappings not read from user/gamecontrollerdb.txt: %s", reason[0] ? reason : "unreadable");
        return SOTB_MAPPING_FAILED;
    }
    snprintf(line, size, "Runtime: %d gamepad mappings read from user/gamecontrollerdb.txt", added > SOTB_MAPPING_COUNT_MAX ? SOTB_MAPPING_COUNT_MAX : added);
    return SOTB_MAPPING_LOADED;
}

#define SOTB_UNMAPPED_SEEN 16
#define SOTB_UNMAPPED_KEY 48
typedef struct { char keys[SOTB_UNMAPPED_SEEN][SOTB_UNMAPPED_KEY]; size_t count, next; } SotbUnmappedSeen;

/* The GUID names a device model; without one the VID:PID and name do. */
static inline void sotb_unmapped_key(const char *name, const char *guid, unsigned vendor, unsigned product, char *key, size_t size) {
    if (!key || !size) return;
    if (guid && *guid) {
        snprintf(key, size, "g:%.32s", guid);
        return;
    }
    char clean[24];
    sotb_clean_text(name, clean, sizeof(clean));
    snprintf(key, size, "v:%04x:%04x:%s", vendor & 0xffffu, product & 0xffffu, clean);
}

/* True the first time an identity is seen; the oldest of SOTB_UNMAPPED_SEEN is forgotten first. */
static inline int sotb_unmapped_first_sight(SotbUnmappedSeen *seen, const char *key) {
    for (size_t i = 0; i < seen->count; ++i) if (!strcmp(seen->keys[i], key)) return 0;
    snprintf(seen->keys[seen->next], SOTB_UNMAPPED_KEY, "%s", key);
    seen->next = (seen->next + 1) % SOTB_UNMAPPED_SEEN;
    if (seen->count < SOTB_UNMAPPED_SEEN) ++seen->count;
    return 1;
}

/* Name (when SDL has one), GUID and VID:PID; no serial number, address or path. */
static inline void sotb_unmapped_line(const char *name, const char *guid, unsigned vendor, unsigned product, char *line, size_t size) {
    char clean[64];
    sotb_clean_text(name, clean, sizeof(clean));
    char ids[16] = "none";
    if (vendor || product) snprintf(ids, sizeof(ids), "%04x:%04x", vendor & 0xffffu, product & 0xffffu);
    snprintf(line, size, "Runtime: joystick without a gamepad mapping: name=%s, guid=%s, vid:pid=%s; add a line for this GUID to gamecontrollerdb.txt in the user folder",
             clean[0] ? clean : "unknown", guid && *guid ? guid : "unknown", ids);
}

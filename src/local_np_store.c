#define _GNU_SOURCE
#include "local_np_store.h"
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCORE_FILE_LIMIT (256u * 1024u)
#define SCORE_HEADER "sotb_local_scores = 1"
#define TUS_MAGIC 0x53555453u
#define TUS_HEADER_SIZE 24

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
    char path[760];
    int loaded, corrupt;
    int present[LOCAL_SCORE_BOARDS];
    LocalScore boards[LOCAL_SCORE_BOARDS];
} scores;

/* Boards 32-95 are the two SpeedRun families, ranked by time; the rest rank by points. */
int local_score_board_valid(uint32_t board) { return board < LOCAL_SCORE_BOARDS; }
int local_score_higher_is_better(uint32_t board) { return board < 32 || board >= 96; }

static void to_hex(const uint8_t *data, size_t size, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) { out[i * 2] = digits[data[i] >> 4]; out[i * 2 + 1] = digits[data[i] & 15]; }
    out[size * 2] = 0;
}
static int hex_value(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static int from_hex(const char *text, uint8_t *out, size_t size) {
    if (strlen(text) != size * 2) return 0;
    for (size_t i = 0; i < size; ++i) {
        const int high = hex_value(text[i * 2]), low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) return 0;
        out[i] = (uint8_t)(high << 4 | low);
    }
    return 1;
}

static int parse_scores(char *text) {
    int board = -1, header = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ') ++line;
        if (!*line || *line == '#') continue;
        if (!header) {
            if (strcmp(line, SCORE_HEADER)) return 0;
            header = 1;
            continue;
        }
        unsigned number;
        if (sscanf(line, "[board.%u]", &number) == 1) {
            if (!local_score_board_valid(number) || scores.present[number]) return 0;
            board = (int)number;
            scores.present[board] = 1;
            continue;
        }
        char key[16], value[LOCAL_SCORE_INFO_SIZE * 2 + 8];
        if (board < 0 || sscanf(line, "%15[a-z] = %405s", key, value) != 2) return 0;
        LocalScore *score = &scores.boards[board];
        if (!strcmp(key, "value")) score->value = strtoll(value, NULL, 10);
        else if (!strcmp(key, "date")) score->date = strtoll(value, NULL, 10);
        else if (!strcmp(key, "info")) { if (!from_hex(value, score->info, sizeof(score->info))) return 0; score->has_info = 1; }
        else if (!strcmp(key, "comment")) { if (!from_hex(value, score->comment, sizeof(score->comment))) return 0; score->has_comment = 1; }
    }
    return header;
}

static void load_scores(void) {
    scores.loaded = 1;
    scores.corrupt = 0;
    memset(scores.present, 0, sizeof(scores.present));
    memset(scores.boards, 0, sizeof(scores.boards));
    FILE *f = fopen(scores.path, "rb");
    if (!f) return;
    char *text = malloc(SCORE_FILE_LIMIT + 1);
    size_t size = text ? fread(text, 1, SCORE_FILE_LIMIT + 1, f) : 0;
    fclose(f);
    if (!text || size > SCORE_FILE_LIMIT || (text[size] = 0, !parse_scores(text))) {
        scores.corrupt = 1;
        memset(scores.present, 0, sizeof(scores.present));
        memset(scores.boards, 0, sizeof(scores.boards));
    }
    free(text);
}

static int save_scores(void) {
    char *text = malloc(SCORE_FILE_LIMIT);
    if (!text) return -1;
    int length = snprintf(text, SCORE_FILE_LIMIT, "# Shadow of the Beast local scores\n%s\n", SCORE_HEADER);
    for (uint32_t board = 0; board < LOCAL_SCORE_BOARDS; ++board) {
        if (!scores.present[board]) continue;
        const LocalScore *score = &scores.boards[board];
        length += snprintf(text + length, SCORE_FILE_LIMIT - (size_t)length, "\n[board.%u]\nvalue = %lld\ndate = %lld\n",
                           board, (long long)score->value, (long long)score->date);
        char hex[LOCAL_SCORE_INFO_SIZE * 2 + 1];
        if (score->has_info) { to_hex(score->info, sizeof(score->info), hex); length += snprintf(text + length, SCORE_FILE_LIMIT - (size_t)length, "info = %s\n", hex); }
        if (score->has_comment) { to_hex(score->comment, sizeof(score->comment), hex); length += snprintf(text + length, SCORE_FILE_LIMIT - (size_t)length, "comment = %s\n", hex); }
    }
    int result = -1;
    if (length > 0 && (size_t)length < SCORE_FILE_LIMIT) {
        char backup[780];
        if (scores.corrupt) {
            snprintf(backup, sizeof(backup), "%s.corrupt", scores.path);
            remove(backup);
            rename(scores.path, backup);
            scores.corrupt = 0;
        }
        result = runtime_file_write_atomic(scores.path, text, (size_t)length);
    }
    free(text);
    return result;
}

void local_score_open(const char *path) {
    pthread_mutex_lock(&lock);
    snprintf(scores.path, sizeof(scores.path), "%s", path);
    scores.loaded = 0;
    char dir[780];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; runtime_file_make_dirs(dir); }
    pthread_mutex_unlock(&lock);
}

LocalScoreResult local_score_record(uint32_t board, int64_t value, const uint8_t *info, const uint8_t *comment, int64_t date) {
    if (!local_score_board_valid(board)) return LOCAL_SCORE_BAD_BOARD;
    pthread_mutex_lock(&lock);
    if (!scores.loaded) load_scores();
    LocalScoreResult result = LOCAL_SCORE_NOT_BEST;
    LocalScore *slot = &scores.boards[board];
    const int better = !scores.present[board] ||
                       (local_score_higher_is_better(board) ? value > slot->value : value < slot->value);
    if (better) {
        LocalScore previous = *slot;
        const int was_present = scores.present[board];
        memset(slot, 0, sizeof(*slot));
        slot->value = value;
        slot->date = date;
        if (info) { memcpy(slot->info, info, sizeof(slot->info)); slot->has_info = 1; }
        if (comment) { memcpy(slot->comment, comment, sizeof(slot->comment)); slot->has_comment = 1; }
        scores.present[board] = 1;
        if (save_scores()) {
            *slot = previous;
            scores.present[board] = was_present;
            result = LOCAL_SCORE_WRITE_FAILED;
        } else {
            result = LOCAL_SCORE_RECORDED;
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

int local_score_get(uint32_t board, LocalScore *out) {
    if (!local_score_board_valid(board)) return 0;
    pthread_mutex_lock(&lock);
    if (!scores.loaded) load_scores();
    const int found = scores.present[board];
    if (found && out) *out = scores.boards[board];
    pthread_mutex_unlock(&lock);
    return found;
}

static char tus_dir[760];

static uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = crc >> 1 ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void local_tus_open(const char *dir) {
    pthread_mutex_lock(&lock);
    snprintf(tus_dir, sizeof(tus_dir), "%s", dir);
    runtime_file_make_dirs(dir);
    pthread_mutex_unlock(&lock);
}

int local_tus_slot_valid(int32_t slot) { return slot >= 0 && slot <= LOCAL_TUS_MAX_SLOT; }

static void slot_path(int32_t slot, char *out, size_t size) { snprintf(out, size, "%s/slot_%d.bin", tus_dir, slot); }

/* Returns the payload (malloc) of a slot that passes its checks, or NULL. */
static uint8_t *load_slot(int32_t slot, LocalTusStatus *status) {
    char path[800];
    slot_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t header[TUS_HEADER_SIZE];
    uint8_t *payload = NULL;
    uint32_t magic, size, crc, version;
    int64_t date;
    if (fread(header, 1, sizeof(header), f) == sizeof(header)) {
        memcpy(&magic, header, 4); memcpy(&version, header + 4, 4);
        memcpy(&date, header + 8, 8); memcpy(&size, header + 16, 4); memcpy(&crc, header + 20, 4);
        if (magic == TUS_MAGIC && version == 1 && size <= LOCAL_TUS_MAX_SIZE) {
            payload = malloc(size ? size : 1);
            if (payload && (fread(payload, 1, size, f) != size || fgetc(f) != EOF || crc32(payload, size) != crc)) {
                free(payload);
                payload = NULL;
            }
            if (payload) { status->size = size; status->date = date; }
        }
    }
    fclose(f);
    return payload;
}

int local_tus_set(int32_t slot, const void *data, size_t size, int64_t date) {
    if (!local_tus_slot_valid(slot) || (!data && size) || size > LOCAL_TUS_MAX_SIZE || !tus_dir[0]) return -1;
    uint8_t *file = malloc(TUS_HEADER_SIZE + size);
    if (!file) return -1;
    const uint32_t magic = TUS_MAGIC, version = 1, length = (uint32_t)size, crc = crc32(data, size);
    memcpy(file, &magic, 4); memcpy(file + 4, &version, 4); memcpy(file + 8, &date, 8);
    memcpy(file + 16, &length, 4); memcpy(file + 20, &crc, 4);
    if (size) memcpy(file + TUS_HEADER_SIZE, data, size);
    char path[800];
    slot_path(slot, path, sizeof(path));
    pthread_mutex_lock(&lock);
    const int result = runtime_file_write_atomic(path, file, TUS_HEADER_SIZE + size);
    pthread_mutex_unlock(&lock);
    free(file);
    return result;
}

int local_tus_status(int32_t slot, LocalTusStatus *out) {
    if (!local_tus_slot_valid(slot)) return 0;
    pthread_mutex_lock(&lock);
    uint8_t *payload = load_slot(slot, out);
    pthread_mutex_unlock(&lock);
    const int found = payload != NULL;
    free(payload);
    return found;
}

int local_tus_read(int32_t slot, void *buffer, size_t capacity, size_t *copied) {
    LocalTusStatus status;
    if (!local_tus_slot_valid(slot)) return 0;
    pthread_mutex_lock(&lock);
    uint8_t *payload = load_slot(slot, &status);
    pthread_mutex_unlock(&lock);
    if (!payload) return 0;
    const size_t count = status.size < capacity ? status.size : capacity;
    if (buffer && count) memcpy(buffer, payload, count);
    if (copied) *copied = count;
    free(payload);
    return 1;
}

int local_tus_delete(int32_t slot) {
    if (!local_tus_slot_valid(slot)) return -1;
    char path[800];
    slot_path(slot, path, sizeof(path));
    pthread_mutex_lock(&lock);
    const int result = remove(path) && errno != ENOENT ? -1 : 0;
    pthread_mutex_unlock(&lock);
    return result;
}

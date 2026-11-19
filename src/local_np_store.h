#ifndef LOCAL_NP_STORE_H
#define LOCAL_NP_STORE_H
#include <stddef.h>
#include <stdint.h>

#define LOCAL_SCORE_BOARDS 104
#define LOCAL_SCORE_INFO_SIZE 200
#define LOCAL_SCORE_COMMENT_SIZE 64
#define LOCAL_TUS_MAX_SLOT 4095
#define LOCAL_TUS_MAX_SIZE (4u << 20)

typedef struct {
    int64_t value, date;
    int has_info, has_comment;
    uint8_t info[LOCAL_SCORE_INFO_SIZE];
    uint8_t comment[LOCAL_SCORE_COMMENT_SIZE];
} LocalScore;

typedef enum { LOCAL_SCORE_NOT_BEST = 0, LOCAL_SCORE_RECORDED = 1, LOCAL_SCORE_BAD_BOARD = -1, LOCAL_SCORE_WRITE_FAILED = -2 } LocalScoreResult;

/* Best score per board for the one local player. The board table is the game's own (LeaderboardsPS4 in its config). */
int local_score_board_valid(uint32_t board);
int local_score_higher_is_better(uint32_t board);
void local_score_open(const char *path);
LocalScoreResult local_score_record(uint32_t board, int64_t value, const uint8_t *info, const uint8_t *comment, int64_t date);
int local_score_get(uint32_t board, LocalScore *out);

typedef struct { uint32_t size; int64_t date; } LocalTusStatus;

/* One file per slot under dir; a file that fails its checks reads as an empty slot. */
void local_tus_open(const char *dir);
int local_tus_slot_valid(int32_t slot);
int local_tus_set(int32_t slot, const void *data, size_t size, int64_t date);
int local_tus_status(int32_t slot, LocalTusStatus *out);
int local_tus_read(int32_t slot, void *buffer, size_t capacity, size_t *copied);
int local_tus_delete(int32_t slot);

#endif

/* Targeted offline compatibility for the game's NP code.
 *
 * Nothing here talks to a network. Callers proven to need a local answer (the game's online-state check and sign-in
 * check) see "connected" and "signed in"; every other caller keeps the offline answers of runtime_services.c.
 * Score and TUS are answered from files under <user>/local_services/<title id>/. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_np_local.h"
#include "local_np_store.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOCAL_USER 1
#define LOCAL_ACCOUNT_ID 0x0000000100000001ull
#define NP_STATE_SIGNED_IN 2
#define NET_CTL_IP_OBTAINED 3
#define NP_ID_SIZE 36
#define ONLINE_ID_CHARS 16
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_USER_NOT_FOUND ((int32_t)0x80550004)
#define NP_NO_RESOURCE ((int32_t)0x80550706)
#define NP_REQUEST_ABORTED ((int32_t)0x80550716)
#define NP_SCORE_NOT_BEST ((int32_t)0x80550815)
#define NP_SCORE_BAD_BOARD ((int32_t)0x80550714)
#define NP_STATE_CALLBACKS 4
#define HANDLES 96
#define FIRST_HANDLE 0x5000
#define RANK_ENTRY_SIZE 0x88
#define RANK_HAS_DATA 0
#define RANK_NP_ID 8
#define RANK_RANK 0x68
#define RANK_SCORE 0x78
#define TUS_STATUS_SIZE 0x1f0
#define TUS_HAS_DATA 0x24
#define TUS_DATE 0x28
#define TUS_DATA_SIZE 0x60

/* Return-address offsets in the eboot of the calls listed in LocalGate. */
static const uintptr_t gate_callers[LOCAL_GATE_COUNT] = {
    [LOCAL_GATE_NETCTL_STATE] = 0x18fc676,
    [LOCAL_GATE_NP_STATE] = 0x18fc66a,
    [LOCAL_GATE_NP_REGISTER] = 0x196f74f,
    [LOCAL_GATE_NP_CHECK_CALLBACK] = 0x198fe40,
    [LOCAL_GATE_NP_AVAILABILITY] = 0x28f43d6,
    [LOCAL_GATE_NP_PLUS] = 0x28f4537,
    [LOCAL_GATE_NP_PARENTAL] = 0x28f4678,
    [LOCAL_GATE_NP_ID] = 0x18db50b,
};

typedef int32_t (ABI *StateCallback)(int32_t user, int32_t state, const void *np_id, void *arg);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t forced_caller = UINTPTR_MAX - 1;
static int storage_ready;
static int32_t next_handle = FIRST_HANDLE;
static struct { int used; int32_t id; int is_context; int domain; int aborted; int32_t result; } handles[HANDLES];
static struct { int used, delivered; StateCallback callback; void *arg; } state_callbacks[NP_STATE_CALLBACKS];

enum { DOMAIN_SCORE = 1, DOMAIN_TUS = 2 };

static void *fallback(const char *symbol) { return (void *)runtime_services_symbol(symbol); }

static uintptr_t caller_offset(void *return_address) {
    return forced_caller != UINTPTR_MAX - 1 ? forced_caller : runtime_guest_offset((uintptr_t)return_address);
}

int local_gate_allows(LocalGate gate, uintptr_t caller) {
    return (unsigned)gate < LOCAL_GATE_COUNT && caller == gate_callers[gate];
}

static int gate_open(LocalGate gate, void *return_address) {
    return local_gate_allows(gate, caller_offset(return_address));
}

void runtime_np_local_force_caller(uintptr_t offset) { forced_caller = offset; }

static void local_np_id(uint8_t id[NP_ID_SIZE]) {
    const uint64_t account = LOCAL_ACCOUNT_ID;
    memset(id, 0, NP_ID_SIZE);
    memcpy(id, runtime_local_user_name(), strnlen(runtime_local_user_name(), ONLINE_ID_CHARS));
    memcpy(id + 20, &account, sizeof(account));
}
static int is_local_online_id(const void *online_id) {
    uint8_t local[NP_ID_SIZE];
    local_np_id(local);
    return online_id && !strncmp((const char *)online_id, (const char *)local, ONLINE_ID_CHARS);
}

/* ---- targeted NetCtl / NP state ---- */
typedef int32_t (ABI *StateFn)(int32_t *);
static ABI int32_t netctl_state(int32_t *state) {
    if (state && gate_open(LOCAL_GATE_NETCTL_STATE, __builtin_return_address(0))) { *state = NET_CTL_IP_OBTAINED; return 0; }
    return ((StateFn)fallback("sceNetCtlGetState"))(state);
}
typedef int32_t (ABI *NpStateFn)(int32_t, int32_t *);
static ABI int32_t np_state(int32_t user, int32_t *state) {
    if (state && gate_open(LOCAL_GATE_NP_STATE, __builtin_return_address(0))) { *state = NP_STATE_SIGNED_IN; return 0; }
    return ((NpStateFn)fallback("sceNpGetState"))(user, state);
}

typedef int32_t (ABI *RegisterFn)(void *, void *);
static ABI int32_t np_register(void *callback, void *arg) {
    if (!callback || !gate_open(LOCAL_GATE_NP_REGISTER, __builtin_return_address(0)))
        return ((RegisterFn)fallback("sceNpRegisterStateCallback"))(callback, arg);
    int32_t result = NP_NO_RESOURCE;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NP_STATE_CALLBACKS && result; ++i) {
        if (state_callbacks[i].used) continue;
        state_callbacks[i].used = 1;
        state_callbacks[i].delivered = 0;
        state_callbacks[i].callback = (StateCallback)callback;
        state_callbacks[i].arg = arg;
        result = 0;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

typedef int32_t (ABI *VoidFn)(void);
/* Each registration is told once that the local user is signed in, on the thread that polls for callbacks. */
static ABI int32_t np_check_callback(void) {
    if (!gate_open(LOCAL_GATE_NP_CHECK_CALLBACK, __builtin_return_address(0))) return ((VoidFn)fallback("sceNpCheckCallback"))();
    struct { StateCallback callback; void *arg; } due[NP_STATE_CALLBACKS];
    int count = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < NP_STATE_CALLBACKS; ++i) {
        if (!state_callbacks[i].used || state_callbacks[i].delivered) continue;
        state_callbacks[i].delivered = 1;
        due[count].callback = state_callbacks[i].callback;
        due[count++].arg = state_callbacks[i].arg;
    }
    pthread_mutex_unlock(&lock);
    uint8_t id[NP_ID_SIZE];
    local_np_id(id);
    for (int i = 0; i < count; ++i) {
        due[i].callback(LOCAL_USER, NP_STATE_SIGNED_IN, id, due[i].arg);
    }
    return 0;
}

typedef int32_t (ABI *AvailabilityFn)(int32_t, const void *, void *);
static ABI int32_t np_check_availability(int32_t request, const void *online_id, void *reserved) {
    if (gate_open(LOCAL_GATE_NP_AVAILABILITY, __builtin_return_address(0))) return 0;
    return ((AvailabilityFn)fallback("sceNpCheckNpAvailability"))(request, online_id, reserved);
}
typedef int32_t (ABI *PlusFn)(int32_t, const void *, uint8_t *);
static ABI int32_t np_check_plus(int32_t request, const void *param, uint8_t *result) {
    if (result && gate_open(LOCAL_GATE_NP_PLUS, __builtin_return_address(0))) { result[0] = 1; return 0; }
    return ((PlusFn)fallback("sceNpCheckPlus"))(request, param, result);
}
typedef int32_t (ABI *ParentalFn)(int32_t, const void *, int8_t *, uint8_t *);
static ABI int32_t np_parental_control(int32_t request, const void *online_id, int8_t *age, uint8_t *info) {
    if (age && info && gate_open(LOCAL_GATE_NP_PARENTAL, __builtin_return_address(0))) {
        *age = 20;
        memset(info, 0, 3);
        return 0;
    }
    return ((ParentalFn)fallback("sceNpGetParentalControlInfo"))(request, online_id, age, info);
}
static ABI int32_t np_get_np_id(int32_t user, uint8_t *np_id) {
    if (np_id && user == LOCAL_USER && gate_open(LOCAL_GATE_NP_ID, __builtin_return_address(0))) { local_np_id(np_id); return 0; }
    return ((NpStateFn)fallback("sceNpGetNpId"))(user, (int32_t *)np_id);
}

/* ---- request handles shared by Score and TUS ---- */
static void ensure_storage(void) {
    if (storage_ready) return;
    char base[800], path[840];
    snprintf(base, sizeof(base), "%s/local_services/%s", runtime_file_user_dir(), runtime_savedata_title());
    snprintf(path, sizeof(path), "%s/scores.toml", base);
    local_score_open(path);
    snprintf(path, sizeof(path), "%s/tus", base);
    local_tus_open(path);
    storage_ready = 1;
}

void runtime_np_local_reset(void) {
    pthread_mutex_lock(&lock);
    memset(handles, 0, sizeof(handles));
    memset(state_callbacks, 0, sizeof(state_callbacks));
    storage_ready = 0;
    next_handle = FIRST_HANDLE;
    pthread_mutex_unlock(&lock);
}

static int32_t handle_create(int is_context, int domain) {
    int32_t id = NP_NO_RESOURCE;
    pthread_mutex_lock(&lock);
    ensure_storage();
    for (int i = 0; i < HANDLES; ++i) {
        if (handles[i].used) continue;
        memset(&handles[i], 0, sizeof(handles[i]));
        handles[i].used = 1;
        handles[i].id = id = next_handle++;
        handles[i].is_context = is_context;
        handles[i].domain = domain;
        break;
    }
    pthread_mutex_unlock(&lock);
    return id;
}
/* The slot of a live handle, locked; -1 (unlocked) when it is not one of this kind. */
static int handle_find(int32_t id, int is_context, int domain) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < HANDLES; ++i)
        if (handles[i].used && handles[i].id == id && handles[i].is_context == is_context && handles[i].domain == domain) return i;
    pthread_mutex_unlock(&lock);
    return -1;
}
static int32_t handle_delete(int32_t id, int is_context, int domain) {
    const int slot = handle_find(id, is_context, domain);
    if (slot < 0) return NP_INVALID_ARGUMENT;
    handles[slot].used = 0;
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t handle_complete(int32_t request, int domain, int32_t result) {
    const int slot = handle_find(request, 0, domain);
    if (slot < 0) return NP_INVALID_ARGUMENT;
    handles[slot].result = result;
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t handle_poll(int32_t request, int domain, int32_t *result) {
    const int slot = handle_find(request, 0, domain);
    if (slot < 0) return NP_INVALID_ARGUMENT;
    if (result) *result = handles[slot].aborted ? NP_REQUEST_ABORTED : handles[slot].result;
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t handle_abort(int32_t request, int domain) {
    const int slot = handle_find(request, 0, domain);
    if (slot < 0) return NP_INVALID_ARGUMENT;
    handles[slot].aborted = 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t request_create(int32_t context, int domain) {
    const int slot = handle_find(context, 1, domain);
    if (slot < 0) return NP_INVALID_ARGUMENT;
    pthread_mutex_unlock(&lock);
    return handle_create(0, domain);
}

/* ---- Score ---- */
static ABI int32_t score_create_context(const void *a, const void *b, const void *c) { (void)a; (void)b; (void)c; return handle_create(1, DOMAIN_SCORE); }
static ABI int32_t score_delete_context(int32_t id) { return handle_delete(id, 1, DOMAIN_SCORE); }
static ABI int32_t score_create_request(int32_t context) { return request_create(context, DOMAIN_SCORE); }
static ABI int32_t score_delete_request(int32_t id) { return handle_delete(id, 0, DOMAIN_SCORE); }
static ABI int32_t score_abort_request(int32_t id) { return handle_abort(id, DOMAIN_SCORE); }
static ABI int32_t score_poll(int32_t id, int32_t *result) { return handle_poll(id, DOMAIN_SCORE, result); }

static ABI int32_t score_record_async(int32_t request, uint32_t board, int64_t value, const uint8_t *comment,
                                      const uint8_t *info, uint32_t *tmp_rank, const void *compare_time, void *option) {
    (void)compare_time; (void)option;
    uint64_t info_size = 0;
    if (info) memcpy(&info_size, info, sizeof(info_size));
    if (info && info_size > LOCAL_SCORE_INFO_SIZE - sizeof(info_size)) return NP_INVALID_ARGUMENT;
    if (!local_score_board_valid(board)) return NP_SCORE_BAD_BOARD;
    const LocalScoreResult outcome = local_score_record(board, value, info, comment, (int64_t)time(NULL));
    if (outcome == LOCAL_SCORE_WRITE_FAILED) return NP_NO_RESOURCE;
    if (tmp_rank) *tmp_rank = 1;
    return handle_complete(request, DOMAIN_SCORE, outcome == LOCAL_SCORE_RECORDED ? 0 : NP_SCORE_NOT_BEST);
}

static ABI int32_t score_by_np_id_async(int32_t request, uint32_t board, const uint8_t *np_ids, uint64_t np_ids_size,
                                        uint8_t *ranks, uint64_t ranks_size, void *comments, uint64_t comments_size,
                                        uint8_t *infos, uint64_t infos_size, uint64_t count, void *last_sort,
                                        uint32_t *total, void *option) {
    (void)comments; (void)comments_size; (void)option;
    if (!local_score_board_valid(board)) return NP_SCORE_BAD_BOARD;
    if (!np_ids || !ranks) return NP_INVALID_ARGUMENT;
    uint64_t limit = np_ids_size / NP_ID_SIZE < ranks_size / RANK_ENTRY_SIZE ? np_ids_size / NP_ID_SIZE : ranks_size / RANK_ENTRY_SIZE;
    if (count < limit) limit = count;
    uint8_t local[NP_ID_SIZE];
    local_np_id(local);
    LocalScore score;
    const int have = local_score_get(board, &score);
    int32_t found = 0;
    for (uint64_t i = 0; i < limit; ++i) {
        uint8_t *entry = ranks + i * RANK_ENTRY_SIZE;
        memset(entry, 0, RANK_ENTRY_SIZE);
        const int own = have && !memcmp(np_ids + i * NP_ID_SIZE, local, NP_ID_SIZE);
        if (infos && infos_size >= (i + 1) * LOCAL_SCORE_INFO_SIZE) memset(infos + i * LOCAL_SCORE_INFO_SIZE, 0, LOCAL_SCORE_INFO_SIZE);
        if (!own) continue;
        const int32_t has_data = 1, rank = 1;
        memcpy(entry + RANK_HAS_DATA, &has_data, 4);
        memcpy(entry + RANK_NP_ID, local, NP_ID_SIZE);
        memcpy(entry + RANK_RANK, &rank, 4);
        memcpy(entry + RANK_SCORE, &score.value, 8);
        if (infos && score.has_info && infos_size >= (i + 1) * LOCAL_SCORE_INFO_SIZE)
            memcpy(infos + i * LOCAL_SCORE_INFO_SIZE, score.info, LOCAL_SCORE_INFO_SIZE);
        ++found;
    }
    if (total) *total = (uint32_t)found;
    if (last_sort) memset(last_sort, 0, 8);
    return handle_complete(request, DOMAIN_SCORE, (int32_t)limit);
}

/* Range and friends tables hold nobody but possibly the local player, who is only returned by id. */
static int32_t score_empty_table(int32_t request, uint32_t board, void *last_sort, uint32_t *total) {
    if (!local_score_board_valid(board)) return NP_SCORE_BAD_BOARD;
    if (total) *total = 0;
    if (last_sort) memset(last_sort, 0, 8);
    return handle_complete(request, DOMAIN_SCORE, 0);
}
static ABI int32_t score_range_async(int32_t request, uint32_t board, uint32_t start, void *ranks, uint64_t ranks_size,
                                     void *comments, uint64_t comments_size, void *infos, uint64_t infos_size,
                                     uint64_t count, void *last_sort, uint32_t *total, void *option) {
    (void)start; (void)ranks; (void)ranks_size; (void)comments; (void)comments_size; (void)infos; (void)infos_size;
    (void)count; (void)option;
    return score_empty_table(request, board, last_sort, total);
}
static ABI int32_t score_friends_async(int32_t request, uint32_t board, int32_t include_self, void *ranks, uint64_t ranks_size,
                                       void *comments, uint64_t comments_size, void *infos, uint64_t infos_size,
                                       uint64_t count, void *last_sort, uint32_t *total, void *option) {
    (void)include_self; (void)ranks; (void)ranks_size; (void)comments; (void)comments_size; (void)infos; (void)infos_size;
    (void)count; (void)option;
    return score_empty_table(request, board, last_sort, total);
}

/* ---- TUS ---- */
static ABI int32_t tus_create_context(const void *a, const void *b, const void *c) { (void)a; (void)b; (void)c; return handle_create(1, DOMAIN_TUS); }
static ABI int32_t tus_delete_context(int32_t id) { return handle_delete(id, 1, DOMAIN_TUS); }
static ABI int32_t tus_create_request(int32_t context) { return request_create(context, DOMAIN_TUS); }
static ABI int32_t tus_delete_request(int32_t id) { return handle_delete(id, 0, DOMAIN_TUS); }
static ABI int32_t tus_poll(int32_t id, int32_t *result) { return handle_poll(id, DOMAIN_TUS, result); }

static void tus_fill_status(uint8_t *status, int present, const LocalTusStatus *info) {
    memset(status, 0, TUS_STATUS_SIZE);
    if (!present) return;
    const int32_t has_data = 1;
    const uint64_t date = (uint64_t)info->date * 1000000u, size = info->size;
    memcpy(status + TUS_HAS_DATA, &has_data, 4);
    memcpy(status + TUS_DATE, &date, 8);
    memcpy(status + TUS_DATA_SIZE, &size, 8);
}

static ABI int32_t tus_status_async(int32_t request, const void *online_id, const int32_t *slots, uint8_t *status,
                                    uint64_t status_size, uint64_t count, void *option) {
    (void)option;
    if (!online_id || !slots || !status || count > status_size / TUS_STATUS_SIZE) return NP_INVALID_ARGUMENT;
    const int local = is_local_online_id(online_id);
    for (uint64_t i = 0; i < count; ++i) {
        LocalTusStatus info;
        const int present = local && local_tus_status(slots[i], &info);
        tus_fill_status(status + i * TUS_STATUS_SIZE, present, &info);
    }
    return handle_complete(request, DOMAIN_TUS, (int32_t)count);
}

static ABI int32_t tus_get_data_async(int32_t request, const void *online_id, int32_t slot, uint8_t *status,
                                      uint64_t status_size, void *data, uint64_t capacity, void *option) {
    (void)option;
    if (!online_id || !status || status_size < TUS_STATUS_SIZE || !local_tus_slot_valid(slot)) return NP_INVALID_ARGUMENT;
    LocalTusStatus info;
    const int present = is_local_online_id(online_id) && local_tus_status(slot, &info);
    tus_fill_status(status, present, &info);
    size_t copied = 0;
    if (present && data && capacity) local_tus_read(slot, data, capacity, &copied);
    return handle_complete(request, DOMAIN_TUS, (int32_t)copied);
}

static ABI int32_t tus_set_data_async(int32_t request, const void *online_id, int32_t slot, uint64_t total_size,
                                      uint64_t send_size, const void *data, const void *info, uint64_t info_size) {
    (void)info; (void)info_size;
    if (!online_id || !local_tus_slot_valid(slot) || send_size != total_size || total_size > LOCAL_TUS_MAX_SIZE || (!data && total_size))
        return NP_INVALID_ARGUMENT;
    if (!is_local_online_id(online_id)) return NP_USER_NOT_FOUND;
    if (local_tus_set(slot, data, (size_t)total_size, (int64_t)time(NULL))) return NP_NO_RESOURCE;
    return handle_complete(request, DOMAIN_TUS, 0);
}

static ABI int32_t tus_delete_async(int32_t request, const void *online_id, const int32_t *slots, uint64_t count, void *option) {
    (void)option;
    if (!online_id || !slots) return NP_INVALID_ARGUMENT;
    if (!is_local_online_id(online_id)) return NP_USER_NOT_FOUND;
    for (uint64_t i = 0; i < count; ++i) {
        if (!local_tus_slot_valid(slots[i])) return NP_INVALID_ARGUMENT;
    }
    for (uint64_t i = 0; i < count; ++i) {
        local_tus_delete(slots[i]);
    }
    return handle_complete(request, DOMAIN_TUS, 0);
}

static const RuntimeExport exports[] = {
    {"sceNetCtlGetState", netctl_state}, {"sceNpGetState", np_state},
    {"sceNpRegisterStateCallback", np_register}, {"sceNpCheckCallback", np_check_callback},
    {"sceNpCheckNpAvailability", np_check_availability}, {"sceNpCheckPlus", np_check_plus},
    {"sceNpGetParentalControlInfo", np_parental_control}, {"sceNpGetNpId", np_get_np_id},
    {"sceNpScoreCreateNpTitleCtx", score_create_context}, {"sceNpScoreDeleteNpTitleCtx", score_delete_context},
    {"sceNpScoreCreateRequest", score_create_request}, {"sceNpScoreDeleteRequest", score_delete_request},
    {"sceNpScoreAbortRequest", score_abort_request}, {"sceNpScorePollAsync", score_poll},
    {"sceNpScoreRecordScoreAsync", score_record_async}, {"sceNpScoreGetRankingByNpIdAsync", score_by_np_id_async},
    {"sceNpScoreGetRankingByRangeAsync", score_range_async}, {"sceNpScoreGetFriendsRankingAsync", score_friends_async},
    {"sceNpTusCreateNpTitleCtx", tus_create_context}, {"sceNpTusDeleteNpTitleCtx", tus_delete_context},
    {"sceNpTusCreateRequest", tus_create_request}, {"sceNpTusDeleteRequest", tus_delete_request},
    {"sceNpTusPollAsync", tus_poll},
    {"sceNpTusGetMultiSlotDataStatusAsync", tus_status_async}, {"sceNpTusGetDataAsync", tus_get_data_async},
    {"sceNpTusSetDataAsync", tus_set_data_async}, {"sceNpTusDeleteMultiSlotDataAsync", tus_delete_async},
};
uintptr_t runtime_np_local_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }

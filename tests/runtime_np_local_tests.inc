/* Included by runtime_services_test.c: targeted offline compatibility, local Score and local TUS. */
#define NP_SCORE_NOT_BEST ((int32_t)0x80550815)
#define NP_REQUEST_ABORTED ((int32_t)0x80550716)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define TEST_USER_DIR "np-local-test-user"
#define SCORE_FILE TEST_USER_DIR "/local_services/CUSA03762/scores.toml"
#define TUS_SLOT_2_FILE TEST_USER_DIR "/local_services/CUSA03762/tus/slot_2.bin"
#define CALLER_NETCTL 0x18fc676
#define CALLER_NP_STATE 0x18fc66a
#define CALLER_REGISTER 0x196f74f
#define CALLER_CHECK_CALLBACK 0x198fe40
#define CALLER_AVAILABILITY 0x28f43d6
#define CALLER_PLUS 0x28f4537
#define CALLER_PARENTAL 0x28f4678
#define CALLER_NP_ID 0x18db50b
#define UNRELATED_CALLER 0x1234567
#define REAL_CALLER (UINTPTR_MAX - 1)

typedef int32_t (ABI *StateFn)(int32_t *);
typedef int32_t (ABI *NpStateFn)(int32_t, int32_t *);
typedef int32_t (ABI *RegisterFn)(void *, void *);
typedef int32_t (ABI *AvailabilityFn)(int32_t, const void *, void *);
typedef int32_t (ABI *PlusFn)(int32_t, const void *, uint8_t *);
typedef int32_t (ABI *ParentalFn)(int32_t, const void *, int8_t *, uint8_t *);
typedef int32_t (ABI *NpIdFn)(int32_t, uint8_t *);
typedef int32_t (ABI *CreateCtxFn)(const void *, const void *, const void *);
typedef int32_t (ABI *HandleFn)(int32_t);
typedef int32_t (ABI *PollFn)(int32_t, int32_t *);
typedef int32_t (ABI *RecordFn)(int32_t, uint32_t, int64_t, const void *, const void *, uint32_t *, const void *, void *);
typedef int32_t (ABI *ByNpIdFn)(int32_t, uint32_t, const void *, uint64_t, void *, uint64_t, void *, uint64_t, void *, uint64_t,
                                 uint64_t, void *, uint32_t *, void *);
typedef int32_t (ABI *RangeFn)(int32_t, uint32_t, uint32_t, void *, uint64_t, void *, uint64_t, void *, uint64_t, uint64_t,
                                void *, uint32_t *, void *);
typedef int32_t (ABI *TusStatusFn)(int32_t, const void *, const int32_t *, uint8_t *, uint64_t, uint64_t, void *);
typedef int32_t (ABI *TusGetFn)(int32_t, const void *, int32_t, uint8_t *, uint64_t, void *, uint64_t, void *);
typedef int32_t (ABI *TusSetFn)(int32_t, const void *, int32_t, uint64_t, uint64_t, const void *, const void *, uint64_t);
typedef int32_t (ABI *TusDeleteFn)(int32_t, const void *, const int32_t *, uint64_t, void *);
typedef int32_t (ABI *Fn6)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

static int32_t callback_user, callback_state, callback_calls;
static uint8_t callback_np_id[36];
static void ABI state_callback(int32_t user, int32_t state, const void *np_id, void *arg) {
    (void)arg;
    callback_user = user;
    callback_state = state;
    ++callback_calls;
    memcpy(callback_np_id, np_id, sizeof(callback_np_id));
}

static void test_targeted_overrides(void) {
    int32_t state = 99;
    StateFn netctl = (StateFn)by_name("sceNetCtlGetState");
    NpStateFn np_state = (NpStateFn)by_name("sceNpGetState");
    runtime_np_local_force_caller(UNRELATED_CALLER);
    CHECK(netctl(&state) == 0 && state == 0);
    CHECK(np_state(1, &state) == 0 && state == 1);
    runtime_np_local_force_caller(CALLER_NETCTL);
    CHECK(netctl(&state) == 0 && state == 3);
    CHECK(np_state(1, &state) == 0 && state == 1);
    runtime_np_local_force_caller(CALLER_NP_STATE);
    CHECK(np_state(1, &state) == 0 && state == 2);
    CHECK(netctl(&state) == 0 && state == 0);
    CHECK(local_gate_allows(LOCAL_GATE_NETCTL_STATE, CALLER_NETCTL) && !local_gate_allows(LOCAL_GATE_NETCTL_STATE, CALLER_NP_STATE));
    CHECK(!local_gate_allows(LOCAL_GATE_NP_STATE, UNRELATED_CALLER) && !local_gate_allows(LOCAL_GATE_COUNT, CALLER_NETCTL));

    AvailabilityFn availability = (AvailabilityFn)by_name("sceNpCheckNpAvailability");
    PlusFn plus = (PlusFn)by_name("sceNpCheckPlus");
    ParentalFn parental = (ParentalFn)by_name("sceNpGetParentalControlInfo");
    uint8_t result[33] = {0}, info[3] = {1, 1, 1};
    int8_t age = 0;
    runtime_np_local_force_caller(UNRELATED_CALLER);
    CHECK(availability(1, "x", NULL) == NP_SIGNED_OUT && plus(1, "x", result) == NP_SIGNED_OUT && !result[0]);
    CHECK(parental(1, "x", &age, info) == NP_SIGNED_OUT && info[0] == 1);
    runtime_np_local_force_caller(CALLER_AVAILABILITY);
    CHECK(availability(1, "x", NULL) == 0 && plus(1, "x", result) == NP_SIGNED_OUT);
    runtime_np_local_force_caller(CALLER_PLUS);
    CHECK(plus(1, "x", result) == 0 && result[0] == 1);
    runtime_np_local_force_caller(CALLER_PARENTAL);
    CHECK(parental(1, "x", &age, info) == 0 && age == 20 && !info[0] && !info[1] && !info[2]);

    NpIdFn np_id = (NpIdFn)by_name("sceNpGetNpId");
    uint8_t id[36] = {0};
    runtime_np_local_force_caller(UNRELATED_CALLER);
    CHECK(np_id(1, id) == NP_SIGNED_OUT);
    runtime_np_local_force_caller(CALLER_NP_ID);
    CHECK(np_id(1, id) == 0 && !memcmp(id, "Player", 6));
    CHECK(np_id(2, id) == NP_SIGNED_OUT);
    runtime_np_local_force_caller(REAL_CALLER);
}

static void test_state_callback_delivery(void) {
    RegisterFn register_callback = (RegisterFn)by_name("sceNpRegisterStateCallback");
    Void check = (Void)by_name("sceNpCheckCallback");
    callback_calls = 0;
    runtime_np_local_force_caller(UNRELATED_CALLER);
    CHECK(register_callback((void *)state_callback, NULL) > 0);
    CHECK(check() == 0 && callback_calls == 0);
    runtime_np_local_force_caller(CALLER_REGISTER);
    CHECK(register_callback((void *)state_callback, NULL) == 0);
    runtime_np_local_force_caller(UNRELATED_CALLER);
    CHECK(check() == 0 && callback_calls == 0);
    runtime_np_local_force_caller(CALLER_CHECK_CALLBACK);
    CHECK(check() == 0 && callback_calls == 1 && callback_user == 1 && callback_state == 2 && !memcmp(callback_np_id, "Player", 6));
    CHECK(check() == 0 && callback_calls == 1);
    runtime_np_local_force_caller(REAL_CALLER);
}

static void test_remote_services_stay_offline(void) {
    runtime_np_local_force_caller(CALLER_NETCTL);
    CHECK(((Fn6)by_name("sceNetSocket"))(2, 1, 0, 0, 0, 0) == (int32_t)0x80410133);
    CHECK(((Fn6)by_name("sceNetBind"))(1, 0, 0, 0, 0, 0) == (int32_t)0x80410133);
    CHECK(((Fn6)by_name("sceHttpSendRequest"))(1, 0, 0, 0, 0, 0) == (int32_t)0x80431063);
    CHECK(((Fn6)by_name("sceNpMatching2ContextStart"))(1, 0, 0, 0, 0, 0) == NP_SIGNED_OUT);
    CHECK(((Fn6)by_name("sceNpMatching2CreateContext"))(1, 0, 0, 0, 0, 0) == NP_SIGNED_OUT);
    CHECK(((Fn6)by_name("sceNpWebApiCreateContext"))(1, 0, 0, 0, 0, 0) == NP_SIGNED_OUT);
    CHECK(((Fn6)by_name("sceNpAuthGetAuthorizationCode"))(1, 0, 0, 0, 0, 0) == NP_SIGNED_OUT);
    CHECK(((Fn6)by_name("sceNpScoreGetFriendsRanking"))(1, 0, 0, 0, 0, 0) == NP_SIGNED_OUT);
    runtime_np_local_force_caller(REAL_CALLER);
}

static void restart_services(void) {
    runtime_np_local_reset();
    local_score_open(SCORE_FILE);
}

static int32_t new_request(const char *context_symbol, const char *request_symbol, int32_t *context) {
    *context = ((CreateCtxFn)by_name(context_symbol))(NULL, NULL, NULL);
    return ((HandleFn)by_name(request_symbol))(*context);
}

static int32_t record(uint32_t board, int64_t value, uint32_t *rank, int32_t *poll_result) {
    int32_t context, request = new_request("sceNpScoreCreateNpTitleCtx", "sceNpScoreCreateRequest", &context);
    uint8_t info[LOCAL_SCORE_INFO_SIZE] = {0};
    const int32_t started = ((RecordFn)by_name("sceNpScoreRecordScoreAsync"))(request, board, value, NULL, info, rank, NULL, NULL);
    if (!started) CHECK(((PollFn)by_name("sceNpScorePollAsync"))(request, poll_result) == 0);
    ((HandleFn)by_name("sceNpScoreDeleteRequest"))(request);
    ((HandleFn)by_name("sceNpScoreDeleteNpTitleCtx"))(context);
    return started;
}

static void own_rank(uint32_t board, uint8_t *rank_entry, uint8_t *info_entry, int use_local_id) {
    uint8_t local[36] = {0}, other[36] = {0};
    runtime_np_local_force_caller(CALLER_NP_ID);
    ((NpIdFn)by_name("sceNpGetNpId"))(1, local);
    runtime_np_local_force_caller(REAL_CALLER);
    memcpy(other, "Someone", 7);
    int32_t context, request = new_request("sceNpScoreCreateNpTitleCtx", "sceNpScoreCreateRequest", &context);
    int32_t result = -1;
    CHECK(((ByNpIdFn)by_name("sceNpScoreGetRankingByNpIdAsync"))(request, board, use_local_id ? local : other, 36, rank_entry, 0x88,
                                                                 NULL, 0, info_entry, LOCAL_SCORE_INFO_SIZE, 1, NULL, NULL, NULL) == 0);
    CHECK(((PollFn)by_name("sceNpScorePollAsync"))(request, &result) == 0 && result == 1);
    ((HandleFn)by_name("sceNpScoreDeleteRequest"))(request);
    ((HandleFn)by_name("sceNpScoreDeleteNpTitleCtx"))(context);
}

static int32_t rank_has_data(const uint8_t *entry) {
    int32_t has_data;
    memcpy(&has_data, entry, 4);
    return has_data;
}

static void test_local_score(void) {
    test_remove_tree(TEST_USER_DIR);
    runtime_file_configure("np-local-test-app", TEST_USER_DIR);
    runtime_savedata_configure("CUSA03762");
    restart_services();
    uint8_t entry[0x88], info[LOCAL_SCORE_INFO_SIZE];
    int32_t result = 123;
    uint32_t rank = 0;
    int64_t value;

    own_rank(12, entry, info, 1);
    CHECK(rank_has_data(entry) == 0);

    CHECK(record(12, 5000, &rank, &result) == 0 && result == 0 && rank == 1);
    CHECK(record(12, 4000, &rank, &result) == 0 && result == NP_SCORE_NOT_BEST);
    CHECK(record(12, 5000, &rank, &result) == 0 && result == NP_SCORE_NOT_BEST);
    CHECK(record(12, 7000, &rank, &result) == 0 && result == 0);
    CHECK(record(40, 9000, &rank, &result) == 0 && result == 0);
    CHECK(record(40, 9500, &rank, &result) == 0 && result == NP_SCORE_NOT_BEST);
    CHECK(record(40, 8000, &rank, &result) == 0 && result == 0);
    CHECK(record(104, 1, &rank, &result) != 0);
    CHECK(record(103, 3, &rank, &result) == 0 && result == 0);

    own_rank(12, entry, info, 1);
    memcpy(&value, entry + 0x78, 8);
    memcpy(&rank, entry + 0x68, 4);
    CHECK(rank_has_data(entry) == 1 && value == 7000 && rank == 1);
    own_rank(40, entry, info, 1);
    memcpy(&value, entry + 0x78, 8);
    CHECK(value == 8000);
    own_rank(12, entry, info, 0);
    CHECK(rank_has_data(entry) == 0);
    own_rank(13, entry, info, 1);
    CHECK(rank_has_data(entry) == 0);

    restart_services();
    own_rank(12, entry, info, 1);
    memcpy(&value, entry + 0x78, 8);
    CHECK(value == 7000);
    CHECK(record(12, 6000, &rank, &result) == 0 && result == NP_SCORE_NOT_BEST);
    FILE *leftover = fopen(SCORE_FILE ".sotb-tmp", "rb");
    CHECK(leftover == NULL);
    if (leftover) fclose(leftover);

    int32_t context, request = new_request("sceNpScoreCreateNpTitleCtx", "sceNpScoreCreateRequest", &context);
    uint32_t total = 7;
    CHECK(((RangeFn)by_name("sceNpScoreGetRankingByRangeAsync"))(request, 12, 1, NULL, 0, NULL, 0, NULL, 0, 10, NULL, &total, NULL) == 0);
    CHECK(((PollFn)by_name("sceNpScorePollAsync"))(request, &result) == 0 && result == 0 && total == 0);
    CHECK(((RangeFn)by_name("sceNpScoreGetFriendsRankingAsync"))(request, 999, 1, NULL, 0, NULL, 0, NULL, 0, 10, NULL, NULL, NULL) != 0);
    CHECK(((HandleFn)by_name("sceNpScoreAbortRequest"))(request) == 0);
    CHECK(((PollFn)by_name("sceNpScorePollAsync"))(request, &result) == 0 && result == NP_REQUEST_ABORTED);
    CHECK(((HandleFn)by_name("sceNpTusDeleteRequest"))(request) == NP_INVALID_ARGUMENT);
    CHECK(((HandleFn)by_name("sceNpScoreDeleteRequest"))(request) == 0);
    CHECK(((PollFn)by_name("sceNpScorePollAsync"))(request, &result) == NP_INVALID_ARGUMENT);
    CHECK(((HandleFn)by_name("sceNpScoreAbortRequest"))(request) == NP_INVALID_ARGUMENT);
    CHECK(((HandleFn)by_name("sceNpScoreDeleteRequest"))(request) == NP_INVALID_ARGUMENT);
    CHECK(((HandleFn)by_name("sceNpScoreCreateRequest"))(0x7fff) == NP_INVALID_ARGUMENT);
    ((HandleFn)by_name("sceNpScoreDeleteNpTitleCtx"))(context);

    FILE *file = fopen(SCORE_FILE, "wb");
    CHECK(file != NULL);
    if (file) { fputs("not a score file\n", file); fclose(file); }
    restart_services();
    own_rank(12, entry, info, 1);
    CHECK(rank_has_data(entry) == 0);
    CHECK(record(12, 100, &rank, &result) == 0 && result == 0);
    file = fopen(SCORE_FILE ".corrupt", "rb");
    CHECK(file != NULL);
    if (file) fclose(file);
    restart_services();
    own_rank(12, entry, info, 1);
    memcpy(&value, entry + 0x78, 8);
    CHECK(value == 100);
}

static void test_local_tus(void) {
    restart_services();
    const char *online_id = "Player";
    int32_t context, request = new_request("sceNpTusCreateNpTitleCtx", "sceNpTusCreateRequest", &context), result = -1;
    uint8_t status[0x1f0 * 2], data[16];
    int32_t slots[2] = {1, 2}, has_data;
    uint64_t size;
    CHECK(((TusStatusFn)by_name("sceNpTusGetMultiSlotDataStatusAsync"))(request, online_id, slots, status, sizeof(status), 2, NULL) == 0);
    CHECK(((PollFn)by_name("sceNpTusPollAsync"))(request, &result) == 0 && result == 2);
    memcpy(&has_data, status + 0x24, 4);
    CHECK(has_data == 0);

    const uint8_t blob[5] = {1, 2, 3, 4, 5};
    CHECK(((TusSetFn)by_name("sceNpTusSetDataAsync"))(request, online_id, 1, 5, 5, blob, NULL, 0) == 0);
    CHECK(((TusSetFn)by_name("sceNpTusSetDataAsync"))(request, online_id, 1, 5, 4, blob, NULL, 0) == NP_INVALID_ARGUMENT);
    CHECK(((TusSetFn)by_name("sceNpTusSetDataAsync"))(request, online_id, -1, 5, 5, blob, NULL, 0) == NP_INVALID_ARGUMENT);
    CHECK(((TusSetFn)by_name("sceNpTusSetDataAsync"))(request, "Other", 1, 5, 5, blob, NULL, 0) == (int32_t)0x80550004);
    CHECK(((TusSetFn)by_name("sceNpTusSetDataAsync"))(request, online_id, 2, 3, 3, blob, NULL, 0) == 0);

    restart_services();
    request = new_request("sceNpTusCreateNpTitleCtx", "sceNpTusCreateRequest", &context);
    CHECK(((TusStatusFn)by_name("sceNpTusGetMultiSlotDataStatusAsync"))(request, online_id, slots, status, sizeof(status), 2, NULL) == 0);
    memcpy(&has_data, status + 0x24, 4);
    memcpy(&size, status + 0x60, 8);
    CHECK(has_data == 1 && size == 5);
    memcpy(&size, status + 0x1f0 + 0x60, 8);
    CHECK(size == 3);
    memset(data, 0, sizeof(data));
    CHECK(((TusGetFn)by_name("sceNpTusGetDataAsync"))(request, online_id, 1, status, sizeof(status), data, sizeof(data), NULL) == 0);
    CHECK(((PollFn)by_name("sceNpTusPollAsync"))(request, &result) == 0 && result == 5 && !memcmp(data, blob, 5));
    CHECK(((TusGetFn)by_name("sceNpTusGetDataAsync"))(request, online_id, 1, status, 0x10, data, 4, NULL) == NP_INVALID_ARGUMENT);
    CHECK(((TusGetFn)by_name("sceNpTusGetDataAsync"))(request, online_id, 1, status, 0x1f0, data, 2, NULL) == 0);
    CHECK(((PollFn)by_name("sceNpTusPollAsync"))(request, &result) == 0 && result == 2);
    CHECK(((TusGetFn)by_name("sceNpTusGetDataAsync"))(request, "Other", 1, status, 0x1f0, NULL, 0, NULL) == 0);
    memcpy(&has_data, status + 0x24, 4);
    CHECK(has_data == 0);

    const int32_t first = 1;
    CHECK(((TusDeleteFn)by_name("sceNpTusDeleteMultiSlotDataAsync"))(request, online_id, &first, 1, NULL) == 0);
    CHECK(((TusGetFn)by_name("sceNpTusGetDataAsync"))(request, online_id, 1, status, 0x1f0, NULL, 0, NULL) == 0);
    memcpy(&has_data, status + 0x24, 4);
    CHECK(has_data == 0);
    CHECK(((HandleFn)by_name("sceNpTusDeleteRequest"))(request) == 0);
    CHECK(((PollFn)by_name("sceNpTusPollAsync"))(request, &result) == NP_INVALID_ARGUMENT);

    FILE *file = fopen(TUS_SLOT_2_FILE, "r+b");
    CHECK(file != NULL);
    if (file) { fseek(file, -1, SEEK_END); fputc(0x7f, file); fclose(file); }
    request = new_request("sceNpTusCreateNpTitleCtx", "sceNpTusCreateRequest", &context);
    CHECK(((TusGetFn)by_name("sceNpTusGetDataAsync"))(request, online_id, 2, status, 0x1f0, NULL, 0, NULL) == 0);
    memcpy(&has_data, status + 0x24, 4);
    CHECK(has_data == 0);
    CHECK(local_tus_set(2, blob, 3, 1) == 0 && local_tus_set(5000, blob, 3, 1) != 0);
}

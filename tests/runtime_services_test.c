#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define IME_BUSY ((int32_t)0x80BC0001)
#define IME_NOT_OPENED ((int32_t)0x80BC0002)
#define IME_INVALID_USER_ID ((int32_t)0x80BC0010)
#define IME_INVALID_OPTION ((int32_t)0x80BC0015)
#define IME_INVALID_HANDLER ((int32_t)0x80BC0022)
#define IME_INVALID_ADDRESS ((int32_t)0x80BC0031)
#define IME_INVALID_RESERVED ((int32_t)0x80BC0032)
#define NP_SIGNED_OUT ((int32_t)0x80550006)

typedef struct { uint32_t option; uint8_t reserved1[4]; void *arg; void *handler; uint8_t reserved2[8]; } KeyboardParam;
typedef int32_t (ABI *OpenFn)(int32_t user, const KeyboardParam *param);
typedef int32_t (ABI *CloseFn)(int32_t user);
typedef int32_t (ABI *UpdateFn)(const void *handler);

static void ABI never_called(void *arg, const void *event) { (void)arg; (void)event; ++failures; }

static void *resolve(const char *nid, const char *name) {
    void *function = (void *)runtime_resolve(nid, 0);
    CHECK(function != NULL);
    CHECK(!strcmp(runtime_import_name(nid), name));
    return function;
}

static void test_ime(void) {
    OpenFn open_keyboard = (OpenFn)resolve("eaFXjfJv3xs#libSceIme", "sceImeKeyboardOpen");
    CloseFn close_keyboard = (CloseFn)resolve("PMVehSlfZ94#libSceIme", "sceImeKeyboardClose");
    UpdateFn update = (UpdateFn)resolve("-4GCfYdNF1s#libSceIme", "sceImeUpdate");
    if (!open_keyboard || !close_keyboard || !update) return;

    KeyboardParam param = {0};
    param.handler = (void *)never_called;
    CHECK(update(never_called) == IME_NOT_OPENED);
    CHECK(close_keyboard(1) == IME_NOT_OPENED);
    CHECK(open_keyboard(1, NULL) == IME_INVALID_ADDRESS);
    KeyboardParam no_handler = {0};
    CHECK(open_keyboard(1, &no_handler) == IME_INVALID_HANDLER);
    KeyboardParam bad_option = param;
    bad_option.option = 0x40;
    CHECK(open_keyboard(1, &bad_option) == IME_INVALID_OPTION);
    KeyboardParam bad_reserved = param;
    bad_reserved.reserved2[7] = 1;
    CHECK(open_keyboard(1, &bad_reserved) == IME_INVALID_RESERVED);
    bad_reserved = param;
    bad_reserved.reserved1[0] = 1;
    CHECK(open_keyboard(1, &bad_reserved) == IME_INVALID_RESERVED);
    CHECK(open_keyboard(-1, &param) == IME_INVALID_USER_ID);
    CHECK(close_keyboard(-1) == IME_INVALID_USER_ID);
    CHECK(update(never_called) == IME_NOT_OPENED);

    param.option = 0x3F;
    CHECK(open_keyboard(1, &param) == 0);
    CHECK(open_keyboard(1, &param) == IME_BUSY);
    CHECK(update(never_called) == 0);
    CHECK(update(NULL) == 0);
    CHECK(close_keyboard(1) == 0);
    CHECK(close_keyboard(1) == IME_NOT_OPENED);
    CHECK(update(never_called) == IME_NOT_OPENED);
    CHECK(open_keyboard(1, &param) == 0);
    CHECK(close_keyboard(1) == 0);
}

static const char *const added[] = {
#include "runtime_services_test_imports.inc"
};

static const struct { const char *nid, *symbol; } names[] = {
#include "import_names.inc"
};

static const char *nid_of(const char *symbol) {
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        if (!strcmp(names[i].symbol, symbol)) return names[i].nid;
    }
    return "";
}

static void test_every_added_import_resolves(void) {
    for (size_t i = 0; i < sizeof(added) / sizeof(*added); ++i) {
        const char *nid = nid_of(added[i]);
        CHECK(nid[0] != 0);
        if (!runtime_resolve(nid, 0)) printf("unresolved: %s\n", added[i]);
        CHECK(runtime_resolve(nid, 0) != 0);
    }
}

typedef int32_t (ABI *Void)(void);
typedef int32_t (ABI *ErrnoFn)(int32_t, int32_t *);
typedef int32_t (ABI *UserByOnline)(const void *, int32_t *);
typedef int32_t (ABI *StatusFn)(void *);
typedef int32_t (ABI *Create)(int32_t *);
typedef int32_t (ABI *Read)(int32_t, void *, uint32_t *);

static void *by_name(const char *symbol) {
    void *function = (void *)runtime_resolve(nid_of(symbol), 0);
    CHECK(function != NULL);
    return function;
}

static void test_offline_contracts(void) {
    int32_t value = 0;
    CHECK(((Void)by_name("sceNpWebApiCreateServicePushEventFilter"))() == NP_SIGNED_OUT);
    CHECK(((Void)by_name("sceNetGetSockInfo"))() == (int32_t)0x80410133);
    CHECK(((ErrnoFn)by_name("sceHttpGetLastErrno"))(1, &value) == 0 && value == 51);
    value = 7;
    CHECK(((UserByOnline)by_name("sceNpGetUserIdByOnlineId"))(NULL, &value) == NP_SIGNED_OUT && value == -1);
    unsigned char status[72];
    memset(status, 0xcd, sizeof(status));
    CHECK(((StatusFn)by_name("sceGameLiveStreamingGetCurrentStatus"))(status) == 0);
    unsigned char zeros[72] = {0};
    CHECK(!memcmp(status, zeros, sizeof(status)));
    Create create = (Create)by_name("sceVoiceQoSCreateLocalEndpoint");
    int32_t first = 0, second = 0;
    CHECK(create && create(&first) == 0 && create(&second) == 0 && first && second != first);
    CHECK(create && create(NULL) == (int32_t)0x80A30002);
    uint32_t size = 9;
    CHECK(((Read)by_name("sceVoiceQoSReadPacket"))(1, NULL, &size) == 0 && size == 0);
}

int main(void) {
    runtime_start(1);
    test_ime();
    test_every_added_import_resolves();
    test_offline_contracts();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

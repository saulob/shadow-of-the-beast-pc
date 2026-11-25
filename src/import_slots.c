#define _GNU_SOURCE
#include "import_slots.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static const unsigned char stub_prefix[] = {0x48, 0x89, 0xfe, 0xbf};
static const unsigned char stub_jump[] = {0x48, 0xb8};
static const unsigned char stub_tail[] = {0xff, 0xe0};

void import_traps_build(unsigned char *code, uint64_t count, ImportTrapHandler *handler) {
    const uintptr_t target = (uintptr_t)handler;
    for (uint64_t i = 0; i < count; ++i) {
        unsigned char *t = code + i * IMPORT_TRAP_SIZE;
        const uint32_t index = (uint32_t)i;
        memset(t, 0xcc, IMPORT_TRAP_SIZE);
        memcpy(t, stub_prefix, sizeof(stub_prefix));
        memcpy(t + 4, &index, 4);
        memcpy(t + 8, stub_jump, sizeof(stub_jump));
        memcpy(t + 10, &target, 8);
        memcpy(t + 18, stub_tail, sizeof(stub_tail));
    }
}

static int in_code_traps(const ImportTraps *traps, uintptr_t value) {
    return value - (uintptr_t)traps->code < (traps->count + 1) * IMPORT_TRAP_SIZE;
}

static int in_data_traps(const ImportTraps *traps, uintptr_t value) {
    return value - traps->data < (traps->count + 1) * traps->page_size;
}

int64_t import_trap_code_index(const ImportTraps *traps, uintptr_t value) {
    const uintptr_t offset = value - (uintptr_t)traps->code;
    if (offset >= traps->count * IMPORT_TRAP_SIZE || offset % IMPORT_TRAP_SIZE) return -1;
    const unsigned char *t = traps->code + offset;
    uint32_t index;
    uintptr_t target;
    memcpy(&index, t + 4, 4);
    memcpy(&target, t + 10, 8);
    if (memcmp(t, stub_prefix, sizeof(stub_prefix)) || memcmp(t + 8, stub_jump, sizeof(stub_jump)) ||
        memcmp(t + 18, stub_tail, sizeof(stub_tail)) || target != (uintptr_t)traps->handler ||
        index >= traps->count) {
        return -1;
    }
    return index;
}

int64_t import_trap_data_index(const ImportTraps *traps, uintptr_t value) {
    const uintptr_t offset = value - traps->data;
    if (offset >= traps->count * traps->page_size) return -1;
    return (int64_t)(offset / traps->page_size);
}

SlotClass import_slot_classify(const ImportTraps *traps, uint64_t index, uint64_t kind, uint64_t addend,
                               uintptr_t resolved, uintptr_t actual) {
    const int code_trap = in_code_traps(traps, actual), data_trap = in_data_traps(traps, actual);
    if (resolved) {
        if (code_trap || data_trap || in_code_traps(traps, resolved) || in_data_traps(traps, resolved))
            return SLOT_RESOLVED_TO_TRAP;
        return actual == resolved + addend ? SLOT_RESOLVED_OK : SLOT_WRONG_TARGET;
    }
    if (kind == IMPORT_KIND_CODE) {
        if (data_trap) return SLOT_DATA_CODE_MISMATCH;
        const int64_t trap = code_trap ? import_trap_code_index(traps, actual) : -1;
        if (trap < 0) return SLOT_WRONG_TARGET;
        return (uint64_t)trap == index ? SLOT_TRAP_OK : SLOT_WRONG_TRAP_INDEX;
    }
    if (code_trap) return SLOT_DATA_CODE_MISMATCH;
    const int64_t trap = data_trap ? import_trap_data_index(traps, actual) : -1;
    if (trap < 0 || (actual - traps->data) % traps->page_size != addend) return SLOT_WRONG_TARGET;
    return (uint64_t)trap == index ? SLOT_TRAP_OK : SLOT_WRONG_TRAP_INDEX;
}

const char *import_slot_class_name(SlotClass slot_class) {
    static const char *const names[SLOT_CLASS_COUNT] = {
        "RESOLVED_OK", "TRAP_OK", "WRONG_TARGET", "WRONG_TRAP_INDEX",
        "RESOLVED_TO_TRAP", "DATA_CODE_MISMATCH", "DUPLICATE_CONFLICT",
    };
    return (unsigned)slot_class < SLOT_CLASS_COUNT ? names[slot_class] : "UNKNOWN";
}

int import_target_mismatch(int linked, uint64_t kind, uint64_t binding_kind, int executable) {
    if (linked) return binding_kind != kind;
    return executable != (kind == IMPORT_KIND_CODE);
}

static _Thread_local uint32_t probed_index;
static _Thread_local uintptr_t probed_argument;

static ABI void trap_probe(uint32_t index, uintptr_t argument) {
    probed_index = index;
    probed_argument = argument;
}

#ifdef _WIN32
static unsigned char *allocate_code(size_t bytes) {
    return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static int make_executable(unsigned char *code, size_t bytes) {
    DWORD old;
    if (!VirtualProtect(code, bytes, PAGE_EXECUTE_READ, &old)) return 0;
    FlushInstructionCache(GetCurrentProcess(), code, bytes);
    return 1;
}

static void release_code(unsigned char *code, size_t bytes) {
    (void)bytes;
    VirtualFree(code, 0, MEM_RELEASE);
}
#else
static unsigned char *allocate_code(size_t bytes) {
    void *code = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return code == MAP_FAILED ? NULL : code;
}

static int make_executable(unsigned char *code, size_t bytes) { return !mprotect(code, bytes, PROT_READ | PROT_EXEC); }

static void release_code(unsigned char *code, size_t bytes) { munmap(code, bytes); }
#endif

uint64_t import_traps_abi_failures(uint64_t count) {
    if (!count) return 0;
    const size_t bytes = count * IMPORT_TRAP_SIZE;
    unsigned char *code = allocate_code(bytes);
    if (!code) return count;
    import_traps_build(code, count, trap_probe);
    if (!make_executable(code, bytes)) {
        release_code(code, bytes);
        return count;
    }
    typedef ABI void GuestCall(uintptr_t rdi, uintptr_t rsi, uintptr_t rdx, uintptr_t rcx);
    uint64_t failures = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const uintptr_t argument = UINT64_C(0x5a5a000000000000) | i;
        probed_index = UINT32_MAX;
        probed_argument = 0;
        ((GuestCall *)(void *)(code + i * IMPORT_TRAP_SIZE))(argument, ~argument, i + 1, ~i);
        if (probed_index != (uint32_t)i || probed_argument != argument) ++failures;
    }
    release_code(code, bytes);
    return failures;
}

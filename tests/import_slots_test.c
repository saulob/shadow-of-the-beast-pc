#include "import_slots.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

static int failures;

static void expect(int condition, const char *what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

static ABI void sysv_handler(uint32_t index, uintptr_t argument) { (void)index; (void)argument; }

static uint32_t ms_index;
static void ms_handler(uint32_t index, uintptr_t argument) { ms_index = index; (void)argument; }

static void test_abi_probe(void) {
    expect(import_traps_abi_failures(864) == 0, "SysV trap stubs deliver their own index and the guest rdi");
}

/* The original defect: a handler without sysv_abi reads the index from ecx, the guest's fourth argument. */
static void test_ms_abi_handler_reads_wrong_index(void) {
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    import_traps_build(code, 117, (ImportTrapHandler *)(void *)ms_handler);
    DWORD old;
    VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old);
    typedef ABI void GuestCall(uintptr_t rdi, uintptr_t rsi, uintptr_t rdx, uintptr_t rcx);
    ((GuestCall *)(void *)(code + 116 * IMPORT_TRAP_SIZE))(0x2000, 0, 2, 1);
    expect(ms_index == 1, "an MS-ABI handler reports rcx instead of the trap index");
    VirtualFree(code, 0, MEM_RELEASE);
}

static void test_classification(void) {
    enum { COUNT = 8, PAGE = 4096 };
    static unsigned char code[(COUNT + 1) * IMPORT_TRAP_SIZE];
    unsigned char *data = calloc(COUNT + 1, PAGE);
    import_traps_build(code, COUNT, sysv_handler);
    const ImportTraps t = {code, (uintptr_t)data, COUNT, PAGE, sysv_handler};
    const uintptr_t trap3 = (uintptr_t)code + 3 * IMPORT_TRAP_SIZE, trap1 = (uintptr_t)code + IMPORT_TRAP_SIZE;
    const uintptr_t host = (uintptr_t)&test_classification;

    expect(import_trap_code_index(&t, trap3) == 3, "stub identity comes from the stub bytes");
    expect(import_trap_code_index(&t, trap3 + 4) == -1, "unaligned trap address is not a stub");
    expect(import_slot_classify(&t, 3, IMPORT_KIND_CODE, 0, host, host) == SLOT_RESOLVED_OK, "resolved code");
    expect(import_slot_classify(&t, 3, IMPORT_KIND_CODE, 0, host, trap3) == SLOT_RESOLVED_TO_TRAP, "resolved to trap");
    expect(import_slot_classify(&t, 3, IMPORT_KIND_CODE, 0, host, host + 16) == SLOT_WRONG_TARGET, "wrong target");
    expect(import_slot_classify(&t, 3, IMPORT_KIND_CODE, 0, 0, trap3) == SLOT_TRAP_OK, "own code trap");
    expect(import_slot_classify(&t, 3, IMPORT_KIND_CODE, 0, 0, trap1) == SLOT_WRONG_TRAP_INDEX, "other import's trap");
    expect(import_slot_classify(&t, 3, IMPORT_KIND_CODE, 0, 0, (uintptr_t)data + 3 * PAGE) == SLOT_DATA_CODE_MISMATCH,
           "code slot on a data trap");
    expect(import_slot_classify(&t, 5, IMPORT_KIND_DATA, 8, 0, (uintptr_t)data + 5 * PAGE + 8) == SLOT_TRAP_OK,
           "own data trap with addend");
    expect(import_slot_classify(&t, 5, IMPORT_KIND_DATA, 8, 0, (uintptr_t)data + 4 * PAGE + 8) == SLOT_WRONG_TRAP_INDEX,
           "other import's data trap");
    expect(import_slot_classify(&t, 5, IMPORT_KIND_DATA, 0, 0, trap1) == SLOT_DATA_CODE_MISMATCH, "data slot on a code trap");
    expect(import_slot_classify(&t, 5, IMPORT_KIND_DATA, 0, 0, 0) == SLOT_WRONG_TARGET, "unpatched data slot");

    code[3 * IMPORT_TRAP_SIZE + 4] = 1;
    expect(import_slot_classify(&t, 3, IMPORT_KIND_CODE, 0, 0, trap3) == SLOT_WRONG_TRAP_INDEX,
           "stub whose embedded index names another import");
    free(data);
}

/* Need_sceLibc: a DATA export of the linked libc that lives inside its executable (R+X) segment. */
static void test_linked_data_in_executable_segment(void) {
    enum { COUNT = 8, PAGE = 4096 };
    static unsigned char code[(COUNT + 1) * IMPORT_TRAP_SIZE];
    unsigned char *data = calloc(COUNT + 1, PAGE);
    import_traps_build(code, COUNT, sysv_handler);
    const ImportTraps t = {code, (uintptr_t)data, COUNT, PAGE, sysv_handler};
    static const unsigned char marker[16];
    const uintptr_t target = (uintptr_t)marker;

    expect(!import_target_mismatch(1, IMPORT_KIND_DATA, IMPORT_KIND_DATA, 1), "linked DATA inside an executable segment");
    expect(!import_target_mismatch(1, IMPORT_KIND_CODE, IMPORT_KIND_CODE, 0), "linked CODE outside an executable segment");
    expect(import_target_mismatch(1, IMPORT_KIND_DATA, IMPORT_KIND_CODE, 0), "linked CODE export bound by a DATA slot");
    expect(import_target_mismatch(1, IMPORT_KIND_CODE, IMPORT_KIND_DATA, 1), "linked DATA export bound by a CODE slot");
    expect(!import_target_mismatch(0, IMPORT_KIND_DATA, 0, 0), "host DATA in non-executable memory");
    expect(import_target_mismatch(0, IMPORT_KIND_DATA, 0, 1), "host DATA in executable memory");
    expect(!import_target_mismatch(0, IMPORT_KIND_CODE, 0, 1), "host CODE in executable memory");
    expect(import_target_mismatch(0, IMPORT_KIND_CODE, 0, 0), "host CODE in non-executable memory");

    expect(import_slot_classify(&t, 5, IMPORT_KIND_DATA, 0, target, target) == SLOT_RESOLVED_OK,
           "linked DATA slot holding its expected address");
    expect(import_slot_classify(&t, 5, IMPORT_KIND_DATA, 8, target, target + 8) == SLOT_RESOLVED_OK,
           "linked DATA slot with addend");
    expect(import_slot_classify(&t, 5, IMPORT_KIND_DATA, 0, target, target + 8) == SLOT_WRONG_TARGET,
           "linked DATA slot holding another address");
    free(data);
}

int main(void) {
    test_abi_probe();
    test_ms_abi_handler_reads_wrong_index();
    test_classification();
    test_linked_data_in_executable_segment();
    printf("import_slots_test: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}

#ifndef SOTB_IMPORT_SLOTS_H
#define SOTB_IMPORT_SLOTS_H
#include <stdint.h>

#include "runtime.h"

#define IMPORT_TRAP_SIZE 32
#define IMPORT_KIND_CODE 1
#define IMPORT_KIND_DATA 2

/* The stub passes the import index in edi and the guest's rdi in rsi: the handler must be SysV. */
typedef ABI void ImportTrapHandler(uint32_t index, uintptr_t argument);

typedef struct {
    const unsigned char *code;
    uintptr_t data;
    uint64_t count, page_size;
    ImportTrapHandler *handler;
} ImportTraps;

typedef enum {
    SLOT_RESOLVED_OK,
    SLOT_TRAP_OK,
    SLOT_WRONG_TARGET,
    SLOT_WRONG_TRAP_INDEX,
    SLOT_RESOLVED_TO_TRAP,
    SLOT_DATA_CODE_MISMATCH,
    SLOT_DUPLICATE_CONFLICT,
    SLOT_CLASS_COUNT
} SlotClass;

void import_traps_build(unsigned char *code, uint64_t count, ImportTrapHandler *handler);
/* Index embedded in the stub at value, or -1 when value is not a well-formed stub of this table. */
int64_t import_trap_code_index(const ImportTraps *traps, uintptr_t value);
int64_t import_trap_data_index(const ImportTraps *traps, uintptr_t value);
SlotClass import_slot_classify(const ImportTraps *traps, uint64_t index, uint64_t kind, uint64_t addend,
                               uintptr_t resolved, uintptr_t actual);
const char *import_slot_class_name(SlotClass slot_class);
/* A linked-module export is typed by its ELF symbol (binding_kind); only host targets are judged by page protection. */
int import_target_mismatch(int linked, uint64_t kind, uint64_t binding_kind, int executable);
/* Runs count generated stubs with clobbered rcx/rdx; returns how many delivered a wrong index or argument. */
uint64_t import_traps_abi_failures(uint64_t count);
#endif

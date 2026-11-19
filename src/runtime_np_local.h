#ifndef RUNTIME_NP_LOCAL_H
#define RUNTIME_NP_LOCAL_H
#include <stdint.h>

typedef enum {
    LOCAL_GATE_NETCTL_STATE,
    LOCAL_GATE_NP_STATE,
    LOCAL_GATE_NP_REGISTER,
    LOCAL_GATE_NP_CHECK_CALLBACK,
    LOCAL_GATE_NP_AVAILABILITY,
    LOCAL_GATE_NP_PLUS,
    LOCAL_GATE_NP_PARENTAL,
    LOCAL_GATE_NP_ID,
    LOCAL_GATE_COUNT
} LocalGate;

/* Guest offset of the return address of the call sites the local services were proven to need. */
int local_gate_allows(LocalGate gate, uintptr_t caller);

/* Tests: answer as if called from this guest offset (UINTPTR_MAX - 1 restores the real caller), and drop all
 * handles and cached state as a new process would. */
void runtime_np_local_force_caller(uintptr_t offset);
void runtime_np_local_reset(void);

#endif

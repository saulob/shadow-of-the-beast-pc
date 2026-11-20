#include "startup_phase.h"

#include <stdio.h>
#include <string.h>

static const char *current_phase = "process start";

void sotb_startup_phase(const char *phase) {
    if (!phase || !*phase) return;
    const char *previous = __atomic_exchange_n(&current_phase, phase, __ATOMIC_ACQ_REL);
    if (previous && !strcmp(previous, phase)) return;
    printf("Stage: %s\n", phase);
    fflush(stdout);
}

const char *sotb_startup_phase_current(void) {
    return __atomic_load_n(&current_phase, __ATOMIC_ACQUIRE);
}

void sotb_startup_phase_once(int *reached, const char *phase) {
    if (!reached || __atomic_load_n(reached, __ATOMIC_RELAXED) || __atomic_exchange_n(reached, 1, __ATOMIC_ACQ_REL)) return;
    sotb_startup_phase(phase);
}

#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Emits "@@sotb-progress checks=N" at most once per second, and only when tools\test.ps1 sets SOTB_TEST_PROGRESS. */
static inline void sotb_test_progress(long long checks) {
    static int enabled = -1;
    static long long next_ms;
    if (enabled < 0) enabled = getenv("SOTB_TEST_PROGRESS") != NULL;
    if (!enabled) return;
    struct timespec now;
    if (!timespec_get(&now, TIME_UTC)) return;
    const long long ms = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    if (ms < next_ms) return;
    next_ms = ms + 1000;
    printf("@@sotb-progress checks=%lld\n", checks);
    fflush(stdout);
}

#include "runtime_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char session_log_path[4200];
static int detailed_logging = 1;
static int clean_exit;
static int crash_reported;
static int finalized;

int runtime_log_should_remove(int detailed, int clean, int crashed) {
    return !detailed && clean && !crashed;
}

void runtime_log_set_detailed(int enabled) { detailed_logging = enabled != 0; }

void runtime_log_mark_clean_exit(void) { clean_exit = 1; }

void runtime_log_mark_crash(void) { crash_reported = 1; }

void runtime_log_set_session_path(const char *path) {
    snprintf(session_log_path, sizeof(session_log_path), "%s", path ? path : "");
    clean_exit = 0;
    crash_reported = 0;
    finalized = 0;
}

const char *runtime_log_session_path(void) { return session_log_path; }

unsigned runtime_log_session_number(const char *name) {
    static const char prefix[] = "shadow_of_the_beast_";
    if (!name || strncmp(name, prefix, sizeof(prefix) - 1)) return 0;
    const char *digits = name + sizeof(prefix) - 1;
    unsigned long long number = 0;
    size_t count = 0;
    while (digits[count] >= '0' && digits[count] <= '9') {
        if (++count > 10) return 0;
        number = number * 10 + (unsigned)(digits[count - 1] - '0');
    }
    if (!count || number > 0xFFFFFFFFull) return 0;
    const char *rest = digits + count;
    return strcmp(rest, ".log") && strcmp(rest, ".dmp") ? 0 : (unsigned)number;
}

int runtime_log_dump_path(const char *log_path, char *out, size_t size) {
    const size_t length = log_path ? strlen(log_path) : 0;
    if (length <= 4 || strcmp(log_path + length - 4, ".log") || length >= size) return 0;
    memcpy(out, log_path, length - 4);
    memcpy(out + length - 4, ".dmp", 5);
    return 1;
}

void runtime_log_finalize(void) {
    if (finalized) return;
    finalized = 1;
    if (!runtime_log_should_remove(detailed_logging, clean_exit, crash_reported)) return;
    if (!session_log_path[0]) return;
    runtime_log_flush();
    runtime_log_release();
    remove(session_log_path);
}

void runtime_log_install_cleanup(void) {
    static int installed;
    if (installed) return;
    installed = 1;
    atexit(runtime_log_finalize);
}

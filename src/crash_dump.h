/* The minidump of a crashed session, written beside its log as shadow_of_the_beast_N.dmp. */
#ifndef CRASH_DUMP_H
#define CRASH_DUMP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRASH_DUMPS_KEPT 5

/* Loads dbghelp from System32 and starts the thread that writes the dump; before any crash, after the log exists.
 * 0 when no dump can be written. */
int crash_dump_prepare(void);

/* Writes the dump of `exception_pointers` (EXCEPTION_POINTERS) for the session log and fills `line` with one log line
 * naming the file, never its folder. Waits a bounded time; 1 when the dump was written. */
int crash_dump_write(void *exception_pointers, char *line, size_t size);

/* Deletes the shadow_of_the_beast_N.dmp files of `dir` beyond the `keep` with the highest N; nothing else. The count
 * deleted. */
unsigned crash_dump_prune(const char *dir, unsigned keep);

#ifdef __cplusplus
}
#endif

#endif

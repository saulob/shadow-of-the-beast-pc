#include "crash_dump.h"

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime_log.h"

typedef BOOL(WINAPI *WriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

enum { DUMP_WAIT_MS = 30000, MOST_DUMPS_SEEN = 1024 };

static WriteDump write_minidump;
static HANDLE request_event, done_event;
static int worker_ready;
static struct {
    EXCEPTION_POINTERS *info;
    DWORD thread;
    char path[4300];
    int written;
    DWORD error;
} request;

static void write_requested(void) {
    HANDLE file = CreateFileA(request.path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        request.error = GetLastError();
        return;
    }
    char comment[128];
    const int used = snprintf(comment, sizeof(comment), "Shadow of the Beast PC crash dump: exception 0x%08lx on thread %lu",
                              request.info ? request.info->ExceptionRecord->ExceptionCode : 0ul, request.thread);
    MINIDUMP_USER_STREAM stream = {CommentStreamA, (ULONG)used + 1, comment};
    MINIDUMP_USER_STREAM_INFORMATION streams = {1, &stream};
    MINIDUMP_EXCEPTION_INFORMATION exception = {request.thread, request.info, FALSE};
    /* Stacks, modules and thread times only: no heap, no process environment, no list of every memory region. */
    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    request.written = write_minidump(GetCurrentProcess(), GetCurrentProcessId(), file, type, request.info ? &exception : NULL,
                                     &streams, NULL);
    request.error = request.written ? 0 : GetLastError();
    CloseHandle(file);
    if (!request.written) DeleteFileA(request.path);
}

/* Started before any crash: a crashing thread may have no stack left and may hold the loader lock. */
static DWORD WINAPI dump_worker(LPVOID unused) {
    (void)unused;
    WaitForSingleObject(request_event, INFINITE);
    write_requested();
    SetEvent(done_event);
    return 0;
}

static const char *file_name(const char *path) {
    const char *name = path;
    for (const char *at = path; *at; ++at) {
        if (*at == '\\' || *at == '/') name = at + 1;
    }
    return name;
}

int crash_dump_prepare(void) {
    char dir[4300];
    const char *log = runtime_log_session_path();
    snprintf(dir, sizeof(dir), "%s", log);
    char *name = (char *)file_name(dir);
    if (name > dir) {
        name[-1] = 0;
        crash_dump_prune(dir, CRASH_DUMPS_KEPT);
    }
    const HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    write_minidump = dbghelp ? (WriteDump)(void *)GetProcAddress(dbghelp, "MiniDumpWriteDump") : NULL;
    if (!write_minidump || !log[0]) return 0;
    request_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (request_event && done_event) {
        HANDLE thread = CreateThread(NULL, 256 * 1024, dump_worker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (thread) {
            worker_ready = 1;
            CloseHandle(thread);
        }
    }
    return 1;
}

int crash_dump_write(void *exception_pointers, char *line, size_t size) {
    if (!write_minidump || !runtime_log_dump_path(runtime_log_session_path(), request.path, sizeof(request.path))) {
        snprintf(line, size, "Crash: minidump not written (%s)\n", write_minidump ? "no session log" : "dbghelp unavailable");
        return 0;
    }
    request.info = (EXCEPTION_POINTERS *)exception_pointers;
    request.thread = GetCurrentThreadId();
    request.written = 0;
    if (worker_ready) {
        SetEvent(request_event);
        if (WaitForSingleObject(done_event, DUMP_WAIT_MS) != WAIT_OBJECT_0) {
            snprintf(line, size, "Crash: minidump not written (timed out)\n");
            return 0;
        }
    } else {
        write_requested();
    }
    if (request.written) snprintf(line, size, "Crash: minidump %s written\n", file_name(request.path));
    else snprintf(line, size, "Crash: minidump not written (error 0x%08lx)\n", request.error);
    return request.written;
}

typedef struct {
    unsigned number;
    char name[48];
} DumpFile;

static int newest_first(const void *a, const void *b) {
    const unsigned x = ((const DumpFile *)a)->number, y = ((const DumpFile *)b)->number;
    return x < y ? 1 : x > y ? -1 : 0;
}

unsigned crash_dump_prune(const char *dir, unsigned keep) {
    static DumpFile dumps[MOST_DUMPS_SEEN];
    char pattern[4300], path[4400];
    if (!dir || !dir[0] || snprintf(pattern, sizeof(pattern), "%s\\shadow_of_the_beast_*.dmp", dir) >= (int)sizeof(pattern)) return 0;
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return 0;
    size_t count = 0;
    do {
        const size_t length = strlen(found.cFileName);
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (length >= sizeof(dumps[0].name) || length < 4 || strcmp(found.cFileName + length - 4, ".dmp")) continue;
        const unsigned number = runtime_log_session_number(found.cFileName);
        if (!number || count == MOST_DUMPS_SEEN) continue;
        dumps[count].number = number;
        memcpy(dumps[count].name, found.cFileName, length + 1);
        ++count;
    } while (FindNextFileA(find, &found));
    FindClose(find);
    if (count <= keep) return 0;
    qsort(dumps, count, sizeof(*dumps), newest_first);
    unsigned removed = 0;
    for (size_t i = keep; i < count; ++i) {
        if (snprintf(path, sizeof(path), "%s\\%s", dir, dumps[i].name) < (int)sizeof(path) && DeleteFileA(path)) ++removed;
    }
    return removed;
}

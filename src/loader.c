/* Native x86-64 loader: maps the prepared PS4 image and runs the original code on the host CPU. */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "import_slots.h"
#include "guest_display.h"
#include "skip_intro.h"
#include "guest_patches.h"
#include "isa_emulate.h"
#include "lzcnt_compat.h"
#include "module_diagnostics.h"
#include "runtime.h"
#include "runtime_log.h"
#include "sotb_launcher.h"
#include "sotb_sdl_compat.h"
#include "sotbgpu.h"
#include "trophies/trophy_runtime.h"
#include "startup_phase.h"
#include "fault_origin.h"

#if !defined(_WIN32)
#error The Shadow of the Beast PC port currently targets Windows only.
#endif
#if !defined(__x86_64__) || !defined(__GNUC__)
#error The Shadow of the Beast PC port requires x86-64 GCC or Clang.
#endif

#include <windows.h>

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;

static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static uint64_t image_size;
static Reloc *relocs;
static uint64_t reloc_count;
static ImportTraps import_traps;
static size_t page_size;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;

static void fail(const char *message) {
    fprintf(stderr, "ERROR: %s\nStage: %s\n", message, sotb_startup_phase_current());
    runtime_log_flush();
    exit(1);
}

static void fail_running(const char *message) {
    fprintf(stderr, "ERROR: %s\nStage: %s\n", message, sotb_startup_phase_current());
    runtime_log_flush();
    _exit(1);
}

static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}

static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }

static void *allocate(size_t size) {
    void *low = runtime_low_map(size, 0x02 | 0x04);
    if (low) return low;
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
    return p;
}

static void protect(void *p, size_t size, unsigned flags) {
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
}

/* -1 for the eboot, the linked module number, or -2 outside the image. */
static int image_unit(uint64_t offset) {
    if (offset >= image_size) return -2;
    for (uint64_t m = 0; m < module_count; ++m) {
        if (offset - modules[m].base < modules[m].size) return (int)m;
    }
    return -1;
}

static const char *unit_name(int unit, char *out, size_t size) {
    if (unit == -1) snprintf(out, size, "eboot");
    else if (unit >= 0) snprintf(out, size, "linked module %d", unit);
    else snprintf(out, size, "outside the image");
    return out;
}

static const Reloc *import_relocation_at(uint64_t target) {
    for (uint64_t i = 0; i < reloc_count; ++i) {
        if (relocs[i].target == target && relocs[i].kind) return &relocs[i];
    }
    return NULL;
}

/* A trap is entered by jmp, so the slot is recovered from the caller: call rel32 to a jmp [rip] thunk, or call [rip]. */
static int call_site_slot(uintptr_t return_address, uint64_t *thunk, uint64_t *slot) {
    const uint64_t ret = return_address - (uintptr_t)image;
    if (ret < 6 || ret > image_size) return 0;
    int32_t disp;
    if (image[ret - 5] == 0xe8) {
        memcpy(&disp, image + ret - 4, 4);
        const uint64_t target = ret + (int64_t)disp;
        if (target + 6 <= image_size && image[target] == 0xff && image[target + 1] == 0x25) {
            memcpy(&disp, image + target + 2, 4);
            *thunk = target;
            *slot = target + 6 + (int64_t)disp;
            return 1;
        }
    }
    if (image[ret - 6] == 0xff && image[ret - 5] == 0x15) {
        memcpy(&disp, image + ret - 4, 4);
        *thunk = 0;
        *slot = ret + (int64_t)disp;
        return 1;
    }
    return 0;
}

static void report_trap_call_path(uint32_t id, uintptr_t return_address) {
    char unit[32];
    uint64_t thunk = 0, slot = 0;
    if (!call_site_slot(return_address, &thunk, &slot)) {
        puts("Call path: GOT slot not recoverable from the caller (indirect or tail call)");
        return;
    }
    const Reloc *r = import_relocation_at(slot);
    if (thunk) printf("Call path: PLT thunk 0x%" PRIx64 " -> GOT slot 0x%" PRIx64 " (%s)", thunk, slot,
                      unit_name(image_unit(slot), unit, sizeof(unit)));
    else printf("Call path: call [GOT slot 0x%" PRIx64 "] (%s)", slot, unit_name(image_unit(slot), unit, sizeof(unit)));
    if (!r) puts(" is not an import slot");
    else if (r->value == id) printf(" -> import %u, matches the trap\n", id);
    else printf(" -> import %" PRIu64 " %s; TRAP MISMATCH with import %u\n", r->value, names[r->value], id);
}

static void report_import_users(uint32_t id) {
    int used[17] = {0};
    for (uint64_t i = 0; i < reloc_count; ++i) {
        const int u = relocs[i].kind && relocs[i].value == id ? image_unit(relocs[i].target) : -2;
        if (u >= -1) used[u + 1] = 1;
    }
    char unit[32];
    fputs("Imported by:", stdout);
    for (int u = 0; u <= (int)module_count; ++u) {
        if (used[u]) printf(" %s", unit_name(u - 1, unit, sizeof(unit)));
    }
    putchar('\n');
}

uintptr_t runtime_guest_offset(uintptr_t address) {
    return image && address - (uintptr_t)image < 0x20000000 ? address - (uintptr_t)image : UINTPTR_MAX;
}

static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    const uintptr_t return_address = (uintptr_t)__builtin_return_address(0);
    if (id >= import_count) fail("bad import trap index");
    const char *api = runtime_import_name(names[id]);
    const char *hash = strchr(names[id], '#');
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", api);
    printf("NID: %.*s\n", hash ? (int)(hash - names[id]) : (int)strlen(names[id]), names[id]);
    printf("Library: %s\n", hash ? hash + 1 : "unknown");
    printf("Import: code trap %u of the global import table (%" PRIu64 " imports shared by the eboot and linked modules)\n",
           id, import_count);
    report_import_users(id);
    printf("Startup stage: %s\n", sotb_startup_phase_current());
    printf("Thread: %lu\n", GetCurrentThreadId());
    uintptr_t caller = return_address - (uintptr_t)image;
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n", caller, argument);
    for (uint64_t m = 0; m < module_count; ++m) {
        if (caller >= modules[m].base && caller - modules[m].base < modules[m].size) {
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n", m,
                   m == 0 ? "libc.prx" : "system module", caller - modules[m].base);
        }
    }
    report_trap_call_path(id, return_address);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete."
                      : "Native libc initialization is incomplete; game entry has not run.");
    printf("Controlled stop: unsupported import in stage '%s'\n", sotb_startup_phase_current());
    runtime_log_flush();
    _exit(20);
}

static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }

static void log_cpu(void) {
    char name[64], sets[96];
    const IsaHostFeatures host = isa_host_features();
    isa_host_name(name, sizeof(name));
    printf("CPU: %s\n", name);
    if (isa_missing_sets(host, 1, sets, sizeof(sets))) printf("CPU: emulating %s\n", sets);
    if (isa_missing_sets(host, 0, sets, sizeof(sets))) printf("CPU: missing %s, which cannot be emulated\n", sets);
}

static int cpu_usable(char *cause, size_t cause_size, char *advice, size_t advice_size) {
    return isa_host_usable(isa_host_features(), cause, cause_size, advice, advice_size);
}

static void print_build_line(const char *line) { printf("  %s\n", line); }

static const char *renderer_settings_file;
static int system_usable(char *cause, size_t cause_size, char *advice, size_t advice_size) {
    return cpu_usable(cause, cause_size, advice, advice_size) &&
           sotb_check_renderer(renderer_settings_file, cause, cause_size, advice, advice_size);
}

static int annotate_address(char *out, size_t size, uintptr_t address) {
    const int64_t data_import = import_traps.data ? import_trap_data_index(&import_traps, address) : -1;
    if (data_import >= 0) {
        snprintf(out, size, "unresolved DATA import %s (index %" PRId64 ") +0x%" PRIxPTR, names[data_import],
                 data_import, (address - import_traps.data) % import_traps.page_size);
        return 1;
    }
    if (image && address - (uintptr_t)image < 0x20000000) {
        snprintf(out, size, "guest offset 0x%" PRIxPTR, address - (uintptr_t)image);
        return 1;
    }
    uint64_t site = 0;
    if (runtime_redzone_site(address, &site)) {
        snprintf(out, size, "guest offset 0x%" PRIx64 " (moved access)", site);
        return 1;
    }
    return module_diagnostics_describe(out, size, address);
}

static void describe_address(char *out, size_t size, uintptr_t address) {
    if (!annotate_address(out, size, address)) snprintf(out, size, "%p", (void *)address);
}

static int fatal_reported;
static DWORD fatal_thread;
static char fatal_line[768];

static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported, 1, __ATOMIC_ACQ_REL)) {
        if (__atomic_load_n(&fatal_thread, __ATOMIC_ACQUIRE) == GetCurrentThreadId()) {
            if (fatal_line[0]) runtime_log_write_direct(fatal_line);
            TerminateProcess(GetCurrentProcess(), 3);
        }
        Sleep(INFINITE);
    }
    __atomic_store_n(&fatal_thread, GetCurrentThreadId(), __ATOMIC_RELEASE);
    runtime_log_mark_crash();
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    const CONTEXT *c = info->ContextRecord;
    char where[MAX_PATH + 64];
    describe_address(where, sizeof(where), (uintptr_t)c->Rip);
    snprintf(fatal_line, sizeof(fatal_line), "FATAL exception 0x%08lx at %p (%s) thread %lu\n",
             record->ExceptionCode, (void *)c->Rip, where, GetCurrentThreadId());
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        fprintf(stderr, "Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0] == 1 ? "write" : record->ExceptionInformation[0] == 8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1], where, GetCurrentThreadId());
    } else {
        fprintf(stderr, "Fault: exception 0x%08lx at %s (thread %lu)\n", record->ExceptionCode, where,
                GetCurrentThreadId());
    }
    module_diagnostics_report_crash((uintptr_t)c->Rip,
                                    record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2
                                        ? (uintptr_t)record->ExceptionInformation[1]
                                        : (uintptr_t)c->Rip,
                                    (uintptr_t)image, 0x20000000);
    module_diagnostics_report_thread_name();
    module_diagnostics_report_backtrace(c, (uintptr_t)image, 0x20000000);
    fprintf(stderr,
            "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
            "  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    if (gpu_enabled) sotbgpu_dump_guest_writes(info);
    uintptr_t words[8];
    SIZE_T read = 0;
    if (ReadProcessMemory(GetCurrentProcess(), (void *)c->Rsp, words, sizeof(words), &read) &&
        read == sizeof(words)) {
        for (int i = 0; i < 8; ++i) {
            if (annotate_address(where, sizeof(where), words[i])) {
                fprintf(stderr, "  [rsp+%02x] %016llx %s\n", i * 8, (unsigned long long)words[i], where);
            } else {
                fprintf(stderr, "  [rsp+%02x] %016llx\n", i * 8, (unsigned long long)words[i]);
            }
        }
    }
    uintptr_t rbp = (uintptr_t)c->Rbp;
    for (int depth = 0; depth < 24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void *)rbp, frame, sizeof(frame), &got) ||
            got != sizeof(frame) || frame[0] <= rbp) {
            break;
        }
        describe_address(where, sizeof(where), frame[1]);
        fprintf(stderr, "  #%d %s\n", depth, where);
        rbp = frame[0];
    }
    runtime_log_flush();
    runtime_log_write_direct(fatal_line);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static FaultAddress classify_fault_address(uintptr_t address, HMODULE self) {
    HMODULE module = NULL;
    if (image && address - (uintptr_t)image < 0x20000000) return FAULT_ADDRESS_GUEST;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &module) || !module) {
        return FAULT_ADDRESS_UNLISTED;
    }
    return module == self ? FAULT_ADDRESS_SELF : FAULT_ADDRESS_OTHER_MODULE;
}

static FaultAddress classify_caller(const CONTEXT *c, HMODULE self) {
    uintptr_t caller = 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)c->Rsp, &caller, sizeof(caller), &got) ||
        got != sizeof(caller)) {
        return FAULT_ADDRESS_UNLISTED;
    }
    return classify_fault_address(caller, self);
}

static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    const DWORD code = record->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        /* GPU page tracking is resolved first, except during a speculative read: resolving a GPU page can
         * wait for the GPU thread, which may wait for a lock the reading worker holds. That fault takes the
         * recovery path below. */
        if (gpu_enabled && !runtime_fault_recover) {
            /* The GPU handlers may fault on another tracked page a level or two deep; a bug there would
             * recurse until the stack runs out (a silent freeze). */
            static __thread int handler_depth;
            if (handler_depth >= 8) return fatal_exception(info);
            ++handler_depth;
            const int handled = sotbgpu_handle_fault(info, (void *)record->ExceptionInformation[1]);
            --handler_depth;
            if (handled) {
                runtime_redzone_after_fault(info->ContextRecord);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        if (runtime_fault_recover) {
            unsigned long long *buffer = *runtime_fault_recover;
            runtime_fault_recover = NULL;
            CONTEXT *c = info->ContextRecord;
            c->Rsp = ((c->Rsp - 128) & ~(DWORD64)15) - 40;
            c->Rcx = (DWORD64)(uintptr_t)buffer;
            c->Rdx = 1;
            c->Rip = (DWORD64)(uintptr_t)sotb_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    /* A red-zone stub holds the instruction too when it was moved there. */
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION && image &&
        ((uintptr_t)info->ContextRecord->Rip - (uintptr_t)image < 0x20000000 ||
         runtime_redzone_site((uintptr_t)info->ContextRecord->Rip, NULL))) {
        CONTEXT *c = info->ContextRecord;
        _Static_assert(offsetof(CONTEXT, R15) - offsetof(CONTEXT, Rax) == 15 * sizeof(DWORD64),
                       "CONTEXT register order");
        uint64_t flags = c->EFlags;
        size_t length = isa_emulate((const uint8_t *)(uintptr_t)c->Rip, c->Rip, &c->Rax, &flags);
        if (length) {
            c->EFlags = (DWORD)flags;
            c->Rip += length;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_DATATYPE_MISALIGNMENT ||
        code == 0xC0000008) {
        HMODULE self = GetModuleHandleA(NULL);
        const FaultAddress at = classify_fault_address((uintptr_t)info->ContextRecord->Rip, self);
        if (fault_is_ours(at, at == FAULT_ADDRESS_UNLISTED ? classify_caller(info->ContextRecord, self) : at)) {
            return fatal_exception(info);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }

static __attribute__((noreturn)) void report_here(uint32_t code) {
    CONTEXT context;
    EXCEPTION_RECORD record;
    EXCEPTION_POINTERS pointers = {&record, &context};
    memset(&record, 0, sizeof(record));
    RtlCaptureContext(&context);
    record.ExceptionCode = code;
    record.ExceptionAddress = (PVOID)(uintptr_t)context.Rip;
    fatal_exception(&pointers);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

static void abort_signal(int sig) {
    (void)sig;
    fputs("STOP: abort() was called: an uncaught C++ exception, a pure virtual call or a failed library check\n", stderr);
    report_here(0x40000015u);
}

static void purecall(void) {
    fputs("STOP: pure virtual call\n", stderr);
    report_here(0x40000015u);
}

/* MinGW makes a failed C runtime parameter check non-fatal (the call fails with EINVAL); the first ones are logged. */
static void invalid_parameter(const wchar_t *expression, const wchar_t *function, const wchar_t *file, unsigned line,
                              uintptr_t reserved) {
    static volatile LONG count;
    (void)expression; (void)function; (void)file; (void)line; (void)reserved;
    if (InterlockedIncrement(&count) > 8) return;
    void *frames[4];
    char text[512], where[160];
    int length = snprintf(text, sizeof(text), "Diagnostics: an invalid C runtime parameter (non-fatal), called from:");
    const USHORT n = RtlCaptureStackBackTrace(1, 4, frames, NULL);
    for (USHORT i = 0; i < n && length > 0 && (size_t)length < sizeof(text) - 170; ++i) {
        describe_address(where, sizeof(where), (uintptr_t)frames[i]);
        length += snprintf(text + length, sizeof(text) - (size_t)length, "%s%s", i ? ", " : " ", where);
    }
    snprintf(text + length, sizeof(text) - (size_t)length, "\n");
    runtime_log_write_direct(text);
}

uint32_t runtime_thread_tcb_offset(void);

static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    const uint32_t displacement = runtime_thread_tcb_offset();
    uint64_t patched = 0;
    for (uint64_t s = 0; s < count; ++s) {
        if (!(segments[s].flags & 1) || segments[s].size < sizeof(load)) continue;
        unsigned char *at = image + segments[s].address, *end = at + segments[s].size - sizeof(load);
        for (; at <= end; ++at) {
            if (at[0] == 0x65 && !memcmp(at, load, sizeof(load))) {
                memcpy(at + 5, &displacement, 4);
                ++patched;
                at += sizeof(load) - 1;
            }
        }
    }
    return patched;
}

static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char data[65536];
    const size_t n = fread(data, 1, sizeof(data), f);
    fclose(f);
    if (n < 20 || memcmp(data, "\0PSF", 4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys, data + 8, 4); memcpy(&values, data + 12, 4); memcpy(&count, data + 16, 4);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= n; ++i) {
        const unsigned char *e = data + 20 + i * 16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset, e, 2); memcpy(&format, e + 2, 2); memcpy(&length, e + 4, 4); memcpy(&offset, e + 12, 4);
        if (keys + key_offset >= n || values + offset + length > n ||
            strcmp((const char *)data + keys + key_offset, key)) continue;
        if (format == 0x0404 && number && length >= 4) { memcpy(number, data + values + offset, 4); return 1; }
        if (text && text_size) {
            const size_t copy = length < text_size - 1 ? length : text_size - 1;
            memcpy(text, data + values + offset, copy);
            text[copy] = 0;
        }
        return 1;
    }
    return 0;
}

static int mapped(const Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i) {
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    }
    return 0;
}

static const char *command_line(void) { return GetCommandLineA(); }

static int lzcnt_rewrite_wanted(void) {
    const char *forced = getenv("SOTB_EMULATE_LZCNT");
    return lzcnt_compat_needed(isa_host_features()) || (forced && *forced);
}

typedef struct {
    const Segment *segments;
    uint64_t segment_count;
    const uint64_t *bindings, *binding_kinds;
    int native_libc, gpu_symbols;
} ImportBindings;

static int executable_address(const ImportBindings *b, uintptr_t address) {
    const uint64_t offset = address - (uintptr_t)image;
    if (offset < image_size) {
        for (uint64_t s = 0; s < b->segment_count; ++s) {
            if (offset - b->segments[s].address < b->segments[s].size) return (b->segments[s].flags & 1) != 0;
        }
        return 0;
    }
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((const void *)address, &info, sizeof(info)) || info.State != MEM_COMMIT) return 0;
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

typedef enum { ORIGIN_TRAP, ORIGIN_HOST, ORIGIN_LINKED, ORIGIN_COUNT } TargetOrigin;

/* Recomputes what each import should bind to, independently of the relocation pass, for one kind. */
static uintptr_t expected_import(const ImportBindings *b, uint64_t index, uint64_t kind, int *kind_mismatch,
                                 TargetOrigin *origin) {
    *kind_mismatch = 0;
    *origin = ORIGIN_TRAP;
    uintptr_t address = runtime_resolve(names[index], kind == IMPORT_KIND_DATA);
    const int linked = !address && b->native_libc && b->bindings[index];
    if (linked) address = (uintptr_t)image + b->bindings[index];
    if (!address) return 0;
    *kind_mismatch = import_target_mismatch(linked, kind, b->binding_kinds[index], executable_address(b, address));
    if (linked && *kind_mismatch) return 0;
    *origin = linked ? ORIGIN_LINKED : ORIGIN_HOST;
    return address;
}

static int compare_reloc_targets(const void *a, const void *b) {
    const uint64_t x = relocs[*(const uint32_t *)a].target, y = relocs[*(const uint32_t *)b].target;
    return x < y ? -1 : x > y;
}

static const char *import_library(uint64_t index) {
    const char *hash = strchr(names[index], '#');
    return hash ? hash + 1 : "unknown";
}

static int gpu_registered_library(const char *library) {
    return !strcmp(library, "libSceGnmDriver") || !strcmp(library, "libSceVideoOut");
}

typedef struct {
    uint64_t slots, resolved, traps, imports, lowest, highest;
} UnitSlots;

static void report_regression(const char *label, const char *name, const SlotClass *classes, int *failed) {
    uint64_t slots = 0, ok = 0, index = 0;
    while (index < import_count && strcmp(names[index], name)) ++index;
    for (uint64_t i = 0; index < import_count && i < reloc_count; ++i) {
        if (!relocs[i].kind || relocs[i].value != index) continue;
        ++slots;
        ok += classes[i] == SLOT_RESOLVED_OK;
    }
    if (index == import_count) printf(" %s not imported;", label);
    else printf(" %s %s (%" PRIu64 "/%" PRIu64 " slots resolved);", label, slots && ok == slots ? "ok" : "FAILED", ok,
                slots);
    if (index < import_count && (!slots || ok != slots)) *failed = 1;
}

static void report_library_regression(const char *library, const SlotClass *classes, int *failed) {
    uint64_t imports = 0, ok = 0;
    for (uint64_t index = 0; index < import_count; ++index) {
        if (strcmp(import_library(index), library)) continue;
        uint64_t slots = 0, resolved = 0;
        for (uint64_t i = 0; i < reloc_count; ++i) {
            if (!relocs[i].kind || relocs[i].value != index) continue;
            ++slots;
            resolved += classes[i] == SLOT_RESOLVED_OK;
        }
        ++imports;
        ok += slots && resolved == slots;
    }
    printf(" %s %s (%" PRIu64 "/%" PRIu64 " imports resolved);", library, ok == imports ? "ok" : "FAILED", ok, imports);
    if (ok != imports) *failed = 1;
}

static uint64_t verify_import_slots(const ImportBindings *b, int detailed) {
    unsigned char *kinds = calloc(import_count ? import_count : 1, 1);
    uintptr_t *expected = calloc(import_count ? import_count * 2 : 1, sizeof(*expected));
    unsigned char *mismatched = calloc(import_count ? import_count * 2 : 1, 1);
    unsigned char *origins = calloc(import_count ? import_count * 2 : 1, 1);
    uint32_t *order = calloc(reloc_count ? reloc_count : 1, sizeof(*order));
    unsigned char *overwritten = calloc(reloc_count ? reloc_count : 1, 1);
    SlotClass *classes = calloc(reloc_count ? reloc_count : 1, sizeof(*classes));
    int64_t *code_owner = malloc((import_count ? import_count : 1) * sizeof(*code_owner));
    int64_t *data_owner = malloc((import_count ? import_count : 1) * sizeof(*data_owner));
    if (!kinds || !expected || !mismatched || !origins || !order || !overwritten || !classes || !code_owner || !data_owner)
        fail("allocation failed");
    for (uint64_t i = 0; i < import_count; ++i) code_owner[i] = data_owner[i] = -1;

    for (uint64_t i = 0; i < reloc_count; ++i) {
        order[i] = (uint32_t)i;
        if (relocs[i].kind) kinds[relocs[i].value] |= (unsigned char)relocs[i].kind;
    }
    for (uint64_t i = 0; i < import_count; ++i) {
        for (uint64_t kind = IMPORT_KIND_CODE; kind <= IMPORT_KIND_DATA; ++kind) {
            int mismatch = 0;
            TargetOrigin origin = ORIGIN_TRAP;
            if (kinds[i] & kind) expected[i * 2 + kind - 1] = expected_import(b, i, kind, &mismatch, &origin);
            origins[i * 2 + kind - 1] = (unsigned char)origin;
            mismatched[i * 2 + kind - 1] = (unsigned char)(mismatch || kinds[i] == 3);
        }
    }
    qsort(order, reloc_count, sizeof(*order), compare_reloc_targets);
    uint64_t other_duplicates = 0;
    for (uint64_t i = 1; i < reloc_count; ++i) {
        if (relocs[order[i]].target != relocs[order[i - 1]].target) continue;
        const uint32_t earlier = order[i] < order[i - 1] ? order[i] : order[i - 1];
        overwritten[earlier] = 1;
        if (!relocs[order[i]].kind && !relocs[order[i - 1]].kind) ++other_duplicates;
    }

    uint64_t counts[SLOT_CLASS_COUNT] = {0}, slots = 0, bijection_errors = 0, shown = 0;
    uint64_t targets[2][ORIGIN_COUNT] = {{0}};
    UnitSlots units[17] = {{0}};
    unsigned char *seen = calloc((module_count + 1) * (import_count ? import_count : 1), 1);
    if (!seen) fail("allocation failed");
    for (uint64_t i = 0; i < reloc_count; ++i) {
        const Reloc *r = &relocs[i];
        if (!r->kind) continue;
        uintptr_t actual;
        memcpy(&actual, image + r->target, 8);
        const uintptr_t want = expected[r->value * 2 + r->kind - 1];
        SlotClass c = overwritten[i] ? SLOT_DUPLICATE_CONFLICT
                      : mismatched[r->value * 2 + r->kind - 1]
                          ? SLOT_DATA_CODE_MISMATCH
                          : import_slot_classify(&import_traps, r->value, r->kind, r->addend, want, actual);
        const int64_t code_trap = import_trap_code_index(&import_traps, actual);
        const int64_t data_trap = import_trap_data_index(&import_traps, actual);
        int64_t *owner = code_trap >= 0 ? &code_owner[code_trap] : data_trap >= 0 ? &data_owner[data_trap] : NULL;
        if (owner) {
            if (*owner < 0) *owner = (int64_t)r->value;
            else if (*owner != (int64_t)r->value) ++bijection_errors;
        }
        classes[i] = c;
        ++counts[c];
        ++slots;
        ++targets[r->kind - 1][origins[r->value * 2 + r->kind - 1]];
        UnitSlots *u = &units[image_unit(r->target) + 1];
        if (!u->slots || r->value < u->lowest) u->lowest = r->value;
        if (!u->slots || r->value > u->highest) u->highest = r->value;
        ++u->slots;
        u->resolved += c == SLOT_RESOLVED_OK;
        u->traps += c == SLOT_TRAP_OK;
        unsigned char *first = &seen[(image_unit(r->target) + 1) * import_count + r->value];
        if (!*first) { *first = 1; ++u->imports; }
        if (c != SLOT_RESOLVED_OK && c != SLOT_TRAP_OK && shown++ < 32) {
            char unit[32];
            printf("IMPORT SLOT %s: slot 0x%" PRIx64 " (%s) import %" PRIu64 " %s (%s) %s addend %" PRIu64
                   " expected %p actual %p\n",
                   import_slot_class_name(c), r->target, unit_name(image_unit(r->target), unit, sizeof(unit)), r->value,
                   names[r->value], runtime_import_name(names[r->value]), r->kind == IMPORT_KIND_DATA ? "DATA" : "CODE",
                   r->addend, (void *)(want ? want + r->addend : 0), (void *)actual);
        }
    }
    uint64_t malformed = 0, used_code = 0, used_data = 0;
    for (uint64_t i = 0; i < import_count; ++i) {
        if (import_trap_code_index(&import_traps, (uintptr_t)import_traps.code + i * IMPORT_TRAP_SIZE) != (int64_t)i)
            ++malformed;
        used_code += code_owner[i] >= 0;
        used_data += data_owner[i] >= 0;
        if ((code_owner[i] >= 0 && code_owner[i] != (int64_t)i) || (data_owner[i] >= 0 && data_owner[i] != (int64_t)i))
            ++bijection_errors;
    }
    const uint64_t abi_failures = import_traps_abi_failures(import_count);
    uint64_t errors = slots - counts[SLOT_RESOLVED_OK] - counts[SLOT_TRAP_OK];
    errors += malformed + bijection_errors + abi_failures + other_duplicates;

    printf("Import slots: %" PRIu64 " slots of %" PRIu64 " imports;", slots, import_count);
    for (int c = 0; c < SLOT_CLASS_COUNT; ++c) printf(" %s=%" PRIu64, import_slot_class_name((SlotClass)c), counts[c]);
    printf("\nImport targets: CODE host %" PRIu64 ", linked %" PRIu64 ", trap %" PRIu64 "; DATA host %" PRIu64
           ", linked %" PRIu64 ", trap %" PRIu64 "\n",
           targets[0][ORIGIN_HOST], targets[0][ORIGIN_LINKED], targets[0][ORIGIN_TRAP], targets[1][ORIGIN_HOST],
           targets[1][ORIGIN_LINKED], targets[1][ORIGIN_TRAP]);
    printf("Import traps: %" PRIu64 " stubs (%" PRIu64 " malformed), %" PRIu64 " code and %" PRIu64
           " data traps in use, %" PRIu64 " trap/import mismatches, SysV index probe %s\n",
           import_count, malformed, used_code, used_data, bijection_errors, abi_failures ? "FAILED" : "ok");
    if (other_duplicates) printf("Import slots: %" PRIu64 " non-import relocations share a target\n", other_duplicates);
    int regression_failed = 0;
    fputs("Import checks:", stdout);
    report_regression("sceNetCtlInit", "gky0+oaNM4k#libSceNetCtl", classes, &regression_failed);
    report_regression("memmove", "+P6FRGH4LfA#libc", classes, &regression_failed);
    report_regression("__stack_chk_fail", "Ou3iL1abvng#libkernel", classes, &regression_failed);
    report_regression("__stack_chk_guard", "f7uOxY9mM1U#libkernel", classes, &regression_failed);
    report_regression("Need_sceLibc", "P330P3dFF68#libc", classes, &regression_failed);
    report_regression("Need_sceLibcInternal", "ZT4ODD2Ts9o#libSceLibcInternal", classes, &regression_failed);
    report_library_regression("libSceNgs2", classes, &regression_failed);
    putchar('\n');
    errors += regression_failed;

    uint64_t resolved_imports = 0, trapped_imports = 0;
    for (uint64_t i = 0; i < import_count; ++i) {
        if (!kinds[i]) continue;
        const int trapped = !expected[i * 2 + (kinds[i] & IMPORT_KIND_CODE ? 0 : 1)];
        resolved_imports += !trapped;
        trapped_imports += trapped;
    }
    printf("Imports: %" PRIu64 " resolved, %" PRIu64 " behind traps%s\n", resolved_imports, trapped_imports,
           b->gpu_symbols ? "" : " (GPU symbols not registered)");
    if (detailed) {
        for (int u = 0; u <= (int)module_count; ++u) {
            char unit[32];
            printf("Import unit %s: %" PRIu64 " slots, %" PRIu64 " imports (global indices %" PRIu64 "..%" PRIu64
                   "), resolved %" PRIu64 ", traps %" PRIu64 "\n",
                   unit_name(u - 1, unit, sizeof(unit)), units[u].slots, units[u].imports, units[u].lowest,
                   units[u].highest, units[u].resolved, units[u].traps);
        }
        fputs("Unresolved imports by library:", stdout);
        for (uint64_t i = 0; i < import_count; ++i) {
            if (!kinds[i] || expected[i * 2 + (kinds[i] & IMPORT_KIND_CODE ? 0 : 1)]) continue;
            const char *library = import_library(i);
            uint64_t earlier = 0, same = 0;
            for (uint64_t j = 0; j < import_count; ++j) {
                if (!kinds[j] || expected[j * 2 + (kinds[j] & IMPORT_KIND_CODE ? 0 : 1)] ||
                    strcmp(import_library(j), library)) continue;
                if (j < i) ++earlier;
                ++same;
            }
            if (!earlier) printf(" %s %" PRIu64 "%s;", library, same,
                                 !b->gpu_symbols && gpu_registered_library(library) ? " (GPU-registered, headless)" : "");
        }
        putchar('\n');
    }
    free(seen); free(data_owner); free(code_owner); free(classes); free(overwritten); free(order); free(origins);
    free(mismatched);
    free(expected); free(kinds);
    return errors;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 4 && !strcmp(argv[1], "--prepare-image")) {
        char error[1024];
        if (sotb_build_boot_image(argv[2], argv[3], error, sizeof(error))) {
            fprintf(stderr, "Boot image failed: %s\n", error);
            return 1;
        }
        printf("Boot image written to %s\n", argv[3]);
        return 0;
    }

    runtime_memory_reserve();
    runtime_thread_tcb_offset();
    module_diagnostics_baseline();

    int cpu_only = 0, strict_imports = 0, check_imports = 0, verify_slots = 0;
    int skip_launcher = 0, force_launcher = 0;
    const char *boot = NULL, *content_profile = NULL, *app0 = NULL, *user_dir = NULL, *logs_dir = NULL, *log_file = NULL;
    int first_option = 1;
    if (argc > 1 && strncmp(argv[1], "--", 2)) { boot = argv[1]; first_option = 2; }
    for (int i = first_option; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--check-imports")) check_imports = 1;
        else if (!strcmp(argv[i], "--verify-import-slots")) verify_slots = 1;
        else if (!strcmp(argv[i], "--content-profile") && i + 1 < argc) content_profile = argv[++i];
        else if (!strcmp(argv[i], "--app0") && i + 1 < argc) app0 = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user_dir = argv[++i];
        else if (!strcmp(argv[i], "--logs") && i + 1 < argc) logs_dir = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_file = argv[++i];
        else if (!strcmp(argv[i], "--sotb-skip-launcher")) skip_launcher = 1;
        else if (!strcmp(argv[i], "--launcher")) force_launcher = 1;
        else {
            fprintf(stderr,
                    "Unknown option: %s\nUsage: %s [boot.bin] [--cpu-only] [--strict-imports] [--check-imports] [--verify-import-slots] "
                    "[--content-profile file] [--app0 dir] [--user dir] [--logs dir] [--log file] "
                    "[--sotb-skip-launcher] [--launcher]\n",
                    argv[i], argv[0]);
            return 1;
        }
    }

    static SotbPaths paths;
    const SotbPathRequest request = {boot, content_profile, app0, user_dir, logs_dir};
    if (!sotb_resolve_paths(&request, &paths)) fail(paths.error[0] ? paths.error : "cannot locate the game folders");
    boot = paths.boot;
    content_profile = paths.content_profile;
    app0 = paths.app0;
    user_dir = paths.user_dir;

    static char settings_file[4200];
    snprintf(settings_file, sizeof(settings_file), "%s/sotb.ini", user_dir);
    char log_path[4200];
    static char log_folder[4200];
    snprintf(log_folder, sizeof(log_folder), "%s", paths.logs_dir);
    const int log_started = runtime_log_start(paths.logs_dir, log_file, log_path, sizeof(log_path));
    if (log_started) {
        time_t now = time(NULL);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", localtime(&now));
        printf("[run] %s\n[run] %s\n[run] log %s\n[run] prepared root %s\n", stamp, command_line(), log_path, app0);
        atexit(runtime_log_flush);
        runtime_log_set_detailed(sotb_detailed_logging(settings_file));
        runtime_log_install_cleanup();
        if (!sotb_logs_directory(paths.logs_dir, log_path, log_folder, sizeof(log_folder)))
            snprintf(log_folder, sizeof(log_folder), "%s", paths.logs_dir);
    } else {
        fprintf(stderr, "Cannot create the log %s\n", log_file ? log_file : paths.logs_dir);
    }

    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    signal(SIGABRT, abort_signal);
    _set_invalid_parameter_handler(invalid_parameter);
    _set_purecall_handler(purecall);

    log_cpu();
    renderer_settings_file = settings_file;
    if (!cpu_only && !check_imports && !verify_slots) {
        SotbLauncherConfig launcher = {app0, paths.image_dir, user_dir, settings_file, log_folder,
                                       skip_launcher, force_launcher, system_usable, log_started ? log_path : NULL};
        sotb_startup_phase("launcher handoff");
        const SotbLauncherResult launched = sotb_launcher_run(&launcher);
        sotb_startup_phase("launcher returned");
        runtime_log_set_detailed(sotb_detailed_logging(settings_file));
        if (launched == SOTB_LAUNCHER_QUIT) { runtime_log_mark_clean_exit(); exit(0); }
        if (launched == SOTB_LAUNCHER_BLOCKED) exit(1);
    }

    {
        char cause[160], advice[160], message[330];
        const int graphics_needed = !cpu_only && !check_imports && !verify_slots;
        if (!(graphics_needed ? system_usable(cause, sizeof(cause), advice, sizeof(advice))
                              : cpu_usable(cause, sizeof(cause), advice, sizeof(advice)))) {
            snprintf(message, sizeof(message), "%s %s", cause, advice);
            fail(message);
        }
    }

    if (!sotb_resolve_paths(&request, &paths)) fail(paths.error[0] ? paths.error : "cannot locate the game folders");
    boot = paths.boot;
    content_profile = paths.content_profile;
    app0 = paths.app0;

    sotb_prepare_sdl_gamepad();
    SotbGameBuild game_build;
    const int known_build = sotb_game_build(paths.image_dir, &game_build);
    if (!known_build) {
        char problem[512];
        sotb_game_build_problem(paths.image_dir, problem, sizeof(problem));
        puts("Unsupported build diagnostics begin");
        sotb_game_build_report(paths.image_dir, print_build_line);
        puts("Unsupported build diagnostics end");
        char message[700];
        snprintf(message, sizeof(message), "unsupported build: %s (install from a supported Shadow of the Beast package)", problem);
        fail(message);
    }
    printf("Build fingerprint: %s, region %s, title %s, version %s, code %s\n", game_build.label, game_build.region,
           game_build.title_id, game_build.version, game_build.code_key);
    sotb_startup_phase("game build verified");
    const char *pinned_build = getenv("SOTB_GAME_BUILD");
    if (pinned_build && *pinned_build) {
        char build_key[64];
        snprintf(build_key, sizeof(build_key), "%s-%s", game_build.title_id, game_build.version);
        if (strcmp(pinned_build, build_key)) {
            char message[256];
            snprintf(message, sizeof(message), "SOTB_GAME_BUILD is %s but the game image is %s", pinned_build, build_key);
            fail(message);
        }
    }

    runtime_pad_set_settings(settings_file);
    runtime_set_language(sotb_text_language(settings_file));
    trophy_runtime_configure(app0, user_dir ? user_dir : "user");
    sotb_log_settings_summary(settings_file, game_build.label);
    const SotbResolution *render_resolution = &sotb_resolutions[sotb_render_resolution(settings_file)];
    const int native_target = render_resolution->width == 1920 && render_resolution->height == 1080;

    if (content_profile) {
        sotb_startup_phase("content profile");
        FILE *profile = fopen(content_profile, "rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data, 1, sizeof(data), profile) != sizeof(data) || fgetc(profile) != EOF ||
            memcmp(data, "SOTBCONT", 8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i = 0; i < 5; ++i) {
            unsigned char *p = data + 8 + i * 4;
            values[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        }
        runtime_content_configure(values);
        sotb_startup_phase("content profile configured");
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        sotb_startup_phase("filesystem configured");
        char sfo[4096], id[16] = "";
        snprintf(sfo, sizeof(sfo), "%s/sce_sys/param.sfo", app0);
        if (sfo_value(sfo, "INSTALL_DIR_SAVEDATA", id, sizeof(id), NULL) ||
            sfo_value(sfo, "TITLE_ID", id, sizeof(id), NULL)) {
            runtime_savedata_configure(id);
            printf("Save data: title %s\n", id);
        }
    }
    sotbgpu_register_kernel();

    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    page_size = system_info.dwPageSize;
    if (getenv("SOTB_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict = {0};
        strict.RaiseExceptionOnInvalidHandleReference = 1;
        strict.HandleExceptionsPermanentlyEnabled = 1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &strict, sizeof(strict)))
            fprintf(stderr, "SOTB_STRICT_HANDLES: policy not set (%lu)\n", GetLastError());
    }

    FILE *f = fopen(boot, "rb");
    if (!f) fail("cannot open boot file; install the game from the launcher first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "SOTBBOT1", 8)) fail("bad boot file signature");
    const uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    const uint64_t capabilities = read64(f);
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    sotb_startup_phase("runtime initialization");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512 * 1024 * 1024 || entry >= size || !ns || ns > 64 || nr > 1000000 ||
        import_count > 100000) {
        fail("boot file limits exceeded");
    }
    const int native_libc = !strict_imports && (capabilities & 1);
    uint64_t main_tls[4] = {0};
    uint64_t procparam = 0;
    uint64_t *bindings = calloc(import_count ? import_count : 1, sizeof(*bindings));
    uint64_t *binding_kinds = calloc(import_count ? import_count : 1, sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    procparam = read64(f);
    for (int i = 0; i < 4; ++i) main_tls[i] = read64(f);
    module_count = read64(f);
    if (!module_count || module_count > sizeof(modules) / sizeof(*modules)) fail("invalid module count");
    for (uint64_t m = 0; m < module_count; ++m) {
        LinkedModule *x = &modules[m];
        x->base = read64(f); x->size = read64(f); x->init = read64(f); x->tls_address = read64(f);
        x->tls_memsz = read64(f); x->tls_filesz = read64(f); x->tls_module = read64(f);
    }
    const uint64_t nb = read64(f);
    if (main_tls[1] > main_tls[2] || main_tls[2] > 1024 * 1024 || main_tls[0] > size ||
        main_tls[1] > size - main_tls[0] || (main_tls[3] & (main_tls[3] - 1)) || main_tls[3] > 4096) {
        fail("invalid eboot TLS metadata");
    }
    for (uint64_t m = 0; m < module_count; ++m) {
        LinkedModule *x = &modules[m];
        if (x->base >= size || !x->size || x->size > size - x->base || x->init < x->base ||
            x->init - x->base >= x->size ||
            (x->tls_module && (x->tls_address >= size || x->tls_memsz > size - x->tls_address ||
                               x->tls_memsz > 1024 * 1024 || x->tls_filesz > x->tls_memsz || x->tls_module < 2 ||
                               x->tls_module > 7))) {
            fail("invalid linked module metadata");
        }
    }
    if (nb > import_count) fail("invalid binding count");
    for (uint64_t i = 0; i < nb; ++i) {
        const uint64_t index = read64(f), address = read64(f), kind = read64(f);
        int inside = 0;
        for (uint64_t m = 0; m < module_count; ++m) {
            if (address >= modules[m].base && address - modules[m].base < modules[m].size) inside = 1;
        }
        if (index >= import_count || !inside || (kind != 1 && kind != 2) || bindings[index])
            fail("invalid native binding");
        bindings[index] = address;
        binding_kinds[index] = kind;
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    reloc_count = nr;
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i) {
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    }
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) {
            fail("bad relocation");
        }
    }
    for (uint64_t m = 0; m < module_count; ++m) {
        if (!mapped(segments, ns, modules[m].init, 1) ||
            (modules[m].tls_module && !mapped(segments, ns, modules[m].tls_address, modules[m].tls_memsz))) {
            fail("unmapped module metadata");
        }
    }
    if (module_count && !mapped(segments, ns, procparam, 64)) fail("unmapped procparam");
    for (uint64_t i = 0; i < import_count; ++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments, ns, bindings[i], 1)) fail("unmapped native export");
        if (binding_kinds[i] == 1) {
            int executable = 0;
            for (uint64_t s = 0; s < ns; ++s) {
                if ((segments[s].flags & 1) && bindings[i] >= segments[s].address &&
                    bindings[i] - segments[s].address < segments[s].size) executable = 1;
            }
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    image_size = size;
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (runtime_memory_prepare()) {
        MessageBoxW(NULL,
                    L"Shadow of the Beast could not reserve or commit guest memory. Freeing virtual memory or disk "
                    L"space for the page file may help. Your saved data has not been opened.",
                    L"Shadow of the Beast - memory unavailable", MB_OK | MB_ICONERROR);
        fail("guest memory unavailable before starting GPU or game threads");
    }
    if (verify_slots) {
        puts("GPU: not initialized while verifying import slots");
    } else if (!cpu_only && !sotbgpu_available()) {
        puts("GPU: no video core in this build; running CPU-side only");
    } else if (!cpu_only) {
        char serial[16] = "UNKNOWN", sfo[4096];
        uint32_t attributes = 0;
        snprintf(sfo, sizeof(sfo), "%s/sce_sys/param.sfo", app0 ? app0 : ".");
        sfo_value(sfo, "TITLE_ID", serial, sizeof(serial), NULL);
        sfo_value(sfo, "ATTRIBUTE", NULL, 0, &attributes);
        uint64_t sdk = 0;
        if (procparam) memcpy(&sdk, image + procparam + 16, 8);
        char running[48];
        snprintf(running, sizeof(running), "%s %s", game_build.title_id, game_build.version);
        SotbGpuConfig gpu = {SOTB_WINDOW_TITLE, running, serial, user_dir, settings_file, (uint32_t)sdk, attributes,
                             1920, 1080, SOTB_GPU_FEATURE_RENDER_RESOLUTION};
        gpu_enabled = 1;
        if (sotbgpu_init(&gpu)) fail("GPU initialization failed");
        sotb_startup_phase("GPU initialized");
        printf("GPU: window and presenter ready; SDK 0x%08x, %u HLE symbols\n", (unsigned)sdk, sotbgpu_symbol_count());
        module_diagnostics_report();
    }

    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    import_traps_build(traps, import_count, unresolved);
    import_traps = (ImportTraps){traps, (uintptr_t)data_traps, import_count, page_size, unresolved};
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2
                              ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                          : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                                                : (uintptr_t)image + relocs[i].value;
        const uintptr_t resolved = relocs[i].kind
                                       ? runtime_resolve(names[relocs[i].value], relocs[i].kind == 2)
                                       : 0;
        if (relocs[i].kind) {
            if (resolved) {
                value = resolved + relocs[i].addend;
            } else if (native_libc && bindings[relocs[i].value]) {
                const uint64_t address = bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value] != relocs[i].kind ||
                    !mapped(segments, ns, address, relocs[i].addend + 1)) {
                    fail("native export kind/range mismatch");
                }
                value = (uintptr_t)image + address + relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    sotb_startup_phase("imports resolved");
    if (check_imports) {
        unsigned char *checked = calloc(import_count ? import_count : 1, 1);
        if (!checked) fail("allocation failed");
        uint64_t missing = 0;
        for (uint64_t i = 0; i < nr; ++i) {
            const uint64_t index = relocs[i].value;
            if (!relocs[i].kind || checked[index]) continue;
            checked[index] = 1;
            if (runtime_resolve(names[index], relocs[i].kind == 2) || (native_libc && bindings[index])) continue;
            printf("MISSING: %s (%s)\n", names[index], runtime_import_name(names[index]));
            ++missing;
        }
        printf("Imports: %" PRIu64 " missing\n", missing);
        runtime_log_flush();
        _exit(missing ? 1 : 0);
    }
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
    {
        const size_t cave_size = round_page(GUEST_PATCH_CAVE_SIZE);
        const int rewrite_lzcnt = lzcnt_rewrite_wanted();
        const size_t lzcnt_stubs = rewrite_lzcnt ? lzcnt_compat_stub_bytes() : 0;
        unsigned char *cave = allocate(round_page(cave_size + lzcnt_stubs));
        if (rewrite_lzcnt) {
            const LzcntBuild *build = NULL;
            const LzcntCompatStatus status =
                lzcnt_compat_install(image, size, entry, cave + cave_size, lzcnt_stubs, &build);
            if (status != LZCNT_COMPAT_OK) {
                char message[160];
                snprintf(message, sizeof(message), "LZCNT compatibility: %s", lzcnt_compat_status_text(status));
                fail(message);
            }
            printf("CPU: LZCNT rewritten in %zu patch ranges of %s\n", build->count, build->build);
        }
        guest_patches_apply(image, size, cave, cave_size);
        if (sotb_skip_intro(settings_file)) {
            char reason[160];
            if (skip_intro_apply(image, size, cave + 1024, cave_size - 1024, reason, sizeof(reason))) {
                printf("Skip Intro: on, startup presentation hidden\n");
            } else {
                printf("Skip Intro: requested but unavailable: %s\n", reason);
            }
        } else {
            printf("Skip Intro: off\n");
        }
        protect(cave, round_page(cave_size + lzcnt_stubs), 5);
    }
    {
        char error[160];
        if (!native_target && !guest_apply_display_size(image, size, render_resolution->width, render_resolution->height, error, sizeof(error))) {
            char message[256];
            snprintf(message, sizeof(message), "render resolution is not available: %s", error);
            fail_running(message);
        }
        printf("Display: render resolution %dx%d (%s)\n", render_resolution->width, render_resolution->height, render_resolution->key);
    }
    sotb_startup_phase("patches installed");
    if (gpu_enabled) {
        uint64_t *triples = calloc(ns * 3, sizeof(*triples));
        if (!triples) fail_running("allocation failed");
        for (uint64_t i = 0; i < ns; ++i) {
            triples[3 * i] = segments[i].address;
            triples[3 * i + 1] = segments[i].size;
            triples[3 * i + 2] = segments[i].flags;
        }
        runtime_redzone_install(image, size, triples, ns);
        free(triples);
    }
    protect(traps, round_page((import_count + 1) * 32), 5);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    const ImportBindings import_bindings = {segments, ns, bindings, binding_kinds, native_libc, gpu_enabled};
    const uint64_t slot_errors = verify_import_slots(&import_bindings, verify_slots);
    if (verify_slots) {
        printf("Import slot verification: %s\n", slot_errors ? "FAILED" : "passed");
        runtime_log_flush();
        _exit(slot_errors ? 1 : 0);
    }
    if (slot_errors) fail_running("import slot verification failed");
    sotb_startup_phase("guest image mapped");
    if (native_libc) {
        for (uint64_t m = 0; m < module_count; ++m) {
            int init_executable = 0;
            for (uint64_t i = 0; i < ns; ++i) {
                if ((segments[i].flags & 1) && modules[m].init >= segments[i].address &&
                    modules[m].init - segments[i].address < segments[i].size) {
                    init_executable = 1;
                }
            }
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module) {
                runtime_set_module_tls(modules[m].tls_module, image + modules[m].tls_address, modules[m].tls_filesz,
                                       modules[m].tls_memsz);
            }
        }
        runtime_set_main_tls(image + main_tls[0], main_tls[1], main_tls[2], main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image + procparam);
        sotb_startup_phase("linked modules initialization");
        for (uint64_t m = 0; m < module_count; ++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",
                   m, modules[m].init, nb);
            typedef int(ABI *ModuleInit)(uint64_t, void *, void *);
            const int result = ((ModuleInit)(image + modules[m].init))(0, NULL, NULL);
            printf("Module %" PRIu64 " initializer returned %d\n", m, result);
            if (result) fail("module initializer failed");
        }
    }
    sotb_startup_phase("guest entry");
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game = 1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    typedef void(ABI *Entry)(void *, void(ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
    fail("entry unexpectedly returned");
}

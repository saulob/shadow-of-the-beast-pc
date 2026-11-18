/* Imports the shared runtime files do not own, with the offline, single-user contracts of runtime_services.c.
 * The libc start-up and service contracts follow Catherine: Full Body PC and MediEvil PC (runtime_extra.c). */
#include "runtime.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define E_SRCH 3
#define E_INVAL 22
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_UNAVAILABLE ((int32_t)0x80B80001)

static ABI int32_t ok_noop(void) { return 0; }
static ABI void *null_pointer(void) { return NULL; }
static ABI int32_t not_found(void) { return ERR(E_SRCH); }

/* ---- libc.prx start-up (SDK 7.00 and 9.50): thread-exit and sanitizer hooks the runtime does not need ---- */
typedef struct { int64_t tv_sec, tv_nsec; } GuestTimespec;
static ABI int32_t kernel_nanosleep(const GuestTimespec *request, GuestTimespec *remain) {
    if (!request) return ERR(E_INVAL);
#ifdef _WIN32
    compat_sleep_ns((uint64_t)request->tv_sec*1000000000u+(uint64_t)request->tv_nsec);
#else
    struct timespec t={(time_t)request->tv_sec,(long)request->tv_nsec};
    while (nanosleep(&t,&t) && errno==EINTR) {}
#endif
    if (remain) remain->tv_sec=remain->tv_nsec=0;
    return 0;
}
static ABI int32_t thread_name(void *thread, char *name) {
    if (!name) return ERR(E_INVAL);
    const char *n=runtime_thread_name(thread);
    if (!n) return ERR(E_SRCH);
    snprintf(name,32,"%s",n);
    return 0;
}
static ABI int32_t kernel_chmod(const char *path, uint16_t mode) { (void)path; (void)mode; return 0; }
static ABI int32_t kernel_utimes(const char *path, const void *times) { (void)path; (void)times; return 0; }

/* The game's crash handler is recorded; the host fault reporter owns the report. */
static void (*coredump_handler)(uint64_t);
static ABI int32_t coredump_register(void *handler, uint64_t stack_size, void *common) {
    (void)stack_size; (void)common;
    coredump_handler=(void (*)(uint64_t))handler;
    printf("Runtime: coredump handler registered (%s)\n", handler ? "game callback" : "none");
    return 0;
}
static ABI int32_t coredump_write(const void *data, uint64_t size) {
    if (!data && size) return ERR(E_INVAL);
    printf("Runtime: coredump user data write (%" PRIu64 " bytes) acknowledged\n", size);
    return 0;
}

/* ---- POSIX rwlocks: errno results, not SCE codes ---- */
typedef int32_t (ABI *RwOne)(void **);
static int32_t posix_code(int32_t r) { return r ? (int32_t)((uint32_t)r & 0xffff) : 0; }
#define POSIX_RW(name,nid) static ABI int32_t posix_rw_##name(void **rw) { \
    return posix_code(((RwOne)runtime_rwlock_resolve(nid))(rw)); }
POSIX_RW(rdlock,"Ox9i0c7L5w0#libkernel")
POSIX_RW(wrlock,"mqdNorrB+gI#libkernel")
POSIX_RW(unlock,"+L98PIbGttk#libkernel")

/* ---- POSIX sockets: the cable is unplugged, as in runtime_services.c's libSceNet ----
 * Every call fails with the errno the PS4 libc would report without a network, so an online
 * subsystem sees a normal offline machine instead of trapping on an unimplemented import. */
#define POSIX_EBADF 9
#define POSIX_ENOTCONN 57
#define POSIX_ENETUNREACH 51
#define POSIX_EINVAL 22
static int socket_reported;
static void socket_offline(const char *call) {
    if (!__atomic_exchange_n(&socket_reported, 1, __ATOMIC_ACQ_REL)) {
        printf("Runtime: %s: network unavailable (offline contract)\n", call);
    }
    *runtime_errno() = POSIX_ENETUNREACH;
}
static ABI int32_t posix_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    socket_offline("socket");
    return -1;
}
static ABI int32_t posix_socket_op(int fd, const void *a, uint32_t b) {
    (void)a; (void)b;
    *runtime_errno() = fd < 0 ? POSIX_EBADF : POSIX_ENETUNREACH;
    return -1;
}
static ABI int32_t posix_listen(int fd, int backlog) {
    (void)backlog;
    *runtime_errno() = fd < 0 ? POSIX_EBADF : POSIX_ENETUNREACH;
    return -1;
}
static ABI int32_t posix_accept(int fd, void *addr, uint32_t *len) {
    (void)addr; (void)len;
    *runtime_errno() = fd < 0 ? POSIX_EBADF : POSIX_ENETUNREACH;
    return -1;
}
static ABI int64_t posix_send(int fd, const void *buf, uint64_t len, int flags) {
    (void)buf; (void)len; (void)flags;
    *runtime_errno() = fd < 0 ? POSIX_EBADF : POSIX_ENETUNREACH;
    return -1;
}
static ABI int64_t posix_recv(int fd, void *buf, uint64_t len, int flags) {
    (void)buf; (void)len; (void)flags;
    *runtime_errno() = fd < 0 ? POSIX_EBADF : POSIX_ENETUNREACH;
    return -1;
}
static ABI int64_t posix_sendto(int fd, const void *buf, uint64_t len, int flags, const void *to, uint32_t tolen) {
    (void)buf; (void)len; (void)flags; (void)to; (void)tolen;
    *runtime_errno() = fd < 0 ? POSIX_EBADF : POSIX_ENETUNREACH;
    return -1;
}
static ABI int64_t posix_recvfrom(int fd, void *buf, uint64_t len, int flags, void *from, uint32_t *fromlen) {
    (void)buf; (void)len; (void)flags; (void)from; (void)fromlen;
    *runtime_errno() = fd < 0 ? POSIX_EBADF : POSIX_ENETUNREACH;
    return -1;
}
static ABI int32_t posix_sockname_op(int fd, void *name, uint32_t *len) {
    (void)name; (void)len;
    *runtime_errno() = fd < 0 ? POSIX_EBADF : POSIX_ENETUNREACH;
    return -1;
}
static ABI int32_t posix_sockopt(int fd, int level, int optname, void *value, uint32_t *len) {
    (void)level; (void)optname; (void)value; (void)len;
    *runtime_errno() = fd < 0 ? POSIX_EBADF : POSIX_ENETUNREACH;
    return -1;
}
static ABI int32_t posix_shutdown(int fd, int how) {
    (void)how;
    *runtime_errno() = fd < 0 ? POSIX_EBADF : POSIX_ENOTCONN;
    return -1;
}
static ABI int32_t posix_select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout) {
    (void)nfds; (void)readfds; (void)writefds; (void)exceptfds; (void)timeout;
    socket_offline("select");
    return -1;
}
/* FreeBSD numbers the guest uses: F_GETFD/F_SETFD/F_GETFL/F_SETFL are the only commands the port needs. */
static ABI int32_t posix_fcntl(int fd, int command, uint64_t argument) {
    (void)fd; (void)argument;
    if (command == 1 || command == 2 || command == 3 || command == 4) {
        return 0;
    }
    *runtime_errno() = POSIX_EINVAL;
    return -1;
}

/* ---- Dialogs the offline console never shows: opening fails, status stays idle ---- */
static ABI int32_t dialog_unavailable(void) { return DIALOG_UNAVAILABLE; }
static ABI int32_t dialog_none(void) { return 0; }

/* ---- HTTP without a network: settings are accepted, nothing can be read back ---- */
static ABI int32_t http_network(void) { return HTTP_NETWORK; }

static const RuntimeExport exports[]={
    {"_nanosleep",kernel_nanosleep}, {"scePthreadGetname",thread_name},
    {"sceKernelChmod",kernel_chmod}, {"sceKernelUtimes",kernel_utimes},
    {"_sceKernelRtldThreadAtexitIncrement",ok_noop}, {"_sceKernelRtldThreadAtexitDecrement",ok_noop},
    {"_sceKernelSetThreadAtexitCount",ok_noop}, {"_sceKernelSetThreadAtexitReport",ok_noop},
    {"_sceKernelSetThreadDtors",ok_noop},
    {"sceKernelIsAddressSanitizerEnabled",ok_noop},
    {"sceKernelGetSanitizerMallocReplaceExternal",null_pointer},
    {"sceKernelGetSanitizerNewReplaceExternal",null_pointer},
    {"sceKernelInternalMemoryGetModuleSegmentInfo",not_found},
    {"sceSysmoduleGetModuleInfoForUnwind",not_found},
    {"pthread_rwlock_rdlock",posix_rw_rdlock}, {"pthread_rwlock_wrlock",posix_rw_wrlock},
    {"pthread_rwlock_unlock",posix_rw_unlock},
    {"socket",posix_socket}, {"bind",posix_socket_op}, {"connect",posix_socket_op},
    {"listen",posix_listen}, {"accept",posix_accept},
    {"send",posix_send}, {"recv",posix_recv}, {"sendto",posix_sendto}, {"recvfrom",posix_recvfrom},
    {"getsockname",posix_sockname_op}, {"getpeername",posix_sockname_op},
    {"setsockopt",posix_sockopt}, {"getsockopt",posix_sockopt},
    {"shutdown",posix_shutdown}, {"fcntl",posix_fcntl}, {"select",posix_select},
    {"sceInvitationDialogInitialize",ok_noop}, {"sceInvitationDialogTerminate",ok_noop},
    {"sceInvitationDialogOpenA",dialog_unavailable}, {"sceInvitationDialogOpen",dialog_unavailable},
    {"sceInvitationDialogUpdateStatus",dialog_none},
    {"sceInvitationDialogGetResultA",dialog_unavailable}, {"sceInvitationDialogGetResult",dialog_unavailable},
    {"sceGameCustomDataDialogInitialize",ok_noop}, {"sceGameCustomDataDialogTerminate",ok_noop},
    {"sceGameCustomDataDialogOpenA",dialog_unavailable}, {"sceGameCustomDataDialogOpen",dialog_unavailable},
    {"sceGameCustomDataDialogUpdateStatus",dialog_none},
    {"sceGameCustomDataDialogGetResultA",dialog_unavailable}, {"sceGameCustomDataDialogGetResult",dialog_unavailable},
    {"sceSigninDialogOpen",dialog_unavailable}, {"sceSigninDialogUpdateStatus",dialog_none},
    {"sceSigninDialogGetResult",dialog_unavailable},
    {"sceNpProfileDialogOpenA",dialog_unavailable}, {"sceNpCommerceDialogGetResult",dialog_unavailable},
    {"sceNpCommerceShowPsStoreIcon",ok_noop}, {"sceNpCommerceHidePsStoreIcon",ok_noop},
    {"sceNpTrophyShowTrophyList",ok_noop}, {"sceNpTrophyCaptureScreenshot",ok_noop},
    {"sceHttpSetAuthEnabled",ok_noop}, {"sceHttpSetAuthInfoCallback",ok_noop}, {"sceHttpSetCookieEnabled",ok_noop},
    {"sceHttpSetRecvTimeOut",ok_noop}, {"sceHttpSetResolveRetry",ok_noop}, {"sceHttpSetResolveTimeOut",ok_noop},
    {"sceHttpSetSendTimeOut",ok_noop}, {"sceHttpsSetSslCallback",ok_noop},
    {"sceHttpGetAllResponseHeaders",http_network}, {"sceHttpGetMemoryPoolStats",http_network},
    {"sceNpRegisterStateCallbackA",ok_noop}, {"sceNpUnregisterStateCallbackA",ok_noop},
    {"sceCoredumpRegisterCoredumpHandler",coredump_register}, {"sceCoredumpWriteUserData",coredump_write},
    {"sceGameLiveStreamingInitialize",ok_noop}, {"sceGameLiveStreamingTerminate",ok_noop},
    /* Console features with no PC counterpart: accepted and ignored. */
    {"sceGameLiveStreamingEnableLiveStreaming",ok_noop}, {"sceVideoRecordingSetInfo",ok_noop},
    {"sceShareUtilityAdvanceInitialize",ok_noop}, {"sceShareUtilityAdvanceTerminate",ok_noop},
    {"sceShareUtilityAdvanceSetUploadContentData",ok_noop},
};
uintptr_t runtime_extra_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }

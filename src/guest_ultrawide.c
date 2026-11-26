#define _GNU_SOURCE
#include "guest_ultrawide.h"
#include "guest_display.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#endif

#define PI 3.14159265358979323846
#define FUNCTION_FNV64 0xf78826940e3bd846ull
#define CONSTRUCTOR_SIZE 56u
#define CONSTRUCTOR_FNV64 0x32fca59308f9c833ull
#define HOOK_RESUME (SOTB_ULTRAWIDE_SITE + SOTB_ULTRAWIDE_PATCH_SIZE)
#define COMPOUND_ARRANGE_OFFSET 0x808a50u
#define FRONTEND_CONSTRUCTOR_FNV64 0x3b66b989fc48756full
#define FRONTEND_TOP_WIDGET_FNV64 0x54b4fdec9cf658aeull
#define FRONTEND_TICK_FNV64 0x1ee921d70e3a1ce7ull
#define FRONTEND_STACK_LIMIT 64

typedef void (GUEST_ABI *ArrangeChildrenFn)(void *self, const GuestSlateGeometry *allotted, void *arranged);
typedef void (GUEST_ABI *FrontendTickFn)(void *self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, float seconds);

static const uint8_t displaced_prologue[SOTB_ULTRAWIDE_PATCH_SIZE] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

static const uint8_t compound_arrange_bytes[] = {
    0x48, 0x8d, 0x87, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x8f, 0x38, 0x01, 0x00, 0x00, 0x48, 0x89, 0xf7,
};

static const struct {
    uint32_t slot, offset;
} layer_vtable[] = {
    {0, 0x279acd0}, {1, 0x279ace0}, {2, 0x145d9f0}, {3, 0x1de5100}, {4, 0x80aa00},
    {25, 0x145da20}, {61, 0x1de5130}, {62, COMPOUND_ARRANGE_OFFSET}, {63, 0x145c760}, {64, 0x145c8d0},
};

static void (*loading_sink)(void);
static void (*frontend_sink)(int context);
static uint64_t (*clock_source)(void);
static uint64_t last_widened_ms;
static int widened_once;
static ArrangeChildrenFn original_layer_arrange;
static FrontendTickFn original_frontend_tick;
static const uint8_t *frontend_image;
static int frontend_context_now;

static int fail(char *error, size_t size, const char *reason) {
    if (error && size) snprintf(error, size, "%s", reason);
    return 0;
}

static uint64_t fnv1a64(const uint8_t *data, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

static uint64_t system_clock_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

static uint64_t now_ms(void) {
    return clock_source ? clock_source() : system_clock_ms();
}

void sotb_ultrawide_set_clock(uint64_t (*clock_ms)(void)) {
    clock_source = clock_ms;
    __atomic_store_n(&widened_once, 0, __ATOMIC_RELEASE);
}

int sotb_ultrawide_wanted(const char *aspect_key) {
    return aspect_key && (!strcmp(aspect_key, "21:9") || !strcmp(aspect_key, "32:9"));
}

int sotb_ultrawide_enabled(const char *aspect_key, int true_ultrawide) {
    return true_ultrawide && sotb_ultrawide_wanted(aspect_key);
}

double sotb_ultrawide_fov(double fov_degrees, double original_aspect, double target_aspect) {
    const double half = fov_degrees * PI / 360.0;
    return atan(tan(half) * target_aspect / original_aspect) * 360.0 / PI;
}

static float read_float(const uint8_t *at) {
    float value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static int32_t read_int(const uint8_t *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

void GUEST_ABI sotb_ultrawide_adjust_view(uint8_t *view_info, const uint8_t *projection_data) {
    if (!view_info || !projection_data) return;
    const int constrained = view_info[SOTB_VIEW_CONSTRAIN] & 1;
    const int perspective = view_info[SOTB_VIEW_PROJECTION_MODE] == 0;
    const double fov = read_float(view_info + SOTB_VIEW_FOV);
    const double aspect = read_float(view_info + SOTB_VIEW_ASPECT_RATIO);
    const uint8_t *rect = projection_data + SOTB_PROJECTION_VIEW_RECT;
    const int32_t width = read_int(rect + 8) - read_int(rect);
    const int32_t height = read_int(rect + 12) - read_int(rect + 4);
    const double viewport = height > 0 ? (double)width / height : 0.0;
    const int wider = aspect > 0.5 && aspect < 4.0 && viewport > aspect * 1.01;
    const int widen = constrained && perspective && wider && fov > 0.0 && fov < 170.0;
    if (!widen) return;
    const float new_fov = (float)sotb_ultrawide_fov(fov, aspect, viewport), new_aspect = (float)viewport;
    memcpy(view_info + SOTB_VIEW_FOV, &new_fov, sizeof(new_fov));
    memcpy(view_info + SOTB_VIEW_ASPECT_RATIO, &new_aspect, sizeof(new_aspect));
    __atomic_store_n(&last_widened_ms, now_ms(), __ATOMIC_RELEASE);
    __atomic_store_n(&widened_once, 1, __ATOMIC_RELEASE);
}

static void set_frontend_context(int context) {
    const int previous = __atomic_exchange_n(&frontend_context_now, context, __ATOMIC_ACQ_REL);
    if (frontend_sink && previous != context) frontend_sink(context);
}

static void loading_screen_seen(const GuestSlateGeometry *allotted) {
    if (!loading_sink || allotted->size_y <= 0.0f || allotted->size_x <= allotted->size_y * (16.0f / 9.0f) * 1.01f) return;
    set_frontend_context(SOTB_CONTEXT_OTHER);
    loading_sink();
}

void sotb_ultrawide_enable_side_fill(void (*mark_loading)(void), void (*set_frontend)(int context)) {
    loading_sink = mark_loading;
    frontend_sink = set_frontend;
    guest_loading_screen_set_observer(mark_loading ? loading_screen_seen : NULL);
}

int sotb_frontend_context_of_screen(int screen) {
    switch (screen) {
    case SOTB_SCREEN_MAIN_MENU: return SOTB_CONTEXT_TITLE;
    case SOTB_SCREEN_MAP:
    case SOTB_SCREEN_MAP_LIVE_FEED:
    case SOTB_SCREEN_MAP_CONFIRMATION:
    case SOTB_SCREEN_MAP_INFO:
    case SOTB_SCREEN_TIMELINE_INFO:
    case SOTB_SCREEN_MANA_OVERLAY: return SOTB_CONTEXT_MAP;
    case SOTB_SCREEN_OPTIONS:
    case SOTB_SCREEN_SHOP_CONTAINER: return SOTB_CONTEXT_MENU;
    default: return SOTB_CONTEXT_OTHER;
    }
}

int sotb_frontend_context(void) {
    return __atomic_load_n(&frontend_context_now, __ATOMIC_ACQUIRE);
}

int sotb_ultrawide_use_safe_area(int context, int widened_recently) {
    if (context == SOTB_CONTEXT_TITLE || context == SOTB_CONTEXT_MAP || context == SOTB_CONTEXT_MENU) return 1;
    return widened_recently;
}

int sotb_ultrawide_safe_area_active(void) {
    if (!__atomic_load_n(&widened_once, __ATOMIC_ACQUIRE)) return 0;
    const uint64_t last = __atomic_load_n(&last_widened_ms, __ATOMIC_ACQUIRE), now = now_ms();
    return now >= last && now - last <= SOTB_SAFE_AREA_HOLD_MS;
}

static void put_bytes(uint8_t **at, const void *bytes, size_t size) {
    memcpy(*at, bytes, size);
    *at += size;
}

static void put64(uint8_t **at, uint64_t value) {
    memcpy(*at, &value, sizeof(value));
    *at += sizeof(value);
}

static void emit_stub(uint8_t *cave, uint8_t *resume) {
    static const uint8_t save[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x48, 0x89, 0xce, 0x48, 0xb8};
    static const uint8_t call_and_restore[] = {0xff, 0xd0, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};
    static const uint8_t jump_load[] = {0x49, 0xba};
    static const uint8_t jump[] = {0x41, 0xff, 0xe2};
    uint8_t *p = cave;
    put_bytes(&p, save, sizeof(save));
    put64(&p, (uint64_t)(uintptr_t)sotb_ultrawide_adjust_view);
    put_bytes(&p, call_and_restore, sizeof(call_and_restore));
    put_bytes(&p, displaced_prologue, sizeof(displaced_prologue));
    put_bytes(&p, jump_load, sizeof(jump_load));
    put64(&p, (uint64_t)(uintptr_t)resume);
    put_bytes(&p, jump, sizeof(jump));
}

static int projection_matches(const uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if ((uint64_t)SOTB_ULTRAWIDE_SITE + SOTB_ULTRAWIDE_SIZE > image_size) return fail(error, error_size, "image too short");
    const uint8_t *function = image + SOTB_ULTRAWIDE_SITE;
    if (memcmp(function, displaced_prologue, sizeof(displaced_prologue)) ||
        fnv1a64(function, SOTB_ULTRAWIDE_SIZE) != FUNCTION_FNV64) {
        return fail(error, error_size, "projection function does not match a supported build");
    }
    return 1;
}

int guest_ultrawide_check(const uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if (!image) return fail(error, error_size, "missing image");
    return projection_matches(image, image_size, error, error_size);
}

int guest_ultrawide_apply(uint8_t *image, size_t image_size, uint8_t *cave, size_t cave_size, char *error,
                          size_t error_size) {
    if (!image || !cave) return fail(error, error_size, "missing image or cave");
    if (cave_size < SOTB_ULTRAWIDE_MIN_CAVE_SIZE) return fail(error, error_size, "cave too small");
    if (!projection_matches(image, image_size, error, error_size)) return 0;
    const intptr_t rel = cave - (image + SOTB_ULTRAWIDE_SITE + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return fail(error, error_size, "cave out of 32-bit jump range");

    emit_stub(cave, image + HOOK_RESUME);
    const int32_t disp = (int32_t)rel;
    image[SOTB_ULTRAWIDE_SITE] = 0xe9;
    memcpy(image + SOTB_ULTRAWIDE_SITE + 1, &disp, sizeof(disp));
    image[SOTB_ULTRAWIDE_SITE + 5] = 0x90;
    return 1;
}

static GUEST_ABI void layer_arrange_children(void *self, const GuestSlateGeometry *allotted, void *arranged) {
    if (!sotb_ultrawide_use_safe_area(sotb_frontend_context(), sotb_ultrawide_safe_area_active())) {
        original_layer_arrange(self, allotted, arranged);
        return;
    }
    const GuestSlateGeometry safe = guest_safe_area_geometry(allotted);
    original_layer_arrange(self, &safe, arranged);
}

int guest_ultrawide_check_safe_area(const uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if (!image) return fail(error, error_size, "missing image");
    const uint64_t vtable_end = (uint64_t)SOTB_GAME_LAYER_VTABLE + 65u * sizeof(uint64_t);
    if (vtable_end > image_size || (uint64_t)SOTB_GAME_LAYER_CONSTRUCTOR + CONSTRUCTOR_SIZE > image_size ||
        (uint64_t)COMPOUND_ARRANGE_OFFSET + sizeof(compound_arrange_bytes) > image_size) {
        return fail(error, error_size, "image too short");
    }
    if (fnv1a64(image + SOTB_GAME_LAYER_CONSTRUCTOR, CONSTRUCTOR_SIZE) != CONSTRUCTOR_FNV64) {
        return fail(error, error_size, "game layer constructor does not match a supported build");
    }
    if (memcmp(image + COMPOUND_ARRANGE_OFFSET, compound_arrange_bytes, sizeof(compound_arrange_bytes))) {
        return fail(error, error_size, "unexpected SCompoundWidget::OnArrangeChildren bytes");
    }
    for (size_t i = 0; i < sizeof(layer_vtable) / sizeof(*layer_vtable); ++i) {
        uint64_t entry = 0;
        memcpy(&entry, image + SOTB_GAME_LAYER_VTABLE + (uint64_t)layer_vtable[i].slot * sizeof(entry), sizeof(entry));
        if (entry != (uint64_t)(uintptr_t)(image + layer_vtable[i].offset)) {
            return fail(error, error_size, "unexpected SGameLayerManager vtable");
        }
    }
    return 1;
}

int guest_ultrawide_apply_safe_area(uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if (!guest_ultrawide_check_safe_area(image, image_size, error, error_size)) return 0;
    original_layer_arrange = (ArrangeChildrenFn)(uintptr_t)(image + COMPOUND_ARRANGE_OFFSET);
    const uint64_t hook = (uint64_t)(uintptr_t)layer_arrange_children;
    memcpy(image + SOTB_GAME_LAYER_VTABLE + (uint64_t)SOTB_GAME_LAYER_ARRANGE_SLOT * sizeof(hook), &hook, sizeof(hook));
    return 1;
}

static int read_guest(const void *from, void *to, size_t size) {
#ifdef _WIN32
    return ReadProcessMemory(GetCurrentProcess(), from, to, size, NULL) != 0;
#else
    const struct iovec local = {to, size}, remote = {(void *)(uintptr_t)from, size};
    const ssize_t read = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (read >= 0 || errno == EFAULT) return read == (ssize_t)size;
    memcpy(to, from, size); /* a sandbox that forbids the call */
    return 1;
#endif
}

/* The FrontendScreen value on top of the manager's screen stack; -1 when the object or its stack cannot be trusted. */
static int top_screen(const uint8_t *manager) {
    uint64_t vtable = 0, entries = 0;
    int32_t count = 0;
    uint8_t screen = 0;
    if (!manager || !frontend_image || !read_guest(manager, &vtable, sizeof(vtable)) ||
        vtable != (uint64_t)(uintptr_t)(frontend_image + SOTB_FRONTEND_VTABLE)) {
        return -1;
    }
    if (!read_guest(manager + SOTB_FRONTEND_STACK_NUM, &count, sizeof(count)) ||
        !read_guest(manager + SOTB_FRONTEND_STACK_DATA, &entries, sizeof(entries)) || count < 0 || count > FRONTEND_STACK_LIMIT) {
        return -1;
    }
    if (count == 0) return 0;
    if (!entries || !read_guest((const uint8_t *)(uintptr_t)entries + (uint64_t)(count - 1) * SOTB_FRONTEND_STACK_ENTRY, &screen, 1)) return -1;
    return screen;
}

static GUEST_ABI void frontend_tick(void *self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, float seconds) {
    original_frontend_tick(self, a1, a2, a3, a4, a5, seconds);
    const int screen = top_screen((const uint8_t *)self);
    if (screen >= 0) set_frontend_context(sotb_frontend_context_of_screen(screen));
}

int guest_ultrawide_check_frontend(const uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if (!image) return fail(error, error_size, "missing image");
    const uint64_t vtable_end = (uint64_t)SOTB_FRONTEND_VTABLE + (SOTB_FRONTEND_TICK_SLOT + 1u) * sizeof(uint64_t);
    if (vtable_end > image_size || (uint64_t)SOTB_FRONTEND_CONSTRUCTOR + SOTB_FRONTEND_CONSTRUCTOR_SIZE > image_size ||
        (uint64_t)SOTB_FRONTEND_TOP_WIDGET + SOTB_FRONTEND_TOP_WIDGET_SIZE > image_size ||
        (uint64_t)SOTB_FRONTEND_TICK + SOTB_FRONTEND_TICK_SIZE > image_size) {
        return fail(error, error_size, "image too short");
    }
    if (fnv1a64(image + SOTB_FRONTEND_CONSTRUCTOR, SOTB_FRONTEND_CONSTRUCTOR_SIZE) != FRONTEND_CONSTRUCTOR_FNV64 ||
        fnv1a64(image + SOTB_FRONTEND_TOP_WIDGET, SOTB_FRONTEND_TOP_WIDGET_SIZE) != FRONTEND_TOP_WIDGET_FNV64 ||
        fnv1a64(image + SOTB_FRONTEND_TICK, SOTB_FRONTEND_TICK_SIZE) != FRONTEND_TICK_FNV64) {
        return fail(error, error_size, "frontend manager does not match a supported build");
    }
    uint64_t entry = 0;
    memcpy(&entry, image + SOTB_FRONTEND_VTABLE + (uint64_t)SOTB_FRONTEND_TICK_SLOT * sizeof(entry), sizeof(entry));
    if (entry != (uint64_t)(uintptr_t)(image + SOTB_FRONTEND_TICK)) return fail(error, error_size, "unexpected frontend manager vtable");
    return 1;
}

int guest_ultrawide_apply_frontend(uint8_t *image, size_t image_size, char *error, size_t error_size) {
    if (!guest_ultrawide_check_frontend(image, image_size, error, error_size)) return 0;
    original_frontend_tick = (FrontendTickFn)(uintptr_t)(image + SOTB_FRONTEND_TICK);
    frontend_image = image;
    const uint64_t hook = (uint64_t)(uintptr_t)frontend_tick;
    memcpy(image + SOTB_FRONTEND_VTABLE + (uint64_t)SOTB_FRONTEND_TICK_SLOT * sizeof(hook), &hook, sizeof(hook));
    return 1;
}

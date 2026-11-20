#include "skip_intro.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define GUEST_ABI __attribute__((sysv_abi))
#define DELAY_CONSTANT 0x30f2950
#define SHOW_START_BUTTON_TARGET 0x268100
#define MIN_CAVE_SIZE 128

/* vmovss xmm0,[1.0f]; vucomiss xmm0,[this+0x308]; jae wait; mov byte [this+0x305],4; mov dword [this+0x308],0 */
static const uint8_t expected[] = {
    0xc5, 0xfa, 0x10, 0x05, 0xec, 0x83, 0xf2, 0x02, 0xc4, 0xc1, 0x78, 0x2e, 0x87, 0x08, 0x03, 0x00, 0x00,
    0x0f, 0x83, 0x0c, 0x08, 0x00, 0x00, 0x41, 0xc6, 0x87, 0x05, 0x03, 0x00, 0x00, 0x04,
    0x41, 0xc7, 0x87, 0x08, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t one_float[] = {0x00, 0x00, 0x80, 0x3f};
static const uint8_t zero_delay[SKIP_INTRO_PATCH_SIZE] = {0xc5, 0xf8, 0x57, 0xc0, 0x90, 0x90, 0x90, 0x90};

/* lea rdx,[rbp-0x320]; xor ecx,ecx; mov rdi,r15; mov rsi,r15; call ShowStartButton; mov rdi,[rbp-0x320] */
static const uint8_t reveal_context[] = {
    0x48, 0x8d, 0x95, 0xe0, 0xfc, 0xff, 0xff, 0x31, 0xc9, 0x4c, 0x89, 0xff, 0x4c, 0x89, 0xfe,
    0xe8, 0x45, 0xda, 0x09, 0x00,
    0x48, 0x8b, 0xbd, 0xe0, 0xfc, 0xff, 0xff,
};
#define REVEAL_CONTEXT_OFFSET (SKIP_INTRO_REVEAL_CALL - 15)

static atomic_int hidden;
static atomic_llong armed_at_ms;

static long long now_ms(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int skip_intro_hide_presentation(void) {
    if (!atomic_load(&hidden)) return 0;
    if (now_ms() - atomic_load(&armed_at_ms) > SKIP_INTRO_HIDE_LIMIT_MS) {
        if (atomic_exchange(&hidden, 0)) printf("Skip Intro: presentation shown after the time limit\n");
        return 0;
    }
    return 1;
}

static GUEST_ABI void reveal(void) {
    if (atomic_exchange(&hidden, 0)) {
        printf("Skip Intro: finished at Press Start\n");
        fflush(stdout);
    }
}

static int fail(char *error, size_t size, const char *reason) {
    if (error && size) snprintf(error, size, "%s", reason);
    return 0;
}

static size_t emit_reveal_stub(uint8_t *cave, const uint8_t *show_start_button) {
    static const uint8_t reserve[] = {0x48, 0x83, 0xec, 0x18};
    static const uint8_t keep[] = {0x48, 0x89, 0x04, 0x24, 0x48, 0x89, 0x54, 0x24, 0x08};
    static const uint8_t restore[] = {0x48, 0x8b, 0x04, 0x24, 0x48, 0x8b, 0x54, 0x24, 0x08, 0x48, 0x83, 0xc4, 0x18, 0xc3};
    const uint64_t original = (uint64_t)(uintptr_t)show_start_button, handler = (uint64_t)(uintptr_t)reveal;
    uint8_t *p = cave;
    memcpy(p, reserve, sizeof(reserve));
    p += sizeof(reserve);
    *p++ = 0x48;
    *p++ = 0xb8;
    memcpy(p, &original, 8);
    p += 8;
    *p++ = 0xff;
    *p++ = 0xd0;
    memcpy(p, keep, sizeof(keep));
    p += sizeof(keep);
    *p++ = 0x48;
    *p++ = 0xb8;
    memcpy(p, &handler, 8);
    p += 8;
    *p++ = 0xff;
    *p++ = 0xd0;
    memcpy(p, restore, sizeof(restore));
    return (size_t)(p + sizeof(restore) - cave);
}

int skip_intro_apply(uint8_t *image, size_t image_size, uint8_t *cave, size_t cave_size, char *error, size_t error_size) {
    atomic_store(&hidden, 0);
    if (!image || !cave || image_size < DELAY_CONSTANT + sizeof(one_float)) return fail(error, error_size, "image too short");
    if (cave_size < MIN_CAVE_SIZE) return fail(error, error_size, "no room for the reveal hook");
    const intptr_t after = (cave + cave_size) - image, before = (image + image_size) - cave;
    if (after >= 0x7ff00000 || before >= 0x7ff00000 || after <= -0x7ff00000 || before <= -0x7ff00000) {
        return fail(error, error_size, "the reveal hook is out of reach");
    }
    if (memcmp(image + SKIP_INTRO_SITE, expected, sizeof(expected))) return fail(error, error_size, "startup code does not match CUSA03762 1.00");
    if (memcmp(image + DELAY_CONSTANT, one_float, sizeof(one_float))) return fail(error, error_size, "startup delay is not the expected constant");
    if (memcmp(image + REVEAL_CONTEXT_OFFSET, reveal_context, sizeof(reveal_context))) return fail(error, error_size, "Press Start code does not match CUSA03762 1.00");
    uint8_t *call = image + SKIP_INTRO_REVEAL_CALL;
    const int32_t stub_rel = (int32_t)(cave - (call + 5));
    emit_reveal_stub(cave, image + SHOW_START_BUTTON_TARGET);
    memcpy(image + SKIP_INTRO_SITE, zero_delay, sizeof(zero_delay));
    memcpy(call + 1, &stub_rel, 4);
    atomic_store(&armed_at_ms, now_ms());
    atomic_store(&hidden, 1);
    return 1;
}

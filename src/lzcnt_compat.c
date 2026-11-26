#include "lzcnt_compat.h"

#include <cpuid.h>
#include <string.h>

enum { STUB_STRIDE = 96, JMP_REL32 = 5, REL32_REACH = 0x7ff00000 };

static const LzcntShape kShapes[] = {
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xcb}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xc8}},
    {10, 0, 0, {0xf3, 0x4b, 0x0f, 0xbd, 0x94, 0x05, 0xa8, 0x03, 0x00, 0x00}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xdf}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xdf}},
    {10, 0, 0, {0xf3, 0x4b, 0x0f, 0xbd, 0x84, 0x05, 0xc8, 0x03, 0x00, 0x00}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xdf}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xdf}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xdf}},
    {10, 0, 0, {0xf3, 0x4b, 0x0f, 0xbd, 0x84, 0x05, 0xd8, 0x03, 0x00, 0x00}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xcf}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xcf}},
    {10, 0, 0, {0xf3, 0x4b, 0x0f, 0xbd, 0x84, 0x05, 0xe8, 0x03, 0x00, 0x00}},
    {10, 0, 0, {0xf3, 0x4a, 0x0f, 0xbd, 0x84, 0x06, 0xf8, 0x03, 0x00, 0x00}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xc6}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xda}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xcb}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xc1}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xd7}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xd7}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xd7}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xc1}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xdf}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xdf}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xdf}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xc1}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xce}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xce}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xc1}},
    {10, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0x84, 0x32, 0xf8, 0x03, 0x00, 0x00}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xc7}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xfe}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xc1, 0xb9, 0x1f, 0x00, 0x00, 0x00}},
    {9, 0, 0, {0xf3, 0x49, 0x0f, 0xbd, 0x85, 0x68, 0x0c, 0x00, 0x00}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xe8}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xca}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xee}},
    {5, 0, 0, {0x66, 0xf3, 0x0f, 0xbd, 0xee}},
    {5, 0, 0, {0xf3, 0x44, 0x0f, 0xbd, 0xd9}},
    {5, 0, 0, {0xf3, 0x41, 0x0f, 0xbd, 0xc9}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xd6, 0x83, 0xf2, 0x3f}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xc8, 0xba, 0x1f, 0x00, 0x00, 0x00}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xc8, 0x83, 0xf1, 0x3f}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xc6, 0xb9, 0x1f, 0x00, 0x00, 0x00}},
    {5, 0, 0, {0xf3, 0x44, 0x0f, 0xbd, 0xd0}},
    {10, 0, 0, {0xf3, 0x0f, 0xbd, 0xce, 0x41, 0xba, 0x1f, 0x00, 0x00, 0x00}},
    {5, 0, 0, {0xf3, 0x45, 0x0f, 0xbd, 0xc0}},
    {10, 0, 0, {0xf3, 0x0f, 0xbd, 0xca, 0x41, 0xb8, 0x1f, 0x00, 0x00, 0x00}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xf9, 0x83, 0xf7, 0x3f}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xca, 0xbf, 0x1f, 0x00, 0x00, 0x00}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xd1, 0x83, 0xf2, 0x3f}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xc8, 0xba, 0x1f, 0x00, 0x00, 0x00}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xd1, 0x83, 0xf2, 0x3f}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xc8, 0xba, 0x1f, 0x00, 0x00, 0x00}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xf1, 0x83, 0xf6, 0x3f}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xca, 0xbe, 0x1f, 0x00, 0x00, 0x00}},
    {5, 0, 0, {0xf3, 0x44, 0x0f, 0xbd, 0xc9}},
    {10, 0, 0, {0xf3, 0x0f, 0xbd, 0xc8, 0x41, 0xb9, 0x1f, 0x00, 0x00, 0x00}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xf1, 0x83, 0xf6, 0x3f}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xca, 0xbe, 0x1f, 0x00, 0x00, 0x00}},
    {5, 0, 0, {0xf3, 0x49, 0x0f, 0xbd, 0xd7}},
    {5, 0, 0, {0xf3, 0x4c, 0x0f, 0xbd, 0xe2}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xce}},
    {5, 0, 0, {0xf3, 0x41, 0x0f, 0xbd, 0xc0}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xc7, 0x8d, 0x48, 0xf8}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xce, 0x83, 0xc1, 0xfb}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xc9, 0x0f, 0xb6, 0xc2}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xd0}},
    {5, 0, 0, {0xf3, 0x49, 0x0f, 0xbd, 0xc3}},
    {5, 0, 0, {0xf3, 0x49, 0x0f, 0xbd, 0xc7}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xfe, 0x8d, 0x4f, 0xf8}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xfb, 0x8d, 0x4f, 0xf8}},
    {6, 0, 0, {0xf3, 0x0f, 0xbd, 0xd6, 0x89, 0xf6}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xf2, 0xbf, 0x1f, 0x00, 0x00, 0x00}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xc6, 0xba, 0x1f, 0x00, 0x00, 0x00}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xc7, 0xba, 0x1f, 0x00, 0x00, 0x00}},
    {9, 0, 0, {0xf3, 0x0f, 0xbd, 0xcf, 0xb8, 0x1f, 0x00, 0x00, 0x00}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xdf}},
    {5, 0, 0, {0xf3, 0x49, 0x0f, 0xbd, 0xf4}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xc7, 0x8d, 0x48, 0xf8}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xc6, 0x8d, 0x48, 0xf8}},
    {9, 0, 4, {0xf3, 0x0f, 0xbd, 0xe9, 0xf3, 0x41, 0x0f, 0xbd, 0xc1}},
    {5, 0, 0, {0xf3, 0x44, 0x0f, 0xbd, 0xf1}},
    {5, 0, 0, {0xf3, 0x41, 0x0f, 0xbd, 0xc9}},
    {5, 0, 0, {0xf3, 0x44, 0x0f, 0xbd, 0xf6}},
    {5, 0, 0, {0xf3, 0x41, 0x0f, 0xbd, 0xc9}},
    {5, 0, 0, {0xf3, 0x49, 0x0f, 0xbd, 0xea}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xc6}},
    {5, 0, 0, {0xf3, 0x4d, 0x0f, 0xbd, 0xfa}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xc6}},
    {5, 0, 0, {0xf3, 0x4d, 0x0f, 0xbd, 0xf9}},
    {5, 0, 0, {0xf3, 0x48, 0x0f, 0xbd, 0xc6}},
    {5, 0, 0, {0xf3, 0x44, 0x0f, 0xbd, 0xce}},
    {7, 0, 0, {0xf3, 0x0f, 0xbd, 0xd7, 0x45, 0x89, 0xc8}},
};

static const uint32_t kOffsets[] = {
    0x520800, 0x520923, 0x520928, 0x520a00, 0x520ad0, 0x520b96, 0x520c60, 0x520d30,
    0x520de0, 0x520ead, 0x520f70, 0x521070, 0x521162, 0x5211b5, 0x521250, 0x521320,
    0x521650, 0x521779, 0x52177e, 0x521840, 0x521900, 0x5219d6, 0x521a90, 0x521b50,
    0x521c00, 0x521ce1, 0x521da0, 0x521eb0, 0x521fad, 0x521ff5, 0x522090, 0x522160,
    0x522373, 0x5226ff, 0x5255a0, 0x525660, 0x527f20, 0x528000, 0x1cdd4b8, 0x1cdd4ce,
    0x1cdd870, 0x1cdd882, 0x1cdd98c, 0x1cdd99e, 0x1cdd9e2, 0x1cdd9f7, 0x1cddba0, 0x1cddbb5,
    0x1cddda6, 0x1cdddb8, 0x1cddfa8, 0x1cddfba, 0x1cde17b, 0x1cde18d, 0x1cde25d, 0x1cde26f,
    0x1cde32e, 0x1cde343, 0x1cde5a6, 0x1cde604, 0x42b318d, 0x42b31bc, 0x42b3258, 0x42b33bf,
    0x42b33e0, 0x42b3476, 0x42b3663, 0x42b36ef, 0x42b410a, 0x42b4128, 0x42b47ea, 0x42b4805,
    0x42b4dd8, 0x42b5659, 0x42b56b9, 0x42b5b48, 0x42b5b88, 0x42b6570, 0x42b658e, 0x42b6e0d,
    0x42b6e28, 0x42b7ee2, 0x42b7f5f, 0x42b7f64, 0x42b7f96, 0x42b7f9f, 0x42b8217, 0x42b821c,
    0x42b82ae, 0x42b82b3, 0x42b82f4, 0x42b82fd, 0x42b858a, 0x42b858f,
};

const LzcntBuild lzcnt_builds[LZCNT_BUILD_COUNT] = {
    {"Shadow of the Beast 1.00", {0x44cc3fc, 0x19105d0}, kShapes, kOffsets, sizeof kOffsets / sizeof *kOffsets},
};

_Static_assert(sizeof kOffsets / sizeof *kOffsets == sizeof kShapes / sizeof *kShapes, "site tables");
_Static_assert(sizeof kOffsets / sizeof *kOffsets <= LZCNT_SITE_MAX, "stub area");

typedef struct {
    uint8_t bytes[STUB_STRIDE];
    size_t length;
    int overflow;
} Code;

typedef struct {
    unsigned bits, dst;
    size_t length, rep;
} Form;

static void put(Code *c, const void *data, size_t length) {
    if (c->length + length > sizeof c->bytes) {
        c->overflow = 1;
        return;
    }
    memcpy(c->bytes + c->length, data, length);
    c->length += length;
}

static void put_byte(Code *c, unsigned value) {
    const uint8_t byte = (uint8_t)value;
    put(c, &byte, 1);
}

static void put_prefixes(Code *c, unsigned bits, unsigned reg, unsigned rm) {
    if (bits == 16) put_byte(c, 0x66);
    const unsigned rex = (bits == 64 ? 8u : 0u) | (reg >= 8 ? 4u : 0u) | (rm >= 8 ? 1u : 0u);
    if (rex) put_byte(c, 0x40 | rex);
}

/* [66] F3 [REX] 0F BD /r with a register or a base/index memory operand; RIP-relative is rejected. */
static int parse_lzcnt(const uint8_t *p, size_t available, Form *form) {
    size_t i = 0, rep = (size_t)-1;
    int operand16 = 0;
    for (; i < available && (p[i] == 0x66 || p[i] == 0xf3); ++i) {
        if (p[i] == 0x66) operand16 = 1;
        else if (rep == (size_t)-1) rep = i;
        else return 0;
    }
    unsigned rex = 0;
    if (i < available && (p[i] & 0xf0) == 0x40) rex = p[i++];
    if (rep == (size_t)-1 || i + 3 > available || p[i] != 0x0f || p[i + 1] != 0xbd) return 0;
    const uint8_t modrm = p[i + 2];
    i += 3;
    const unsigned mod = modrm >> 6, rm = modrm & 7;
    if (mod != 3 && rm == 4) {
        if (i >= available) return 0;
        const uint8_t sib = p[i++];
        if (mod == 0 && (sib & 7) == 5) i += 4;
    } else if (mod == 0 && rm == 5) {
        return 0;
    }
    i += mod == 1 ? 1 : mod == 2 ? 4 : 0;
    if (i > available) return 0;
    form->bits = rex & 8 ? 64 : operand16 ? 16 : 32;
    form->dst = (modrm >> 3 & 7) | (rex & 4 ? 8 : 0);
    form->length = i;
    form->rep = rep;
    return 1;
}

/* BSR with the same operands, then width - 1 - index, or width with CF set for zero. */
static void emit_lzcnt(Code *c, const uint8_t *insn, const Form *form) {
    for (size_t i = 0; i < form->length; ++i)
        if (i != form->rep) put_byte(c, insn[i]);

    const unsigned bits = form->bits, dst = form->dst;
    Code nonzero = {{0}, 0, 0}, zero = {{0}, 0, 0};
    put_prefixes(&nonzero, bits, 0, dst);
    put_byte(&nonzero, 0x83);
    put_byte(&nonzero, 0xf0 | (dst & 7));
    put_byte(&nonzero, bits - 1);

    const uint32_t width = bits;
    put_prefixes(&zero, bits, 0, dst);
    if (bits == 64) {
        put_byte(&zero, 0xc7);
        put_byte(&zero, 0xc0 | (dst & 7));
    } else {
        put_byte(&zero, 0xb8 | (dst & 7));
    }
    put(&zero, &width, bits == 16 ? 2 : 4);
    put_prefixes(&zero, bits, dst, dst);
    put_byte(&zero, 0x85);
    put_byte(&zero, 0xc0 | (dst & 7) << 3 | (dst & 7));
    put_byte(&zero, 0xf9);

    put_byte(c, 0x74);
    put_byte(c, (unsigned)nonzero.length + 2);
    put(c, nonzero.bytes, nonzero.length);
    put_byte(c, 0xeb);
    put_byte(c, (unsigned)zero.length);
    put(c, zero.bytes, zero.length);
    c->overflow |= nonzero.overflow | zero.overflow;
}

static int build_stub(const LzcntShape *shape, const uint8_t *stub, const uint8_t *resume, Code *c) {
    size_t at = 0;
    const size_t starts[2] = {shape->first, shape->second};
    for (size_t k = 0; k < (shape->second ? 2u : 1u); ++k) {
        Form form;
        if (starts[k] < at || starts[k] >= shape->length ||
            !parse_lzcnt(shape->bytes + starts[k], shape->length - starts[k], &form))
            return 0;
        put(c, shape->bytes + at, starts[k] - at);
        emit_lzcnt(c, shape->bytes + starts[k], &form);
        at = starts[k] + form.length;
    }
    put(c, shape->bytes + at, shape->length - at);
    const int64_t distance = (int64_t)(intptr_t)resume - (int64_t)(intptr_t)(stub + c->length + JMP_REL32);
    const int32_t rel = (int32_t)distance;
    put_byte(c, 0xe9);
    put(c, &rel, 4);
    return !c->overflow && distance == rel;
}

const char *lzcnt_compat_status_text(LzcntCompatStatus status) {
    switch (status) {
    case LZCNT_COMPAT_OK: return "ok";
    case LZCNT_COMPAT_UNKNOWN_IMAGE: return "not a supported game image";
    case LZCNT_COMPAT_BAD_SITE: return "unexpected bytes at a site";
    case LZCNT_COMPAT_NO_ROOM: return "stub area too small or too far";
    }
    return "";
}

int lzcnt_compat_needed(IsaHostFeatures host) {
    return !(host.ext1_ecx & bit_LZCNT);
}

size_t lzcnt_compat_stub_bytes(void) {
    return LZCNT_SITE_MAX * STUB_STRIDE;
}

static int site_holds(const uint8_t *image, uint64_t size, uint32_t offset, const LzcntShape *shape) {
    return shape->length >= JMP_REL32 && shape->length <= LZCNT_SHAPE_MAX && (uint64_t)offset + shape->length <= size &&
           !memcmp(image + offset, shape->bytes, shape->length);
}

LzcntCompatStatus lzcnt_compat_check(const uint8_t *image, uint64_t size, const LzcntBuild *build, size_t *matched) {
    size_t held = 0;
    int overlap = 0;
    for (size_t i = 0; image && build && i < build->count; ++i) {
        const uint32_t offset = build->offsets[i];
        const LzcntShape *shape = &build->shapes[i];
        Code scratch = {{0}, 0, 0};
        held += site_holds(image, size, offset, shape) && build_stub(shape, image + offset, image + offset + shape->length, &scratch);
        for (size_t j = 0; j < i; ++j)
            overlap |= offset < build->offsets[j] + build->shapes[j].length && build->offsets[j] < offset + shape->length;
    }
    if (matched) *matched = held;
    return build && image && held == build->count && !overlap ? LZCNT_COMPAT_OK : LZCNT_COMPAT_BAD_SITE;
}

LzcntCompatStatus lzcnt_compat_apply(uint8_t *image, uint64_t size, const uint32_t *offsets, const LzcntShape *shapes,
                                     size_t count, uint8_t *stubs, size_t stubs_size) {
    if (count * STUB_STRIDE > stubs_size) return LZCNT_COMPAT_NO_ROOM;
    for (size_t i = 0; i < count; ++i) {
        const LzcntShape *shape = &shapes[i];
        const uint8_t *site = image + offsets[i], *stub = stubs + i * STUB_STRIDE;
        if (!site_holds(image, size, offsets[i], shape)) return LZCNT_COMPAT_BAD_SITE;
        for (size_t j = 0; j < i; ++j)
            if (offsets[i] < offsets[j] + shapes[j].length && offsets[j] < offsets[i] + shape->length)
                return LZCNT_COMPAT_BAD_SITE;
        const int64_t distance = (int64_t)(intptr_t)stub - (int64_t)(intptr_t)site;
        if (distance > REL32_REACH || distance < -REL32_REACH) return LZCNT_COMPAT_NO_ROOM;
        Code scratch = {{0}, 0, 0};
        if (!build_stub(shape, stub, site + shape->length, &scratch)) return LZCNT_COMPAT_BAD_SITE;
    }
    for (size_t i = 0; i < count; ++i) {
        uint8_t *site = image + offsets[i], *stub = stubs + i * STUB_STRIDE;
        Code code = {{0}, 0, 0};
        build_stub(&shapes[i], stub, site + shapes[i].length, &code);
        memset(stub, 0xcc, STUB_STRIDE);
        memcpy(stub, code.bytes, code.length);
        const int32_t rel = (int32_t)((int64_t)(intptr_t)stub - (int64_t)(intptr_t)(site + JMP_REL32));
        site[0] = 0xe9;
        memcpy(site + 1, &rel, 4);
        memset(site + JMP_REL32, 0x90, shapes[i].length - JMP_REL32);
    }
    return LZCNT_COMPAT_OK;
}

LzcntCompatStatus lzcnt_compat_install(uint8_t *image, uint64_t size, uint64_t entry, uint8_t *stubs, size_t stubs_size,
                                       const LzcntBuild **build) {
    for (size_t i = 0; i < LZCNT_BUILD_COUNT; ++i) {
        const LzcntBuild *known = &lzcnt_builds[i];
        if (known->image.size != size || known->image.entry != entry) continue;
        if (build) *build = known;
        return lzcnt_compat_apply(image, size, known->offsets, known->shapes, known->count, stubs, stubs_size);
    }
    return LZCNT_COMPAT_UNKNOWN_IMAGE;
}

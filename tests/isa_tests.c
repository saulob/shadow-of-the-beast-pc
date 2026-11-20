#include "isa_emulate.h"
#include "lzcnt_compat.h"
#include <cpuid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static int failures, checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (0)

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

static size_t run(const uint8_t *code, size_t size, uint64_t rip, uint64_t gpr[16]) {
    uint8_t padded[32] = {0};
    memcpy(padded, code, size);
    return isa_emulate_movbe(padded, rip, gpr);
}

static void crash_site(void) {
    const uint8_t code[] = {0x48, 0x0f, 0x38, 0xf1, 0x11};
    uint8_t memory[8] = {0};
    uint64_t gpr[16] = {0};
    gpr[RCX] = (uintptr_t)memory;
    gpr[RDX] = 0x65006e006f004e00ull;
    CHECK(run(code, sizeof code, 0, gpr) == 5);
    const uint8_t expected[8] = {0x65, 0x00, 0x6e, 0x00, 0x6f, 0x00, 0x4e, 0x00};
    CHECK(memcmp(memory, expected, 8) == 0);
    CHECK(gpr[RCX] == (uintptr_t)memory && gpr[RDX] == 0x65006e006f004e00ull);
}

static void load_widths(void) {
    uint8_t memory[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint64_t gpr[16];
    const uint8_t load64[] = {0x48, 0x0f, 0x38, 0xf0, 0x00};
    memset(gpr, 0xaa, sizeof gpr);
    gpr[RAX] = (uintptr_t)memory;
    CHECK(run(load64, sizeof load64, 0, gpr) == 5);
    CHECK(gpr[RAX] == 0x1122334455667788ull);

    const uint8_t load32[] = {0x0f, 0x38, 0xf0, 0x08};
    gpr[RAX] = (uintptr_t)memory;
    gpr[RCX] = ~0ull;
    CHECK(run(load32, sizeof load32, 0, gpr) == 4);
    CHECK(gpr[RCX] == 0x11223344u);

    const uint8_t load16[] = {0x66, 0x0f, 0x38, 0xf0, 0x08};
    gpr[RAX] = (uintptr_t)memory;
    gpr[RCX] = 0xffffffffffffffffull;
    CHECK(run(load16, sizeof load16, 0, gpr) == 5);
    CHECK(gpr[RCX] == 0xffffffffffff1122ull);
}

static void store_widths(void) {
    uint8_t memory[8];
    uint64_t gpr[16] = {0};
    const uint8_t store16[] = {0x66, 0x0f, 0x38, 0xf1, 0x08};
    memset(memory, 0xcc, sizeof memory);
    gpr[RAX] = (uintptr_t)memory;
    gpr[RCX] = 0x1234;
    CHECK(run(store16, sizeof store16, 0, gpr) == 5);
    CHECK(memory[0] == 0x12 && memory[1] == 0x34 && memory[2] == 0xcc);

    const uint8_t store32[] = {0x0f, 0x38, 0xf1, 0x08};
    memset(memory, 0xcc, sizeof memory);
    gpr[RCX] = 0xdeadbeef12345678ull;
    CHECK(run(store32, sizeof store32, 0, gpr) == 4);
    CHECK(memory[0] == 0x12 && memory[1] == 0x34 && memory[2] == 0x56 && memory[3] == 0x78 && memory[4] == 0xcc);
}

static void addressing_forms(void) {
    uint8_t memory[256] = {0};
    uint64_t gpr[16] = {0};
    const uint8_t sib_disp8[] = {0x48, 0x0f, 0x38, 0xf1, 0x74, 0x3b, 0x48};
    gpr[RBX] = (uintptr_t)memory;
    gpr[RDI] = 8;
    gpr[RSI] = 0x0102030405060708ull;
    CHECK(run(sib_disp8, sizeof sib_disp8, 0, gpr) == 7);
    CHECK(memory[0x48 + 8] == 0x01 && memory[0x48 + 15] == 0x08);

    const uint8_t extended[] = {0x66, 0x44, 0x0f, 0x38, 0xf1, 0x59, 0x32};
    gpr[RCX] = (uintptr_t)memory;
    gpr[R11] = 0xabcd;
    CHECK(run(extended, sizeof extended, 0, gpr) == 7);
    CHECK(memory[0x32] == 0xab && memory[0x33] == 0xcd);

    const uint8_t r12_base[] = {0x49, 0x0f, 0x38, 0xf0, 0x04, 0x24};
    gpr[R12] = (uintptr_t)memory + 0x50;
    CHECK(run(r12_base, sizeof r12_base, 0, gpr) == 6);
    CHECK(gpr[RAX] == 0x0102030405060708ull);

    const uint8_t scaled_index_r9[] = {0x4a, 0x0f, 0x38, 0xf1, 0x14, 0xc8};
    memset(memory, 0, sizeof memory);
    gpr[RAX] = (uintptr_t)memory;
    gpr[R9] = 2;
    gpr[RDX] = 0x1122334455667788ull;
    CHECK(run(scaled_index_r9, sizeof scaled_index_r9, 0, gpr) == 6);
    CHECK(memory[16] == 0x11 && memory[23] == 0x88);

    const uint8_t no_index[] = {0x48, 0x0f, 0x38, 0xf1, 0x14, 0x24};
    gpr[RSP] = (uintptr_t)memory;
    CHECK(run(no_index, sizeof no_index, 0, gpr) == 6);
    CHECK(memory[0] == 0x11);
}

static void rip_relative(void) {
    uint8_t memory[16] = {0};
    uint64_t gpr[16] = {0};
    const uint8_t code[] = {0x48, 0x0f, 0x38, 0xf1, 0x05, 0x10, 0x00, 0x00, 0x00};
    const uint64_t rip = (uintptr_t)memory;
    gpr[RAX] = 0x0102030405060708ull;
    CHECK(run(code, sizeof code, rip - 0x10 - sizeof code, gpr) == sizeof code);
    CHECK(memory[0] == 0x01 && memory[7] == 0x08);
}

static void rejected(void) {
    uint64_t gpr[16] = {0};
    uint64_t before[16];
    uint8_t memory[8] = {0};
    gpr[RAX] = (uintptr_t)memory;
    memcpy(before, gpr, sizeof gpr);

    const uint8_t register_form[] = {0x48, 0x0f, 0x38, 0xf1, 0xc1};
    const uint8_t crc32[] = {0xf2, 0x48, 0x0f, 0x38, 0xf1, 0x00};
    const uint8_t other_opcode[] = {0x48, 0x0f, 0x38, 0xf2, 0x00};
    const uint8_t not_0f38[] = {0x48, 0x0f, 0x3a, 0xf1, 0x00};
    const uint8_t ud2[] = {0x0f, 0x0b};
    const uint8_t segment[] = {0x65, 0x48, 0x0f, 0x38, 0xf1, 0x00};
    CHECK(run(register_form, sizeof register_form, 0, gpr) == 0);
    CHECK(run(crc32, sizeof crc32, 0, gpr) == 0);
    CHECK(run(other_opcode, sizeof other_opcode, 0, gpr) == 0);
    CHECK(run(not_0f38, sizeof not_0f38, 0, gpr) == 0);
    CHECK(run(ud2, sizeof ud2, 0, gpr) == 0);
    CHECK(run(segment, sizeof segment, 0, gpr) == 0);
    CHECK(memcmp(gpr, before, sizeof gpr) == 0);
    CHECK(memory[0] == 0 && memory[7] == 0);
}

enum { CF = 1 << 0, ZF = 1 << 6, SF = 1 << 7, OF = 1 << 11, OTHER_FLAGS = 0x202 };

static size_t run_flags(const uint8_t *code, size_t size, uint64_t gpr[16], uint64_t *flags) {
    uint8_t padded[32] = {0};
    memcpy(padded, code, size);
    return isa_emulate(padded, 0, gpr, flags);
}

static void strlen_site(void) {
    const uint8_t andn_rax_rax_rdx[] = {0xc4, 0xe2, 0xf8, 0xf2, 0xc2};
    uint64_t gpr[16] = {0}, flags = OTHER_FLAGS | CF | OF | ZF | SF;
    gpr[RAX] = 0x486362694c656353ull;
    gpr[RDX] = 0x476261684b646252ull;
    CHECK(run_flags(andn_rax_rax_rdx, sizeof andn_rax_rax_rdx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0x0700010003000000ull);
    CHECK(gpr[RDX] == 0x476261684b646252ull);
    CHECK(flags == OTHER_FLAGS);
}

static void andn_forms(void) {
    uint64_t gpr[16] = {0}, flags = OTHER_FLAGS;
    const uint8_t andn_esi_ecx_eax[] = {0xc4, 0xe2, 0x70, 0xf2, 0xf0};
    gpr[RSI] = ~0ull;
    gpr[RCX] = 0xffffffff0000ffffull;
    gpr[RAX] = 0x123456787fff0000ull;
    CHECK(run_flags(andn_esi_ecx_eax, sizeof andn_esi_ecx_eax, gpr, &flags) == 5);
    CHECK(gpr[RSI] == 0x7fff0000ull);
    CHECK(flags == OTHER_FLAGS);

    uint8_t memory[0x80] = {0};
    const uint32_t value = 0x80000001u;
    memcpy(memory + 0x70, &value, 4);
    const uint8_t andn_eax_eax_r15[] = {0xc4, 0xc2, 0x78, 0xf2, 0x47, 0x70};
    gpr[R15] = (uintptr_t)memory;
    gpr[RAX] = 1;
    CHECK(run_flags(andn_eax_eax_r15, sizeof andn_eax_eax_r15, gpr, &flags) == 6);
    CHECK(gpr[RAX] == 0x80000000ull);
    CHECK(flags == (OTHER_FLAGS | SF));

    const uint8_t andn_r11d_r11d_rbx_disp32[] = {0xc4, 0x62, 0x20, 0xf2, 0x9b, 0x08, 0x69, 0x01, 0x00};
    gpr[RBX] = (uintptr_t)memory + 0x70 - 0x16908;
    gpr[R11] = 0x80000001ull;
    CHECK(run_flags(andn_r11d_r11d_rbx_disp32, sizeof andn_r11d_r11d_rbx_disp32, gpr, &flags) == 9);
    CHECK(gpr[R11] == 0);
    CHECK(flags == (OTHER_FLAGS | ZF));
}

static void bextr_forms(void) {
    uint64_t gpr[16] = {0}, flags = OTHER_FLAGS | CF | OF;
    uint32_t table[4] = {0, 0, 0xabcd1234u, 0};
    const uint8_t bextr_eax_sib_edx[] = {0xc4, 0xe2, 0x68, 0xf7, 0x04, 0x81};
    gpr[RCX] = (uintptr_t)table;
    gpr[RAX] = 2;
    gpr[RDX] = 0x0804;
    CHECK(run_flags(bextr_eax_sib_edx, sizeof bextr_eax_sib_edx, gpr, &flags) == 6);
    CHECK(gpr[RAX] == 0x23);
    CHECK(flags == OTHER_FLAGS);

    const uint8_t bextr_r15d_rbx_r15d[] = {0xc4, 0x62, 0x00, 0xf7, 0x3b};
    gpr[RBX] = (uintptr_t)&table[2];
    gpr[R15] = 0x101c;
    CHECK(run_flags(bextr_r15d_rbx_r15d, sizeof bextr_r15d_rbx_r15d, gpr, &flags) == 5);
    CHECK(gpr[R15] == 0xa);

    const uint8_t bextr_ecx_eax_r14d[] = {0xc4, 0xe2, 0x08, 0xf7, 0xc8};
    gpr[RAX] = 0xffffffff80000000ull;
    gpr[R14] = 0x401f;
    CHECK(run_flags(bextr_ecx_eax_r14d, sizeof bextr_ecx_eax_r14d, gpr, &flags) == 5);
    CHECK(gpr[RCX] == 1);
    gpr[R14] = 0x0820;
    CHECK(run_flags(bextr_ecx_eax_r14d, sizeof bextr_ecx_eax_r14d, gpr, &flags) == 5);
    CHECK(gpr[RCX] == 0 && flags == (OTHER_FLAGS | ZF));
    gpr[R14] = 0x0004;
    CHECK(run_flags(bextr_ecx_eax_r14d, sizeof bextr_ecx_eax_r14d, gpr, &flags) == 5);
    CHECK(gpr[RCX] == 0);

    const uint8_t bextr_rax_rcx_rdx[] = {0xc4, 0xe2, 0xe8, 0xf7, 0xc1};
    gpr[RCX] = 0xf000000000000000ull;
    gpr[RDX] = 0xff3c;
    CHECK(run_flags(bextr_rax_rcx_rdx, sizeof bextr_rax_rcx_rdx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0xf);
}

static void bls_forms(void) {
    uint64_t gpr[16] = {0}, flags = OTHER_FLAGS;
    const uint8_t blsi_eax_ecx[] = {0xc4, 0xe2, 0x78, 0xf3, 0xd9};
    gpr[RAX] = ~0ull;
    gpr[RCX] = 0x1234567800000c00ull;
    CHECK(run_flags(blsi_eax_ecx, sizeof blsi_eax_ecx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0x400 && flags == (OTHER_FLAGS | CF));
    gpr[RCX] = 0;
    CHECK(run_flags(blsi_eax_ecx, sizeof blsi_eax_ecx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0 && flags == (OTHER_FLAGS | ZF));

    const uint8_t blsr_rax_rcx[] = {0xc4, 0xe2, 0xf8, 0xf3, 0xc9};
    gpr[RCX] = 0x8000000000000000ull;
    CHECK(run_flags(blsr_rax_rcx, sizeof blsr_rax_rcx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0 && flags == (OTHER_FLAGS | ZF));
    gpr[RCX] = 0;
    CHECK(run_flags(blsr_rax_rcx, sizeof blsr_rax_rcx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0 && flags == (OTHER_FLAGS | CF | ZF));

    const uint8_t blsmsk_eax_ecx[] = {0xc4, 0xe2, 0x78, 0xf3, 0xd1};
    gpr[RCX] = 0x18;
    CHECK(run_flags(blsmsk_eax_ecx, sizeof blsmsk_eax_ecx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0xf && flags == OTHER_FLAGS);
    gpr[RCX] = 0;
    CHECK(run_flags(blsmsk_eax_ecx, sizeof blsmsk_eax_ecx, gpr, &flags) == 5);
    CHECK(gpr[RAX] == 0xffffffffull && flags == (OTHER_FLAGS | CF | SF));
}

static void bmi1_rejected(void) {
    uint64_t gpr[16], before[16], flags = OTHER_FLAGS;
    for (int i = 0; i < 16; ++i) gpr[i] = 0x1111111111111111ull * (uint64_t)i;
    memcpy(before, gpr, sizeof gpr);
    const uint8_t shlx[] = {0xc4, 0xe2, 0xf9, 0xf7, 0xc1};
    const uint8_t andn_l1[] = {0xc4, 0xe2, 0xfc, 0xf2, 0xc2};
    const uint8_t bls_group0[] = {0xc4, 0xe2, 0x78, 0xf3, 0xc1};
    const uint8_t bls_group4[] = {0xc4, 0xe2, 0x78, 0xf3, 0xe1};
    const uint8_t map_0f[] = {0xc4, 0xe1, 0xf8, 0xf2, 0xc2};
    const uint8_t bzhi[] = {0xc4, 0xe2, 0xf8, 0xf5, 0xc1};
    const uint8_t vex2[] = {0xc5, 0xf8, 0xf2, 0xc2};
    CHECK(run_flags(shlx, sizeof shlx, gpr, &flags) == 0);
    CHECK(run_flags(andn_l1, sizeof andn_l1, gpr, &flags) == 0);
    CHECK(run_flags(bls_group0, sizeof bls_group0, gpr, &flags) == 0);
    CHECK(run_flags(bls_group4, sizeof bls_group4, gpr, &flags) == 0);
    CHECK(run_flags(map_0f, sizeof map_0f, gpr, &flags) == 0);
    CHECK(run_flags(bzhi, sizeof bzhi, gpr, &flags) == 0);
    CHECK(run_flags(vex2, sizeof vex2, gpr, &flags) == 0);
    CHECK(memcmp(gpr, before, sizeof gpr) == 0 && flags == OTHER_FLAGS);
}

static void host_sets(void) {
    char text[64];
    const IsaHostFeatures haswell = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX | bit_MOVBE, bit_BMI, bit_LZCNT};
    CHECK(isa_missing_sets(haswell, 0, text, sizeof text) == 0 && text[0] == 0);
    CHECK(isa_missing_sets(haswell, 1, text, sizeof text) == 0);
    CHECK(!lzcnt_compat_needed(haswell));

    const IsaHostFeatures ivy_bridge = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX, 0, 0};
    CHECK(isa_missing_sets(ivy_bridge, 0, text, sizeof text) == 0 && text[0] == 0);
    CHECK(isa_missing_sets(ivy_bridge, 1, text, sizeof text) == 3 && strcmp(text, "LZCNT, MOVBE, BMI1") == 0);
    CHECK(lzcnt_compat_needed(ivy_bridge));

    const IsaHostFeatures bulldozer = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX, 0, bit_LZCNT};
    CHECK(isa_missing_sets(bulldozer, 0, text, sizeof text) == 0);
    CHECK(isa_missing_sets(bulldozer, 1, text, sizeof text) == 2 && strcmp(text, "MOVBE, BMI1") == 0);
    CHECK(!lzcnt_compat_needed(bulldozer));

    const IsaHostFeatures nehalem = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT, 0, bit_LZCNT};
    CHECK(isa_missing_sets(nehalem, 0, text, sizeof text) == 1 && strcmp(text, "AVX") == 0);
    const IsaHostFeatures no_popcnt = {bit_SSE4_1 | bit_SSE4_2 | bit_AVX, 0, 0};
    CHECK(isa_missing_sets(no_popcnt, 0, text, sizeof text) == 1 && strcmp(text, "POPCNT") == 0);
    const IsaHostFeatures penryn = {bit_SSE4_1, 0, 0};
    CHECK(isa_missing_sets(penryn, 0, text, sizeof text) == 3 && strcmp(text, "SSE4.2, POPCNT, AVX") == 0);

    const IsaHostFeatures no_movbe = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX, bit_BMI, bit_LZCNT};
    CHECK(isa_missing_sets(no_movbe, 0, text, sizeof text) == 0);
    CHECK(isa_missing_sets(no_movbe, 1, text, sizeof text) == 1 && strcmp(text, "MOVBE") == 0);
    CHECK(isa_emulation_needed(no_movbe) == ISA_EMULATE_MOVBE && !lzcnt_compat_needed(no_movbe));
    const IsaHostFeatures no_bmi1 = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX | bit_MOVBE, 0, bit_LZCNT};
    CHECK(isa_missing_sets(no_bmi1, 0, text, sizeof text) == 0);
    CHECK(isa_missing_sets(no_bmi1, 1, text, sizeof text) == 1 && strcmp(text, "BMI1") == 0);
    CHECK(isa_emulation_needed(no_bmi1) == ISA_EMULATE_BMI1 && !lzcnt_compat_needed(no_bmi1));
    const IsaHostFeatures no_lzcnt = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_AVX | bit_MOVBE, bit_BMI, 0};
    CHECK(isa_missing_sets(no_lzcnt, 0, text, sizeof text) == 0);
    CHECK(isa_missing_sets(no_lzcnt, 1, text, sizeof text) == 1 && strcmp(text, "LZCNT") == 0);
    CHECK(isa_emulation_needed(no_lzcnt) == 0 && lzcnt_compat_needed(no_lzcnt));
    CHECK(isa_emulation_needed(haswell) == 0 &&
          isa_emulation_needed(ivy_bridge) == (ISA_EMULATE_MOVBE | ISA_EMULATE_BMI1));
    const IsaHostFeatures no_avx = {bit_SSE4_1 | bit_SSE4_2 | bit_POPCNT | bit_MOVBE, bit_BMI, bit_LZCNT};
    CHECK(isa_missing_sets(no_avx, 0, text, sizeof text) == 1 && strcmp(text, "AVX") == 0);
    const IsaHostFeatures no_sse42 = {bit_SSE4_1 | bit_POPCNT | bit_AVX | bit_MOVBE, bit_BMI, bit_LZCNT};
    CHECK(isa_missing_sets(no_sse42, 0, text, sizeof text) == 1 && strcmp(text, "SSE4.2") == 0);

    char cause[160], advice[160];
    CHECK(isa_host_usable(haswell, cause, sizeof cause, advice, sizeof advice));
    CHECK(isa_host_usable(ivy_bridge, cause, sizeof cause, advice, sizeof advice));
    CHECK(isa_host_usable(bulldozer, cause, sizeof cause, advice, sizeof advice));
    CHECK(isa_host_usable(no_movbe, cause, sizeof cause, advice, sizeof advice));
    CHECK(isa_host_usable(no_bmi1, cause, sizeof cause, advice, sizeof advice));
    CHECK(isa_host_usable(no_lzcnt, cause, sizeof cause, advice, sizeof advice));
    CHECK(!isa_host_usable(no_avx, cause, sizeof cause, advice, sizeof advice) && strstr(cause, "AVX") && advice[0]);
    CHECK(!isa_host_usable(no_popcnt, cause, sizeof cause, advice, sizeof advice) && strstr(cause, "POPCNT"));
    CHECK(!isa_host_usable(no_sse42, cause, sizeof cause, advice, sizeof advice) && strstr(cause, "SSE4.2"));
    CHECK(!isa_host_usable(penryn, cause, sizeof cause, advice, sizeof advice) && strstr(cause, "SSE4.2, POPCNT, AVX"));

    const IsaHostFeatures none = {0, 0, 0};
    CHECK(isa_missing_sets(none, 0, text, sizeof text) == 4 && strcmp(text, "SSE4.1, SSE4.2, POPCNT, AVX") == 0);
    CHECK(isa_missing_sets(none, 0, text, 8) == 4 && strlen(text) < 8);
    CHECK(!isa_host_usable(none, cause, sizeof cause, advice, sizeof advice));
    CHECK(isa_missing_sets(none, 1, text, sizeof text) == 3 && strcmp(text, "LZCNT, MOVBE, BMI1") == 0);

    isa_host_name(text, sizeof text);
    CHECK(text[0] != 0 && text[0] != ' ');
}

enum { PROBE_PAGE = 4096, PROBE_RSP = 4, PROBE_FLAGS = 11 };

uint64_t sotb_probe_in[16], sotb_probe_out[16];
const void *sotb_probe_code;
void sotb_probe_run(void);
__asm__(".text\n.globl sotb_probe_run\nsotb_probe_run:\n"
        " push %rbx\n push %rbp\n push %rdi\n push %rsi\n push %r12\n push %r13\n push %r14\n push %r15\n"
        " lea sotb_probe_in(%rip), %rax\n"
        " mov 8(%rax), %rcx\n mov 16(%rax), %rdx\n mov 24(%rax), %rbx\n mov 40(%rax), %rbp\n"
        " mov 48(%rax), %rsi\n mov 56(%rax), %rdi\n mov 64(%rax), %r8\n mov 72(%rax), %r9\n mov 80(%rax), %r10\n"
        " mov 88(%rax), %r11\n mov 96(%rax), %r12\n mov 104(%rax), %r13\n mov 112(%rax), %r14\n mov 120(%rax), %r15\n"
        " mov (%rax), %rax\n pxor %xmm0, %xmm0\n call *sotb_probe_code(%rip)\n"
        " mov %rax, sotb_probe_out(%rip)\n mov %rcx, sotb_probe_out+8(%rip)\n mov %rdx, sotb_probe_out+16(%rip)\n"
        " mov %rbx, sotb_probe_out+24(%rip)\n mov %rbp, sotb_probe_out+40(%rip)\n mov %rsi, sotb_probe_out+48(%rip)\n"
        " mov %rdi, sotb_probe_out+56(%rip)\n mov %r8, sotb_probe_out+64(%rip)\n mov %r9, sotb_probe_out+72(%rip)\n"
        " mov %r10, sotb_probe_out+80(%rip)\n mov %r11, sotb_probe_out+88(%rip)\n mov %r12, sotb_probe_out+96(%rip)\n"
        " mov %r13, sotb_probe_out+104(%rip)\n mov %r14, sotb_probe_out+112(%rip)\n mov %r15, sotb_probe_out+120(%rip)\n"
        " pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rsi\n pop %rdi\n pop %rbp\n pop %rbx\n ret\n");


static uint8_t *executable_block(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *block = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return block == MAP_FAILED ? NULL : block;
#endif
}

static uint64_t leading_zeros(uint64_t value, unsigned bits) {
    if (bits < 64) value &= (1ull << bits) - 1;
    return value ? (uint64_t)__builtin_clzll(value) - (64 - bits) : bits;
}

static int host_has_lzcnt(void) {
    return (isa_host_features().ext1_ecx & bit_LZCNT) != 0;
}

static const uint8_t probe_epilogue[] = {0x9c, 0x41, 0x5b, 0xc3}; /* pushfq; pop r11; ret */

/* Page 0 holds the shape under test and the epilogue, rewritten; page 1 the stub; page 2 the same code untouched. */
static uint8_t *probe_block;
static uint8_t probe_arena[PROBE_PAGE] __attribute__((aligned(64)));

static int prepare_shape(const LzcntShape *shape) {
    if (!probe_block) probe_block = executable_block(3 * PROBE_PAGE);
    if (!probe_block) return 0;
    memset(probe_block, 0xcc, 3 * PROBE_PAGE);
    memcpy(probe_block, shape->bytes, shape->length);
    memcpy(probe_block + shape->length, probe_epilogue, sizeof probe_epilogue);
    memcpy(probe_block + 2 * PROBE_PAGE, probe_block, shape->length + sizeof probe_epilogue);
    const uint32_t offset = 0;
    return lzcnt_compat_apply(probe_block, PROBE_PAGE, &offset, shape, 1, probe_block + PROBE_PAGE, PROBE_PAGE) ==
           LZCNT_COMPAT_OK;
}

static int prepare_probe(const uint8_t *insn, size_t length) {
    LzcntShape shape = {(uint8_t)(length < 5 ? length + 1 : length), 0, 0, {0}};
    memcpy(shape.bytes, insn, length);
    if (length < 5) shape.bytes[length] = probe_epilogue[0];
    if (!probe_block) probe_block = executable_block(3 * PROBE_PAGE);
    if (!probe_block) return 0;
    memset(probe_block, 0xcc, 3 * PROBE_PAGE);
    memcpy(probe_block, insn, length);
    memcpy(probe_block + length, probe_epilogue, sizeof probe_epilogue);
    memcpy(probe_block + 2 * PROBE_PAGE, probe_block, length + sizeof probe_epilogue);
    const uint32_t offset = 0;
    return lzcnt_compat_apply(probe_block, PROBE_PAGE, &offset, &shape, 1, probe_block + PROBE_PAGE, PROBE_PAGE) ==
           LZCNT_COMPAT_OK;
}

static void probe(const uint8_t *code, const uint64_t in[16], uint64_t out[16]) {
    memcpy(sotb_probe_in, in, sizeof sotb_probe_in);
    sotb_probe_code = code;
    sotb_probe_run();
    memcpy(out, sotb_probe_out, sizeof sotb_probe_out);
}

static const uint64_t probe_values[] = {
    0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x12345, 0x7fffffffull, 0x80000000ull,
    0xffffffffull, 0x100000000ull, 0x0123456789abcdefull, 0x00ff00ff00ff00ffull, 0x7fffffffffffffffull,
    0x8000000000000000ull, ~0ull,
};
enum { PROBE_VALUE_COUNT = sizeof probe_values / sizeof *probe_values };

/* Runs one LZCNT form on each value, rewritten and, on a host that has LZCNT, as it is. `source` is the register
 * holding the value, or -1 for the memory operand at `value_at`. */
static void check_form(const uint8_t *insn, size_t length, unsigned bits, unsigned dst, int source,
                       const uint64_t regs[16], uint8_t *value_at, int all_bits) {
    if (!prepare_probe(insn, length)) {
        CHECK(0 && "probe block");
        return;
    }
    const uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    for (size_t n = 0; n < PROBE_VALUE_COUNT + (all_bits ? 64u : 0u); ++n) {
        const uint64_t value = n < PROBE_VALUE_COUNT ? probe_values[n] : 1ull << (n - PROBE_VALUE_COUNT);
        uint64_t in[16], out[16];
        memcpy(in, regs, sizeof in);
        if (source >= 0) in[source] = value;
        else memcpy(value_at, &value, bits / 8);
        const uint64_t result = leading_zeros(value, bits);
        const uint64_t expected = bits == 16 ? (in[dst] & ~0xffffull) | result : result;
        for (int pass = 0; pass < 2; ++pass) {
            if (pass && !host_has_lzcnt()) break;
            probe(probe_block + (pass ? 2 * PROBE_PAGE : 0), in, out);
            int same = out[dst] == expected;
            for (unsigned r = 0; r < 16; ++r)
                if (r != dst && r != PROBE_RSP && r != PROBE_FLAGS && out[r] != in[r]) same = 0;
            const uint64_t flags = out[PROBE_FLAGS];
            same = same && (flags & 1) == ((value & mask) == 0) && (flags >> 6 & 1) == (result == 0);
            if (!same)
                printf("FAIL %s bits=%u dst=%u source=%d value=%llx: got %llx flags %llx, expected %llx\n",
                       pass ? "hardware" : "rewritten", bits, dst, source, (unsigned long long)value,
                       (unsigned long long)out[dst], (unsigned long long)flags, (unsigned long long)expected);
            CHECK(same);
        }
    }
}

static void fill_registers(uint64_t regs[16]) {
    for (unsigned r = 0; r < 16; ++r) regs[r] = 0xc0de0000face0000ull + r * 0x100001ull;
}

static size_t encode_register_form(uint8_t *out, unsigned bits, unsigned dst, unsigned source) {
    size_t n = 0;
    if (bits == 16) out[n++] = 0x66;
    out[n++] = 0xf3;
    const unsigned rex = (bits == 64 ? 8u : 0u) | (dst >= 8 ? 4u : 0u) | (source >= 8 ? 1u : 0u);
    if (rex) out[n++] = (uint8_t)(0x40 | rex);
    out[n++] = 0x0f;
    out[n++] = 0xbd;
    out[n++] = (uint8_t)(0xc0 | (dst & 7) << 3 | (source & 7));
    return n;
}

/* mode 0: [base], 1: [base+0x10], 2: [base+0x3a8], 3: [base+rcx*4+8]; *displacement is what the address adds. */
static size_t encode_memory_form(uint8_t *out, unsigned bits, unsigned dst, unsigned base, int mode,
                                 uint64_t *displacement) {
    size_t n = 0;
    if (bits == 16) out[n++] = 0x66;
    out[n++] = 0xf3;
    const unsigned rex = (bits == 64 ? 8u : 0u) | (dst >= 8 ? 4u : 0u) | (base >= 8 ? 1u : 0u);
    if (rex) out[n++] = (uint8_t)(0x40 | rex);
    out[n++] = 0x0f;
    out[n++] = 0xbd;
    const int indexed = mode == 3, sib = indexed || (base & 7) == 4;
    const unsigned mod = mode == 0 ? 0 : mode == 2 ? 2 : 1;
    out[n++] = (uint8_t)(mod << 6 | (dst & 7) << 3 | (sib ? 4 : base & 7));
    if (sib) out[n++] = (uint8_t)(indexed ? 0x88 | (base & 7) : 0x24);
    *displacement = mode == 0 ? 0 : mode == 1 ? 0x10 : mode == 2 ? 0x3a8 : 8;
    if (mod == 1) out[n++] = (uint8_t)*displacement;
    if (mod == 2) {
        const uint32_t d = 0x3a8;
        memcpy(out + n, &d, 4);
        n += 4;
    }
    return n;
}

static const unsigned probe_registers[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15};
enum { PROBE_REGISTER_COUNT = sizeof probe_registers / sizeof *probe_registers };
static const unsigned probe_widths[] = {16, 32, 64};

static void rewritten_register_forms(void) {
    for (unsigned w = 0; w < 3; ++w)
        for (unsigned d = 0; d < PROBE_REGISTER_COUNT; ++d)
            for (unsigned s = 0; s < PROBE_REGISTER_COUNT; ++s) {
                uint8_t insn[16];
                uint64_t regs[16];
                fill_registers(regs);
                const size_t length =
                    encode_register_form(insn, probe_widths[w], probe_registers[d], probe_registers[s]);
                check_form(insn, length, probe_widths[w], probe_registers[d], (int)probe_registers[s], regs, NULL,
                           d == s && s < 3);
            }
}

static void rewritten_memory_forms(void) {
    static const unsigned bases[] = {0, 3, 5, 6, 8, 12, 13, 15};
    for (unsigned w = 0; w < 3; ++w)
        for (unsigned d = 0; d < PROBE_REGISTER_COUNT; ++d)
            for (unsigned b = 0; b < sizeof bases / sizeof *bases; ++b)
                for (int mode = 0; mode < 4; ++mode) {
                    const unsigned base = bases[b];
                    if ((mode == 0 && (base & 7) == 5) || (mode == 2 && ((base & 7) == 4 || probe_widths[w] == 16)))
                        continue;
                    uint8_t insn[16];
                    uint64_t regs[16], displacement;
                    fill_registers(regs);
                    const size_t length =
                        encode_memory_form(insn, probe_widths[w], probe_registers[d], base, mode, &displacement);
                    regs[1] = mode == 3 ? 4 : regs[1];
                    regs[base] = (uint64_t)(uintptr_t)probe_arena + 64;
                    uint8_t *value_at = probe_arena + 64 + displacement + (mode == 3 ? 16 : 0);
                    check_form(insn, length, probe_widths[w], probe_registers[d], -1, regs, value_at, 0);
                }
}

static const LzcntShape *find_shape(int (*wanted)(const LzcntShape *)) {
    for (size_t b = 0; b < LZCNT_BUILD_COUNT; ++b)
        for (size_t i = 0; i < lzcnt_builds[b].count; ++i)
            if (wanted(&lzcnt_builds[b].shapes[i])) return &lzcnt_builds[b].shapes[i];
    return NULL;
}

static int is_pair_ebp_ecx_eax_r9d(const LzcntShape *s) {
    static const uint8_t pair[] = {0xf3, 0x0f, 0xbd, 0xe9, 0xf3, 0x41, 0x0f, 0xbd, 0xc1};
    return s->second == 4 && s->length == sizeof pair && !memcmp(s->bytes, pair, sizeof pair);
}

static void rewritten_game_forms(void) {
    uint64_t regs[16], out[16];
    fill_registers(regs);
    regs[13] = (uint64_t)(uintptr_t)probe_arena;
    regs[8] = 0x10;
    const uint8_t r13_r8[] = {0xf3, 0x4b, 0x0f, 0xbd, 0x94, 0x05, 0xa8, 0x03, 0x00, 0x00};
    check_form(r13_r8, sizeof r13_r8, 64, 2, -1, regs, probe_arena + 0x3a8 + 0x10, 0);
    const uint8_t r13_disp32[] = {0xf3, 0x49, 0x0f, 0xbd, 0x85, 0x68, 0x0c, 0x00, 0x00};
    check_form(r13_disp32, sizeof r13_disp32, 64, 0, -1, regs, probe_arena + 0xc68, 0);
    regs[2] = (uint64_t)(uintptr_t)probe_arena;
    regs[6] = 0x20;
    const uint8_t rdx_rsi[] = {0xf3, 0x48, 0x0f, 0xbd, 0x84, 0x32, 0xf8, 0x03, 0x00, 0x00};
    check_form(rdx_rsi, sizeof rdx_rsi, 64, 0, -1, regs, probe_arena + 0x3f8 + 0x20, 0);

    /* Two LZCNTs in one range: the first's result, the second's result and flags. */
    const LzcntShape *pair = find_shape(is_pair_ebp_ecx_eax_r9d);
    CHECK(pair != NULL);
    if (pair && prepare_shape(pair)) {
        fill_registers(regs);
        regs[1] = 0x00ff0000;
        regs[9] = 0;
        probe(probe_block, regs, out);
        CHECK(out[5] == 8 && (out[0] & 0xffffffff) == 32);
        CHECK((out[PROBE_FLAGS] & 1) == 1 && (out[PROBE_FLAGS] >> 6 & 1) == 0);
        regs[1] = 0;
        regs[9] = 0x80000000;
        probe(probe_block, regs, out);
        CHECK(out[5] == 32 && (out[0] & 0xffffffff) == 0);
        CHECK((out[PROBE_FLAGS] & 1) == 0 && (out[PROBE_FLAGS] >> 6 & 1) == 1);
        for (unsigned r = 0; r < 16; ++r)
            if (r != 0 && r != 5 && r != PROBE_RSP && r != PROBE_FLAGS) CHECK(out[r] == regs[r]);
    }

    /* The shape code supports an instruction ahead of the LZCNT, which none of the game's sites has:
     * or esi, 1; lzcnt edx, esi. */
    static const LzcntShape lead = {7, 3, 0, {0x83, 0xce, 0x01, 0xf3, 0x0f, 0xbd, 0xd6}};
    if (prepare_shape(&lead)) {
        static const struct { uint64_t rsi, rdx; int zf; } cases[] = {
            {0, 31, 0}, {0x80000000ull, 0, 1}, {0xffffffff00000000ull, 31, 0}, {0x00010000ull, 15, 0}, {~0ull, 0, 1},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
            fill_registers(regs);
            regs[6] = cases[i].rsi;
            probe(probe_block, regs, out);
            CHECK(out[6] == (uint32_t)(cases[i].rsi | 1) && out[2] == cases[i].rdx);
            CHECK((out[PROBE_FLAGS] & 1) == 0 && (int)(out[PROBE_FLAGS] >> 6 & 1) == cases[i].zf);
            for (unsigned r = 0; r < 16; ++r)
                if (r != 2 && r != 6 && r != PROBE_RSP && r != PROBE_FLAGS) CHECK(out[r] == regs[r]);
        }
    } else {
        CHECK(0 && "lead shape");
    }
}

typedef struct {
    int memory, base, index;
} Operand;

static Operand operand_of(const LzcntShape *shape) {
    Operand operand = {0, -1, -1};
    size_t i = shape->first;
    while (shape->bytes[i] == 0x66 || shape->bytes[i] == 0xf3) ++i;
    const unsigned rex = (shape->bytes[i] & 0xf0) == 0x40 ? shape->bytes[i++] : 0;
    const unsigned modrm = shape->bytes[i + 2], mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) return operand;
    operand.memory = 1;
    if (rm == 4) {
        const unsigned sib = shape->bytes[i + 3], index = (sib >> 3 & 7) | (rex & 2 ? 8 : 0);
        if (index != 4) operand.index = (int)index;
        if (mod != 0 || (sib & 7) != 5) operand.base = (int)((sib & 7) | (rex & 1 ? 8 : 0));
    } else {
        operand.base = (int)(rm | (rex & 1 ? 8 : 0));
    }
    return operand;
}

/* Every site shape of every build, rewritten and as it is, on the same registers and memory: all registers, the
 * memory the shape writes, CF and ZF must match. A memory operand's base register points into the arena and its
 * index is 0x10. */
static void table_shapes(void) {
    if (!host_has_lzcnt()) {
        puts("no LZCNT on this host: table shapes not compared");
        return;
    }
    static uint8_t arena_after[2][PROBE_PAGE];
    size_t compared = 0;
    for (size_t b = 0; b < LZCNT_BUILD_COUNT; ++b)
        for (size_t i = 0; i < lzcnt_builds[b].count; ++i) {
            const LzcntShape *shape = &lzcnt_builds[b].shapes[i];
            if (!prepare_shape(shape)) {
                printf("FAIL %s shape %zu cannot be rewritten\n", lzcnt_builds[b].build, i);
                CHECK(0);
                continue;
            }
            const Operand operand = operand_of(shape);
            const int memory = operand.memory;
            CHECK(!memory || operand.base >= 0);
            for (size_t n = 0; n < 2 * PROBE_VALUE_COUNT; ++n) {
                const uint64_t value = probe_values[n % PROBE_VALUE_COUNT];
                uint64_t in[16], out[2][16];
                for (unsigned r = 0; r < 16; ++r)
                    in[r] = memory ? 0x10 : n < PROBE_VALUE_COUNT ? value : value >> r | value << (64 - r) % 64;
                if (memory) in[operand.base] = (uint64_t)(uintptr_t)probe_arena;
                for (int pass = 0; pass < 2; ++pass) {
                    for (size_t k = 0; k < PROBE_PAGE / 8; ++k) memcpy(probe_arena + 8 * k, &value, 8);
                    probe(probe_block + (pass ? 2 * PROBE_PAGE : 0), in, out[pass]);
                    memcpy(arena_after[pass], probe_arena, PROBE_PAGE);
                }
                int same = memcmp(arena_after[0], arena_after[1], PROBE_PAGE) == 0;
                for (unsigned r = 0; r < 16; ++r)
                    if (r != PROBE_RSP && r != PROBE_FLAGS && out[0][r] != out[1][r]) same = 0;
                same = same && (out[0][PROBE_FLAGS] & 0x41) == (out[1][PROBE_FLAGS] & 0x41);
                if (!same) {
                    printf("FAIL %s shape %zu value %llx:", lzcnt_builds[b].build, i, (unsigned long long)value);
                    for (unsigned r = 0; r < 16; ++r)
                        if (out[0][r] != out[1][r])
                            printf(" r%u %llx/%llx", r, (unsigned long long)out[0][r], (unsigned long long)out[1][r]);
                    printf("\n");
                }
                CHECK(same);
                ++compared;
            }
        }
    printf("table shapes: %zu runs matched the processor's LZCNT\n", compared);
}

static int count_lzcnt_bytes(const uint8_t *image, uint64_t size) {
    int count = 0;
    for (uint64_t i = 0; i + 3 < size; ++i) {
        if (image[i] != 0xf3) continue;
        const uint64_t at = (image[i + 1] & 0xf0) == 0x40 ? i + 2 : i + 1;
        count += at + 1 < size && image[at] == 0x0f && image[at + 1] == 0xbd;
    }
    return count;
}

/* The LZCNT instructions each build holds. */
static const int build_lzcnts[LZCNT_BUILD_COUNT] = {95};

/* A readable image holding every site of the build, followed by the stub area. */
typedef struct {
    uint8_t *bytes;
    uint8_t *stubs;
} SyntheticImage;

static SyntheticImage synthetic_image(const LzcntBuild *build) {
    SyntheticImage image = {NULL, NULL};
    image.bytes = calloc(1, build->image.size + lzcnt_compat_stub_bytes());
    if (!image.bytes) return image;
    image.stubs = image.bytes + build->image.size;
    for (size_t i = 0; i < build->count; ++i)
        memcpy(image.bytes + build->offsets[i], build->shapes[i].bytes, build->shapes[i].length);
    return image;
}

static void site_tables(void) {
    const size_t stubs = lzcnt_compat_stub_bytes(), stride = stubs / LZCNT_SITE_MAX;
    for (size_t b = 0; b < LZCNT_BUILD_COUNT; ++b) {
        const LzcntBuild *build = &lzcnt_builds[b];
        int lzcnts = 0;
        CHECK(build->count <= LZCNT_SITE_MAX);
        for (size_t i = 0; i < build->count; ++i) {
            const LzcntShape *shape = &build->shapes[i];
            CHECK(shape->length >= 5 && shape->length <= LZCNT_SHAPE_MAX && shape->first < shape->length);
            CHECK(!shape->second || shape->second > shape->first);
            lzcnts += count_lzcnt_bytes(shape->bytes, shape->length);
        }
        CHECK(lzcnts == build_lzcnts[b]);
        for (size_t i = 1; i < build->count; ++i)
            CHECK(build->offsets[i] >= build->offsets[i - 1] + build->shapes[i - 1].length);
        CHECK(build->offsets[build->count - 1] + build->shapes[build->count - 1].length <= build->image.size);
        for (size_t o = 0; o < b; ++o)
            CHECK(lzcnt_builds[o].image.size != build->image.size || lzcnt_builds[o].image.entry != build->image.entry);

        SyntheticImage image = synthetic_image(build);
        if (!image.bytes) {
            CHECK(0 && "synthetic image");
            continue;
        }
        uint8_t *before = malloc(build->image.size);
        memcpy(before, image.bytes, build->image.size);
        const LzcntBuild *found = NULL;
        CHECK(lzcnt_compat_install(image.bytes, build->image.size, build->image.entry, image.stubs, stubs, &found) ==
              LZCNT_COMPAT_OK);
        CHECK(found == build);
        CHECK(count_lzcnt_bytes(image.bytes, build->image.size) == 0);
        size_t changed = 0, allowed = 0;
        for (size_t i = 0; i < build->count; ++i) {
            const uint8_t *site = image.bytes + build->offsets[i];
            int32_t rel;
            memcpy(&rel, site + 1, 4);
            CHECK(site[0] == 0xe9 && site + 5 + rel == image.stubs + i * stride);
            for (unsigned k = 5; k < build->shapes[i].length; ++k) CHECK(site[k] == 0x90);
            allowed += build->shapes[i].length;
        }
        for (uint64_t k = 0; k < build->image.size; ++k) changed += image.bytes[k] != before[k];
        CHECK(changed <= allowed);
        free(before);
        free(image.bytes);
    }
}

static int sites_untouched(const SyntheticImage *image, const uint8_t *before, const LzcntBuild *build) {
    for (size_t i = 0; i < build->count; ++i)
        if (memcmp(image->bytes + build->offsets[i], before + build->offsets[i], build->shapes[i].length)) return 0;
    return 1;
}

static size_t shape_key(const LzcntShape *shape) {
    const Operand operand = operand_of(shape);
    size_t i = shape->first;
    const int prefix = shape->bytes[i] == 0x66;
    while (shape->bytes[i] == 0x66 || shape->bytes[i] == 0xf3) ++i;
    const int rex = (shape->bytes[i] & 0xf0) == 0x40;
    return shape->length | (size_t)shape->second << 4 | (size_t)prefix << 8 | (size_t)rex << 9 |
           (size_t)operand.memory << 10 | (size_t)(operand.index >= 0) << 11;
}

static void patch_rejections(void) {
    const LzcntBuild *build = &lzcnt_builds[0];
    SyntheticImage image = synthetic_image(build);
    if (!image.bytes) {
        CHECK(0 && "synthetic image");
        return;
    }
    const size_t stubs = lzcnt_compat_stub_bytes(), total = build->image.size + stubs, stride = stubs / LZCNT_SITE_MAX;
    uint8_t *before = malloc(total);
    memcpy(before, image.bytes, total);
    /* An apply writes only at the sites and in the stub area; the whole image is compared at the end. */
#define UNTOUCHED() (sites_untouched(&image, before, build) && memcmp(before + build->image.size, image.stubs, stubs) == 0)

    CHECK(lzcnt_compat_install(image.bytes, build->image.size + 16, build->image.entry, image.stubs, stubs, NULL) ==
          LZCNT_COMPAT_UNKNOWN_IMAGE);
    CHECK(lzcnt_compat_install(image.bytes, build->image.size, build->image.entry + 16, image.stubs, stubs, NULL) ==
          LZCNT_COMPAT_UNKNOWN_IMAGE);
    CHECK(lzcnt_compat_install(image.bytes, build->image.size - 1, build->image.entry, image.stubs, stubs, NULL) ==
          LZCNT_COMPAT_UNKNOWN_IMAGE);
    CHECK(lzcnt_compat_apply(image.bytes, build->image.size, build->offsets, build->shapes, build->count, image.stubs,
                             build->count * stride - 1) == LZCNT_COMPAT_NO_ROOM);
    CHECK(lzcnt_compat_apply(image.bytes, build->image.size, build->offsets, build->shapes, build->count, image.stubs,
                             0) == LZCNT_COMPAT_NO_ROOM);
    uint8_t *far_stubs = (uint8_t *)((uintptr_t)image.bytes + 0x90000000u);
    CHECK(lzcnt_compat_install(image.bytes, build->image.size, build->image.entry, far_stubs, stubs, NULL) ==
          LZCNT_COMPAT_NO_ROOM);
    uint8_t *behind_stubs = (uint8_t *)((uintptr_t)image.bytes - 0x90000000u);
    CHECK(lzcnt_compat_install(image.bytes, build->image.size, build->image.entry, behind_stubs, stubs, NULL) ==
          LZCNT_COMPAT_NO_ROOM);
    CHECK(UNTOUCHED());

    size_t keys[LZCNT_SITE_MAX], key_count = 0, mutations = 0;
    for (size_t i = 0; i < build->count; ++i) {
        const size_t key = shape_key(&build->shapes[i]);
        size_t k = 0;
        while (k < key_count && keys[k] != key) ++k;
        if (k < key_count) continue;
        keys[key_count++] = key;
        for (unsigned k2 = 0; k2 < build->shapes[i].length; ++k2) {
            image.bytes[build->offsets[i] + k2] ^= 0x40;
            CHECK(lzcnt_compat_install(image.bytes, build->image.size, build->image.entry, image.stubs, stubs, NULL) ==
                  LZCNT_COMPAT_BAD_SITE);
            image.bytes[build->offsets[i] + k2] ^= 0x40;
            CHECK(UNTOUCHED());
            ++mutations;
        }
    }
    printf("site table: %zu shape types, %zu single-byte mutations refused\n", key_count, mutations);
    CHECK(lzcnt_compat_apply(image.bytes, build->offsets[build->count - 1] + 4, build->offsets, build->shapes,
                             build->count, image.stubs, stubs) == LZCNT_COMPAT_BAD_SITE);
    CHECK(UNTOUCHED());

    uint32_t shifted[LZCNT_SITE_MAX];
    for (size_t i = 0; i < build->count; ++i) shifted[i] = build->offsets[i] + 1;
    CHECK(lzcnt_compat_apply(image.bytes, build->image.size, shifted, build->shapes, build->count, image.stubs,
                             stubs) == LZCNT_COMPAT_BAD_SITE);
    CHECK(UNTOUCHED());
    CHECK(memcmp(before, image.bytes, total) == 0);
    free(before);
    free(image.bytes);
#undef UNTOUCHED

    /* Shapes the rewrite cannot handle, each matching the bytes it sits on. */
    static const LzcntShape bad[] = {
        {4, 0, 0, {0xf3, 0x0f, 0xbd, 0xc8}},                                      /* too short for a jmp */
        {8, 0, 0, {0xf3, 0x0f, 0xbd, 0x05, 0x10, 0x00, 0x00, 0x00}},              /* RIP-relative operand */
        {5, 0, 0, {0x0f, 0xbd, 0xc8, 0x90, 0x90}},                                /* BSR, not LZCNT */
        {5, 0, 0, {0xf2, 0xf3, 0x0f, 0xbd, 0xc8}},                                /* another prefix */
        {6, 0, 0, {0xf3, 0x0f, 0xbc, 0xc8, 0x90, 0x90}},                          /* TZCNT */
        {9, 0, 4, {0xf3, 0x0f, 0xbd, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90}},        /* the second is not an LZCNT */
        {9, 0, 9, {0xf3, 0x0f, 0xbd, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90}},        /* the second is beyond the range */
        {5, 0, 0, {0xf3, 0x0f, 0xbd, 0x44, 0x24}},                                /* operand cut by the range */
        {11, 0, 0, {0xf3, 0x0f, 0xbd, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}}, /* longer than a range may be */
        {7, 2, 0, {0x83, 0xce, 0x01, 0xf3, 0x0f, 0xbd, 0xd6}},                    /* first is not at the LZCNT */
        {7, 7, 0, {0x83, 0xce, 0x01, 0xf3, 0x0f, 0xbd, 0xd6}},                    /* first is beyond the range */
        {7, 0, 0, {0x83, 0xce, 0x01, 0xf3, 0x0f, 0xbd, 0xd6}},                    /* a lead without first */
        {8, 4, 3, {0xf3, 0x0f, 0xbd, 0xc0, 0xf3, 0x0f, 0xbd, 0xc9}},              /* second ahead of first */
    };
    {
        uint8_t buffer[64 + 256], copy[sizeof buffer];
        memset(buffer, 0, sizeof buffer);
        memcpy(buffer + 16, build->shapes[0].bytes, build->shapes[0].length);
        memcpy(copy, buffer, sizeof buffer);
        const uint32_t twice[2] = {16, 16};
        const LzcntShape both[2] = {build->shapes[0], build->shapes[0]};
        CHECK(lzcnt_compat_apply(buffer, 64, twice, both, 2, buffer + 64, 256) == LZCNT_COMPAT_BAD_SITE);
        CHECK(memcmp(buffer, copy, sizeof buffer) == 0);
        const uint32_t next[2] = {16, 16 + build->shapes[0].length - 1};
        CHECK(lzcnt_compat_apply(buffer, 64, next, both, 2, buffer + 64, 256) == LZCNT_COMPAT_BAD_SITE);
        CHECK(memcmp(buffer, copy, sizeof buffer) == 0);
        const uint32_t ok[1] = {16};
        CHECK(lzcnt_compat_apply(buffer, 64, ok, both, 1, buffer + 64, 256) == LZCNT_COMPAT_OK);
    }
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        uint8_t buffer[64 + 128], copy[sizeof buffer];
        memset(buffer, 0, sizeof buffer);
        const uint32_t offset = 16;
        memcpy(buffer + offset, bad[i].bytes, LZCNT_SHAPE_MAX);
        memcpy(copy, buffer, sizeof buffer);
        CHECK(lzcnt_compat_apply(buffer, 64, &offset, &bad[i], 1, buffer + 64, 128) == LZCNT_COMPAT_BAD_SITE);
        CHECK(memcmp(buffer, copy, sizeof buffer) == 0);
    }
}

static uint64_t read64(const uint8_t *data, size_t offset) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = value << 8 | data[offset + (size_t)i];
    return value;
}

/* The memory image inside boot.bin (layout written by launcher/boot_image.cpp), read only, with room for stubs. */
static uint8_t *boot_image(uint64_t *size, uint64_t *entry, size_t extra) {
    FILE *file = fopen(SOTB_BOOT_IMAGE, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = length > 0 ? malloc((size_t)length) : NULL;
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length || memcmp(data, "SOTBBOT1", 8)) {
        fclose(file);
        free(data);
        return NULL;
    }
    fclose(file);
    *size = read64(data, 8);
    *entry = read64(data, 16);
    const uint64_t segments = read64(data, 24), relocations = read64(data, 32), imports = read64(data, 40);
    size_t offset = 8 + 6 * 8 + 8 + 4 * 8;
    offset += 8 + read64(data, offset) * 56;
    offset += 8 + read64(data, offset) * 24;
    offset += segments * 24 + imports * 128 + relocations * 32;
    uint8_t *image = calloc(1, *size + extra);
    if (image && offset + *size <= (size_t)length) memcpy(image, data + offset, *size);
    else free(image), image = NULL;
    free(data);
    return image;
}

static void real_image(void) {
    const size_t stubs = lzcnt_compat_stub_bytes();
    const LzcntBuild *build = &lzcnt_builds[0];
    uint64_t size = 0, entry = 0;
    uint8_t *image = boot_image(&size, &entry, stubs);
    if (!image) {
        printf("real image: SKIPPED, no boot image at %s\n", SOTB_BOOT_IMAGE);
        return;
    }
    CHECK(size == build->image.size && entry == build->image.entry);
    const int lzcnts = count_lzcnt_bytes(image, size);
    CHECK(lzcnts == build_lzcnts[0]);
    for (size_t i = 0; i < build->count; ++i) CHECK(!memcmp(image + build->offsets[i], build->shapes[i].bytes, build->shapes[i].length));
    uint8_t *before = malloc(size);
    memcpy(before, image, size);

    uint32_t shifted[LZCNT_SITE_MAX];
    for (size_t i = 0; i < build->count; ++i) shifted[i] = build->offsets[i] + 1;
    CHECK(lzcnt_compat_apply(image, size, shifted, build->shapes, build->count, image + size, stubs) ==
          LZCNT_COMPAT_BAD_SITE);
    CHECK(memcmp(image, before, size) == 0);

    CHECK(lzcnt_compat_install(image, size, entry, image + size, stubs, NULL) == LZCNT_COMPAT_OK);
    CHECK(count_lzcnt_bytes(image, size) == 0);
    size_t changed = 0, inside = 0, with_two = 0;
    for (uint64_t k = 0; k < size; ++k) {
        if (image[k] == before[k]) continue;
        ++changed;
        for (size_t i = 0; i < build->count; ++i)
            inside += k >= build->offsets[i] && k < build->offsets[i] + build->shapes[i].length;
    }
    for (size_t i = 0; i < build->count; ++i) with_two += build->shapes[i].second != 0;
    CHECK(changed == inside);
    printf("real image %s: %d LZCNT, %zu patch ranges, %zu with two LZCNT, %zu bytes changed in a copy\n", build->build,
           lzcnts, build->count, with_two, changed);
    free(before);
    free(image);
}

int main(void) {
    crash_site();
    load_widths();
    store_widths();
    addressing_forms();
    rip_relative();
    rejected();
    strlen_site();
    andn_forms();
    bextr_forms();
    bls_forms();
    bmi1_rejected();
    host_sets();
    site_tables();
    patch_rejections();
    rewritten_register_forms();
    rewritten_memory_forms();
    rewritten_game_forms();
    table_shapes();
    real_image();
    if (failures) {
        printf("isa tests failed: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("isa tests passed: %d checks\n", checks);
    return 0;
}

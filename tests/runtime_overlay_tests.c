/* The runtime file overlay the Xbox prompt art is served through: the host file, zeros past its end, and sorted patches on
 * top, read through the real file layer in a temporary folder. The original file is hashed before and after. */
#include "../src/runtime_file.c"
#include "test_platform.h"

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
int32_t runtime_guest_errno(int e) { return e; }
static int32_t guest_errno_value;
int32_t *runtime_errno(void) { return &guest_errno_value; }
void runtime_memory_io_begin(void) {}
void runtime_memory_io_end(void) {}

enum { HOST_SIZE = 1600000, LENGTH = 1700000 };

static unsigned char host_byte(uint64_t i) { return (unsigned char)(i * 31 + 7); }

static uint32_t crc32_of(const unsigned char *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

static uint32_t file_crc(const char *path) {
    FILE *in = fopen(path, "rb");
    if (!in) return 0;
    unsigned char *data = malloc(HOST_SIZE);
    const size_t n = fread(data, 1, HOST_SIZE, in);
    fclose(in);
    const uint32_t crc = n == HOST_SIZE ? crc32_of(data, n) : 0;
    free(data);
    return crc;
}

static unsigned char patch1[50], patch2[20], patch3[30], patch4[100], patch5[4096];

static void fill(unsigned char *data, size_t size, unsigned char base) {
    for (size_t i = 0; i < size; ++i) data[i] = (unsigned char)(base + i * 3);
}

static unsigned char *expected_image(const RuntimeFilePatch *patches, size_t count) {
    unsigned char *image = calloc(LENGTH, 1);
    for (uint64_t i = 0; i < HOST_SIZE; ++i) image[i] = host_byte(i);
    for (size_t i = 0; i < count; ++i) memcpy(image + patches[i].offset, patches[i].data, patches[i].size);
    return image;
}

static int same(const unsigned char *a, const unsigned char *b, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (a[i] != b[i]) { printf("  first difference at %zu: %02x != %02x\n", i, a[i], b[i]); return 0; }
    return 1;
}

int main(void) {
    char dir[512], host[600], other[600];
    test_temp_folder(dir, sizeof(dir), "sotb_overlay");
    snprintf(host, sizeof(host), "%s/pak.bin", dir);
    snprintf(other, sizeof(other), "%s/other.bin", dir);
    FILE *out = fopen(host, "wb");
    CHECK(out != NULL);
    if (!out) return 1;
    for (uint64_t i = 0; i < HOST_SIZE; ++i) fputc(host_byte(i), out);
    fclose(out);
    out = fopen(other, "wb");
    for (int i = 0; i < 1000; ++i) fputc(i, out);
    fclose(out);
    const uint32_t before = file_crc(host);
    CHECK(before != 0);
    CHECK(runtime_file_mount("/ov", dir) == 0);

    fill(patch1, sizeof(patch1), 0xA0);
    fill(patch2, sizeof(patch2), 0xB0);
    fill(patch3, sizeof(patch3), 0xC0);
    fill(patch4, sizeof(patch4), 0xD0);
    fill(patch5, sizeof(patch5), 0xE0);
    // Unsorted on purpose; patch2 crosses the 512 KiB read chunk, patch3 the end of the host file, patch4 is past it.
    const RuntimeFilePatch patches[] = {
        {1000000, sizeof(patch5), patch5}, {100, sizeof(patch1), patch1}, {1650000, sizeof(patch4), patch4},
        {524280, sizeof(patch2), patch2}, {1599990, sizeof(patch3), patch3},
    };

    // Refusals register nothing.
    {
        const RuntimeFilePatch outside[] = {{LENGTH - 10, 20, patch2}};
        CHECK(runtime_file_overlay("/ov/pak.bin", LENGTH, outside, 1) == -1);
        const RuntimeFilePatch overlapping[] = {{100, 50, patch1}, {120, 50, patch1}};
        CHECK(runtime_file_overlay("/ov/pak.bin", LENGTH, overlapping, 2) == -1);
        const RuntimeFilePatch no_data[] = {{100, 50, NULL}};
        CHECK(runtime_file_overlay("/ov/pak.bin", LENGTH, no_data, 1) == -1);
        CHECK(runtime_file_overlay(NULL, LENGTH, patches, 5) == -1);
        const int64_t raw = runtime_file_open("/ov/pak.bin", 0, 0);
        CHECK(raw >= 3);
        unsigned char one[8];
        CHECK(runtime_file_pread((int)raw, one, 8, 100) == 8 && one[0] == host_byte(100));
        int64_t st[32] = {0};
        CHECK(runtime_file_fstat((int)raw, st) == 0 && ((GuestStat *)st)->size == HOST_SIZE);
        runtime_file_close((int)raw);
    }

    CHECK(runtime_file_overlay("/ov/pak.bin", LENGTH, patches, 5) == 0);
    CHECK(runtime_file_overlay("/ov/pak.bin", LENGTH, patches, 5) == -1);  // already registered
    CHECK(runtime_file_overlay("/ov/PAK.BIN", LENGTH, patches, 5) == -1);  // the same file in another case

    unsigned char *expected = expected_image(patches, 5);
    unsigned char *buffer = malloc(LENGTH + 64);

    const int64_t fd = runtime_file_open("/ov/Pak.Bin", 0, 0);  // opened in another case: the overlay still matches
    CHECK(fd >= 3);
    if (fd >= 3) {
        // The whole file in one read: more than one internal chunk.
        memset(buffer, 0x55, LENGTH + 64);
        CHECK(runtime_file_pread((int)fd, buffer, LENGTH, 0) == LENGTH);
        CHECK(same(buffer, expected, LENGTH));
        CHECK(buffer[LENGTH] == 0x55);
        // Past the end.
        CHECK(runtime_file_pread((int)fd, buffer, 100, LENGTH) == 0);
        CHECK(runtime_file_pread((int)fd, buffer, 100, LENGTH + 5000) == 0);
        CHECK(runtime_file_pread((int)fd, buffer, 100, LENGTH - 10) == 10 && same(buffer, expected + LENGTH - 10, 10));
        CHECK(runtime_file_pread((int)fd, buffer, 0, 5) == 0);
        CHECK(runtime_file_pread((int)fd, buffer, 10, -1) < 0);
        // Reads around every patch edge.
        for (size_t p = 0; p < 5; ++p) {
            const uint64_t at = patches[p].offset, size = patches[p].size;
            const struct { int64_t from; uint64_t length; } cases[] = {
                {(int64_t)at - 1, 1}, {(int64_t)at - 1, 2}, {(int64_t)at, 1}, {(int64_t)at, size}, {(int64_t)at + 1, size - 1},
                {(int64_t)(at + size) - 1, 1}, {(int64_t)(at + size) - 1, 2}, {(int64_t)(at + size), 1}, {(int64_t)at - 100, size + 200},
            };
            for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); ++c) {
                const uint64_t want = (uint64_t)cases[c].from + cases[c].length > LENGTH ? LENGTH - (uint64_t)cases[c].from : cases[c].length;
                CHECK(runtime_file_pread((int)fd, buffer, cases[c].length, cases[c].from) == (int64_t)want);
                CHECK(same(buffer, expected + cases[c].from, want));
            }
        }
        // Many reads of different offsets and sizes.
        uint32_t state = 99;
        for (int i = 0; i < 3000; ++i) {
            state = state * 1664525u + 1013904223u;
            const uint64_t at = (state >> 8) % LENGTH;
            state = state * 1664525u + 1013904223u;
            uint64_t size = 1 + (state >> 8) % 70000;
            if (at + size > LENGTH) size = LENGTH - at;
            CHECK(runtime_file_pread((int)fd, buffer, size, (int64_t)at) == (int64_t)size && memcmp(buffer, expected + at, size) == 0);
        }
        // Sequential reads move the position and end at the overlay's length, not the host file's.
        CHECK(runtime_file_lseek((int)fd, 0, 0) == 0);
        uint64_t total = 0;
        for (;;) {
            const int64_t n = runtime_file_read((int)fd, buffer, 77777);
            CHECK(n >= 0);
            if (n <= 0) break;
            CHECK(memcmp(buffer, expected + total, (size_t)n) == 0);
            total += (uint64_t)n;
        }
        CHECK(total == LENGTH);
        CHECK(runtime_file_lseek((int)fd, 0, 2) == LENGTH);
        CHECK(runtime_file_lseek((int)fd, -100, 2) == LENGTH - 100);
        CHECK(runtime_file_read((int)fd, buffer, 1000) == 100 && memcmp(buffer, expected + LENGTH - 100, 100) == 0);
        CHECK(runtime_file_lseek((int)fd, 1500000, 0) == 1500000);
        CHECK(runtime_file_lseek((int)fd, 10, 1) == 1500010);
        CHECK(runtime_file_read((int)fd, buffer, 16) == 16 && memcmp(buffer, expected + 1500010, 16) == 0);
        // The size the guest sees.
        int64_t st[32] = {0};
        CHECK(runtime_file_fstat((int)fd, st) == 0 && ((GuestStat *)st)->size == LENGTH);
        memset(st, 0, sizeof(st));
        CHECK(runtime_file_stat("/ov/pak.bin", st) == 0 && ((GuestStat *)st)->size == LENGTH);
        runtime_file_close((int)fd);
    }

    // A file without an overlay is served as it is.
    {
        const int64_t plain = runtime_file_open("/ov/other.bin", 0, 0);
        CHECK(plain >= 3);
        unsigned char data[1000];
        CHECK(runtime_file_pread((int)plain, data, 1000, 0) == 1000);
        int same_bytes = 1;
        for (int i = 0; i < 1000; ++i) same_bytes &= data[i] == (unsigned char)i;
        CHECK(same_bytes);
        CHECK(runtime_file_pread((int)plain, data, 10, 1000) == 0);
        int64_t st[32] = {0};
        CHECK(runtime_file_fstat((int)plain, st) == 0 && ((GuestStat *)st)->size == 1000);
        runtime_file_close((int)plain);
    }

    // The table holds four overlays; a fifth is refused and registers nothing.
    {
        const RuntimeFilePatch tiny[] = {{0, 4, patch1}};
        CHECK(runtime_file_overlay("/ov/a.bin", 10, tiny, 1) == 0);
        CHECK(runtime_file_overlay("/ov/b.bin", 10, tiny, 1) == 0);
        CHECK(runtime_file_overlay("/ov/c.bin", 10, tiny, 1) == 0);
        CHECK(runtime_file_overlay("/ov/d.bin", 10, tiny, 1) == -1);
    }

    // The host file never changed.
    CHECK(file_crc(host) == before);
    free(expected);
    free(buffer);
    test_remove_file(host);
    test_remove_file(other);
    test_remove_folder(dir);
    printf("runtime overlay tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

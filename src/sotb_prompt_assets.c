#include "sotb_prompt_assets.h"

#include <stdlib.h>
#include <string.h>

#define PAK_FOOTER_SIZE 44
#define PAK_MAGIC 0x5A6F12E1u
#define PAK_VERSION 3u

uint32_t sotb_prompts_crc32(const void *data, size_t size) {
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    const unsigned char *bytes = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

enum { MAX_BITS = 15 };

typedef struct {
    const unsigned char *in;
    size_t in_size, in_pos;
    unsigned char *out;
    size_t out_size, out_pos;
    uint32_t bit_buffer;
    int bit_count;
} Inflate;

typedef struct {
    short count[MAX_BITS + 1];
    short symbol[288];
} Huffman;

static int take_bits(Inflate *s, int need, unsigned *value) {
    uint32_t buffer = s->bit_buffer;
    while (s->bit_count < need) {
        if (s->in_pos == s->in_size) return -1;
        buffer |= (uint32_t)s->in[s->in_pos++] << s->bit_count;
        s->bit_count += 8;
    }
    s->bit_buffer = buffer >> need;
    s->bit_count -= need;
    *value = buffer & ((1u << need) - 1);
    return 0;
}

static int decode_symbol(Inflate *s, const Huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAX_BITS; ++len) {
        unsigned bit;
        if (take_bits(s, 1, &bit)) return -1;
        code |= (int)bit;
        const int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* 0 for a complete code, positive for an incomplete one, negative for an over-subscribed one. */
static int build_huffman(Huffman *h, const short *length, int n) {
    short offsets[MAX_BITS + 1];
    for (int len = 0; len <= MAX_BITS; ++len) h->count[len] = 0;
    for (int symbol = 0; symbol < n; ++symbol) ++h->count[length[symbol]];
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAX_BITS; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offsets[1] = 0;
    for (int len = 1; len < MAX_BITS; ++len) offsets[len + 1] = (short)(offsets[len] + h->count[len]);
    for (int symbol = 0; symbol < n; ++symbol)
        if (length[symbol]) h->symbol[offsets[length[symbol]]++] = (short)symbol;
    return left;
}

static int inflate_codes(Inflate *s, const Huffman *lengths, const Huffman *distances) {
    static const short length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int symbol = decode_symbol(s, lengths);
        if (symbol < 0) return -1;
        if (symbol < 256) {
            if (s->out_pos == s->out_size) return -1;
            s->out[s->out_pos++] = (unsigned char)symbol;
        } else if (symbol == 256) {
            return 0;
        } else {
            symbol -= 257;
            if (symbol >= 29) return -1;
            unsigned extra;
            if (take_bits(s, length_extra[symbol], &extra)) return -1;
            const size_t length = (size_t)length_base[symbol] + extra;
            const int distance_symbol = decode_symbol(s, distances);
            if (distance_symbol < 0 || distance_symbol >= 30) return -1;
            if (take_bits(s, distance_extra[distance_symbol], &extra)) return -1;
            const size_t distance = (size_t)distance_base[distance_symbol] + extra;
            if (distance > s->out_pos || length > s->out_size - s->out_pos) return -1;
            for (size_t i = 0; i < length; ++i, ++s->out_pos) s->out[s->out_pos] = s->out[s->out_pos - distance];
        }
    }
}

static int inflate_stored(Inflate *s) {
    s->bit_buffer = 0;
    s->bit_count = 0;
    if (s->in_size - s->in_pos < 4) return -1;
    const unsigned len = s->in[s->in_pos] | (unsigned)s->in[s->in_pos + 1] << 8;
    const unsigned inverse = s->in[s->in_pos + 2] | (unsigned)s->in[s->in_pos + 3] << 8;
    s->in_pos += 4;
    if (len != (~inverse & 0xFFFFu) || len > s->in_size - s->in_pos || len > s->out_size - s->out_pos) return -1;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return 0;
}

static int inflate_fixed(Inflate *s) {
    Huffman lengths, distances;
    short code_lengths[288];
    int symbol = 0;
    for (; symbol < 144; ++symbol) code_lengths[symbol] = 8;
    for (; symbol < 256; ++symbol) code_lengths[symbol] = 9;
    for (; symbol < 280; ++symbol) code_lengths[symbol] = 7;
    for (; symbol < 288; ++symbol) code_lengths[symbol] = 8;
    build_huffman(&lengths, code_lengths, 288);
    for (symbol = 0; symbol < 30; ++symbol) code_lengths[symbol] = 5;
    build_huffman(&distances, code_lengths, 30);
    return inflate_codes(s, &lengths, &distances);
}

static int inflate_dynamic(Inflate *s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    unsigned nlen, ndist, ncode;
    if (take_bits(s, 5, &nlen) || take_bits(s, 5, &ndist) || take_bits(s, 4, &ncode)) return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) return -1;
    short code_lengths[320];
    Huffman lengths, distances;
    unsigned index = 0;
    for (; index < ncode; ++index) {
        unsigned value;
        if (take_bits(s, 3, &value)) return -1;
        code_lengths[order[index]] = (short)value;
    }
    for (; index < 19; ++index) code_lengths[order[index]] = 0;
    if (build_huffman(&lengths, code_lengths, 19) != 0) return -1;
    index = 0;
    while (index < nlen + ndist) {
        const int symbol = decode_symbol(s, &lengths);
        if (symbol < 0) return -1;
        if (symbol < 16) {
            code_lengths[index++] = (short)symbol;
            continue;
        }
        short previous = 0;
        unsigned repeat, value;
        if (symbol == 16) {
            if (index == 0) return -1;
            previous = code_lengths[index - 1];
            if (take_bits(s, 2, &value)) return -1;
            repeat = 3 + value;
        } else if (symbol == 17) {
            if (take_bits(s, 3, &value)) return -1;
            repeat = 3 + value;
        } else {
            if (take_bits(s, 7, &value)) return -1;
            repeat = 11 + value;
        }
        if (index + repeat > nlen + ndist) return -1;
        while (repeat--) code_lengths[index++] = previous;
    }
    if (code_lengths[256] == 0) return -1;
    int status = build_huffman(&lengths, code_lengths, (int)nlen);
    if (status < 0 || (status > 0 && nlen - lengths.count[0] != 1)) return -1;
    status = build_huffman(&distances, code_lengths + nlen, (int)ndist);
    if (status < 0 || (status > 0 && ndist - distances.count[0] != 1)) return -1;
    return inflate_codes(s, &lengths, &distances);
}

int sotb_prompts_inflate(const unsigned char *in, size_t in_size, unsigned char *out, size_t out_size) {
    Inflate s = {in, in_size, 0, out, out_size, 0, 0, 0};
    unsigned last, type;
    do {
        if (take_bits(&s, 1, &last) || take_bits(&s, 2, &type)) return -1;
        int status;
        if (type == 0) status = inflate_stored(&s);
        else if (type == 1) status = inflate_fixed(&s);
        else if (type == 2) status = inflate_dynamic(&s);
        else return -1;
        if (status) return -1;
    } while (!last);
    return s.out_pos == out_size ? 0 : -1;
}

const SotbPromptSet *sotb_prompt_set_for_build(const char *build_key) {
    if (!build_key) return NULL;
    for (size_t i = 0; i < sotb_prompt_set_count; ++i)
        if (!strcmp(sotb_prompt_sets[i].build, build_key)) return &sotb_prompt_sets[i];
    return NULL;
}

const char *sotb_prompts_status_text(SotbPromptStatus status) {
    switch (status) {
    case SOTB_PROMPTS_OK: return "installed";
    case SOTB_PROMPTS_UNSUPPORTED_BUILD: return "no prompt set for this build";
    case SOTB_PROMPTS_FILE_SIZE: return "pak size differs";
    case SOTB_PROMPTS_READ_FAILED: return "pak could not be read";
    case SOTB_PROMPTS_FOOTER: return "pak footer differs";
    case SOTB_PROMPTS_INDEX: return "pak index differs";
    case SOTB_PROMPTS_TABLE: return "generated table is inconsistent";
    case SOTB_PROMPTS_ASSET_BOUNDS: return "asset is outside the pak";
    case SOTB_PROMPTS_ASSET_CRC: return "asset differs from the expected original";
    case SOTB_PROMPTS_REPLACEMENT: return "replacement does not fit its asset";
    case SOTB_PROMPTS_MEMORY: return "out of memory";
    case SOTB_PROMPTS_OVERLAY_REFUSED: return "overlay refused";
    }
    return "unknown";
}

static uint32_t read_u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t read_u64(const unsigned char *p) { return (uint64_t)read_u32(p) | (uint64_t)read_u32(p + 4) << 32; }

static int footer_matches(const SotbPromptSet *set, const SotbPromptSource *source) {
    unsigned char footer[PAK_FOOTER_SIZE];
    if (source->read(source->user, set->file_size - PAK_FOOTER_SIZE, footer, sizeof(footer))) return -1;
    return read_u32(footer) == PAK_MAGIC && read_u32(footer + 4) == PAK_VERSION && read_u64(footer + 8) == set->index_offset &&
           read_u64(footer + 16) == set->index_size ? 0 : 1;
}

static int index_matches(const SotbPromptSet *set, const SotbPromptSource *source) {
    unsigned char *index = malloc(set->index_size);
    if (!index) return -2;
    int result = source->read(source->user, set->index_offset, index, set->index_size) ? -1
               : sotb_prompts_crc32(index, set->index_size) == set->index_crc32 ? 0 : 1;
    free(index);
    return result;
}

static int table_is_consistent(const SotbPromptSet *set) {
    size_t used = 0;
    for (size_t a = 0; a < set->asset_count; ++a) {
        const SotbPromptAsset *asset = &set->assets[a];
        if (asset->first_run != used || asset->first_run + (size_t)asset->run_count > set->run_count) return 0;
        uint32_t end = 0;
        for (uint32_t r = 0; r < asset->run_count; ++r) {
            const SotbPromptRun *run = &set->runs[asset->first_run + r];
            if (!run->length || run->offset < end || run->offset > asset->size || run->length > asset->size - run->offset ||
                run->blob_offset > set->blob_size || run->length > set->blob_size - run->blob_offset) return 0;
            end = run->offset + run->length;
        }
        used += asset->run_count;
    }
    return used == set->run_count;
}

SotbPromptResult sotb_prompts_install(const SotbPromptSet *set, const SotbPromptSource *source, SotbPromptOverlay overlay) {
    SotbPromptResult result = {SOTB_PROMPTS_UNSUPPORTED_BUILD, NULL, 0, 0};
    if (!set || !source || !source->read || !overlay) return result;
    if (!table_is_consistent(set)) { result.status = SOTB_PROMPTS_TABLE; return result; }
    if (source->file_size != set->file_size || set->file_size < PAK_FOOTER_SIZE) { result.status = SOTB_PROMPTS_FILE_SIZE; return result; }
    if (set->index_offset > set->file_size - PAK_FOOTER_SIZE || set->index_size > set->file_size - PAK_FOOTER_SIZE - set->index_offset) {
        result.status = SOTB_PROMPTS_FOOTER;
        return result;
    }
    int check = footer_matches(set, source);
    if (check) { result.status = check < 0 ? SOTB_PROMPTS_READ_FAILED : SOTB_PROMPTS_FOOTER; return result; }
    check = index_matches(set, source);
    if (check) {
        result.status = check == -1 ? SOTB_PROMPTS_READ_FAILED : check == -2 ? SOTB_PROMPTS_MEMORY : SOTB_PROMPTS_INDEX;
        return result;
    }

    uint32_t largest = 0;
    for (size_t a = 0; a < set->asset_count; ++a) {
        const SotbPromptAsset *asset = &set->assets[a];
        if (asset->data_offset > set->index_offset || asset->size > set->index_offset - asset->data_offset) {
            result.status = SOTB_PROMPTS_ASSET_BOUNDS;
            result.asset = asset->name;
            return result;
        }
        if (asset->size > largest) largest = asset->size;
    }
    unsigned char *blob = malloc(set->blob_size ? set->blob_size : 1);
    unsigned char *scratch = malloc(largest ? largest : 1);
    SotbPromptPatch *patches = calloc(set->run_count ? set->run_count : 1, sizeof(*patches));
    if (!blob || !scratch || !patches) {
        result.status = SOTB_PROMPTS_MEMORY;
        goto fail;
    }
    if (sotb_prompts_inflate(set->blob, set->blob_stored_size, blob, set->blob_size) || sotb_prompts_crc32(blob, set->blob_size) != set->blob_crc32) {
        result.status = SOTB_PROMPTS_REPLACEMENT;
        goto fail;
    }
    for (size_t a = 0; a < set->asset_count; ++a) {
        const SotbPromptAsset *asset = &set->assets[a];
        result.asset = asset->name;
        if (source->read(source->user, asset->data_offset, scratch, asset->size)) { result.status = SOTB_PROMPTS_READ_FAILED; goto fail; }
        if (sotb_prompts_crc32(scratch, asset->size) != asset->original_crc32) { result.status = SOTB_PROMPTS_ASSET_CRC; goto fail; }
        for (uint32_t r = 0; r < asset->run_count; ++r) {
            const SotbPromptRun *run = &set->runs[asset->first_run + r];
            memcpy(scratch + run->offset, blob + run->blob_offset, run->length);
            patches[asset->first_run + r] = (SotbPromptPatch){asset->data_offset + run->offset, run->length, blob + run->blob_offset};
        }
        if (sotb_prompts_crc32(scratch, asset->size) != asset->replaced_crc32) { result.status = SOTB_PROMPTS_REPLACEMENT; goto fail; }
    }
    result.asset = NULL;
    if (overlay(set->guest_path, set->file_size, patches, set->run_count)) {
        result.status = SOTB_PROMPTS_OVERLAY_REFUSED;
        goto fail;
    }
    free(scratch);
    free(patches);
    result.status = SOTB_PROMPTS_OK;
    result.resources = set->asset_count;
    result.patches = set->run_count;
    return result;
fail:
    free(blob);
    free(scratch);
    free(patches);
    return result;
}

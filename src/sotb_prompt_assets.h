/* Xbox controller prompt art for the game's own textures, served over the original pak at read time. The pak on disk is
 * only read: every texture to replace is identified by its position and CRC-32 first, and the replacement is installed
 * as one overlay or not at all. */
#ifndef SOTB_PROMPT_ASSETS_H
#define SOTB_PROMPT_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SOTB_PROMPT_FORMAT_DXT1 = 1, SOTB_PROMPT_FORMAT_DXT5 = 2, SOTB_PROMPT_FORMAT_BGRA8 = 3 } SotbPromptFormat;

/* Bytes that replace `length` bytes at `offset` inside the asset's package data. */
typedef struct {
    uint32_t offset;
    uint32_t length;
    uint32_t blob_offset;
} SotbPromptRun;

typedef struct {
    const char *name;           /* the entry in the pak, e.g. Beast/Content/HUD/Textures/ButtonCross.uasset */
    uint64_t data_offset;       /* file offset of the package data, after the entry's own header */
    uint32_t size;
    uint32_t original_crc32;
    uint32_t replaced_crc32;    /* of the whole package data once the runs are applied */
    uint32_t first_run, run_count;
    uint16_t format, width, height, mip_count;
} SotbPromptAsset;

typedef struct {
    const char *build;          /* CUSA03762-1.00 */
    const char *guest_path;
    uint64_t file_size;
    uint64_t index_offset;
    uint32_t index_size;
    uint32_t index_crc32;
    const SotbPromptAsset *assets;
    size_t asset_count;
    const SotbPromptRun *runs;
    size_t run_count;
    const unsigned char *blob;  /* raw deflate of every run's bytes, in run order */
    size_t blob_stored_size;
    size_t blob_size;
    uint32_t blob_crc32;
} SotbPromptSet;

extern const SotbPromptSet sotb_prompt_sets[];
extern const size_t sotb_prompt_set_count;

const SotbPromptSet *sotb_prompt_set_for_build(const char *build_key);

typedef struct {
    uint64_t offset, size;
    const void *data;
} SotbPromptPatch;

typedef struct {
    int (*read)(void *user, uint64_t offset, void *buffer, size_t length); /* 0 when all bytes were read */
    uint64_t file_size;
    void *user;
} SotbPromptSource;

/* Registers the replacement of `length` bytes of the guest file: 0, or -1 with nothing registered. The table is copied;
 * the data it points to stays valid for the life of the process. */
typedef int (*SotbPromptOverlay)(const char *guest_path, uint64_t length, const SotbPromptPatch *patches, size_t count);

typedef enum {
    SOTB_PROMPTS_OK,
    SOTB_PROMPTS_UNSUPPORTED_BUILD,
    SOTB_PROMPTS_FILE_SIZE,
    SOTB_PROMPTS_READ_FAILED,
    SOTB_PROMPTS_FOOTER,
    SOTB_PROMPTS_INDEX,
    SOTB_PROMPTS_TABLE,
    SOTB_PROMPTS_ASSET_BOUNDS,
    SOTB_PROMPTS_ASSET_CRC,
    SOTB_PROMPTS_REPLACEMENT,
    SOTB_PROMPTS_MEMORY,
    SOTB_PROMPTS_OVERLAY_REFUSED
} SotbPromptStatus;

typedef struct {
    SotbPromptStatus status;
    const char *asset;          /* the entry the status is about, when it is about one */
    size_t resources;
    size_t patches;
} SotbPromptResult;

SotbPromptResult sotb_prompts_install(const SotbPromptSet *set, const SotbPromptSource *source, SotbPromptOverlay overlay);
const char *sotb_prompts_status_text(SotbPromptStatus status);

uint32_t sotb_prompts_crc32(const void *data, size_t size);
/* Raw deflate (RFC 1951) into exactly `out_size` bytes; 0 on success. */
int sotb_prompts_inflate(const unsigned char *in, size_t in_size, unsigned char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif

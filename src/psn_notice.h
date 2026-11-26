#ifndef PSN_NOTICE_H
#define PSN_NOTICE_H
#include "guest_patches.h"
#include "patch_sites.h"
#include <stddef.h>
#include <stdint.h>
#include <uchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSN_NOTICE_HOOK_SITE 0x5861a0
#define PSN_NOTICE_HOOK_PATCH_SIZE 6
#define PSN_NOTICE_MIN_CAVE_SIZE 128

/* Layout of the engine's FString (TArray<TCHAR>): characters including the terminator. */
typedef struct {
    const char16_t *data;
    int32_t num;
    int32_t max;
} PsnNoticeString;

int GUEST_ABI psn_notice_matches(const PsnNoticeString *ns, const PsnNoticeString *key);

/* Makes the successful PlayStation Network connection text a single space by redirecting its localization lookup to
 * an unlocalized key. Everything is checked first; on false the image is untouched and `error` says why. */
int psn_notice_apply(uint8_t *image, size_t image_size, uint8_t *cave, size_t cave_size, char *error,
                     size_t error_size);

/* The image checks psn_notice_apply makes, without writing. */
int psn_notice_check(const uint8_t *image, size_t image_size, char *error, size_t error_size);
size_t psn_notice_sites(const SotbPatchSite **sites);

#ifdef __cplusplus
}
#endif

#endif

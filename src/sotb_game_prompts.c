#include "sotb_game_prompts.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sotb_keys.h"

static void read_mode(const char *key, const char *value, void *user) {
    if (!strcmp(key, SOTB_GAME_PROMPTS_KEY)) *(SotbPromptMode *)user = sotb_prompt_mode_from_name(value);
}

SotbPromptMode sotb_game_prompts_mode(const char *settings_file) {
    SotbPromptMode mode = SOTB_PROMPT_AUTO;
    sotb_ini_read(settings_file, read_mode, &mode);
    return mode;
}

typedef struct {
    FILE *file;
} PakFile;

static int read_pak(void *user, uint64_t offset, void *buffer, size_t length) {
    FILE *file = ((PakFile *)user)->file;
#ifdef _WIN32
    if (_fseeki64(file, (long long)offset, SEEK_SET)) return -1;
#else
    if (fseeko(file, (off_t)offset, SEEK_SET)) return -1;
#endif
    return fread(buffer, 1, length, file) == length ? 0 : -1;
}

static uint64_t pak_size(FILE *file) {
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END)) return 0;
    const long long size = _ftelli64(file);
#else
    if (fseeko(file, 0, SEEK_END)) return 0;
    const long long size = (long long)ftello(file);
#endif
    return size > 0 ? (uint64_t)size : 0;
}

static void say(const SotbGamePromptsRequest *request, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void say(const SotbGamePromptsRequest *request, const char *format, ...) {
    if (!request->log) return;
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    request->log(request->log_user, line);
}

SotbGamePromptsOutcome sotb_game_prompts_apply(const SotbGamePromptsRequest *request) {
    SotbGamePromptsOutcome outcome = {SOTB_PROMPT_AUTO, SOTB_PAD_KIND_UNKNOWN, SOTB_PROMPT_FAMILY_PLAYSTATION, 0, SOTB_PROMPTS_OK};
    outcome.requested = sotb_game_prompts_mode(request->settings_file);
    if (outcome.requested == SOTB_PROMPT_AUTO && request->probe_startup_kind) outcome.startup_kind = request->probe_startup_kind();
    outcome.effective = sotb_prompt_family(outcome.requested, outcome.startup_kind);
    say(request, "Controller prompts: requested=%s, startup controller=%s, effective=%s, build=%s",
        sotb_prompt_mode_name(outcome.requested), sotb_pad_kind_name(outcome.startup_kind),
        outcome.effective == SOTB_PROMPT_FAMILY_XBOX ? "xbox" : "playstation", request->build_key ? request->build_key : "unknown");
    if (outcome.effective != SOTB_PROMPT_FAMILY_XBOX) {
        say(request, "Controller prompts: PlayStation originals kept, no overlay");
        return outcome;
    }
    const SotbPromptSet *set = request->set ? request->set : sotb_prompt_set_for_build(request->build_key);
    if (!set) {
        outcome.status = SOTB_PROMPTS_UNSUPPORTED_BUILD;
        say(request, "Controller prompts: no Xbox art for build %s, using PlayStation original", request->build_key ? request->build_key : "unknown");
        return outcome;
    }
    FILE *file = request->pak_path ? fopen(request->pak_path, "rb") : NULL;
    if (!file) {
        outcome.status = SOTB_PROMPTS_READ_FAILED;
        say(request, "Controller prompts: %s could not be opened, using PlayStation original", strrchr(set->guest_path, '/') ? strrchr(set->guest_path, '/') + 1 : set->guest_path);
        return outcome;
    }
    PakFile pak = {file};
    const SotbPromptSource source = {read_pak, pak_size(file), &pak};
    const SotbPromptResult result = sotb_prompts_install(set, &source, request->overlay);
    fclose(file);
    outcome.status = result.status;
    if (result.status == SOTB_PROMPTS_OK) {
        outcome.installed = 1;
        say(request, "Controller prompts: Xbox overlay installed, %zu resources, %zu patches", result.resources, result.patches);
    } else if (result.asset) {
        say(request, "Controller prompts: unsupported asset %s (%s), using PlayStation original", result.asset, sotb_prompts_status_text(result.status));
    } else {
        say(request, "Controller prompts: Xbox art refused (%s), using PlayStation original", sotb_prompts_status_text(result.status));
    }
    return outcome;
}

/* The game's own button icons follow the Controller prompts setting: decided once, before the guest starts and before
 * any asset is read. PlayStation keeps the game's art untouched; Xbox serves the Xbox art over the pak. A failure keeps
 * the PlayStation art, which the game ships and always works. */
#ifndef SOTB_GAME_PROMPTS_H
#define SOTB_GAME_PROMPTS_H

#include "sotb_pad_prompts.h"
#include "sotb_prompt_assets.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOTB_GAME_PROMPTS_KEY "controller_prompts"

typedef struct {
    const char *settings_file;
    const char *pak_path;       /* the host file behind SotbPromptSet.guest_path */
    const char *build_key;      /* e.g. CUSA03762-1.00 */
    const SotbPromptSet *set;   /* NULL looks the build up in the generated table */
    /* The kind of the first connected controller. Called only when the mode is Auto; NULL stands for none. */
    SotbPadKind (*probe_startup_kind)(void);
    SotbPromptOverlay overlay;
    void (*log)(void *user, const char *line);
    void *log_user;
} SotbGamePromptsRequest;

typedef struct {
    SotbPromptMode requested;
    SotbPadKind startup_kind;
    SotbPromptFamily effective;
    int installed;              /* the Xbox art is served */
    SotbPromptStatus status;
} SotbGamePromptsOutcome;

SotbPromptMode sotb_game_prompts_mode(const char *settings_file);
SotbGamePromptsOutcome sotb_game_prompts_apply(const SotbGamePromptsRequest *request);

#ifdef __cplusplus
}
#endif

#endif

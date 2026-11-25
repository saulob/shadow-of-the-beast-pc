// The launcher, called by the loader before the guest and the game window exist.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*SotbGraphicsCheck)(char* cause, size_t cause_size, char* advice, size_t advice_size);

typedef struct {
    const char* app0;
    const char* image_dir;
    const char* user_dir;
    const char* settings_file;
    const char* logs_dir;
    int skip_once;
    int force_show;
    SotbGraphicsCheck check_graphics;
    const char* log_file;
} SotbLauncherConfig;

typedef struct {
    const char* boot;
    const char* content_profile;
    const char* app0;
    const char* user_dir;
    const char* logs_dir;
} SotbPathRequest;

typedef struct {
    char boot[4096];
    char content_profile[4096];
    char app0[4096];
    char image_dir[4096];
    char user_dir[4096];
    char logs_dir[4096];
    char error[512];
} SotbPaths;

int sotb_resolve_paths(const SotbPathRequest* request, SotbPaths* paths);
int sotb_logs_directory(const char* logs_dir, const char* log_file, char* out, size_t size);
int sotb_build_boot_image(const char* app0, const char* out, char* error, size_t error_size);

typedef enum {
    SOTB_LAUNCHER_PLAY = 0,
    SOTB_LAUNCHER_QUIT = 1,
    SOTB_LAUNCHER_BLOCKED = 2,
} SotbLauncherResult;

SotbLauncherResult sotb_launcher_run(const SotbLauncherConfig* config);

typedef struct {
    const char* label;
    const char* title_id;
    const char* version;
    const char* code_key;
    const char* region;
    int original_circle_confirm;
} SotbGameBuild;

int sotb_game_build(const char* image_dir, SotbGameBuild* build);
void sotb_game_build_problem(const char* image_dir, char* out, size_t size);
/* The deterministic build verdict and the fields behind it, one call of `emit` per log line; read-only. */
void sotb_game_build_report(const char* image_dir, void (*emit)(const char* line));

int sotb_text_language(const char* settings_file);
const char* sotb_text_language_key(const char* settings_file);
int sotb_detailed_logging(const char* settings_file);
int sotb_render_resolution(const char* settings_file);
int sotb_skip_intro(const char* settings_file);
void sotb_log_settings_summary(const char* settings_file, const char* game_build);

int sotb_check_vulkan(char* cause, size_t cause_size, char* advice, size_t advice_size);
/* 1 when the saved Renderer can run: only Vulkan chosen by name needs a working Vulkan. Otherwise fills cause and advice. */
int sotb_check_renderer(const char* settings_file, char* cause, size_t cause_size, char* advice, size_t advice_size);

#ifdef __cplusplus
}
#endif

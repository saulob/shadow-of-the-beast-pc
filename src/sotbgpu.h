/* Interface between the loader/runtime and the video core (shadPS4-derived, gpu/shim/sotbgpu.cpp). */
#ifndef SOTBGPU_H
#define SOTBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;
    const char *build;
    const char *serial;
    const char *user_dir;
    const char *settings_file;
    uint32_t sdk_version;
    uint32_t psf_attributes;
    int32_t width, height;
    uint32_t features;
} SotbGpuConfig;
#define SOTB_WINDOW_TITLE "Shadow of the Beast"
#define SOTB_GPU_FEATURE_RENDER_RESOLUTION (1u << 0)
int sotbgpu_available(void);
void sotbgpu_register_kernel(void);
int sotbgpu_init(const SotbGpuConfig *config);
uintptr_t sotbgpu_resolve(const char *scoped_nid);
int sotbgpu_handle_fault(void *context, void *address);
void sotbgpu_dump_guest_writes(void *context);
unsigned sotbgpu_symbol_count(void);
int sotbgpu_request_close(void);
int sotbgpu_exit_confirm_begin(int original_layout, int xbox_prompts, const char *locale);
void sotbgpu_exit_confirm_set_save_check(int (*save_active)(void));
int sotbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int sotbgpu_text_input_poll(char *out_utf8, uint64_t size);
int sotbgpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int sotbgpu_choice_poll(void);
void sotbgpu_choice_set_input(uint32_t (*buttons)(void));
int sotbgpu_overlay_captures_input(void);
int sotbgpu_controller_rumble(void);
int sotbgpu_latency_trace_enabled(void);
uint64_t sotbgpu_latency_press_ns(void);
/* The loading screen is being drawn in the central 16:9 part: the sides show a blurred copy of it for a short time. */
void sotbgpu_mark_loading_fill(void);
/* The open frontend screen: 1 the title, 2 the map, anything else none. Their sides get the vignette for exactly that long. */
void sotbgpu_set_frontend_fill(int context);
#ifdef __cplusplus
}
#endif
#endif

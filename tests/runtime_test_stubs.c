#include "sotbgpu.h"

int sotbgpu_available(void) { return 0; }
uintptr_t sotbgpu_resolve(const char *scoped_nid) { (void)scoped_nid; return 0; }
int sotbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8) { (void)initial_utf8; (void)prompt_utf8; return -1; }
int sotbgpu_text_input_poll(char *out_utf8, uint64_t size) { (void)out_utf8; (void)size; return -1; }
int sotbgpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus) {
    (void)title; (void)items; (void)images; (void)count; (void)focus;
    return -1;
}
int sotbgpu_choice_poll(void) { return -1; }
void sotbgpu_choice_set_input(uint32_t (*buttons)(void)) { (void)buttons; }
int sotbgpu_overlay_captures_input(void) { return 0; }
int sotbgpu_controller_rumble(void) { return 1; }
int sotbgpu_latency_trace_enabled(void) { return 0; }
uint64_t sotbgpu_latency_press_ns(void) { return 0; }

uintptr_t runtime_guest_offset(uintptr_t address) { (void)address; return UINTPTR_MAX; }

int32_t trophy_runtime_create_context(int32_t *context, int32_t user, uint32_t label, uint64_t options) { (void)context; (void)user; (void)label; (void)options; return (int32_t)0x80551601; }
int32_t trophy_runtime_create_handle(int32_t *handle) { (void)handle; return (int32_t)0x80551601; }
int32_t trophy_runtime_register_context(int32_t context, int32_t handle, uint64_t options) { (void)context; (void)handle; (void)options; return (int32_t)0x80551601; }
int32_t trophy_runtime_destroy_handle(int32_t handle) { (void)handle; return 0; }
int32_t trophy_runtime_destroy_context(int32_t context) { (void)context; return 0; }
int32_t trophy_runtime_unlock(int32_t context, int32_t handle, int32_t trophy_id, int32_t *platinum_id) { (void)context; (void)handle; (void)trophy_id; (void)platinum_id; return (int32_t)0x80551601; }
int32_t trophy_runtime_game_info(int32_t context, int32_t handle, void *details, void *data) { (void)context; (void)handle; (void)details; (void)data; return (int32_t)0x80551601; }
int32_t trophy_runtime_trophy_info(int32_t context, int32_t handle, int32_t trophy_id, void *details, void *data) { (void)context; (void)handle; (void)trophy_id; (void)details; (void)data; return (int32_t)0x80551601; }
int32_t trophy_runtime_unlock_state(int32_t context, int32_t handle, uint32_t *flags, uint32_t *count) { (void)context; (void)handle; (void)flags; (void)count; return (int32_t)0x80551601; }

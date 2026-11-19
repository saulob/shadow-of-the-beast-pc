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
int sotbgpu_latency_trace_enabled(void) { return 0; }
uint64_t sotbgpu_latency_press_ns(void) { return 0; }

uintptr_t runtime_guest_offset(uintptr_t address) { (void)address; return UINTPTR_MAX; }

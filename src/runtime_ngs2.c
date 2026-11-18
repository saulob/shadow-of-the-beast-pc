// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "runtime_ngs2.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HANDLE_SYSTEM = 1, HANDLE_RACK = 2, HANDLE_VOICE = 3 };
enum { STATE_ALIGNMENT = 8 };

typedef struct {
    uintptr_t self;
    void *owner;
    int32_t ref_count;
    uint32_t type, id;
} Head;

typedef struct {
    Head head;
    uint32_t index, state_flags;
} Voice;

typedef struct Rack Rack;
typedef struct System System;

struct Rack {
    Head head;
    Ngs2ContextBufferInfo buffer_info;
    Ngs2BufferFreeHandler host_free;
    Rack *next;
    uint32_t rack_id, max_voices;
    Voice voices[];
};

struct System {
    Head head;
    Ngs2ContextBufferInfo buffer_info;
    Ngs2BufferFreeHandler host_free;
    System *next;
    Rack *racks;
    uint32_t sample_rate, max_grain_samples, num_grain_samples, rack_count;
    uint64_t render_count;
};

typedef struct {
    uint32_t sample_rate, max_grain_samples, num_grain_samples;
} SystemSetup;

typedef struct {
    Ngs2BufferFreeHandler host_free;
    Ngs2ContextBufferInfo buffer_info;
} Release;

_Static_assert(sizeof(Head) == 32, "Ngs2 handle header layout");
_Static_assert(sizeof(System) % STATE_ALIGNMENT == 0 && sizeof(Voice) % STATE_ALIGNMENT == 0, "Ngs2 state alignment");

enum {
    API_SYSTEM_CREATE,
    API_SYSTEM_DESTROY,
    API_SYSTEM_RENDER,
    API_RACK_CREATE,
    API_RACK_DESTROY,
    API_RACK_VOICE_HANDLE,
    API_VOICE_CONTROL,
    API_VOICE_STATE,
    API_VOICE_STATE_FLAGS,
    API_PAN_INIT,
    API_PAN_MATRIX,
    API_PARSE_WAVEFORM,
    API_COUNT
};

static const char *const api_names[API_COUNT] = {
    "sceNgs2SystemCreateWithAllocator", "sceNgs2SystemDestroy", "sceNgs2SystemRender",
    "sceNgs2RackCreateWithAllocator", "sceNgs2RackDestroy", "sceNgs2RackGetVoiceHandle",
    "sceNgs2VoiceControl", "sceNgs2VoiceGetState", "sceNgs2VoiceGetStateFlags",
    "sceNgs2PanInit", "sceNgs2PanGetVolumeMatrix", "sceNgs2ParseWaveformData",
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static System *systems;
static uint32_t next_id = 1;
static _Atomic unsigned module_refs;
static _Atomic int first_call_seen[API_COUNT];
static _Atomic size_t systems_created, systems_destroyed, racks_created, racks_destroyed, voices_obtained;
static _Atomic size_t voice_controls, voice_state_queries, renders, pan_inits, pan_matrices, waveforms_rejected;

void runtime_ngs2_module_load(void) {
    if (!atomic_fetch_add(&module_refs, 1)) puts("Runtime: libSceNgs2 provided by the host runtime (system module 0xb)");
}

void runtime_ngs2_module_unload(void) {
    unsigned refs = atomic_load(&module_refs);
    while (refs && !atomic_compare_exchange_weak(&module_refs, &refs, refs - 1)) {}
}

static void enter(int api) {
    if (!atomic_load(&module_refs)) {
        fprintf(stderr, "STOP: %s called before system module 0xb was loaded\n", api_names[api]);
        exit(21);
    }
    if (!atomic_exchange(&first_call_seen[api], 1)) printf("Runtime: Ngs2 %s first call\n", api_names[api]);
}

static int valid_grain_samples(uint32_t samples) {
    return samples >= 64 && samples <= 1024 && !(samples & 63);
}

static int valid_sample_rate(uint32_t rate) {
    static const uint32_t rates[] = {11025, 12000, 22050, 24000, 44100, 48000, 88200, 96000, 176400, 192000};
    for (size_t i = 0; i < sizeof(rates) / sizeof(*rates); ++i) {
        if (rates[i] == rate) return 1;
    }
    return 0;
}

static int32_t system_validate(const Ngs2SystemOption *option, SystemSetup *setup) {
    *setup = (SystemSetup){48000, 512, 256};
    if (option) {
        if (option->size != sizeof(*option)) return NGS2_ERROR_INVALID_OPTION_SIZE;
        *setup = (SystemSetup){option->sample_rate, option->max_grain_samples, option->num_grain_samples};
    }
    if (!valid_grain_samples(setup->max_grain_samples)) return NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES;
    if (!valid_grain_samples(setup->num_grain_samples)) return NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
    if (!valid_sample_rate(setup->sample_rate)) return NGS2_ERROR_INVALID_SAMPLE_RATE;
    return 0;
}

static size_t system_buffer_size(void) { return sizeof(System) + STATE_ALIGNMENT; }

static size_t rack_buffer_size(uint32_t max_voices) {
    return sizeof(Rack) + (size_t)max_voices * sizeof(Voice) + STATE_ALIGNMENT;
}

static void *aligned_state(const Ngs2ContextBufferInfo *info) {
    return (void *)(((uintptr_t)info->host_buffer + STATE_ALIGNMENT - 1) & ~(uintptr_t)(STATE_ALIGNMENT - 1));
}

static System *find_system(Ngs2Handle handle) {
    for (System *s = systems; s; s = s->next) {
        if ((Ngs2Handle)s == handle) return s;
    }
    return NULL;
}

static Rack *find_rack(Ngs2Handle handle, System **owner) {
    for (System *s = systems; s; s = s->next) {
        for (Rack *r = s->racks; r; r = r->next) {
            if ((Ngs2Handle)r != handle) continue;
            if (owner) *owner = s;
            return r;
        }
    }
    return NULL;
}

static Voice *find_voice(Ngs2Handle handle) {
    for (System *s = systems; s; s = s->next) {
        for (Rack *r = s->racks; r; r = r->next) {
            const Ngs2Handle first = (Ngs2Handle)r->voices;
            const size_t bytes = (size_t)r->max_voices * sizeof(Voice);
            if (handle >= first && handle - first < bytes && (handle - first) % sizeof(Voice) == 0) {
                return (Voice *)handle;
            }
        }
    }
    return NULL;
}

static void init_head(Head *head, void *owner, uint32_t type) {
    head->self = (uintptr_t)head;
    head->owner = owner;
    head->ref_count = 1;
    head->type = type;
    head->id = next_id++;
}

static void release_buffer(const Release *release, Ngs2ContextBufferInfo *out_info) {
    Ngs2ContextBufferInfo info = release->buffer_info;
    if (out_info) *out_info = info;
    if (release->host_free) release->host_free(&info);
}

static int32_t system_setup(const SystemSetup *setup, const Ngs2ContextBufferInfo *info,
                            Ngs2BufferFreeHandler host_free, Ngs2Handle *out_handle) {
    if (!info->host_buffer) return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    if (info->host_buffer_size < system_buffer_size()) return NGS2_ERROR_INVALID_BUFFER_SIZE;
    System *system = aligned_state(info);
    memset(system, 0, sizeof(*system));
    system->buffer_info = *info;
    system->host_free = host_free;
    system->sample_rate = setup->sample_rate;
    system->max_grain_samples = setup->max_grain_samples;
    system->num_grain_samples = setup->num_grain_samples;
    pthread_mutex_lock(&lock);
    init_head(&system->head, system, HANDLE_SYSTEM);
    system->next = systems;
    systems = system;
    pthread_mutex_unlock(&lock);
    *out_handle = (Ngs2Handle)system;
    return 0;
}

static ABI int32_t system_create_with_allocator(const Ngs2SystemOption *option, const Ngs2BufferAllocator *allocator,
                                                Ngs2Handle *out_handle) {
    enter(API_SYSTEM_CREATE);
    if (!allocator || !allocator->alloc_handler) return NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
    if (!out_handle) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    SystemSetup setup;
    int32_t result = system_validate(option, &setup);
    if (result < 0) return result;
    Ngs2ContextBufferInfo info = {0};
    info.host_buffer_size = system_buffer_size();
    info.user_data = allocator->user_data;
    result = allocator->alloc_handler(&info);
    if (result < 0) return result;
    result = system_setup(&setup, &info, allocator->free_handler, out_handle);
    if (result < 0) {
        if (allocator->free_handler) allocator->free_handler(&info);
        return result;
    }
    atomic_fetch_add(&systems_created, 1);
    return 0;
}

static ABI int32_t system_destroy(Ngs2Handle system_handle, Ngs2ContextBufferInfo *out_buffer_info) {
    enter(API_SYSTEM_DESTROY);
    pthread_mutex_lock(&lock);
    System *system = find_system(system_handle);
    if (!system) {
        pthread_mutex_unlock(&lock);
        return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    }
    for (System **link = &systems; *link; link = &(*link)->next) {
        if (*link != system) continue;
        *link = system->next;
        break;
    }
    while (system->racks) {
        Rack *rack = system->racks;
        system->racks = rack->next;
        const Release release = {rack->host_free, rack->buffer_info};
        pthread_mutex_unlock(&lock);
        release_buffer(&release, NULL);
        atomic_fetch_add(&racks_destroyed, 1);
        pthread_mutex_lock(&lock);
    }
    const Release release = {system->host_free, system->buffer_info};
    pthread_mutex_unlock(&lock);
    release_buffer(&release, out_buffer_info);
    atomic_fetch_add(&systems_destroyed, 1);
    return 0;
}

static ABI int32_t system_render(Ngs2Handle system_handle, const Ngs2RenderBufferInfo *buffer_infos,
                                 uint32_t num_buffer_infos) {
    enter(API_SYSTEM_RENDER);
    pthread_mutex_lock(&lock);
    System *system = find_system(system_handle);
    if (system) ++system->render_count;
    pthread_mutex_unlock(&lock);
    if (!system) return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if (num_buffer_infos && !buffer_infos) return NGS2_ERROR_INVALID_BUFFER_INFO;
    for (uint32_t i = 0; i < num_buffer_infos; ++i) {
        if (buffer_infos[i].buffer && buffer_infos[i].buffer_size) {
            memset(buffer_infos[i].buffer, 0, buffer_infos[i].buffer_size);
        }
    }
    atomic_fetch_add(&renders, 1);
    return 0;
}

static ABI int32_t rack_create_with_allocator(Ngs2Handle system_handle, uint32_t rack_id,
                                              const Ngs2RackOption *option, const Ngs2BufferAllocator *allocator,
                                              Ngs2Handle *out_handle) {
    enter(API_RACK_CREATE);
    if (!system_handle) return NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    if (!allocator || !allocator->alloc_handler) return NGS2_ERROR_INVALID_BUFFER_ALLOCATOR;
    if (!out_handle) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (!option) return NGS2_ERROR_INVALID_OPTION_ADDRESS;
    if (option->size < sizeof(*option)) return NGS2_ERROR_INVALID_OPTION_SIZE;
    pthread_mutex_lock(&lock);
    const int known = find_system(system_handle) != NULL;
    pthread_mutex_unlock(&lock);
    if (!known) return NGS2_ERROR_INVALID_SYSTEM_HANDLE;

    const uint32_t max_voices = option->max_voices;
    Ngs2ContextBufferInfo info = {0};
    info.host_buffer_size = rack_buffer_size(max_voices);
    info.user_data = allocator->user_data;
    int32_t result = allocator->alloc_handler(&info);
    if (result < 0) return result;

    pthread_mutex_lock(&lock);
    System *system = find_system(system_handle);
    if (!system) result = NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    else if (!info.host_buffer) result = NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    else if (info.host_buffer_size < rack_buffer_size(max_voices)) result = NGS2_ERROR_INVALID_BUFFER_SIZE;
    if (result < 0) {
        pthread_mutex_unlock(&lock);
        if (allocator->free_handler) allocator->free_handler(&info);
        return result;
    }
    Rack *rack = aligned_state(&info);
    memset(rack, 0, sizeof(*rack) + (size_t)max_voices * sizeof(Voice));
    rack->buffer_info = info;
    rack->host_free = allocator->free_handler;
    rack->rack_id = rack_id;
    rack->max_voices = max_voices;
    init_head(&rack->head, system, HANDLE_RACK);
    for (uint32_t i = 0; i < max_voices; ++i) {
        init_head(&rack->voices[i].head, rack, HANDLE_VOICE);
        rack->voices[i].index = i;
    }
    rack->next = system->racks;
    system->racks = rack;
    ++system->rack_count;
    pthread_mutex_unlock(&lock);
    *out_handle = (Ngs2Handle)rack;
    atomic_fetch_add(&racks_created, 1);
    return 0;
}

static ABI int32_t rack_destroy(Ngs2Handle rack_handle, Ngs2ContextBufferInfo *out_buffer_info) {
    enter(API_RACK_DESTROY);
    pthread_mutex_lock(&lock);
    System *system = NULL;
    Rack *rack = find_rack(rack_handle, &system);
    if (!rack) {
        pthread_mutex_unlock(&lock);
        return NGS2_ERROR_INVALID_RACK_HANDLE;
    }
    for (Rack **link = &system->racks; *link; link = &(*link)->next) {
        if (*link != rack) continue;
        *link = rack->next;
        break;
    }
    --system->rack_count;
    const Release release = {rack->host_free, rack->buffer_info};
    pthread_mutex_unlock(&lock);
    release_buffer(&release, out_buffer_info);
    atomic_fetch_add(&racks_destroyed, 1);
    return 0;
}

static ABI int32_t rack_get_voice_handle(Ngs2Handle rack_handle, uint32_t voice_index, Ngs2Handle *out_handle) {
    enter(API_RACK_VOICE_HANDLE);
    pthread_mutex_lock(&lock);
    Rack *rack = find_rack(rack_handle, NULL);
    int32_t result = !rack ? NGS2_ERROR_INVALID_RACK_HANDLE
                     : !out_handle ? NGS2_ERROR_INVALID_OUT_ADDRESS
                     : voice_index >= rack->max_voices ? NGS2_ERROR_INVALID_VOICE_INDEX : 0;
    if (!result) *out_handle = (Ngs2Handle)&rack->voices[voice_index];
    pthread_mutex_unlock(&lock);
    if (!result) atomic_fetch_add(&voices_obtained, 1);
    return result;
}

static ABI int32_t voice_control(Ngs2Handle voice_handle, const Ngs2VoiceParamHeader *param_list) {
    enter(API_VOICE_CONTROL);
    pthread_mutex_lock(&lock);
    const int known = find_voice(voice_handle) != NULL;
    pthread_mutex_unlock(&lock);
    if (!known) return NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (!param_list) return NGS2_ERROR_INVALID_VOICE_CONTROL_ADDRESS;
    atomic_fetch_add(&voice_controls, 1);
    return 0;
}

static ABI int32_t voice_get_state(Ngs2Handle voice_handle, void *out_state, size_t state_size) {
    enter(API_VOICE_STATE);
    pthread_mutex_lock(&lock);
    Voice *voice = find_voice(voice_handle);
    const uint32_t flags = voice ? voice->state_flags : 0;
    pthread_mutex_unlock(&lock);
    if (!voice) return NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (!out_state) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (state_size < sizeof(Ngs2VoiceState)) return NGS2_ERROR_INVALID_OUT_SIZE;
    memset(out_state, 0, state_size);
    memcpy(out_state, &flags, sizeof(flags));
    atomic_fetch_add(&voice_state_queries, 1);
    return 0;
}

static ABI int32_t voice_get_state_flags(Ngs2Handle voice_handle, uint32_t *out_state_flags) {
    enter(API_VOICE_STATE_FLAGS);
    pthread_mutex_lock(&lock);
    Voice *voice = find_voice(voice_handle);
    const uint32_t flags = voice ? voice->state_flags : 0;
    pthread_mutex_unlock(&lock);
    if (!voice) return NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (!out_state_flags) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    *out_state_flags = flags;
    atomic_fetch_add(&voice_state_queries, 1);
    return 0;
}

static ABI int32_t pan_init(Ngs2PanWork *work, const float *speaker_angle, float unit_angle, uint32_t num_speakers) {
    (void)speaker_angle;
    (void)unit_angle;
    (void)num_speakers;
    enter(API_PAN_INIT);
    if (!work) return NGS2_ERROR_INVALID_PAN_WORK;
    atomic_fetch_add(&pan_inits, 1);
    return 0;
}

static ABI int32_t pan_get_volume_matrix(Ngs2PanWork *work, const Ngs2PanParam *params, uint32_t num_params,
                                         uint32_t matrix_format, float *out_volume_matrix) {
    (void)matrix_format;
    enter(API_PAN_MATRIX);
    if (!work) return NGS2_ERROR_INVALID_PAN_WORK;
    if (num_params && !params) return NGS2_ERROR_INVALID_PAN_PARAM;
    if (!out_volume_matrix) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    atomic_fetch_add(&pan_matrices, 1);
    return 0;
}

static ABI int32_t parse_waveform_data(const void *data, size_t data_size, void *out_info) {
    enter(API_PARSE_WAVEFORM);
    if (!data) return NGS2_ERROR_INVALID_WAVEFORM_ADDRESS;
    if (!data_size) return NGS2_ERROR_INVALID_WAVEFORM_SIZE;
    if (!out_info) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    atomic_fetch_add(&waveforms_rejected, 1);
    return NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT;
}

static const RuntimeExport exports[] = {
    {"sceNgs2SystemCreateWithAllocator", system_create_with_allocator},
    {"sceNgs2SystemDestroy", system_destroy},
    {"sceNgs2SystemRender", system_render},
    {"sceNgs2RackCreateWithAllocator", rack_create_with_allocator},
    {"sceNgs2RackDestroy", rack_destroy},
    {"sceNgs2RackGetVoiceHandle", rack_get_voice_handle},
    {"sceNgs2VoiceControl", voice_control},
    {"sceNgs2VoiceGetState", voice_get_state},
    {"sceNgs2VoiceGetStateFlags", voice_get_state_flags},
    {"sceNgs2PanInit", pan_init},
    {"sceNgs2PanGetVolumeMatrix", pan_get_volume_matrix},
    {"sceNgs2ParseWaveformData", parse_waveform_data},
};

uintptr_t runtime_ngs2_resolve(const char *name) {
    return strstr(name, "#libSceNgs2") ? RUNTIME_LOOKUP(exports, name) : 0;
}

void runtime_ngs2_report(void) {
    printf("Runtime: Ngs2 systems=%zu/%zu, racks=%zu/%zu (created/destroyed), voices obtained=%zu, voice control=%zu, "
           "voice state=%zu, renders=%zu, pan init=%zu, pan matrix=%zu, waveform parses refused=%zu\n",
           (size_t)systems_created, (size_t)systems_destroyed, (size_t)racks_created, (size_t)racks_destroyed,
           (size_t)voices_obtained, (size_t)voice_controls, (size_t)voice_state_queries, (size_t)renders,
           (size_t)pan_inits, (size_t)pan_matrices, (size_t)waveforms_rejected);
}

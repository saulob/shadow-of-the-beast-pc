// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef SOTB_RUNTIME_NGS2_H
#define SOTB_RUNTIME_NGS2_H
#include <stddef.h>
#include <stdint.h>

#include "runtime.h"

#define NGS2_SYSTEM_NAME_LENGTH 16
#define NGS2_RACK_NAME_LENGTH 16
#define NGS2_MAX_VOICE_CHANNELS 8
#define NGS2_SYSMODULE_ID 0xb

#define NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES ((int32_t)0x804A0050)
#define NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES ((int32_t)0x804A0051)
#define NGS2_ERROR_INVALID_OUT_ADDRESS ((int32_t)0x804A0053)
#define NGS2_ERROR_INVALID_OUT_SIZE ((int32_t)0x804A0054)
#define NGS2_ERROR_INVALID_OPTION_ADDRESS ((int32_t)0x804A0080)
#define NGS2_ERROR_INVALID_OPTION_SIZE ((int32_t)0x804A0081)
#define NGS2_ERROR_INVALID_SAMPLE_RATE ((int32_t)0x804A0201)
#define NGS2_ERROR_INVALID_BUFFER_INFO ((int32_t)0x804A0206)
#define NGS2_ERROR_INVALID_BUFFER_ADDRESS ((int32_t)0x804A0207)
#define NGS2_ERROR_INVALID_BUFFER_SIZE ((int32_t)0x804A0209)
#define NGS2_ERROR_INVALID_BUFFER_ALLOCATOR ((int32_t)0x804A020A)
#define NGS2_ERROR_INVALID_SYSTEM_HANDLE ((int32_t)0x804A0230)
#define NGS2_ERROR_INVALID_RACK_HANDLE ((int32_t)0x804A0261)
#define NGS2_ERROR_INVALID_VOICE_HANDLE ((int32_t)0x804A0300)
#define NGS2_ERROR_INVALID_VOICE_INDEX ((int32_t)0x804A0302)
#define NGS2_ERROR_INVALID_VOICE_CONTROL_ADDRESS ((int32_t)0x804A0309)
#define NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT ((int32_t)0x804A0401)
#define NGS2_ERROR_INVALID_WAVEFORM_ADDRESS ((int32_t)0x804A0406)
#define NGS2_ERROR_INVALID_WAVEFORM_SIZE ((int32_t)0x804A0407)
#define NGS2_ERROR_INVALID_PAN_WORK ((int32_t)0x804A0913)
#define NGS2_ERROR_INVALID_PAN_PARAM ((int32_t)0x804A0914)

typedef uintptr_t Ngs2Handle;

typedef struct {
    void *host_buffer;
    size_t host_buffer_size;
    uintptr_t reserved[5];
    uintptr_t user_data;
} Ngs2ContextBufferInfo;

typedef int32_t (ABI *Ngs2BufferAllocHandler)(Ngs2ContextBufferInfo *io_buffer_info);
typedef int32_t (ABI *Ngs2BufferFreeHandler)(Ngs2ContextBufferInfo *io_buffer_info);

typedef struct {
    Ngs2BufferAllocHandler alloc_handler;
    Ngs2BufferFreeHandler free_handler;
    uintptr_t user_data;
} Ngs2BufferAllocator;

typedef struct {
    size_t size;
    char name[NGS2_SYSTEM_NAME_LENGTH];
    uint32_t flags;
    uint32_t max_grain_samples;
    uint32_t num_grain_samples;
    uint32_t sample_rate;
    uint32_t reserved[6];
} Ngs2SystemOption;

typedef struct {
    size_t size;
    char name[NGS2_RACK_NAME_LENGTH];
    uint32_t flags;
    uint32_t max_grain_samples;
    uint32_t max_voices;
    uint32_t max_input_delay_blocks;
    uint32_t max_matrices;
    uint32_t max_ports;
    uint32_t reserved[20];
} Ngs2RackOption;

typedef struct {
    uint16_t size;
    int16_t next;
    uint32_t id;
} Ngs2VoiceParamHeader;

typedef struct {
    uint32_t state_flags;
} Ngs2VoiceState;

typedef struct {
    void *buffer;
    size_t buffer_size;
    uint32_t waveform_type;
    uint32_t num_channels;
} Ngs2RenderBufferInfo;

typedef struct {
    float angle;
    float distance;
    float fbw_level;
    float lfe_level;
} Ngs2PanParam;

typedef struct {
    float speaker_angle[NGS2_MAX_VOICE_CHANNELS];
    float unit_angle;
    uint32_t num_speakers;
} Ngs2PanWork;

_Static_assert(sizeof(Ngs2ContextBufferInfo) == 64, "Ngs2 buffer info layout");
_Static_assert(sizeof(Ngs2BufferAllocator) == 24, "Ngs2 allocator layout");
_Static_assert(sizeof(Ngs2SystemOption) == 64, "Ngs2 system option layout");
_Static_assert(sizeof(Ngs2RackOption) == 128, "Ngs2 rack option layout");
_Static_assert(sizeof(Ngs2VoiceParamHeader) == 8, "Ngs2 voice parameter header layout");
_Static_assert(sizeof(Ngs2RenderBufferInfo) == 24, "Ngs2 render buffer layout");
_Static_assert(sizeof(Ngs2PanParam) == 16, "Ngs2 pan parameter layout");
_Static_assert(sizeof(Ngs2PanWork) == 40, "Ngs2 pan work layout");

uintptr_t runtime_ngs2_resolve(const char *name);
void runtime_ngs2_report(void);
void runtime_ngs2_module_load(void);
void runtime_ngs2_module_unload(void);
#endif

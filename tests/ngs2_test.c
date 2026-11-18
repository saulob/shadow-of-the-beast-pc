#include "runtime_ngs2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

static const struct { const char *nid, *symbol; } imports[] = {
#include "import_names.inc"
};

const char *runtime_symbol(const char *nid) {
    for (size_t i = 0; i < sizeof(imports) / sizeof(*imports); ++i) {
        if (!strcmp(nid, imports[i].nid)) return imports[i].symbol;
    }
    return NULL;
}

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    const char *symbol = runtime_symbol(nid);
    for (size_t i = 0; symbol && i < count; ++i) {
        if (!strcmp(symbol, table[i].name)) return (uintptr_t)table[i].function;
    }
    return 0;
}

static const struct { const char *nid, *symbol; } ngs2_imports[] = {
    {"hyVLT2VlOYk#libSceNgs2", "sceNgs2ParseWaveformData"},
    {"-TOuuAQ-buE#libSceNgs2", "sceNgs2VoiceGetState"},
    {"uu94irFOGpA#libSceNgs2", "sceNgs2VoiceControl"},
    {"mPYgU4oYpuY#libSceNgs2", "sceNgs2SystemCreateWithAllocator"},
    {"U546k6orxQo#libSceNgs2", "sceNgs2RackCreateWithAllocator"},
    {"MwmHz8pAdAo#libSceNgs2", "sceNgs2RackGetVoiceHandle"},
    {"lCqD7oycmIM#libSceNgs2", "sceNgs2RackDestroy"},
    {"u-WrYDaJA3k#libSceNgs2", "sceNgs2SystemDestroy"},
    {"i0VnXM-C9fc#libSceNgs2", "sceNgs2SystemRender"},
    {"xa8oL9dmXkM#libSceNgs2", "sceNgs2PanInit"},
    {"gbMKV+8Enuo#libSceNgs2", "sceNgs2PanGetVolumeMatrix"},
    {"rEh728kXk3w#libSceNgs2", "sceNgs2VoiceGetStateFlags"},
};

typedef int32_t (ABI *SystemCreate)(const Ngs2SystemOption *, const Ngs2BufferAllocator *, Ngs2Handle *);
typedef int32_t (ABI *SystemDestroy)(Ngs2Handle, Ngs2ContextBufferInfo *);
typedef int32_t (ABI *SystemRender)(Ngs2Handle, const Ngs2RenderBufferInfo *, uint32_t);
typedef int32_t (ABI *RackCreate)(Ngs2Handle, uint32_t, const Ngs2RackOption *, const Ngs2BufferAllocator *,
                                  Ngs2Handle *);
typedef int32_t (ABI *RackDestroy)(Ngs2Handle, Ngs2ContextBufferInfo *);
typedef int32_t (ABI *RackVoice)(Ngs2Handle, uint32_t, Ngs2Handle *);
typedef int32_t (ABI *VoiceControl)(Ngs2Handle, const Ngs2VoiceParamHeader *);
typedef int32_t (ABI *VoiceState)(Ngs2Handle, void *, size_t);
typedef int32_t (ABI *VoiceFlags)(Ngs2Handle, uint32_t *);
typedef int32_t (ABI *PanInit)(Ngs2PanWork *, const float *, float, uint32_t);
typedef int32_t (ABI *PanMatrix)(Ngs2PanWork *, const Ngs2PanParam *, uint32_t, uint32_t, float *);
typedef int32_t (ABI *ParseWaveform)(const void *, size_t, void *);

static void *api(const char *symbol) {
    for (size_t i = 0; i < sizeof(ngs2_imports) / sizeof(*ngs2_imports); ++i) {
        if (!strcmp(ngs2_imports[i].symbol, symbol)) return (void *)runtime_ngs2_resolve(ngs2_imports[i].nid);
    }
    return NULL;
}

static int allocations, frees;
static uintptr_t seen_user_data;
static size_t seen_size;
static void *last_freed;

static ABI int32_t alloc_handler(Ngs2ContextBufferInfo *info) {
    ++allocations;
    seen_user_data = info->user_data;
    seen_size = info->host_buffer_size;
    info->host_buffer = malloc(info->host_buffer_size);
    return info->host_buffer ? 0 : -1;
}

static ABI int32_t free_handler(Ngs2ContextBufferInfo *info) {
    ++frees;
    last_freed = info->host_buffer;
    free(info->host_buffer);
    return 0;
}

static ABI int32_t failing_alloc_handler(Ngs2ContextBufferInfo *info) {
    (void)info;
    return (int32_t)0x80020001;
}

static ABI int32_t empty_alloc_handler(Ngs2ContextBufferInfo *info) {
    info->host_buffer = NULL;
    return 0;
}

static const Ngs2BufferAllocator allocator = {alloc_handler, free_handler, 0x1234};

static Ngs2SystemOption system_option(void) {
    Ngs2SystemOption option = {.size = sizeof(option)};
    option.max_grain_samples = 512;
    option.num_grain_samples = 256;
    option.sample_rate = 48000;
    return option;
}

static Ngs2RackOption rack_option(uint32_t voices) {
    Ngs2RackOption option = {.size = sizeof(option)};
    option.max_grain_samples = 512;
    option.max_voices = voices;
    return option;
}

static void test_resolver(void) {
    for (size_t i = 0; i < sizeof(ngs2_imports) / sizeof(*ngs2_imports); ++i) {
        expect(runtime_ngs2_resolve(ngs2_imports[i].nid) != 0, ngs2_imports[i].symbol);
        expect(runtime_symbol(ngs2_imports[i].nid) && !strcmp(runtime_symbol(ngs2_imports[i].nid), ngs2_imports[i].symbol),
               "import name table agrees with the Ngs2 NID");
    }
    expect(runtime_ngs2_resolve("sceNgs2SystemCreate#libSceNgs2") == 0, "unknown Ngs2 NID");
    expect(runtime_ngs2_resolve("mPYgU4oYpuY#libc") == 0, "Ngs2 NID of another library");
    expect(runtime_ngs2_resolve("xa8oL9dmXkM#libSceNgs2") != runtime_ngs2_resolve("gbMKV+8Enuo#libSceNgs2"),
           "distinct exports");
}

static void test_system_lifecycle(void) {
    SystemCreate create = api("sceNgs2SystemCreateWithAllocator");
    SystemDestroy destroy = api("sceNgs2SystemDestroy");
    const Ngs2SystemOption option = system_option();
    Ngs2Handle first = 0, second = 0;
    allocations = frees = 0;

    expect(create(&option, &allocator, &first) == 0, "system create");
    expect(first != 0 && allocations == 1 && seen_user_data == 0x1234, "allocator callback gets its user data");
    expect(seen_size > 0, "allocator callback gets the required size");
    expect(create(NULL, &allocator, &second) == 0, "system create with default option");
    expect(second != 0 && second != first, "distinct system handles");

    Ngs2ContextBufferInfo info;
    expect(destroy(first, &info) == 0 && info.host_buffer && info.host_buffer_size == seen_size,
           "destroy reports the released buffer");
    expect(frees == 1 && last_freed == info.host_buffer, "destroy calls the free handler with the buffer");
    expect(destroy(first, NULL) == NGS2_ERROR_INVALID_SYSTEM_HANDLE, "destroyed handle is rejected");
    expect(destroy(0, NULL) == NGS2_ERROR_INVALID_SYSTEM_HANDLE, "null system handle");
    expect(destroy(second + 1, NULL) == NGS2_ERROR_INVALID_SYSTEM_HANDLE, "interior pointer is not a handle");
    expect(destroy(second, NULL) == 0 && frees == 2, "second system destroy");
}

static void test_system_arguments(void) {
    SystemCreate create = api("sceNgs2SystemCreateWithAllocator");
    Ngs2SystemOption option = system_option();
    Ngs2BufferAllocator broken = {failing_alloc_handler, free_handler, 0};
    Ngs2BufferAllocator empty = {empty_alloc_handler, free_handler, 0};
    Ngs2BufferAllocator no_alloc = {NULL, free_handler, 0};
    Ngs2Handle handle = 0;
    allocations = frees = 0;

    expect(create(&option, NULL, &handle) == NGS2_ERROR_INVALID_BUFFER_ALLOCATOR, "null allocator");
    expect(create(&option, &no_alloc, &handle) == NGS2_ERROR_INVALID_BUFFER_ALLOCATOR, "allocator without alloc handler");
    expect(create(&option, &allocator, NULL) == NGS2_ERROR_INVALID_OUT_ADDRESS, "null out handle");
    option.size = 63;
    expect(create(&option, &allocator, &handle) == NGS2_ERROR_INVALID_OPTION_SIZE, "option size");
    option = system_option();
    option.max_grain_samples = 100;
    expect(create(&option, &allocator, &handle) == NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES, "max grain samples");
    option = system_option();
    option.num_grain_samples = 2048;
    expect(create(&option, &allocator, &handle) == NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES, "num grain samples");
    option = system_option();
    option.sample_rate = 47999;
    expect(create(&option, &allocator, &handle) == NGS2_ERROR_INVALID_SAMPLE_RATE, "sample rate");
    expect(allocations == 0 && handle == 0, "invalid options never reach the allocator");
    option = system_option();
    expect(create(&option, &broken, &handle) == (int32_t)0x80020001 && handle == 0, "allocator failure is returned");
    expect(create(&option, &empty, &handle) == NGS2_ERROR_INVALID_BUFFER_ADDRESS && handle == 0,
           "allocator that returns no buffer");
}

static void test_rack_and_voices(void) {
    SystemCreate create_system = api("sceNgs2SystemCreateWithAllocator");
    SystemDestroy destroy_system = api("sceNgs2SystemDestroy");
    RackCreate create_rack = api("sceNgs2RackCreateWithAllocator");
    RackDestroy destroy_rack = api("sceNgs2RackDestroy");
    RackVoice voice_handle = api("sceNgs2RackGetVoiceHandle");
    VoiceControl control = api("sceNgs2VoiceControl");
    VoiceState state = api("sceNgs2VoiceGetState");
    VoiceFlags flags = api("sceNgs2VoiceGetStateFlags");
    const Ngs2SystemOption soption = system_option();
    const Ngs2RackOption roption = rack_option(3);
    Ngs2Handle system = 0, rack = 0, rack2 = 0, voice = 0, other = 0;
    allocations = frees = 0;

    expect(create_system(&soption, &allocator, &system) == 0, "system for racks");
    expect(create_rack(0, 0x3000, &roption, &allocator, &rack) == NGS2_ERROR_INVALID_SYSTEM_HANDLE, "null system");
    expect(create_rack(system + 8, 0x3000, &roption, &allocator, &rack) == NGS2_ERROR_INVALID_SYSTEM_HANDLE,
           "unknown system");
    expect(create_rack(system, 0x3000, &roption, NULL, &rack) == NGS2_ERROR_INVALID_BUFFER_ALLOCATOR, "rack allocator");
    expect(create_rack(system, 0x3000, &roption, &allocator, NULL) == NGS2_ERROR_INVALID_OUT_ADDRESS, "rack out");
    expect(create_rack(system, 0x3000, NULL, &allocator, &rack) == NGS2_ERROR_INVALID_OPTION_ADDRESS, "rack option");
    Ngs2RackOption small = roption;
    small.size = sizeof(small) - 8;
    expect(create_rack(system, 0x3000, &small, &allocator, &rack) == NGS2_ERROR_INVALID_OPTION_SIZE, "rack option size");
    expect(allocations == 1, "failed rack creates never reach the allocator");

    Ngs2RackOption extended = roption;
    extended.size = sizeof(extended) + 8;
    expect(create_rack(system, 0x3000, &extended, &allocator, &rack) == 0, "rack with an extended option");
    expect(rack != 0 && rack != system && seen_user_data == 0x1234, "rack handle");
    expect(seen_size >= sizeof(Ngs2RackOption), "rack buffer covers its voices");
    expect(create_rack(system, 0x1000, &roption, &allocator, &rack2) == 0 && rack2 != rack, "second rack");

    expect(voice_handle(rack, 0, &voice) == 0 && voice != 0, "voice 0");
    expect(voice_handle(rack, 0, &other) == 0 && other == voice, "voice handle is stable");
    expect(voice_handle(rack, 2, &other) == 0 && other != voice, "voice 2 differs from voice 0");
    expect(voice_handle(rack, 3, &other) == NGS2_ERROR_INVALID_VOICE_INDEX, "voice index past maxVoices");
    expect(voice_handle(rack, 0, NULL) == NGS2_ERROR_INVALID_OUT_ADDRESS, "voice out");
    expect(voice_handle(system, 0, &other) == NGS2_ERROR_INVALID_RACK_HANDLE, "system handle is not a rack");
    expect(voice_handle(0, 0, &other) == NGS2_ERROR_INVALID_RACK_HANDLE, "null rack");
    Ngs2Handle voice_of_second = 0;
    expect(voice_handle(rack2, 0, &voice_of_second) == 0 && voice_of_second != voice, "voices of different racks");

    const Ngs2VoiceParamHeader header = {sizeof(header), 0, 0};
    expect(control(voice, &header) == 0, "voice control");
    expect(control(voice, NULL) == NGS2_ERROR_INVALID_VOICE_CONTROL_ADDRESS, "voice control without a list");
    expect(control(rack, &header) == NGS2_ERROR_INVALID_VOICE_HANDLE, "rack handle is not a voice");
    expect(control(voice + 1, &header) == NGS2_ERROR_INVALID_VOICE_HANDLE, "interior pointer is not a voice");
    expect(control(0, &header) == NGS2_ERROR_INVALID_VOICE_HANDLE, "null voice");

    uint32_t value = 0xdeadbeef;
    expect(flags(voice, &value) == 0 && value == 0, "voice state flags");
    expect(flags(voice, NULL) == NGS2_ERROR_INVALID_OUT_ADDRESS, "state flags out");
    expect(flags(0, &value) == NGS2_ERROR_INVALID_VOICE_HANDLE, "state flags of a null voice");
    unsigned char blob[24];
    memset(blob, 0xcd, sizeof(blob));
    expect(state(voice, blob, sizeof(blob)) == 0, "voice state");
    unsigned char zeros[24] = {0};
    expect(!memcmp(blob, zeros, sizeof(blob)), "voice state covers the whole caller buffer");
    memset(blob, 0xcd, sizeof(blob));
    expect(state(voice, blob, sizeof(Ngs2VoiceState)) == 0 && blob[4] == 0xcd, "voice state stays inside its size");
    expect(state(voice, blob, 2) == NGS2_ERROR_INVALID_OUT_SIZE, "voice state size");
    expect(state(voice, NULL, 4) == NGS2_ERROR_INVALID_OUT_ADDRESS, "voice state out");

    expect(destroy_rack(rack, NULL) == 0 && frees == 1, "rack destroy frees its buffer");
    expect(destroy_rack(rack, NULL) == NGS2_ERROR_INVALID_RACK_HANDLE, "destroyed rack");
    expect(control(voice, &header) == NGS2_ERROR_INVALID_VOICE_HANDLE, "voice of a destroyed rack");
    expect(voice_handle(rack, 0, &other) == NGS2_ERROR_INVALID_RACK_HANDLE, "voice of a destroyed rack");
    expect(control(voice_of_second, &header) == 0, "voices of the other rack survive");

    expect(destroy_system(system, NULL) == 0 && frees == 3, "system destroy releases the remaining rack");
    expect(destroy_rack(rack2, NULL) == NGS2_ERROR_INVALID_RACK_HANDLE, "rack of a destroyed system");
    expect(control(voice_of_second, &header) == NGS2_ERROR_INVALID_VOICE_HANDLE, "voice of a destroyed system");
}

static void test_render(void) {
    SystemCreate create = api("sceNgs2SystemCreateWithAllocator");
    SystemDestroy destroy = api("sceNgs2SystemDestroy");
    SystemRender render = api("sceNgs2SystemRender");
    const Ngs2SystemOption option = system_option();
    Ngs2Handle system = 0;
    float samples[64];
    for (int i = 0; i < 64; ++i) samples[i] = 1.0f;
    Ngs2RenderBufferInfo buffers[2] = {{samples, 32 * sizeof(float), 0x18, 2}, {NULL, 0, 0x18, 2}};

    expect(create(&option, &allocator, &system) == 0, "system for render");
    expect(render(system, buffers, 2) == 0, "render");
    int silent = 1, untouched = 1;
    for (int i = 0; i < 32; ++i) silent &= samples[i] == 0.0f;
    for (int i = 32; i < 64; ++i) untouched &= samples[i] == 1.0f;
    expect(silent, "render clears the buffers it was given");
    expect(untouched, "render stays inside bufferSize");
    expect(render(system, NULL, 0) == 0, "render without buffers");
    expect(render(system, NULL, 1) == NGS2_ERROR_INVALID_BUFFER_INFO, "render with a missing buffer list");
    expect(render(0, buffers, 2) == NGS2_ERROR_INVALID_SYSTEM_HANDLE, "render on a null system");
    expect(render(system + 8, buffers, 2) == NGS2_ERROR_INVALID_SYSTEM_HANDLE, "render on an unknown system");
    expect(destroy(system, NULL) == 0, "render system destroy");
    expect(render(system, buffers, 2) == NGS2_ERROR_INVALID_SYSTEM_HANDLE, "render on a destroyed system");
}

static void test_pan_and_waveform(void) {
    PanInit init = api("sceNgs2PanInit");
    PanMatrix matrix = api("sceNgs2PanGetVolumeMatrix");
    ParseWaveform parse = api("sceNgs2ParseWaveformData");
    Ngs2PanWork work;
    memset(&work, 0xab, sizeof(work));
    float angles[2] = {30.0f, -30.0f};
    const Ngs2PanParam param = {0.0f, 1.0f, 1.0f, 0.0f};
    float out[64];

    expect(init(&work, NULL, 15.0f, 7) == 0, "PanInit with the default speaker layout");
    expect(init(&work, angles, 15.0f, 2) == 0, "PanInit with speaker angles");
    expect(init(NULL, angles, 15.0f, 2) == NGS2_ERROR_INVALID_PAN_WORK, "PanInit without a work area");
    expect(matrix(&work, &param, 1, 0, out) == 0, "PanGetVolumeMatrix");
    expect(matrix(NULL, &param, 1, 0, out) == NGS2_ERROR_INVALID_PAN_WORK, "matrix without a work area");
    expect(matrix(&work, NULL, 1, 0, out) == NGS2_ERROR_INVALID_PAN_PARAM, "matrix without parameters");
    expect(matrix(&work, &param, 1, 0, NULL) == NGS2_ERROR_INVALID_OUT_ADDRESS, "matrix without an output");

    unsigned char data[16] = {0};
    unsigned char info[256];
    expect(parse(data, sizeof(data), info) == NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT, "waveform parsing is refused");
    expect(parse(NULL, sizeof(data), info) == NGS2_ERROR_INVALID_WAVEFORM_ADDRESS, "waveform without data");
    expect(parse(data, 0, info) == NGS2_ERROR_INVALID_WAVEFORM_SIZE, "waveform without a size");
    expect(parse(data, sizeof(data), NULL) == NGS2_ERROR_INVALID_OUT_ADDRESS, "waveform without an output");
}

int main(void) {
    runtime_ngs2_module_load();
    test_resolver();
    test_system_lifecycle();
    test_system_arguments();
    test_rack_and_voices();
    test_render();
    test_pan_and_waveform();
    runtime_ngs2_report();
    runtime_ngs2_module_unload();
    printf("ngs2_test: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}

// Glue between the C loader (src/sotbgpu.h) and the vendored shadPS4 video core.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "sotb_copy_pool.h"
#include "sotb_latency_trace.h"
#include "sotb_overlay.h"
#include "sotb_renderer.h"
#include "sotb_vulkan_support.h"
#include "sotb_png.h"
#include "sotb_settings.h"
#include "sotb_title.h"
#include "sotb_toggles.h"
#include "sotb_write_log.h"
#include "sotbgpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/rdtsc.h"
#include "video_core/renderdoc.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
// runtime.c
void runtime_report(void);
// runtime_log.c
void runtime_log_flush(void);
void runtime_log_mark_clean_exit(void);
void runtime_log_finalize(void);
// startup_phase.c
void sotb_startup_phase(const char* phase);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const SotbGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(VideoCore::RasterizerBackend* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // Large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !SotbCopyPool::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    SotbCopyPool::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    SotbWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, SotbWriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    SotbStats::Timer timer{SotbStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    SotbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    SotbStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        SotbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        SotbStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
// The video core's own GnmDriver packets stay base PS4; the game's answer comes from runtime_tsr.c.
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; }
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

extern "C" int sotbgpu_available(void) {
    return 1;
}

// Each renderer needs its own kind of window, so the renderer is chosen, after Vulkan's preflight, before there is one.
static bool SelectRenderer() {
    const auto choice = SotbRenderer::Choice(SotbSettings::Get().renderer.load());
    const std::string requested(SotbRenderer::ChoiceKeys[choice]);
    std::printf("Renderer: requested %s\n", requested.c_str());
    bool vulkan_usable = true;
    std::string problem;
    if (choice == SotbRenderer::Vulkan || choice == SotbRenderer::Auto) {
        const auto preflight = SotbVulkan::CheckSystemVulkan();
        std::printf("Renderer: %s\n", preflight.Summary().c_str());
        vulkan_usable = preflight.Passed();
        if (!vulkan_usable) {
            problem = preflight.problem->cause;
            if (choice == SotbRenderer::Vulkan) {
                std::printf("Renderer: Vulkan was requested and cannot run: %s %s\n", problem.c_str(),
                            preflight.problem->advice.c_str());
                return false;
            }
        }
    }
    const auto resolution = SotbRenderer::Resolve(choice, vulkan_usable, problem);
    SotbRenderer::SetActive(resolution.backend);
    std::printf("Renderer: selected %s, active %s\n", requested.c_str(), SotbRenderer::BackendName(resolution.backend));
    if (!resolution.fallback_reason.empty()) {
        std::printf("Renderer: Auto fell back to OpenGL: %s\n", resolution.fallback_reason.c_str());
    }
    return true;
}

static_assert(SOTB_GPU_FEATURE_RENDER_RESOLUTION == SotbSettings::RenderResolution);

extern "C" int sotbgpu_init(const SotbGpuConfig* config) {
    SotbSettings::Load(config->settings_file);
    SotbSettings::Get().features = config->features;
    if (!SelectRenderer()) {
        return 1;
    }
    const std::string user_dir = config->user_dir;
    Common::FS::SetUserDir(std::filesystem::path(std::u8string(user_dir.begin(), user_dir.end())));
    g_sdk_version = config->sdk_version;
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : SOTB_WINDOW_TITLE;
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("sotb:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str(),
                                               Frontend::GraphicsApiFor(SotbRenderer::Active()));
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        sotb_startup_phase("runtime window closed");
        LOG_INFO(Frontend, "Window closed by user");
        std::puts("Runtime: window closed by user");
        runtime_report();
        runtime_log_mark_clean_exit();
        runtime_log_finalize();
        runtime_log_flush();
        sotb_startup_phase("process exit");
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void sotbgpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
}

extern "C" uintptr_t sotbgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int sotbgpu_handle_fault(void* ucontext, void* address) {
    return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
}

extern "C" unsigned sotbgpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("sotb:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" int sotbgpu_controller_rumble(void) {
    return SotbSettings::Get().controller_rumble ? 1 : 0;
}

extern "C" int sotbgpu_overlay_captures_input(void) {
    return SotbOverlay::CapturesInput() ? 1 : 0;
}

extern "C" int sotbgpu_latency_trace_enabled(void) {
    return SotbLatencyTrace::enabled ? 1 : 0;
}

extern "C" uint64_t sotbgpu_latency_press_ns(void) {
    return SotbLatencyTrace::press_ns.load(std::memory_order_relaxed);
}

static int (*g_exit_save_active)(void) = nullptr;

extern "C" int sotbgpu_request_close(void) {
    if (!g_window || !g_window->RequestClose()) return 0;
    std::printf("Exit Game: host exit scheduled\n");
    return 1;
}

extern "C" void sotbgpu_exit_confirm_set_save_check(int (*save_active)(void)) {
    g_exit_save_active = save_active;
}

extern "C" int sotbgpu_exit_confirm_begin(int original_layout, int xbox_prompts, const char* locale) {
    if (!g_window) return 0;
    return SotbOverlay::BeginExitConfirm(original_layout != 0, xbox_prompts != 0, locale, g_exit_save_active,
                                         sotbgpu_request_close) ? 1 : 0;
}

extern "C" int sotbgpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int sotbgpu_choice_begin(const char* title, const char* const* items, const char* const* images,
                                    int count, int focus) {
    if (!g_window || count <= 0) return 0;
    // Pictures are decoded here, on the game's thread, before the list is shown.
    std::vector<SotbPng::Image> pictures(count);
    for (int i = 0; images && i < count; ++i) {
        if (images[i] && !SotbPng::LoadRgba(images[i], pictures[i])) {
            LOG_WARNING(Lib_SaveDataDialog, "save list: no picture from {}", images[i]);
        }
    }
    SotbOverlay::BeginChoice(title ? title : "", std::vector<std::string>(items, items + count),
                             std::move(pictures), focus);
    return 1;
}

extern "C" int sotbgpu_choice_poll(void) {
    SotbOverlay::PollChoiceInput(); // the game polls the dialog; the list reads the pad here too
    return SotbOverlay::PollChoice();
}

extern "C" void sotbgpu_choice_set_input(uint32_t (*buttons)(void)) {
    SotbOverlay::SetChoiceInput(buttons);
}

extern "C" int sotbgpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}

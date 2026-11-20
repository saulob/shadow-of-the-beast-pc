// sotb: GPU-side assertion failures stop the port with exit code 23.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_device_lost.h"

// After a device loss the first report owns the reason and the process end; this assertion is its consequence.
[[noreturn]] static void wait_for_device_lost_exit() {
    using namespace Vulkan::DeviceLost;
    std::fputs("STOP: GPU library assertion after the device was lost (a consequence of the first report)\n", stderr);
    std::fflush(stderr);
    const auto deadline = std::chrono::steady_clock::now() + 2 * ShutdownGrace;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::fflush(stdout);
    std::_Exit(ExitStatus);
}

void assert_fail_impl() {
    if (Vulkan::DeviceLost::IsLost()) {
        wait_for_device_lost_exit();
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}

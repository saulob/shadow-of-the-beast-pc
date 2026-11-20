// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <tuple>
#include "video_core/renderer_vulkan/vk_device_lost.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

namespace Vulkan {

// Long enough to cost nothing while waiting, short enough to notice a device lost on another thread.
constexpr u64 WAIT_SLICE_NS = 100'000'000;

Semaphore::Semaphore(const Instance& instance_, DeviceLost::Timeline timeline_)
    : instance{instance_}, timeline{timeline_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        vk::Result counter_result{};
        const s32 result = DeviceLost::CallWhileHealthy(
            DeviceLost::Global(), DeviceLost::Operation::GetSemaphoreCounterValue, "semread", this_tick,
            [&] {
                std::tie(counter_result, counter) =
                    instance.GetDevice().getSemaphoreCounterValue(*semaphore);
                return counter_result;
            },
            timeline);
        if (result == DeviceLost::DeviceLostResult) {
            return;
        }
        ASSERT_MSG(result == DeviceLost::Success, "Failed to get master semaphore value: {}",
                   vk::to_string(vk::Result(result)));
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait. A lost device never signals again:
    // the wait ends instead of blocking shutdown.
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };
    const s32 result = DeviceLost::WaitWhileHealthy(
        DeviceLost::Global(), DeviceLost::Operation::WaitSemaphores, "semwait", tick, [&] {
            return instance.GetDevice().waitSemaphores(&wait_info, WAIT_SLICE_NS);
        },
        timeline);
    if (result == DeviceLost::DeviceLostResult) {
        return;
    }
    ASSERT_MSG(result == DeviceLost::Success, "Timeline semaphore wait failed: {}",
               vk::to_string(vk::Result(result)));
    Refresh();
}

} // namespace Vulkan

// SPDX-License-Identifier: GPL-2.0-or-later
// sotb: helpers of the background pipeline cache warm-up (vk_pipeline_serialization.cpp).

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace Vulkan {

/// Warm-up threads: one on four cores or fewer, otherwise all but two, at most six.
inline unsigned WarmupThreadCount(unsigned cores) {
    return cores <= 4 ? 1u : std::min(cores - 2, 6u);
}

/// Items made on worker threads, handed to the GPU thread a few at a time so that a frame
/// never pays for the whole cache at once.
template <typename T>
class WarmupInbox {
public:
    void Push(T item) {
        std::scoped_lock lock{mutex};
        items.push_back(std::move(item));
    }

    /// At most `max` items, oldest first.
    std::vector<T> Take(std::size_t max) {
        std::scoped_lock lock{mutex};
        const std::size_t count = std::min(max, items.size());
        std::vector<T> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(std::move(items.front()));
            items.pop_front();
        }
        return out;
    }

    bool Empty() {
        std::scoped_lock lock{mutex};
        return items.empty();
    }

private:
    std::mutex mutex;
    std::deque<T> items;
};

/// Pipelines handed to the pool at most (queued, not yet running): a large cache never piles up an unbounded backlog,
/// and the reader waits for the workers instead.
inline constexpr std::size_t WarmupMaxQueuedBuilds = 256;
inline constexpr std::size_t WarmupBatchEntries = 32;

/// How many cached entries the GPU thread may turn into build jobs now.
inline constexpr std::size_t WarmupBatchRoom(std::size_t queued_builds) {
    return queued_builds >= WarmupMaxQueuedBuilds
               ? 0
               : std::min(WarmupBatchEntries, WarmupMaxQueuedBuilds - queued_builds);
}

/// What the GPU thread does with a cached entry the readers delivered. A pipeline the game already created, or is
/// creating right now, is never built a second time.
enum class WarmEntryAction { Build, SkipPublished, SkipOnDemandPending };

inline constexpr WarmEntryAction ClassifyWarmEntry(bool published, bool on_demand_pending) {
    if (published) {
        return WarmEntryAction::SkipPublished;
    }
    return on_demand_pending ? WarmEntryAction::SkipOnDemandPending : WarmEntryAction::Build;
}

/// The background warm-up's worker threads. A job returns whether it produced its pipeline; one that fails or throws is
/// counted and never run again (the game builds that pipeline when it needs it). `on_thread_start` runs on each worker
/// before its first job, to lower its priority.
class WarmupPool {
public:
    explicit WarmupPool(unsigned count, std::function<void()> on_thread_start = {}) {
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this, on_thread_start] {
                if (on_thread_start) {
                    on_thread_start();
                }
                Run();
            });
        }
    }
    WarmupPool(const WarmupPool&) = delete;
    WarmupPool& operator=(const WarmupPool&) = delete;
    ~WarmupPool() {
        {
            std::scoped_lock lock{mutex};
            cancelled += static_cast<unsigned>(jobs.size());
            jobs.clear(); // only the running ones finish
            closing = true;
        }
        cv.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }
    /// False once the pool is draining or closing: the job is dropped and counted as cancelled.
    bool Push(std::function<bool()> job) {
        {
            std::scoped_lock lock{mutex};
            if (closing || refusing) {
                ++cancelled;
                return false;
            }
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
        return true;
    }
    /// Waits for every job; `tick` runs on this thread meanwhile (the loading screen).
    void Wait(const std::function<void()>& tick) {
        std::unique_lock lock{mutex};
        while (!jobs.empty() || running) {
            idle.wait_for(lock, std::chrono::milliseconds(50));
            lock.unlock();
            tick();
            lock.lock();
        }
    }
    /// Drops the queued jobs and waits for the running ones; the threads stay parked and new jobs are refused.
    void Drain() {
        std::unique_lock lock{mutex};
        cancelled += static_cast<unsigned>(jobs.size());
        jobs.clear();
        refusing = true;
        idle.wait(lock, [this] { return running == 0; });
    }
    bool Idle() {
        std::scoped_lock lock{mutex};
        return jobs.empty() && !running;
    }
    std::size_t Queued() {
        std::scoped_lock lock{mutex};
        return jobs.size();
    }
    unsigned Built() const { return built.load(std::memory_order_relaxed); }
    unsigned Failed() const { return failed.load(std::memory_order_relaxed); }
    unsigned Cancelled() {
        std::scoped_lock lock{mutex};
        return cancelled;
    }
    std::atomic<unsigned> built{0};
    std::atomic<unsigned> failed{0};

private:
    void Run() {
        std::unique_lock lock{mutex};
        while (true) {
            cv.wait(lock, [this] { return closing || !jobs.empty(); });
            if (jobs.empty()) {
                return;
            }
            auto job = std::move(jobs.front());
            jobs.pop_front();
            ++running;
            lock.unlock();
            bool ok = false;
            try {
                ok = job();
            } catch (...) {
            }
            (ok ? built : failed).fetch_add(1, std::memory_order_relaxed);
            lock.lock();
            --running;
            idle.notify_all();
        }
    }

    std::mutex mutex;
    std::condition_variable cv, idle;
    std::deque<std::function<bool()>> jobs;
    std::vector<std::thread> threads;
    unsigned running = 0;
    unsigned cancelled = 0;
    bool closing = false;
    bool refusing = false;
};

/// The first pipeline ready for a key is kept: a warm-up pipeline that arrives after the
/// game compiled the same one (or the reverse) is dropped. True when `value` was stored.
template <typename Map, typename Key, typename Value>
bool PublishFirst(Map& map, const Key& key, Value&& value) {
    return map.try_emplace(key, std::forward<Value>(value)).second;
}

} // namespace Vulkan

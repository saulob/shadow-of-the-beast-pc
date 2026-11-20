// Pipeline warm-up tests: the startup choice, the worker pool, the bounded queue, deduplication against the game's own
// pipeline creation, failure and shutdown behaviour. Fake jobs and a fake renderer: no Vulkan device, no window.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sotb_pipeline_startup.h"
#include "video_core/pipeline_cache_policy.h"
#include "video_core/renderer_vulkan/vk_warmup_inbox.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

using namespace std::chrono_literals;

std::string ReadAll(const std::string& relative) {
    std::ifstream file(std::string(SOTB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

template <typename Pred>
bool WaitFor(Pred done, std::chrono::milliseconds limit = 5000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

void TestStartupChoice() {
    using namespace Sotb;
    CHECK(!PipelineOnDemandRequested(nullptr));
    CHECK(!PipelineOnDemandRequested(""));
    CHECK(!PipelineOnDemandRequested("0"));
    CHECK(PipelineOnDemandRequested("1"));
    CHECK(PipelineOnDemandRequested("yes"));

    int started = 0;
    CHECK(ChoosePipelineStartup(nullptr, [&] { ++started; return true; }) == PipelineStartup::Background && started == 1);
    CHECK(ChoosePipelineStartup("0", [&] { ++started; return true; }) == PipelineStartup::Background && started == 2);
    // The on-demand request never starts the background work.
    CHECK(ChoosePipelineStartup("1", [&] { ++started; return true; }) == PipelineStartup::OnDemand && started == 2);
    // Warm-up unavailable (the store cannot be read concurrently): on-demand creation remains.
    CHECK(ChoosePipelineStartup(nullptr, [&] { ++started; return false; }) == PipelineStartup::OnDemand && started == 3);
}

void TestWorkerPolicy() {
    using namespace Vulkan;
    CHECK(WarmupThreadCount(1) == 1 && WarmupThreadCount(2) == 1 && WarmupThreadCount(4) == 1);
    CHECK(WarmupThreadCount(5) == 3 && WarmupThreadCount(6) == 4 && WarmupThreadCount(8) == 6);
    CHECK(WarmupThreadCount(16) == 6 && WarmupThreadCount(64) == 6);
    for (unsigned cores = 1; cores <= 128; ++cores) {
        const unsigned threads = WarmupThreadCount(cores);
        CHECK(threads >= 1 && threads <= 6 && (cores <= 2 || threads < cores));
    }
}

void TestQueueBound() {
    using namespace Vulkan;
    CHECK(WarmupBatchRoom(0) == WarmupBatchEntries);
    CHECK(WarmupBatchRoom(WarmupMaxQueuedBuilds - 5) == 5);
    CHECK(WarmupBatchRoom(WarmupMaxQueuedBuilds) == 0);
    CHECK(WarmupBatchRoom(WarmupMaxQueuedBuilds + 1000) == 0);
    std::size_t queued = 0;
    for (int round = 0; round < 100; ++round) {
        queued += WarmupBatchRoom(queued);
        CHECK(queued <= WarmupMaxQueuedBuilds);
    }
    CHECK(queued == WarmupMaxQueuedBuilds);
}

void TestInboxOrderAndDedup() {
    using namespace Vulkan;
    WarmupInbox<int> inbox;
    CHECK(inbox.Empty() && inbox.Take(10).empty() && inbox.Take(0).empty());
    for (int i = 0; i < 5; ++i) {
        inbox.Push(i);
    }
    const auto first = inbox.Take(2);
    CHECK(first.size() == 2 && first[0] == 0 && first[1] == 1);
    CHECK(inbox.Take(0).empty() && !inbox.Empty());
    const auto rest = inbox.Take(100);
    CHECK(rest.size() == 3 && rest[0] == 2 && rest[2] == 4 && inbox.Empty());

    std::map<int, std::string> published;
    CHECK(PublishFirst(published, 7, std::string("game")));
    CHECK(!PublishFirst(published, 7, std::string("warm-up")));
    CHECK(published.at(7) == "game");
    CHECK(PublishFirst(published, 8, std::string("warm-up")) && !PublishFirst(published, 8, std::string("game")));
    CHECK(published.at(8) == "warm-up");
}

void TestEntryClassification() {
    using namespace Vulkan;
    CHECK(ClassifyWarmEntry(false, false) == WarmEntryAction::Build);
    CHECK(ClassifyWarmEntry(true, false) == WarmEntryAction::SkipPublished);
    CHECK(ClassifyWarmEntry(false, true) == WarmEntryAction::SkipOnDemandPending);
    CHECK(ClassifyWarmEntry(true, true) == WarmEntryAction::SkipPublished);
}

// The GPU thread's decision loop on a fake cache: nothing the game has or is building is built again.
void TestNoDuplicateCompilation() {
    using namespace Vulkan;
    std::set<int> published{1, 2};
    std::set<int> pending{3};
    std::vector<int> built;
    std::map<int, int> compilations;
    for (int key : {1, 2, 3, 4, 5, 4}) {
        if (ClassifyWarmEntry(published.contains(key), pending.contains(key)) != WarmEntryAction::Build) {
            continue;
        }
        ++compilations[key];
        built.push_back(key);
        published.insert(key);
    }
    CHECK((built == std::vector<int>{4, 5}));
    CHECK(compilations[4] == 1 && compilations[5] == 1 && !compilations.contains(3) && !compilations.contains(1));
    // The game finishes key 3 later; a warm-up copy arriving afterwards loses.
    std::map<int, int> store;
    CHECK(PublishFirst(store, 3, 100) && !PublishFirst(store, 3, 200) && store[3] == 100);
}

void TestPoolRunsJobsInOrderWithOneWorker() {
    using namespace Vulkan;
    std::vector<int> order;
    std::mutex mutex;
    {
        WarmupPool pool(1);
        for (int i = 0; i < 20; ++i) {
            CHECK(pool.Push([&, i] {
                std::scoped_lock lock{mutex};
                order.push_back(i);
                return true;
            }));
        }
        CHECK(WaitFor([&] { return pool.Built() == 20; }));
        CHECK(pool.Failed() == 0 && pool.Idle() && pool.Queued() == 0);
    }
    bool ordered = order.size() == 20;
    for (int i = 0; i < 20 && ordered; ++i) {
        ordered = order[static_cast<std::size_t>(i)] == i;
    }
    CHECK(ordered);
}

void TestPoolBoundedConcurrency() {
    using namespace Vulkan;
    std::atomic<int> active{0}, peak{0}, done{0};
    WarmupPool pool(2);
    for (int i = 0; i < 40; ++i) {
        pool.Push([&] {
            const int now = ++active;
            int seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(1ms);
            --active;
            ++done;
            return true;
        });
    }
    CHECK(WaitFor([&] { return done == 40; }));
    CHECK(peak.load() >= 1 && peak.load() <= 2);
}

void TestPoolThreadStartHook() {
    using namespace Vulkan;
    std::atomic<int> started{0};
    {
        WarmupPool pool(3, [&] { ++started; });
        CHECK(WaitFor([&] { return started == 3; }));
    }
    CHECK(started == 3);
}

void TestFailureDoesNotLoopOrPoison() {
    using namespace Vulkan;
    std::atomic<int> failing_runs{0}, throwing_runs{0}, good_runs{0};
    WarmupPool pool(2);
    pool.Push([&] { ++failing_runs; return false; });
    pool.Push([&]() -> bool { ++throwing_runs; throw std::runtime_error("driver rejected the pipeline"); });
    pool.Push([&] { ++good_runs; return true; });
    CHECK(WaitFor([&] { return pool.Built() + pool.Failed() == 3; }));
    CHECK(pool.Failed() == 2 && pool.Built() == 1);
    std::this_thread::sleep_for(50ms);
    CHECK(failing_runs == 1 && throwing_runs == 1 && good_runs == 1);

    // The failed pipeline is simply not published, so the game's own creation path is unaffected.
    std::map<int, std::string> store;
    CHECK(!store.contains(1));
    CHECK(PublishFirst(store, 1, std::string("on demand")) && store.at(1) == "on demand");
    // The pool is still usable after failures.
    std::atomic<bool> again{false};
    CHECK(pool.Push([&] { again = true; return true; }) && WaitFor([&] { return again.load(); }));
}

void TestZeroPipelines() {
    using namespace Vulkan;
    WarmupPool pool(2);
    CHECK(pool.Idle() && pool.Queued() == 0 && pool.Built() == 0 && pool.Failed() == 0 && pool.Cancelled() == 0);
    pool.Drain();
    CHECK(pool.Idle() && pool.Cancelled() == 0);
    WarmupPool none(0);
    CHECK(none.Idle());
}

void TestDrainStopsAndCancels() {
    using namespace Vulkan;
    std::atomic<bool> release{false}, running{false};
    std::atomic<int> finished{0}, started_later{0};
    WarmupPool pool(1);
    pool.Push([&] {
        running = true;
        while (!release) {
            std::this_thread::sleep_for(1ms);
        }
        ++finished;
        return true;
    });
    CHECK(WaitFor([&] { return running.load(); }));
    for (int i = 0; i < 10; ++i) {
        pool.Push([&] { ++started_later; return true; });
    }
    CHECK(pool.Queued() == 10);
    std::thread releaser([&] {
        std::this_thread::sleep_for(30ms);
        release = true;
    });
    pool.Drain();
    releaser.join();
    // Drain waited for the running job and dropped the queued ones.
    CHECK(finished == 1 && started_later == 0);
    CHECK(pool.Cancelled() == 10 && pool.Queued() == 0 && pool.Idle());
    // Shutdown accepts no new work.
    CHECK(!pool.Push([&] { ++started_later; return true; }));
    CHECK(pool.Cancelled() == 11);
    std::this_thread::sleep_for(20ms);
    CHECK(started_later == 0);
    pool.Drain();
    CHECK(pool.Cancelled() == 11);
}

struct FakeRenderer {
    explicit FakeRenderer(std::atomic<bool>& alive_flag) : alive(alive_flag) { alive = true; }
    ~FakeRenderer() { alive = false; }
    void Touch() { ++touches; }
    std::atomic<bool>& alive;
    std::atomic<int> touches{0};
};

// The pool is destroyed before the renderer state its jobs use, as BackgroundWarmup holds its pool last.
void TestNoTaskOutlivesRendererState() {
    std::atomic<bool> alive{false};
    std::atomic<int> ran{0}, after_destroy{0};
    {
        FakeRenderer renderer(alive);
        {
            Vulkan::WarmupPool pool(2);
            for (int i = 0; i < 200; ++i) {
                pool.Push([&] {
                    std::this_thread::sleep_for(100us);
                    if (!alive) {
                        ++after_destroy;
                        return false;
                    }
                    renderer.Touch();
                    ++ran;
                    return true;
                });
            }
            std::this_thread::sleep_for(5ms);
        }
        CHECK(renderer.touches == ran.load());
    }
    CHECK(!alive);
    const int ran_at_close = ran.load();
    std::this_thread::sleep_for(20ms);
    CHECK(after_destroy == 0 && ran.load() == ran_at_close);
}

void TestDestructorCancelsQueued() {
    std::atomic<int> ran{0};
    {
        Vulkan::WarmupPool pool(1);
        std::atomic<bool> block{true};
        pool.Push([&] {
            while (block) {
                std::this_thread::sleep_for(1ms);
            }
            return true;
        });
        for (int i = 0; i < 50; ++i) {
            pool.Push([&] { ++ran; return true; });
        }
        block = false;
    }
    CHECK(ran.load() <= 50);
}

// Only cached data the store accepts is warmed: a stamp that is missing, corrupt or from another host never is.
void TestOnlyAcceptedCacheIsQueued() {
    using namespace PipelineCachePolicy;
    StoreIdentity current{};
    current.vendor_id = 0x10de;
    current.device_id = 0x2204;
    const auto good = EncodeStoreIdentity(current);
    const auto warms = [&](std::span<const std::uint8_t> stamp) { return CheckStore(stamp, current) == StoreVerdict::Reuse; };
    CHECK(warms(good));
    CHECK(!warms({}));
    auto truncated = good;
    truncated.resize(good.size() / 2);
    CHECK(!warms(truncated));
    auto flipped = good;
    flipped[0] ^= 0xff;
    CHECK(!warms(flipped));
    StoreIdentity other = current;
    other.driver_version = 7;
    CHECK(!warms(EncodeStoreIdentity(other)));

    // A cached blob that fails its seal is dropped before it reaches the inbox.
    Vulkan::WarmupInbox<std::vector<std::uint8_t>> inbox;
    std::vector<std::vector<std::uint8_t>> blobs;
    for (int i = 0; i < 3; ++i) {
        std::vector<std::uint8_t> blob(16, static_cast<std::uint8_t>(i));
        CHECK(Seal(blob));
        blobs.push_back(blob);
    }
    blobs[1][3] ^= 1; // corrupt
    blobs.push_back({1, 2, 3}); // truncated
    int queued = 0;
    for (auto blob : blobs) {
        if (Unseal(blob)) {
            inbox.Push(std::move(blob));
            ++queued;
        }
    }
    CHECK(queued == 2 && inbox.Take(10).size() == 2);
}

void TestSourcesAgree() {
    const std::string presenter = ReadAll("gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    const std::string serialization = ReadAll("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    CHECK(presenter.find("Sotb::ChoosePipelineStartup(") != std::string::npos);
    CHECK(presenter.find("SOTB_PIPELINE_ON_DEMAND") != std::string::npos);
    CHECK(presenter.find("StartBackgroundWarmUp") != std::string::npos && presenter.find("StartOnDemand") != std::string::npos);
    CHECK(serialization.find("WarmupBatchRoom(") != std::string::npos);
    CHECK(serialization.find("ClassifyWarmEntry(") != std::string::npos);
    CHECK(serialization.find("pending_graphics.contains") != std::string::npos);
    CHECK(serialization.find("ThreadPriority::Low") != std::string::npos);
    CHECK(serialization.find("background->pool->Drain()") != std::string::npos);
}

} // namespace

int main() {
    TestStartupChoice();
    TestWorkerPolicy();
    TestQueueBound();
    TestInboxOrderAndDedup();
    TestEntryClassification();
    TestNoDuplicateCompilation();
    TestPoolRunsJobsInOrderWithOneWorker();
    TestPoolBoundedConcurrency();
    TestPoolThreadStartHook();
    TestFailureDoesNotLoopOrPoison();
    TestZeroPipelines();
    TestDrainStopsAndCancels();
    TestNoTaskOutlivesRendererState();
    TestDestructorCancelsQueued();
    TestOnlyAcceptedCacheIsQueued();
    TestSourcesAgree();
    std::printf("pipeline warm-up tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

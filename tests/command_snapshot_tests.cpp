// GPU command snapshot tests: the command words a worker reads are an owned copy taken at submission, in order,
// bounded in size, and kept until the last consumer is done. Synthetic command buffers, no GPU, no window.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "video_core/amdgpu/command_snapshot.h"

namespace {

using namespace AmdGpu;

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

std::vector<u32> Words(u32 first, size_t count) {
    std::vector<u32> words(count);
    for (size_t i = 0; i < count; ++i) {
        words[i] = first + u32(i);
    }
    return words;
}

bool Equals(std::span<const u32> a, const std::vector<u32>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

std::string ReadSource(const std::string& relative) {
    std::ifstream file(std::string(SOTB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// A model of the GPU queue: submissions are queued, a worker thread consumes them in order and hands each
// snapshot back to the pool only after the consumer is done with it.
class Queue {
public:
    using Consumer = std::function<void(const CommandSnapshot&)>;

    explicit Queue(CommandSnapshots& pool_, Consumer consumer_)
        : pool{pool_}, consumer{std::move(consumer_)}, worker{[this] { Run(); }} {}

    ~Queue() {
        Drain();
    }

    void Submit(std::unique_ptr<CommandSnapshot> snapshot) {
        std::scoped_lock lock{mutex};
        queue.push(std::move(snapshot));
        cv.notify_one();
    }

    void Hold() {
        std::scoped_lock lock{mutex};
        held = true;
    }

    void Release() {
        {
            std::scoped_lock lock{mutex};
            held = false;
        }
        cv.notify_all();
    }

    void Drain() {
        {
            std::scoped_lock lock{mutex};
            held = false;
            stopping = true;
        }
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    size_t Pending() {
        std::scoped_lock lock{mutex};
        return queue.size();
    }

private:
    void Run() {
        for (;;) {
            std::unique_ptr<CommandSnapshot> snapshot;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [this] { return stopping || (!held && !queue.empty()); });
                if (queue.empty()) {
                    return;
                }
                snapshot = std::move(queue.front());
                queue.pop();
            }
            consumer(*snapshot);
            pool.Return(std::move(snapshot));
        }
    }

    CommandSnapshots& pool;
    Consumer consumer;
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::unique_ptr<CommandSnapshot>> queue;
    bool held{};
    bool stopping{};
    std::thread worker;
};

void TestSnapshotIsIsolatedFromGuest() {
    CommandSnapshots pool;
    auto guest = Words(100, 64);
    const auto submitted = guest;
    auto snapshot = pool.Take(guest, {});
    CHECK(snapshot != nullptr);
    CHECK(Equals(snapshot->dcb, submitted));
    CHECK(snapshot->dcb.data() != guest.data());
    CHECK(snapshot->guest_dcb == guest.data());

    for (auto& word : guest) {
        word = 0xdeadbeef;
    }
    CHECK(Equals(snapshot->dcb, submitted));
    CHECK(FirstRewrittenWord(snapshot->dcb, guest.data()) == 0);

    guest = submitted;
    CHECK(FirstRewrittenWord(snapshot->dcb, guest.data()) == guest.size());
    guest[17] ^= 1;
    CHECK(FirstRewrittenWord(snapshot->dcb, guest.data()) == 17);

    const auto rest = snapshot->dcb.subspan(10);
    const auto guest_rest = GuestRest(rest, snapshot->dcb.data(), guest.data());
    CHECK(guest_rest.data() == guest.data() + 10 && guest_rest.size() == rest.size());
}

void TestWorkerSeesSnapshotNotMutatedGuest() {
    CommandSnapshots pool;
    auto guest = Words(1, 256);
    const auto submitted = guest;
    std::vector<u32> seen;
    Queue queue{pool, [&](const CommandSnapshot& s) { seen.assign(s.dcb.begin(), s.dcb.end()); }};
    queue.Hold();
    queue.Submit(pool.Take(guest, {}));
    std::fill(guest.begin(), guest.end(), 0u);
    guest.clear();
    guest.shrink_to_fit();
    queue.Release();
    queue.Drain();
    CHECK(seen == submitted);
}

void TestOrderingAndMultipleSubmissionsIsolated() {
    CommandSnapshots pool;
    std::vector<std::vector<u32>> guests;
    for (u32 i = 0; i < 6; ++i) {
        guests.push_back(Words(i * 1000, 8 + i * 3));
    }
    const auto submitted = guests;
    std::vector<std::vector<u32>> seen;
    {
        Queue queue{pool, [&](const CommandSnapshot& s) { seen.emplace_back(s.dcb.begin(), s.dcb.end()); }};
        queue.Hold();
        for (auto& guest : guests) {
            queue.Submit(pool.Take(guest, {}));
            std::fill(guest.begin(), guest.end(), 0xffffffffu);
        }
        CHECK(queue.Pending() == 6);
        queue.Release();
    }
    CHECK(seen.size() == submitted.size());
    for (size_t i = 0; i < seen.size() && i < submitted.size(); ++i) {
        CHECK(seen[i] == submitted[i]);
    }

    const auto dcb = Words(10, 5);
    const auto ccb = Words(500, 3);
    auto both = pool.Take(dcb, ccb);
    CHECK(both != nullptr);
    CHECK(Equals(both->dcb, dcb));
    CHECK(Equals(both->ccb, ccb));
    CHECK(both->ccb.data() == both->dcb.data() + dcb.size());
    CHECK(both->words.size() == dcb.size() + ccb.size());
}

void TestZeroLength() {
    CommandSnapshots pool;
    auto empty = pool.Take({}, {});
    CHECK(empty != nullptr);
    CHECK(empty->dcb.empty() && empty->ccb.empty());
    CHECK(empty->words.empty());
    CHECK(FirstRewrittenWord(empty->dcb, nullptr) == 0);
    CHECK(CheckSnapshotSize(0, 0) == SnapshotStatus::Ok);

    const auto dcb = Words(1, 4);
    auto only_dcb = pool.Take(dcb, {});
    CHECK(only_dcb != nullptr && only_dcb->ccb.empty() && only_dcb->dcb.size() == 4);
    pool.Return(std::move(empty));
    pool.Return(std::move(only_dcb));
    pool.Return(nullptr);
    CHECK(pool.SpareCount() == 2);
}

void TestSizeBoundary() {
    CommandSnapshots pool;
    CHECK(CheckSnapshotSize(MaxSnapshotWords, 0) == SnapshotStatus::Ok);
    CHECK(CheckSnapshotSize(0, MaxSnapshotWords) == SnapshotStatus::Ok);
    CHECK(CheckSnapshotSize(MaxSnapshotWords - 5, 5) == SnapshotStatus::Ok);
    CHECK(CheckSnapshotSize(MaxSnapshotWords - 5, 6) == SnapshotStatus::TooLarge);
    CHECK(CheckSnapshotSize(MaxSnapshotWords + 1, 0) == SnapshotStatus::TooLarge);
    CHECK(CheckSnapshotSize(0, MaxSnapshotWords + 1) == SnapshotStatus::TooLarge);

    std::vector<u32> biggest(MaxSnapshotWords, 7u);
    auto snapshot = pool.Take(biggest, {});
    CHECK(snapshot != nullptr);
    CHECK(snapshot->dcb.size() == MaxSnapshotWords);
    CHECK(snapshot->dcb.front() == 7u && snapshot->dcb.back() == 7u);
    pool.Return(std::move(snapshot));
    CHECK(pool.SpareCount() == 0);
}

void TestOversizeAndOverflowRejected() {
    CommandSnapshots pool;
    const u32 word = 0;
    const std::span<const u32> fake_huge{&word, MaxSnapshotWords + 1};
    CHECK(pool.Take(fake_huge, {}) == nullptr);
    CHECK(pool.Take({}, fake_huge) == nullptr);
    CHECK(pool.Take(fake_huge, fake_huge) == nullptr);

    const size_t max = std::numeric_limits<size_t>::max();
    CHECK(CheckSnapshotSize(max, 1) == SnapshotStatus::TooLarge);
    CHECK(CheckSnapshotSize(1, max) == SnapshotStatus::TooLarge);
    CHECK(CheckSnapshotSize(max, max) == SnapshotStatus::TooLarge);
    CHECK(CheckSnapshotSize(max / 2 + 1, max / 2 + 1) == SnapshotStatus::TooLarge);
    CHECK(pool.SpareCount() == 0);
}

void TestSnapshotLifetimeAndRelease() {
    CommandSnapshots pool;
    auto guest = std::make_unique<std::vector<u32>>(Words(40, 32));
    const auto submitted = *guest;
    std::promise<void> started, finish;
    std::atomic<bool> intact{false};
    std::atomic<bool> done{false};
    auto finished = finish.get_future();
    Queue queue{pool, [&](const CommandSnapshot& s) {
                    started.set_value();
                    finished.wait();
                    intact = Equals(s.dcb, submitted);
                    done = true;
                }};
    queue.Submit(pool.Take(*guest, {}));
    started.get_future().wait();
    guest.reset();
    CHECK(pool.SpareCount() == 0);
    finish.set_value();
    queue.Drain();
    CHECK(done && intact);
    CHECK(pool.SpareCount() == 1);

    auto reused = pool.Take(Words(9, 4), {});
    CHECK(reused != nullptr && reused->dcb.size() == 4);
    CHECK(pool.SpareCount() == 0);
    pool.Return(std::move(reused));
}

void TestSpareBounds() {
    CommandSnapshots pool;
    std::vector<std::unique_ptr<CommandSnapshot>> taken;
    for (size_t i = 0; i < CommandSnapshots::MaxSpare + 4; ++i) {
        taken.push_back(pool.Take(Words(1, 4), {}));
    }
    for (auto& snapshot : taken) {
        pool.Return(std::move(snapshot));
    }
    CHECK(pool.SpareCount() == CommandSnapshots::MaxSpare);

    CommandSnapshots big_pool;
    std::vector<u32> large(MaxRetainedWords + 1, 1u);
    auto big = big_pool.Take(large, {});
    CHECK(big != nullptr);
    big_pool.Return(std::move(big));
    CHECK(big_pool.SpareCount() == 0);
}

void TestShutdownDrain() {
    CommandSnapshots pool;
    std::atomic<u32> consumed{0};
    std::atomic<u32> mismatched{0};
    std::vector<std::vector<u32>> originals;
    {
        auto guests = std::make_unique<std::vector<std::vector<u32>>>();
        for (u32 i = 0; i < 20; ++i) {
            guests->push_back(Words(i * 50, 30));
        }
        originals = *guests;
        Queue queue{pool, [&](const CommandSnapshot& s) {
                        const u32 index = consumed.fetch_add(1);
                        if (index >= originals.size() || !Equals(s.dcb, originals[index])) {
                            ++mismatched;
                        }
                    }};
        queue.Hold();
        for (auto& guest : *guests) {
            queue.Submit(pool.Take(guest, {}));
        }
        guests.reset();
        // The queue is destroyed with work pending: it drains instead of dropping or reading freed guest memory.
    }
    CHECK(consumed == 20);
    CHECK(mismatched == 0);
    CHECK(pool.SpareCount() == CommandSnapshots::MaxSpare);
}

void TestEngineWiring() {
    const auto liverpool = ReadSource("gpu/shadps4/video_core/amdgpu/liverpool.cpp");
    const auto header = ReadSource("gpu/shadps4/video_core/amdgpu/liverpool.h");
    const auto check = liverpool.find("CheckSnapshotSize(dcb.size(), ccb.size())");
    const auto take = liverpool.find("command_snapshots.Take(dcb, {})");
    CHECK(check != std::string::npos && take != std::string::npos && check < take);
    CHECK(liverpool.find("ProcessGraphics(commands->dcb, seq, commands->guest_dcb)") != std::string::npos);
    CHECK(liverpool.find("queue.submits.push({task.handle, std::move(commands)})") != std::string::npos);
    CHECK(liverpool.find("command_snapshots.Return(std::move(queue.submits.front().commands))") != std::string::npos);
    CHECK(liverpool.find("std::vector<u32> ce_words(ccb.begin(), ccb.end())") != std::string::npos);
    CHECK(header.find("std::unique_ptr<CommandSnapshot> commands") != std::string::npos);
    const auto destructor = liverpool.find("Liverpool::~Liverpool()");
    const auto join = liverpool.find("process_thread.join()", destructor);
    const auto destroy = liverpool.find("queue.submits.front().task.destroy()", destructor);
    CHECK(destructor != std::string::npos && join != std::string::npos && destroy != std::string::npos &&
          join < destroy);
}

} // namespace

int main() {
    TestSnapshotIsIsolatedFromGuest();
    TestWorkerSeesSnapshotNotMutatedGuest();
    TestOrderingAndMultipleSubmissionsIsolated();
    TestZeroLength();
    TestSizeBoundary();
    TestOversizeAndOverflowRejected();
    TestSnapshotLifetimeAndRelease();
    TestSpareBounds();
    TestShutdownDrain();
    TestEngineWiring();
    std::printf("command snapshot tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

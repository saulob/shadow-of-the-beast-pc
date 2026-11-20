// GPU memory fallback tests: the order and validity of the memory types an arena backing may use when the preferred
// one is full, which errors move on and which stop, the telemetry, and the hand-off to the device-lost state. Fake
// memory types and allocations; no Vulkan device, no real allocation.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "video_core/buffer_cache/backing_memory.h"
#include "video_core/renderer_vulkan/vk_device_lost.h"

namespace {

using namespace VideoCore::BackingMemory;
namespace DeviceLost = Vulkan::DeviceLost;

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

std::string ReadSource(const std::string& relative) {
    std::ifstream file(std::string(SOTB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    std::string source = text.str();
    source.erase(std::remove(source.begin(), source.end(), char(13)), source.end());
    return source;
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// 0: device local (preferred), 1: device local, 2: system, 3: system, 4: device local but lazily allocated,
// 5: system and protected.
std::vector<TypeTraits> Types() {
    return {{true, false}, {true, false}, {false, false}, {false, false}, {true, true}, {false, true}};
}

constexpr std::uint32_t AllBits = 0xffffffffu;

struct FakeDriver {
    FakeDriver(std::initializer_list<std::int32_t> results) : script(results) {}

    std::vector<std::uint32_t> attempted;
    std::vector<std::int32_t> script; // result per attempt; the last one repeats

    std::int32_t operator()(std::uint32_t type) {
        attempted.push_back(type);
        const auto index = std::min(attempted.size(), script.size()) - 1;
        return script[index];
    }
};

void TestPreferredTypeSucceeds() {
    const auto candidates = CandidateTypes(Types(), AllBits, 0);
    FakeDriver driver{ResultSuccess};
    const Allocation allocation = AllocateBacking(candidates, std::ref(driver));
    CHECK(allocation.result == ResultSuccess && allocation.type == 0 && allocation.attempts == 1);
    CHECK(driver.attempted == std::vector<std::uint32_t>{0});
    CHECK(Classify(Types(), allocation.type, 0) == Placement::PreferredDeviceLocal);
}

void TestPreferredOutOfDeviceMemoryFallsToAnotherLocalType() {
    const auto candidates = CandidateTypes(Types(), AllBits, 0);
    FakeDriver driver{ResultOutOfDeviceMemory, ResultSuccess};
    const Allocation allocation = AllocateBacking(candidates, std::ref(driver));
    CHECK(allocation.result == ResultSuccess && allocation.type == 1 && allocation.attempts == 2);
    CHECK(Classify(Types(), allocation.type, 0) == Placement::OtherDeviceLocal);
}

void TestLocalTypesFullThenSystemMemory() {
    const auto candidates = CandidateTypes(Types(), AllBits, 0);
    FakeDriver driver{ResultOutOfDeviceMemory, ResultOutOfDeviceMemory, ResultSuccess};
    const Allocation allocation = AllocateBacking(candidates, std::ref(driver));
    CHECK(allocation.result == ResultSuccess && allocation.type == 2 && allocation.attempts == 3);
    CHECK(Classify(Types(), allocation.type, 0) == Placement::SystemMemory);
    CHECK(driver.attempted == (std::vector<std::uint32_t>{0, 1, 2}));
}

void TestErrorsThatStopTheSearch() {
    const auto candidates = CandidateTypes(Types(), AllBits, 0);
    for (const std::int32_t error : {-3, -5, -1000001004, 7, ResultOutOfHostMemory}) {
        FakeDriver driver{error, ResultSuccess};
        const Allocation allocation = AllocateBacking(candidates, std::ref(driver));
        CHECK(allocation.result == error && allocation.type == 0 && allocation.attempts == 1);
        CHECK(driver.attempted.size() == 1);
    }

    FakeDriver late{ResultOutOfDeviceMemory, ResultOutOfHostMemory, ResultSuccess};
    const Allocation allocation = AllocateBacking(candidates, std::ref(late));
    CHECK(allocation.result == ResultOutOfHostMemory && allocation.attempts == 2 && late.attempted.size() == 2);

    FakeDriver lost{ResultOutOfDeviceMemory, ResultDeviceLost, ResultSuccess};
    const Allocation lost_allocation = AllocateBacking(candidates, std::ref(lost));
    CHECK(lost_allocation.result == ResultDeviceLost && lost_allocation.attempts == 2);
    CHECK(lost.attempted.size() == 2);
}

void TestNoCompatibleType() {
    CHECK(CandidateTypes(Types(), 0, 0).empty());
    CHECK(CandidateTypes({}, AllBits, std::nullopt).empty());
    CHECK(CandidateTypes(Types(), 0b110000, 4).empty());
    FakeDriver driver{ResultSuccess};
    const auto candidates = CandidateTypes(Types(), 0, 0);
    const Allocation allocation = AllocateBacking(candidates, std::ref(driver));
    CHECK(allocation.attempts == 0 && driver.attempted.empty());
    CHECK(allocation.result == ResultOutOfDeviceMemory);
}

void TestPreferredAppearsOnce() {
    for (std::uint32_t preferred = 0; preferred < 4; ++preferred) {
        const auto candidates = CandidateTypes(Types(), AllBits, preferred);
        CHECK(std::count(candidates.begin(), candidates.end(), preferred) == 1);
        CHECK(!candidates.empty() && candidates.front() == preferred);
    }
}

void TestInvalidTypesAreNeverAttempted() {
    const auto types = Types();
    // The memory requirements allow types 1, 3 and 4 only; type 4 is excluded, bits 6 and up have no type.
    const auto candidates = CandidateTypes(types, (1u << 1) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 31), 1);
    CHECK(candidates == (std::vector<std::uint32_t>{1, 3}));
    // A preferred type the requirements do not allow is not tried either.
    const auto not_allowed = CandidateTypes(types, (1u << 1) | (1u << 2), 0);
    CHECK(not_allowed == (std::vector<std::uint32_t>{1, 2}));
    const auto excluded_preferred = CandidateTypes(types, AllBits, 4);
    CHECK(std::find(excluded_preferred.begin(), excluded_preferred.end(), 4) == excluded_preferred.end());
    CHECK(std::find(excluded_preferred.begin(), excluded_preferred.end(), 5) == excluded_preferred.end());
    const auto out_of_range = CandidateTypes(types, AllBits, 40);
    CHECK(out_of_range == (std::vector<std::uint32_t>{0, 1, 2, 3}));
    std::vector<TypeTraits> many(40, TypeTraits{true, false});
    CHECK(CandidateTypes(many, AllBits, 0).size() == MaxMemoryTypes);
}

void TestCandidateOrderIsDeterministic() {
    const auto first = CandidateTypes(Types(), AllBits, 1);
    CHECK(first == (std::vector<std::uint32_t>{1, 0, 2, 3}));
    for (int i = 0; i < 5; ++i) {
        CHECK(CandidateTypes(Types(), AllBits, 1) == first);
    }
    const auto no_preferred = CandidateTypes(Types(), AllBits, std::nullopt);
    CHECK(no_preferred == (std::vector<std::uint32_t>{0, 1, 2, 3}));
}

void TestEachCandidateIsAttemptedOnceAndAllFull() {
    const auto candidates = CandidateTypes(Types(), AllBits, 0);
    FakeDriver driver{ResultOutOfDeviceMemory};
    const Allocation allocation = AllocateBacking(candidates, std::ref(driver));
    CHECK(allocation.result == ResultOutOfDeviceMemory);
    CHECK(allocation.attempts == candidates.size());
    CHECK(driver.attempted == candidates);
    const std::set<std::uint32_t> unique(driver.attempted.begin(), driver.attempted.end());
    CHECK(unique.size() == driver.attempted.size());
}

void TestAllocationSizeAndIndexArePassedThrough() {
    const auto candidates = CandidateTypes(Types(), AllBits, 0);
    std::vector<std::pair<std::uint32_t, std::uint64_t>> seen;
    const std::uint64_t size = 96ull << 20;
    int attempt = 0;
    const Allocation allocation = AllocateBacking(candidates, [&](std::uint32_t type) {
        seen.emplace_back(type, size);
        return ++attempt < 3 ? ResultOutOfDeviceMemory : ResultSuccess;
    });
    CHECK(allocation.type == seen.back().first && allocation.type == candidates[2]);
    CHECK(seen.size() == 3);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        CHECK(seen[i].first == candidates[i]);
        CHECK(seen[i].second == size);
    }
}

void TestSparseAndFullyResidentCompatibility() {
    const auto cache = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp");
    const auto ensure = cache.find("void BufferCache::EnsureResident");
    CHECK(ensure != std::string::npos);
    const auto resident_only = cache.find("if (!sparse_supported) {", ensure);
    const auto allocate = cache.find("AllocateBacking(backing_memory_types", ensure);
    CHECK(resident_only != std::string::npos && allocate != std::string::npos && resident_only < allocate);
    CHECK(Contains(cache, "vk::MemoryAllocateFlagBits::eDeviceAddress"));
    CHECK(Contains(cache, ".allocationSize = *backing_bytes"));
    CHECK(Contains(cache, "alloc_info.memoryTypeIndex = type;"));
    CHECK(!Contains(cache, "spill_memory_type_index"));
    CHECK(Contains(cache, "BackingMemory::CandidateTypes(memory_type_traits, reqs.memoryTypeBits"));
    CHECK(Contains(cache, "vk::MemoryPropertyFlagBits::eLazilyAllocated"));
    CHECK(Contains(cache, "vk::MemoryPropertyFlagBits::eProtected"));

    // Fully resident arenas and images take their memory from VMA, which already moves on to host memory.
    const auto buffer = ReadSource("gpu/shadps4/video_core/buffer_cache/buffer.cpp");
    CHECK(Contains(buffer, "VMA_MEMORY_USAGE_AUTO_PREFER_HOST"));
    CHECK(Contains(buffer, "DeviceLost::Operation::AllocateMemory"));
    const auto image = ReadSource("gpu/shadps4/video_core/texture_cache/image.cpp");
    CHECK(Contains(image, "VMA_MEMORY_USAGE_AUTO_PREFER_HOST"));
    CHECK(Contains(image, "DeviceLost::Operation::AllocateMemory"));
}

void TestTelemetryClassificationAndRateLimit() {
    const auto types = Types();
    CHECK(Classify(types, 0, 0) == Placement::PreferredDeviceLocal);
    CHECK(Classify(types, 1, 0) == Placement::OtherDeviceLocal);
    CHECK(Classify(types, 2, 0) == Placement::SystemMemory);
    CHECK(Classify(types, 3, 1) == Placement::SystemMemory);
    CHECK(Classify(types, 99, 0) == Placement::SystemMemory);

    FallbackTelemetry telemetry;
    int logged = 0;
    for (int i = 0; i < 1000; ++i) {
        logged += telemetry.Record(1 << 20, i % 2 ? Placement::SystemMemory : Placement::OtherDeviceLocal) ? 1 : 0;
    }
    CHECK(telemetry.Events() == 1000);
    CHECK(telemetry.TotalBytes() == 1000ull << 20);
    CHECK(telemetry.SystemEvents() == 500 && telemetry.SystemBytes() == 500ull << 20);
    CHECK(logged < 30 && logged >= int(FallbackTelemetry::LoggedFirst));
    FallbackTelemetry fresh;
    CHECK(fresh.Record(1, Placement::SystemMemory));
}

void TestLargeAllocationArithmetic() {
    CHECK(BackingBytes(0, 16) == 0);
    CHECK(BackingBytes(1000, 16) == 1000ull << 16);
    CHECK(BackingBytes(0xffffffffull, 30) == (0xffffffffull << 30));
    CHECK(BackingBytes(UINT64_MAX >> 16, 16).has_value());
    CHECK(!BackingBytes((UINT64_MAX >> 16) + 1, 16).has_value());
    CHECK(!BackingBytes(1, 64).has_value());
    CHECK(!BackingBytes(UINT64_MAX, 1).has_value());
    CHECK(BackingBytes(1, 63) == (1ull << 63));
}

void TestDeviceLostRoutesThroughTheSharedState() {
    DeviceLost::Monitor monitor;
    const auto candidates = CandidateTypes(Types(), AllBits, 0);
    int reached = 0;
    const auto allocate = [&](std::uint32_t) {
        return DeviceLost::CallWhileHealthy(monitor, DeviceLost::Operation::AllocateMemory, "arena backing", 0, [&] {
            ++reached;
            return reached == 1 ? ResultOutOfDeviceMemory : ResultDeviceLost;
        });
    };
    const Allocation allocation = AllocateBacking(candidates, allocate);
    CHECK(allocation.result == ResultDeviceLost && allocation.attempts == 2 && reached == 2);
    CHECK(monitor.IsLost());
    CHECK(monitor.GetIncident().operation == DeviceLost::Operation::AllocateMemory);
    CHECK(std::string(monitor.GetIncident().stage) == "arena backing");

    // Later allocations never reach the lost device.
    const Allocation after = AllocateBacking(candidates, allocate);
    CHECK(after.result == ResultDeviceLost && after.attempts == 1 && reached == 2);

    // A full device alone does not mark the device lost.
    DeviceLost::Monitor healthy;
    const Allocation full = AllocateBacking(candidates, [&](std::uint32_t) {
        return DeviceLost::CallWhileHealthy(healthy, DeviceLost::Operation::AllocateMemory, "arena backing", 0,
                                            [] { return ResultOutOfDeviceMemory; });
    });
    CHECK(full.result == ResultOutOfDeviceMemory && !healthy.IsLost());
    CHECK(Contains(ReadSource("gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp"),
                   "DeviceLost::Operation::AllocateMemory, \"arena backing\""));
}

} // namespace

int main() {
    TestPreferredTypeSucceeds();
    TestPreferredOutOfDeviceMemoryFallsToAnotherLocalType();
    TestLocalTypesFullThenSystemMemory();
    TestErrorsThatStopTheSearch();
    TestNoCompatibleType();
    TestPreferredAppearsOnce();
    TestInvalidTypesAreNeverAttempted();
    TestCandidateOrderIsDeterministic();
    TestEachCandidateIsAttemptedOnceAndAllFull();
    TestAllocationSizeAndIndexArePassedThrough();
    TestSparseAndFullyResidentCompatibility();
    TestTelemetryClassificationAndRateLimit();
    TestLargeAllocationArithmetic();
    TestDeviceLostRoutesThroughTheSharedState();
    std::printf("gpu memory fallback tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

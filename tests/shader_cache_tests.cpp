// Shader cache tests: when the background loading note may appear, what the persistent cache is keyed by, where it
// lives and that builds do not share it. Synthetic data in temporary folders; no Vulkan device, no window, no real cache.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "sotb_background_loading.h"
#include "video_core/pipeline_cache_policy.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;

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

struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) : path{fs::temp_directory_path() / name} {
        std::error_code error;
        fs::remove_all(path, error);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::string ReadSource(const std::string& relative) {
    std::ifstream file(std::string(SOTB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

Bytes ReadBytes(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
    const auto start = text.find(from);
    if (start == std::string::npos) {
        return {};
    }
    const auto end = text.find(to, start);
    return end == std::string::npos ? std::string{} : text.substr(start, end - start);
}

Sotb::BackgroundLoadingClock::Rep At(std::chrono::milliseconds ms) {
    return std::chrono::steady_clock::duration{ms}.count() + 1'000'000;
}

PipelineCachePolicy::StoreIdentity Rtx4070() {
    PipelineCachePolicy::StoreIdentity id;
    id.vendor_id = 0x10de;
    id.device_id = 0x2786;
    id.driver_id = 4;
    id.driver_version = 0x80f9c040;
    id.api_version = 0x004040dd;
    id.shader_binary_version = 3;
    id.shader_meta_version = 5;
    id.pipeline_key_version = 8;
    id.pipeline_cache_uuid = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    return id;
}

void TestNoteHiddenBelowThreshold() {
    Sotb::BackgroundLoadingClock clock;
    CHECK(!clock.Shown(At(0ms)));
    clock.Report(346, At(0ms));
    CHECK(!clock.Shown(At(0ms)));
    clock.Report(346, At(300ms));
    CHECK(!clock.Shown(At(300ms)));
    CHECK(!clock.Shown(At(499ms)));
    clock.Report(0, At(320ms));
    CHECK(!clock.Shown(At(320ms)));
    CHECK(!clock.Shown(At(5000ms)));
}

void TestNoteAppearsWhenLoadingTakesLong() {
    Sotb::BackgroundLoadingClock clock;
    clock.Report(346, At(0ms));
    CHECK(!clock.Shown(At(499ms)));
    CHECK(clock.Shown(At(500ms)));
    for (int step = 1; step <= 100; ++step) {
        clock.Report(346, At(std::chrono::milliseconds(step * 100)));
    }
    CHECK(clock.Shown(At(10000ms)));
}

void TestNoteLeavesWithTheLoad() {
    Sotb::BackgroundLoadingClock clock;
    clock.Report(346, At(0ms));
    clock.Report(346, At(2000ms));
    CHECK(clock.Shown(At(2000ms)));
    clock.Report(0, At(2010ms));
    CHECK(!clock.Shown(At(2010ms)));
    CHECK(!clock.Shown(At(60000ms)));
}

void TestEachLoadStartsItsOwnDelay() {
    Sotb::BackgroundLoadingClock clock;
    clock.Report(100, At(0ms));
    clock.Report(0, At(900ms));
    clock.Report(100, At(5000ms));
    CHECK(!clock.Shown(At(5100ms)));
    CHECK(clock.Shown(At(5500ms)));
}

void TestSlowReportsDoNotRestartTheDelay() {
    Sotb::BackgroundLoadingClock clock;
    clock.Report(200, At(0ms));
    clock.Report(200, At(400ms));
    clock.Report(200, At(800ms));
    CHECK(clock.Shown(At(800ms)));
}

void TestOverlayUsesTheGate() {
    const std::string overlay = ReadSource("gpu/shim/sotb_overlay.cpp");
    const std::string visible = Between(overlay, "bool Visible() {", "\n}\n");
    CHECK(!visible.empty());
    CHECK(visible.find("BackgroundLoadingShown()") != std::string::npos);
    CHECK(visible.find("background_total") == std::string::npos);
    CHECK(overlay.find("if (BackgroundLoadingShown() && !loading) {") != std::string::npos);
    CHECK(overlay.find("if (background_total && !loading)") == std::string::npos);
    const std::string set = Between(overlay, "void SetBackgroundLoading(", "\n}\n");
    CHECK(set.find("background_clock.Report(total,") != std::string::npos);
}

void TestProgressEndsAndLeaves() {
    const std::string serialization =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    CHECK(serialization.find("SetBackgroundLoading(done, std::max(warm.total.load(), done))") !=
          std::string::npos);
    const std::string finish = Between(serialization, "warm.finished = true;", "FinishPreload();");
    CHECK(finish.find("SetBackgroundLoading(0, 0)") != std::string::npos);
    const std::string stop = Between(serialization, "void PipelineCache::StopWarmUp()", "\n}\n");
    CHECK(stop.find("SetBackgroundLoading(0, 0)") != std::string::npos);
}

void TestIdentityIsDeterministic() {
    using namespace PipelineCachePolicy;
    const auto first = EncodeStoreIdentity(Rtx4070());
    const auto second = EncodeStoreIdentity(Rtx4070());
    CHECK(first == second);
    CHECK(CheckStore(first, Rtx4070()) == StoreVerdict::Reuse);
    CHECK(CheckStore(second, Rtx4070()) == StoreVerdict::Reuse);
}

void TestRelevantChangesInvalidate() {
    using namespace PipelineCachePolicy;
    const auto stored = EncodeStoreIdentity(Rtx4070());
    const auto invalidates = [&](auto change) {
        auto id = Rtx4070();
        change(id);
        return CheckStore(stored, id) == StoreVerdict::Incompatible;
    };
    CHECK(invalidates([](StoreIdentity& id) { id.vendor_id = 0x1002; }));
    CHECK(invalidates([](StoreIdentity& id) { id.device_id = 0x2204; }));
    CHECK(invalidates([](StoreIdentity& id) { id.driver_id = 1; }));
    CHECK(invalidates([](StoreIdentity& id) { id.driver_version += 1; }));
    CHECK(invalidates([](StoreIdentity& id) { id.api_version += 1; }));
    CHECK(invalidates([](StoreIdentity& id) { id.pipeline_cache_uuid[15] ^= 1; }));
    CHECK(invalidates([](StoreIdentity& id) { id.format_version += 1; }));
    CHECK(invalidates([](StoreIdentity& id) { id.shader_binary_version += 1; }));
    CHECK(invalidates([](StoreIdentity& id) { id.shader_meta_version += 1; }));
    CHECK(invalidates([](StoreIdentity& id) { id.pipeline_key_version += 1; }));
}

void TestIdentityHasNoRunDependentInput() {
    const std::string source =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    const std::string identity = Between(source, "StoreIdentity current{", "};");
    CHECK(!identity.empty());
    for (const char* forbidden : {"chrono", "time(", "getpid", "GetCurrentProcessId", "rand", "EmulatorSettings",
                                  "SotbSettings", "GetEnv", "getenv", "this", "path"}) {
        CHECK(identity.find(forbidden) == std::string::npos);
    }
    for (const char* wanted : {"GetVendorID()", "GetDeviceID()", "GetDriverID()", "GetDriverVersion()", "ApiVersion()",
                               "ShaderBinaryVersion", "ShaderMetaVersion", "PipelineKeyVersion"}) {
        CHECK(identity.find(wanted) != std::string::npos);
    }
    CHECK(source.find("GetPipelineCacheUUID()") != std::string::npos);
}

void TestCacheIsOnlyClearedOnAMismatch() {
    const std::string source =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    const std::string check = Between(source, "bool PipelineCache::CheckCacheProfile()", "\n}\n");
    const auto reuse = check.find("verdict == StoreVerdict::Reuse && profile_match");
    const auto clear = check.find("db.Clear()");
    CHECK(reuse != std::string::npos && clear != std::string::npos && reuse < clear);
    CHECK(check.find("return true;", reuse) < clear);
    CHECK(source.find(".Clear()") == source.rfind(".Clear()"));
}

void TestCacheLivesInTheWritableUserFolder() {
    const std::string paths = ReadSource("gpu/shadps4/common/path_util.cpp");
    const std::string header = ReadSource("gpu/shadps4/common/path_util.h");
    const std::string storage = ReadSource("gpu/shadps4/video_core/cache_storage.cpp");
    CHECK(header.find("constexpr auto CACHE_DIR = \"cache\";") != std::string::npos);
    CHECK(paths.find("{PathType::CacheDir, user_dir / CACHE_DIR}") != std::string::npos);
    CHECK(storage.find("GetUserPath(PathType::CacheDir) / game_info.GameSerial()") != std::string::npos);
    for (const char* readonly : {"/app0", "game/prepared", "game/image", "temp_directory_path", "TempDataDir"}) {
        CHECK(storage.find(readonly) == std::string::npos);
    }
    const std::string open = Between(storage, "void DataBase::Open()", "\n}\n");
    CHECK(open.find("IsStaleTemp(entry.path())") != std::string::npos);
    CHECK(open.find("remove_all") == std::string::npos);
    CHECK(storage.find("IsCacheArtifact(entry.path())") != std::string::npos);
    CHECK(storage.find("remove_all") == std::string::npos);
}

void TestBuildsDoNotShareACache() {
    using namespace PipelineCachePolicy;
    TempDir root{"sotb_shader_cache_builds"};
    const fs::path usa = root.path / "CUSA02871";
    const fs::path korea = root.path / "CUSA03762";
    fs::create_directories(usa);
    fs::create_directories(korea);

    auto stamp = EncodeStoreIdentity(Rtx4070());
    CHECK(Seal(stamp));
    CHECK(WriteFileAtomically(usa / "identity.bin", stamp));

    auto stored = ReadBytes(usa / "identity.bin");
    CHECK(Unseal(stored) && CheckStore(stored, Rtx4070()) == StoreVerdict::Reuse);
    auto other = ReadBytes(korea / "identity.bin");
    CHECK(other.empty() && CheckStore(other, Rtx4070()) == StoreVerdict::Missing);
    CHECK(!fs::exists(korea / "identity.bin"));
}

void TestStoredBlobsReuseAndRejection() {
    using namespace PipelineCachePolicy;
    TempDir dir{"sotb_shader_cache_blobs"};
    Bytes payload(4096);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = std::uint8_t(i * 31 + 7);
    }
    auto sealed = payload;
    CHECK(Seal(sealed));
    const fs::path file = dir.path / "0x0000000000000001.meta";
    CHECK(WriteFileAtomically(file, sealed));
    CHECK(!fs::exists(file.string() + ".tmp"));

    auto again = ReadBytes(file);
    CHECK(Unseal(again) && again == payload);

    auto truncated = ReadBytes(file);
    truncated.resize(truncated.size() / 2);
    const auto truncated_copy = truncated;
    CHECK(!Unseal(truncated) && truncated == truncated_copy);

    auto flipped = ReadBytes(file);
    flipped[100] ^= 0x40;
    CHECK(!Unseal(flipped));

    auto zeroed = Bytes(sealed.size(), 0);
    CHECK(!Unseal(zeroed));
    Bytes empty;
    CHECK(!Unseal(empty));
}

void TestCorruptIdentityFailsSafely() {
    using namespace PipelineCachePolicy;
    auto stamp = EncodeStoreIdentity(Rtx4070());
    auto cut = stamp;
    cut.resize(cut.size() - 5);
    CHECK(CheckStore(cut, Rtx4070()) == StoreVerdict::Corrupt);
    stamp[0] ^= 0xff;
    CHECK(CheckStore(stamp, Rtx4070()) == StoreVerdict::Corrupt);
    CHECK(CheckStore({}, Rtx4070()) == StoreVerdict::Missing);
}

} // namespace

int main() {
    TestNoteHiddenBelowThreshold();
    TestNoteAppearsWhenLoadingTakesLong();
    TestNoteLeavesWithTheLoad();
    TestEachLoadStartsItsOwnDelay();
    TestSlowReportsDoNotRestartTheDelay();
    TestOverlayUsesTheGate();
    TestProgressEndsAndLeaves();
    TestIdentityIsDeterministic();
    TestRelevantChangesInvalidate();
    TestIdentityHasNoRunDependentInput();
    TestCacheIsOnlyClearedOnAMismatch();
    TestCacheLivesInTheWritableUserFolder();
    TestBuildsDoNotShareACache();
    TestStoredBlobsReuseAndRejection();
    TestCorruptIdentityFailsSafely();
    std::printf("shader cache tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

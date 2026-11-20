// Pipeline cache tests: blob sealing, host identity, atomic writes, stale temporaries and which files the cache may
// delete, on synthetic data in temporary folders. No Vulkan device, no real cache file is read or written.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "common/serdes.h"
#include "video_core/pipeline_cache_policy.h"

namespace {

using namespace PipelineCachePolicy;
namespace fs = std::filesystem;
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

Bytes ReadAll(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

void WriteRaw(const fs::path& file, const Bytes& bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

Bytes Sealed(Bytes payload) {
    CHECK(Seal(payload));
    return payload;
}

std::string ReadSource(const std::string& relative) {
    std::ifstream file(std::string(SOTB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

StoreIdentity Rx580() {
    StoreIdentity id;
    id.vendor_id = 0x1002;
    id.device_id = 0x6fdf;
    id.driver_id = 1;
    id.driver_version = 0x800117;
    id.api_version = 0x403104;
    id.shader_binary_version = 20;
    id.shader_meta_version = 10;
    id.pipeline_key_version = 8;
    for (std::size_t i = 0; i < id.pipeline_cache_uuid.size(); ++i) {
        id.pipeline_cache_uuid[i] = std::uint8_t(0xa0 + i);
    }
    return id;
}

void TestValidCacheLoads() {
    const Bytes payload{1, 2, 3, 4, 5, 6, 7, 8, 9};
    auto blob = Sealed(payload);
    CHECK(blob.size() == payload.size() + SealSize);
    CHECK(Unseal(blob));
    CHECK(blob == payload);

    Bytes empty;
    CHECK(Seal(empty));
    CHECK(empty.size() == SealSize);
    CHECK(Unseal(empty));
    CHECK(empty.empty());

    const auto id = Rx580();
    const auto stored = EncodeStoreIdentity(id);
    CHECK(CheckStore(stored, id) == StoreVerdict::Reuse);
    CHECK(DescribeVerdict(StoreVerdict::Reuse, stored, id) == "identity matches");
}

void TestCorruptHeaderRejected() {
    const Bytes payload{9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    const auto good = Sealed(payload);
    for (std::size_t field = 0; field < 4; ++field) {
        auto bad = good;
        bad[payload.size() + field] ^= 0xff;
        const auto before = bad;
        CHECK(!Unseal(bad));
        CHECK(bad == before);
    }
    auto flipped = good;
    flipped[3] ^= 0x01;
    CHECK(!Unseal(flipped));
    auto bad_version = good;
    bad_version[payload.size() + 4] ^= 0x01;
    CHECK(!Unseal(bad_version));

    Bytes zeros(64, 0);
    CHECK(!Unseal(zeros));
    Bytes foreign(64, 0xcd);
    CHECK(!Unseal(foreign));
    Bytes none;
    CHECK(!Unseal(none));
}

void TestTruncatedRejected() {
    const Bytes payload(200, 0x5a);
    const auto good = Sealed(payload);
    for (std::size_t length = 0; length < good.size(); ++length) {
        Bytes cut(good.begin(), good.begin() + std::ptrdiff_t(length));
        CHECK(!Unseal(cut));
    }
    Bytes extended = good;
    extended.push_back(0);
    CHECK(!Unseal(extended));
}

void TestInvalidLengthRejected() {
    const Bytes payload(32, 0x11);
    const auto good = Sealed(payload);
    auto too_long = good;
    const std::uint64_t claimed = payload.size() + 1;
    std::memcpy(too_long.data() + payload.size() + 8, &claimed, sizeof(claimed));
    CHECK(!Unseal(too_long));

    auto huge = good;
    const std::uint64_t absurd = ~std::uint64_t{0};
    std::memcpy(huge.data() + payload.size() + 8, &absurd, sizeof(absurd));
    CHECK(!Unseal(huge));

    auto wrong_hash = good;
    wrong_hash[payload.size() + 16] ^= 0x01;
    CHECK(!Unseal(wrong_hash));

    Bytes oversized(MaxBlobBytes + SealSize + 1, 0);
    CHECK(!Unseal(oversized));

    bool threw = false;
    std::vector<std::uint32_t> list;
    try {
        Serialization::Archive lying_ar{Bytes(sizeof(std::uint64_t), 0xff)};
        Serialization::Reader lying_reader{lying_ar};
        lying_reader.Read(list);
    } catch (const Serialization::CorruptData&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(list.empty());

    threw = false;
    try {
        Serialization::Archive short_ar{Bytes(3, 0)};
        Serialization::Reader short_reader{short_ar};
        std::uint64_t value = 0;
        short_reader.Read(value);
    } catch (const Serialization::CorruptData&) {
        threw = true;
    }
    CHECK(threw);
}

void TestIdentityMismatches() {
    const auto id = Rx580();
    const auto stored = EncodeStoreIdentity(id);

    auto other_gpu = id;
    other_gpu.device_id = 0x73bf;
    CHECK(CheckStore(stored, other_gpu) == StoreVerdict::Incompatible);
    CHECK(DescribeChange(id, other_gpu) == "GPU device");
    auto other_vendor = id;
    other_vendor.vendor_id = 0x10de;
    CHECK(DescribeChange(id, other_vendor) == "GPU vendor");

    auto other_driver = id;
    other_driver.driver_version += 1;
    CHECK(CheckStore(stored, other_driver) == StoreVerdict::Incompatible);
    CHECK(DescribeChange(id, other_driver) == "driver version");
    auto other_driver_id = id;
    other_driver_id.driver_id = 4;
    CHECK(DescribeChange(id, other_driver_id) == "driver");
    auto other_api = id;
    other_api.api_version += 1;
    CHECK(DescribeChange(id, other_api) == "Vulkan version");

    auto other_uuid = id;
    other_uuid.pipeline_cache_uuid[15] ^= 1;
    CHECK(CheckStore(stored, other_uuid) == StoreVerdict::Incompatible);
    CHECK(DescribeChange(id, other_uuid) == "pipeline cache UUID");

    auto other_format = id;
    other_format.format_version += 1;
    CHECK(CheckStore(stored, other_format) == StoreVerdict::Incompatible);
    CHECK(DescribeChange(id, other_format) == "cache format");
    auto other_binary = id;
    other_binary.shader_binary_version += 1;
    CHECK(DescribeChange(id, other_binary) == "shader binary format");
    auto other_meta = id;
    other_meta.shader_meta_version += 1;
    CHECK(DescribeChange(id, other_meta) == "shader meta format");
    auto other_key = id;
    other_key.pipeline_key_version += 1;
    CHECK(DescribeChange(id, other_key) == "pipeline key format");
    auto other_backend = id;
    other_backend.backend = 2;
    CHECK(CheckStore(stored, other_backend) == StoreVerdict::Incompatible);
    CHECK(DescribeChange(id, other_backend) == "renderer backend");

    auto several = id;
    several.vendor_id = 0x10de;
    several.driver_version += 5;
    CHECK(DescribeChange(id, several) == "GPU vendor, driver version");
    CHECK(DescribeVerdict(StoreVerdict::Incompatible, stored, several) ==
          "identity changed (GPU vendor, driver version)");

    CHECK(CheckStore({}, id) == StoreVerdict::Missing);
    CHECK(DescribeVerdict(StoreVerdict::Missing, {}, id) == "no identity stamp");
}

void TestFormatAndRecordVersion() {
    const auto id = Rx580();
    auto bytes = EncodeStoreIdentity(id);
    auto bad_magic = bytes;
    bad_magic[0] ^= 1;
    CHECK(CheckStore(bad_magic, id) == StoreVerdict::Corrupt);
    auto bad_record = bytes;
    bad_record[4] ^= 1;
    CHECK(CheckStore(bad_record, id) == StoreVerdict::Corrupt);
    auto bad_size = bytes;
    bad_size[8] ^= 1;
    CHECK(CheckStore(bad_size, id) == StoreVerdict::Corrupt);
    auto shorter = bytes;
    shorter.pop_back();
    CHECK(CheckStore(shorter, id) == StoreVerdict::Corrupt);
    auto longer = bytes;
    longer.push_back(0);
    CHECK(CheckStore(longer, id) == StoreVerdict::Corrupt);
    CHECK(DescribeVerdict(StoreVerdict::Corrupt, shorter, id) == "corrupted identity stamp");
    CHECK(!DecodeStoreIdentity(Bytes(bytes.size(), 0)).has_value());
}

void TestStaleTempFiles() {
    TempDir dir{"sotb-pcache-temp"};
    const std::vector<std::string> ours{"0x1.meta.tmp", "0x2.spv.tmp", "g_0x3.key.tmp", "profile.bin.tmp"};
    const std::vector<std::string> theirs{"notes.tmp", "save.dat.tmp", "readme.txt", "x.meta", "x.tmp.bak",
                                          "0x4.cfg.tmp", "movie.mp4"};
    for (const auto& name : ours) {
        WriteRaw(dir.path / name, {1});
    }
    for (const auto& name : theirs) {
        WriteRaw(dir.path / name, {2});
    }
    for (const auto& name : ours) {
        CHECK(IsStaleTemp(dir.path / name));
    }
    for (const auto& name : theirs) {
        CHECK(!IsStaleTemp(dir.path / name));
    }

    for (const auto& entry : fs::directory_iterator{dir.path}) {
        if (IsStaleTemp(entry.path())) {
            fs::remove(entry.path());
        }
    }
    for (const auto& name : ours) {
        CHECK(!fs::exists(dir.path / name));
    }
    for (const auto& name : theirs) {
        CHECK(fs::exists(dir.path / name));
    }
}

void TestAtomicReplacement() {
    TempDir dir{"sotb-pcache-atomic"};
    const auto file = dir.path / "0x1.meta";
    const Bytes first = Sealed(Bytes{1, 1, 1});
    const Bytes second = Sealed(Bytes{2, 2, 2, 2});
    CHECK(WriteFileAtomically(file, first));
    CHECK(ReadAll(file) == first);
    CHECK(!fs::exists(dir.path / "0x1.meta.tmp"));
    CHECK(WriteFileAtomically(file, second));
    CHECK(ReadAll(file) == second);
    CHECK(!fs::exists(dir.path / "0x1.meta.tmp"));

    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator{dir.path}) {
        names.insert(entry.path().filename().string());
    }
    CHECK(names == std::set<std::string>{"0x1.meta"});

    Bytes copy = ReadAll(file);
    CHECK(Unseal(copy));
    CHECK((copy == Bytes{2, 2, 2, 2}));
}

void TestWriteFailureKeepsPreviousCache() {
    TempDir dir{"sotb-pcache-fail"};
    const auto file = dir.path / "0x1.key";
    const Bytes previous = Sealed(Bytes{7, 7, 7, 7});
    CHECK(WriteFileAtomically(file, previous));

    fs::create_directory(dir.path / "0x1.key.tmp");
    CHECK(!WriteFileAtomically(file, Sealed(Bytes{9, 9}), 1));
    CHECK(ReadAll(file) == previous);
    fs::remove(dir.path / "0x1.key.tmp");

    const auto blocked = dir.path / "0x2.spv";
    fs::create_directory(blocked);
    WriteRaw(blocked / "inner", {1});
    CHECK(!WriteFileAtomically(blocked, Sealed(Bytes{5}), 2));
    CHECK(fs::is_directory(blocked));
    CHECK(!fs::exists(dir.path / "0x2.spv.tmp"));

    CHECK(!WriteFileAtomically(dir.path / "missing" / "0x3.meta", previous, 1));
    CHECK(ReadAll(file) == previous);
}

void TestFailureFallsBackToRebuild() {
    const auto id = Rx580();
    Bytes truncated = EncodeStoreIdentity(id);
    truncated.resize(7);
    for (const auto& stored : {Bytes{}, truncated, Bytes(40, 0xcc)}) {
        const auto verdict = CheckStore(stored, id);
        CHECK(verdict != StoreVerdict::Reuse);
    }

    TempDir dir{"sotb-pcache-rebuild"};
    const auto identity_file = dir.path / "identity.bin";
    WriteRaw(identity_file, Bytes(30, 0xee));
    Bytes stored = ReadAll(identity_file);
    const bool sealed_ok = Unseal(stored);
    CHECK(!sealed_ok);
    stored.clear();
    CHECK(CheckStore(stored, id) == StoreVerdict::Missing);

    CHECK(WriteFileAtomically(identity_file, Sealed(EncodeStoreIdentity(id))));
    Bytes reloaded = ReadAll(identity_file);
    CHECK(Unseal(reloaded));
    CHECK(CheckStore(reloaded, id) == StoreVerdict::Reuse);

    const auto storage = ReadSource("gpu/shadps4/video_core/cache_storage.cpp");
    CHECK(storage.find("PipelineCachePolicy::Unseal") != std::string::npos);
    CHECK(storage.find("PipelineCachePolicy::Seal") != std::string::npos);
    CHECK(storage.find("PipelineCachePolicy::WriteFileAtomically") != std::string::npos);
    CHECK(storage.find("PipelineCachePolicy::IsStaleTemp") != std::string::npos);
    const auto serialization = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    CHECK(serialization.find("CheckStore(identity_data, current)") != std::string::npos);
    CHECK(serialization.find("GetPipelineCacheUUID") != std::string::npos);
    CHECK(serialization.find("GetDriverVersion") != std::string::npos);
    CHECK(serialization.find("db.Clear()") != std::string::npos);
    const auto warm = serialization.find("pldata.Read(version)");
    const auto warm_try = serialization.rfind("try {", warm);
    CHECK(warm != std::string::npos && warm_try != std::string::npos && warm - warm_try < 200);
}

void TestDeterministicIdentity() {
    const auto id = Rx580();
    const auto a = EncodeStoreIdentity(id);
    const auto b = EncodeStoreIdentity(Rx580());
    CHECK(a == b);
    CHECK(a.size() == 12 + IdentityPayloadSize);
    const auto decoded = DecodeStoreIdentity(a);
    CHECK(decoded.has_value() && *decoded == id);
    CHECK(EncodeStoreIdentity(*decoded) == a);

    const Bytes prefix{'S', 'B', 'I', 'D', 1, 0, 0, 0, std::uint8_t(IdentityPayloadSize), 0, 0, 0,
                       0x02, 0x10, 0x00, 0x00, 0xdf, 0x6f, 0x00, 0x00};
    CHECK(std::equal(prefix.begin(), prefix.end(), a.begin()));
    CHECK(std::equal(id.pipeline_cache_uuid.begin(), id.pipeline_cache_uuid.end(), a.end() - 16));

    auto changed = id;
    changed.pipeline_key_version = 9;
    CHECK(EncodeStoreIdentity(changed) != a);
    CHECK(Fnv1a(a) == Fnv1a(b));
}

void TestUnrelatedFilesKept() {
    TempDir dir{"sotb-pcache-clear"};
    const std::vector<std::string> cache{"0x1.meta", "0x2.spv", "g_0x3.key", "profile.bin", "identity.bin",
                                         "0x9.meta.tmp"};
    const std::vector<std::string> unrelated{"readme.txt", "game.sav", "screenshot.png", "x.tmp", "notes.cfg",
                                             "log.old", "0x5.dat"};
    for (const auto& name : cache) {
        WriteRaw(dir.path / name, {1});
    }
    for (const auto& name : unrelated) {
        WriteRaw(dir.path / name, {2});
    }
    fs::create_directory(dir.path / "sub.meta");

    for (const auto& entry : fs::directory_iterator{dir.path}) {
        std::error_code ec;
        if (entry.is_regular_file(ec) && IsCacheArtifact(entry.path())) {
            fs::remove(entry.path());
        }
    }
    for (const auto& name : cache) {
        CHECK(!fs::exists(dir.path / name));
    }
    for (const auto& name : unrelated) {
        CHECK(fs::exists(dir.path / name));
    }
    CHECK(fs::is_directory(dir.path / "sub.meta"));

    const auto storage = ReadSource("gpu/shadps4/video_core/cache_storage.cpp");
    const auto clear = storage.find("void DataBase::Clear()");
    CHECK(clear != std::string::npos);
    CHECK(storage.find("IsCacheArtifact", clear) != std::string::npos);
}

} // namespace

int main() {
    TestValidCacheLoads();
    TestCorruptHeaderRejected();
    TestTruncatedRejected();
    TestInvalidLengthRejected();
    TestIdentityMismatches();
    TestFormatAndRecordVersion();
    TestStaleTempFiles();
    TestAtomicReplacement();
    TestWriteFailureKeepsPreviousCache();
    TestFailureFallsBackToRebuild();
    TestDeterministicIdentity();
    TestUnrelatedFilesKept();
    std::printf("pipeline cache tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

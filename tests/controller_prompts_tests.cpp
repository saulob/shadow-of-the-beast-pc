// In-game controller prompts: the style decision (Auto, Xbox, PlayStation) taken once at startup, the identity checks of
// the Xbox art over the pak, all-or-nothing installation, fallback to the PlayStation original, and the wiring that keeps
// the decision out of every frame. Synthetic paks and fake callbacks only: no game is needed, nothing is started.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sotb_game_prompts.h"
#include "sotb_prompt_assets.h"

namespace fs = std::filesystem;

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

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string Source(const char* relative) { return ReadAll(fs::path(SOTB_SOURCE_DIR) / relative); }

std::vector<uint8_t> FromHex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

void PutU32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

void PutU64(std::vector<uint8_t>& out, size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i) out[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// ---- A synthetic pak, its generated-style table and the callbacks that stand in for the runtime. ----

struct World {
    std::vector<uint8_t> pak;
    std::vector<SotbPromptAsset> assets;
    std::vector<SotbPromptRun> runs;
    std::vector<uint8_t> blob;
    std::vector<uint8_t> stored;
    std::vector<std::vector<uint8_t>> replaced;
    SotbPromptSet set{};
};

constexpr uint64_t kAssetOffset[3] = {1000, 2000, 3500};
constexpr uint32_t kAssetSize[3] = {400, 700, 256};
constexpr uint64_t kIndexOffset = 6000;
constexpr uint32_t kIndexSize = 300;
const char* const kNames[3] = {"Beast/Content/Test/A.uasset", "Beast/Content/Test/B.uasset", "Beast/Content/Test/C.uasset"};
const char* const kGuest = "/app0/test/pak.pak";

World MakeWorld() {
    World w;
    w.pak.resize(kIndexOffset + kIndexSize + 44);
    uint32_t state = 12345;
    for (auto& b : w.pak) {
        state = state * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(state >> 24);
    }
    PutU32(w.pak, kIndexOffset + kIndexSize, 0x5A6F12E1u);
    PutU32(w.pak, kIndexOffset + kIndexSize + 4, 3);
    PutU64(w.pak, kIndexOffset + kIndexSize + 8, kIndexOffset);
    PutU64(w.pak, kIndexOffset + kIndexSize + 16, kIndexSize);
    const uint32_t spec[4][3] = {{10, 20, 0}, {100, 50, 0}, {0, 64, 1}, {200, 56, 2}};
    for (const auto& run : spec) {
        w.runs.push_back({run[0], run[1], static_cast<uint32_t>(w.blob.size())});
        for (uint32_t i = 0; i < run[1]; ++i) w.blob.push_back(static_cast<uint8_t>(0xA0 + run[2] * 16 + (i % 13)));
    }
    const uint32_t first[3] = {0, 2, 3};
    const uint32_t count[3] = {2, 1, 1};
    for (int a = 0; a < 3; ++a) {
        std::vector<uint8_t> original(w.pak.begin() + kAssetOffset[a], w.pak.begin() + kAssetOffset[a] + kAssetSize[a]);
        std::vector<uint8_t> patched = original;
        for (uint32_t r = 0; r < count[a]; ++r) {
            const SotbPromptRun& run = w.runs[first[a] + r];
            std::memcpy(patched.data() + run.offset, w.blob.data() + run.blob_offset, run.length);
        }
        w.replaced.push_back(patched);
        w.assets.push_back({kNames[a], kAssetOffset[a], kAssetSize[a], sotb_prompts_crc32(original.data(), original.size()),
                            sotb_prompts_crc32(patched.data(), patched.size()), first[a], count[a], SOTB_PROMPT_FORMAT_DXT5, 128, 128, 8});
    }
    const uint16_t len = static_cast<uint16_t>(w.blob.size());
    w.stored = {0x01, static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(~len), static_cast<uint8_t>((~len) >> 8)};
    w.stored.insert(w.stored.end(), w.blob.begin(), w.blob.end());
    std::vector<uint8_t> index(w.pak.begin() + kIndexOffset, w.pak.begin() + kIndexOffset + kIndexSize);
    w.set = {"TEST-1.00", kGuest, w.pak.size(), kIndexOffset, kIndexSize, sotb_prompts_crc32(index.data(), index.size()),
             nullptr, 3, nullptr, 4, nullptr, w.stored.size(), w.blob.size(), sotb_prompts_crc32(w.blob.data(), w.blob.size())};
    return w;
}

void Bind(World& w) {
    w.set.assets = w.assets.data();
    w.set.runs = w.runs.data();
    w.set.blob = w.stored.data();
}

struct Recorder {
    int overlay_calls = 0;
    std::string guest;
    uint64_t length = 0;
    std::vector<SotbPromptPatch> patches;
    int refuse = 0;
    int probe_calls = 0;
    SotbPadKind probe_kind = SOTB_PAD_KIND_UNKNOWN;
    std::vector<std::string> lines;
};

Recorder* g_rec = nullptr;

int FakeOverlay(const char* guest, uint64_t length, const SotbPromptPatch* patches, size_t count) {
    ++g_rec->overlay_calls;
    if (g_rec->refuse) return -1;
    g_rec->guest = guest;
    g_rec->length = length;
    g_rec->patches.assign(patches, patches + count);
    return 0;
}

SotbPadKind FakeProbe() {
    ++g_rec->probe_calls;
    return g_rec->probe_kind;
}

void FakeLog(void*, const char* line) { g_rec->lines.push_back(line); }

struct MemoryPak {
    const std::vector<uint8_t>* bytes;
};

int ReadMemory(void* user, uint64_t offset, void* buffer, size_t length) {
    const auto& bytes = *static_cast<MemoryPak*>(user)->bytes;
    if (offset > bytes.size() || length > bytes.size() - offset) return -1;
    std::memcpy(buffer, bytes.data() + offset, length);
    return 0;
}

SotbPromptResult Install(const World& w, const std::vector<uint8_t>& pak, Recorder& rec, uint64_t reported_size = 0, const SotbPromptSet* override_set = nullptr) {
    g_rec = &rec;
    MemoryPak memory{&pak};
    const SotbPromptSource source{ReadMemory, reported_size ? reported_size : pak.size(), &memory};
    return sotb_prompts_install(override_set ? override_set : &w.set, &source, FakeOverlay);
}

fs::path WriteFile(const fs::path& dir, const std::string& name, const std::string& text) {
    fs::create_directories(dir);
    const fs::path path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out << text;
    return path;
}

fs::path WritePak(const fs::path& dir, const std::vector<uint8_t>& pak) {
    return WriteFile(dir, "pak.pak", std::string(pak.begin(), pak.end()));
}

// ---- Tests ----

void TestCrcAndInflate() {
    CHECK(sotb_prompts_crc32("123456789", 9) == 0xCBF43926u);
    CHECK(sotb_prompts_crc32("", 0) == 0);

    const std::string text = "hello hello hello hello, controller prompts";
    const auto fixed = FromHex("cb48cdc9c957c84027751492f3f34a8af27372528b140a8af2730b4a8a01");
    std::vector<uint8_t> out(text.size());
    CHECK(sotb_prompts_inflate(fixed.data(), fixed.size(), out.data(), out.size()) == 0);
    CHECK(std::string(out.begin(), out.end()) == text);

    std::vector<uint8_t> expected;
    for (int i = 0; i < 600; ++i) expected.push_back(static_cast<uint8_t>((i * 7 + (i >> 3)) & 0xFF));
    for (int i = 0; i < 40; ++i) expected.insert(expected.end(), {'A', 'B', 'C', 'A', 'B', 'C', 'A', 'B', 'C'});
    const auto dynamic = FromHex(
        "edd1d737d7710087f19f88684846198528b350668510910a999192919d116959292a2b4aa8cc923d5a4629a2925146f6c82633eda1e1fb3ee7f337b8ea9ce7f275f7d0989671f20989cb286ddfb9d7f8808d93e7c9809088989bb7b2ef959457bd69e91e9af8f4934699d5c2129b604c2c6d9d6122af25dc867955dfda0343b788950b668b9ab61eccb15381176072ee3f7a06333cf9f917cc9a75929b614c0fdab9c05c8e4d4c83a96e687b07b38079f94a98adea3afa305ea7cf5e84c97df0b8026664eacb2c0cfffa0db23066878eb8c244c525dd81a9696cef85a167615b05b34d6397018cf799a04b30790f4b2b6146a7bffe861110d92807b3dfcafe284c747c723a4c6d53471f0cc3e215dc30ca3b74f7c11cf73d170a935ff8e439ccfb0fdffec0088a4ac9c3981f767083b9723d2503a6ee6d673fccc225ec3c302a9abb0d617cfcce87c114143d7d013336f3fd2fcc5a316905180b6b477798ab375233615e37770dc0302ee5e08551d5da630473c23f381ce66e71d94b98f18f3ffec1504b1561a8a51e30d4d22c186ae9200cd9ae48b67b90ed5964fb20d9ce47b61b93ed11647b15d94e23db95c8764fb23d5b4d5de37ff3d01c");
    std::vector<uint8_t> big(expected.size());
    CHECK(sotb_prompts_inflate(dynamic.data(), dynamic.size(), big.data(), big.size()) == 0);
    CHECK(big == expected);
    CHECK(sotb_prompts_crc32(big.data(), big.size()) == 4227193812u);

    // Stored blocks, wrong sizes, truncation and garbage are errors, never an overrun.
    const uint8_t stored[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'};
    uint8_t three[3];
    CHECK(sotb_prompts_inflate(stored, sizeof(stored), three, 3) == 0 && !std::memcmp(three, "abc", 3));
    CHECK(sotb_prompts_inflate(stored, sizeof(stored), three, 2) != 0);
    uint8_t four[4];
    CHECK(sotb_prompts_inflate(stored, sizeof(stored), four, 4) != 0);
    CHECK(sotb_prompts_inflate(stored, 6, three, 3) != 0);
    CHECK(sotb_prompts_inflate(dynamic.data(), dynamic.size() / 2, big.data(), big.size()) != 0);
    std::vector<uint8_t> small(10);
    CHECK(sotb_prompts_inflate(dynamic.data(), dynamic.size(), small.data(), small.size()) != 0);
    const uint8_t reserved[] = {0x07, 0x00};
    CHECK(sotb_prompts_inflate(reserved, sizeof(reserved), three, 3) != 0);
    const uint8_t bad_complement[] = {0x01, 0x03, 0x00, 0x00, 0x00, 'a', 'b', 'c'};
    CHECK(sotb_prompts_inflate(bad_complement, sizeof(bad_complement), three, 3) != 0);
}

void TestGeneratedTable() {
    const SotbPromptSet* set = sotb_prompt_set_for_build("CUSA03762-1.00");
    CHECK(set != nullptr);
    CHECK(sotb_prompt_set_for_build("CUSA03762-1.01") == nullptr && sotb_prompt_set_for_build("") == nullptr && sotb_prompt_set_for_build(nullptr) == nullptr);
    CHECK(sotb_prompt_set_count == 1);
    if (!set) return;
    CHECK(std::string(set->guest_path) == "/app0/beast/content/paks/pakchunk0-ps4.pak");
    CHECK(set->file_size == 6634484704ull && set->index_offset == 6633116901ull && set->index_size == 1367759u);
    CHECK(set->index_crc32 != 0 && set->blob_crc32 != 0);
    CHECK(set->index_offset + set->index_size + 44 == set->file_size);
    CHECK(set->asset_count >= 80 && set->run_count > set->asset_count);

    std::vector<uint8_t> blob(set->blob_size);
    CHECK(sotb_prompts_inflate(set->blob, set->blob_stored_size, blob.data(), blob.size()) == 0);
    CHECK(sotb_prompts_crc32(blob.data(), blob.size()) == set->blob_crc32);
    CHECK(set->blob_stored_size < set->blob_size);

    std::set<std::string> names;
    uint64_t previous_end = 0;
    size_t expected_first = 0, total_run_bytes = 0;
    for (size_t a = 0; a < set->asset_count; ++a) {
        const SotbPromptAsset& asset = set->assets[a];
        const std::string name = asset.name;
        CHECK(names.insert(name).second);
        CHECK(name.rfind("Beast/Content/", 0) == 0 && name.size() > 7 && name.substr(name.size() - 7) == ".uasset");
        CHECK(asset.data_offset > 0 && asset.size > 0);
        CHECK(asset.data_offset >= previous_end + 53);  // each entry keeps its own 53 byte header between neighbours
        CHECK(asset.data_offset + asset.size <= set->index_offset);
        previous_end = asset.data_offset + asset.size;
        CHECK(asset.original_crc32 != 0 && asset.replaced_crc32 != 0 && asset.original_crc32 != asset.replaced_crc32);
        CHECK(asset.format >= SOTB_PROMPT_FORMAT_DXT1 && asset.format <= SOTB_PROMPT_FORMAT_BGRA8);
        CHECK(asset.width >= 20 && asset.height >= 4 && asset.mip_count >= 1);
        CHECK(asset.first_run == expected_first && asset.run_count >= 1);
        expected_first += asset.run_count;
        uint32_t end = 0;
        for (uint32_t r = 0; r < asset.run_count; ++r) {
            const SotbPromptRun& run = set->runs[asset.first_run + r];
            CHECK(run.length > 0 && run.offset >= end && run.offset + run.length <= asset.size);
            CHECK(run.offset >= 24);  // never the package header's first bytes
            CHECK(static_cast<size_t>(run.blob_offset) + run.length <= set->blob_size);
            end = run.offset + run.length;
            total_run_bytes += run.length;
        }
    }
    CHECK(expected_first == set->run_count && total_run_bytes == set->blob_size);

    // The data covers every prompt the game draws: names are checked by the static asset tests against the real pak.
    const char* const required[] = {"HUD/Textures/ButtonCross.uasset", "HUD/Textures/ButtonCircle.uasset", "HUD/Textures/ButtonSquare.uasset",
                                    "HUD/Textures/ButtonTriangle.uasset", "HUD/Textures/ButtonL1.uasset", "HUD/Textures/ButtonR1.uasset",
                                    "HUD/Textures/ButtonL2.uasset", "HUD/Textures/ButtonR2.uasset", "HUD/Textures/ButtonL3.uasset",
                                    "HUD/Textures/ButtonR3.uasset", "HUD/Textures/ButtonOptions.uasset", "HUD/Textures/ButtonTouchpad.uasset",
                                    "HUD/Textures/DPad_4Way.uasset", "HUD/Textures/ButtonL3andR3.uasset", "HUD/PressStartScreen/ControllerMapStart.uasset",
                                    "HUD/Fonts/ButtonIconFont_48.uasset", "Frontend/OptionsScreen/Textures/3DPanels/Controls/controller.uasset"};
    for (const char* part : required) CHECK(names.count(std::string("Beast/Content/") + part) == 1);

    const SotbPromptAsset* font = nullptr;
    for (size_t a = 0; a < set->asset_count; ++a)
        if (std::string(set->assets[a].name) == "Beast/Content/HUD/Fonts/ButtonIconFont_48.uasset") font = &set->assets[a];
    CHECK(font != nullptr && font->format == SOTB_PROMPT_FORMAT_DXT5 && font->width == 256 && font->height == 256 &&
          font->mip_count == 1 && font->run_count >= 1);
}

void TestInstallSucceedsWithExactBytes() {
    World w = MakeWorld();
    Bind(w);
    Recorder rec;
    const SotbPromptResult r = Install(w, w.pak, rec);
    CHECK(r.status == SOTB_PROMPTS_OK && r.asset == nullptr && r.resources == 3 && r.patches == 4);
    CHECK(rec.overlay_calls == 1 && rec.guest == kGuest && rec.length == w.pak.size() && rec.patches.size() == 4);

    // Applying the patches to the pak changes only the runs and gives exactly the replacement.
    std::vector<uint8_t> served = w.pak;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& p : rec.patches) {
        CHECK(p.offset + p.size <= served.size() && p.data != nullptr);
        std::memcpy(served.data() + p.offset, p.data, p.size);
        ranges.emplace_back(p.offset, p.offset + p.size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) CHECK(ranges[i - 1].second <= ranges[i].first);
    for (int a = 0; a < 3; ++a)
        CHECK(std::equal(w.replaced[a].begin(), w.replaced[a].end(), served.begin() + kAssetOffset[a]));
    size_t differing = 0;
    for (size_t i = 0; i < served.size(); ++i) {
        if (served[i] == w.pak[i]) continue;
        ++differing;
        const bool covered = std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) { return i >= range.first && i < range.second; });
        CHECK(covered);
    }
    CHECK(differing > 0);
    // The source is only read: the original bytes still read back unchanged.
    World again = MakeWorld();
    CHECK(again.pak == w.pak);
}

void TestInstallRefusals() {
    {
        World w = MakeWorld();
        Bind(w);
        Recorder rec;
        std::vector<uint8_t> altered = w.pak;
        altered[kAssetOffset[0] + 5] ^= 1;
        const SotbPromptResult r = Install(w, altered, rec);
        CHECK(r.status == SOTB_PROMPTS_ASSET_CRC && r.asset && std::string(r.asset) == kNames[0] && rec.overlay_calls == 0);
    }
    {
        World w = MakeWorld();
        Bind(w);
        Recorder rec;
        std::vector<uint8_t> altered = w.pak;
        altered[kAssetOffset[2] + kAssetSize[2] - 1] ^= 0x80;  // the last asset: the first two were fine, nothing may be installed
        const SotbPromptResult r = Install(w, altered, rec);
        CHECK(r.status == SOTB_PROMPTS_ASSET_CRC && std::string(r.asset) == kNames[2]);
        CHECK(rec.overlay_calls == 0 && rec.patches.empty());
    }
    {
        World w = MakeWorld();
        Bind(w);
        Recorder rec;
        std::vector<uint8_t> truncated(w.pak.begin(), w.pak.end() - 1);
        const SotbPromptResult r = Install(w, truncated, rec);
        CHECK(r.status == SOTB_PROMPTS_FILE_SIZE && rec.overlay_calls == 0);
        std::vector<uint8_t> grown = w.pak;
        grown.push_back(0);
        CHECK(Install(w, grown, rec).status == SOTB_PROMPTS_FILE_SIZE && rec.overlay_calls == 0);
        std::vector<uint8_t> empty;
        CHECK(Install(w, empty, rec).status == SOTB_PROMPTS_FILE_SIZE);
    }
    {
        World w = MakeWorld();
        Bind(w);
        Recorder rec;
        std::vector<uint8_t> altered = w.pak;
        altered[kIndexOffset + 100] ^= 4;
        CHECK(Install(w, altered, rec).status == SOTB_PROMPTS_INDEX && rec.overlay_calls == 0);
        altered = w.pak;
        altered[kIndexOffset + kIndexSize + 4] = 4;  // another pak version
        CHECK(Install(w, altered, rec).status == SOTB_PROMPTS_FOOTER && rec.overlay_calls == 0);
        altered = w.pak;
        altered[kIndexOffset + kIndexSize] ^= 1;  // magic
        CHECK(Install(w, altered, rec).status == SOTB_PROMPTS_FOOTER && rec.overlay_calls == 0);
        altered = w.pak;
        altered[kIndexOffset + kIndexSize + 8] ^= 1;  // index offset
        CHECK(Install(w, altered, rec).status == SOTB_PROMPTS_FOOTER && rec.overlay_calls == 0);
    }
    {
        World w = MakeWorld();
        Bind(w);
        Recorder rec;
        w.assets[1].original_crc32 ^= 1;  // a table written for another original
        CHECK(Install(w, w.pak, rec).status == SOTB_PROMPTS_ASSET_CRC && rec.overlay_calls == 0);
        w.assets[1].original_crc32 ^= 1;
        w.assets[1].replaced_crc32 ^= 1;  // a replacement that is not what the table promises
        const SotbPromptResult r = Install(w, w.pak, rec);
        CHECK(r.status == SOTB_PROMPTS_REPLACEMENT && std::string(r.asset) == kNames[1] && rec.overlay_calls == 0);
        w.assets[1].replaced_crc32 ^= 1;
        w.set.blob_crc32 ^= 1;
        CHECK(Install(w, w.pak, rec).status == SOTB_PROMPTS_REPLACEMENT && rec.overlay_calls == 0);
        w.set.blob_crc32 ^= 1;
        CHECK(Install(w, w.pak, rec).status == SOTB_PROMPTS_OK);
    }
    {
        World w = MakeWorld();
        Bind(w);
        Recorder rec;
        w.assets[2].data_offset = kIndexOffset;  // outside the entries
        CHECK(Install(w, w.pak, rec).status == SOTB_PROMPTS_ASSET_BOUNDS && rec.overlay_calls == 0);
        w.assets[2].data_offset = kAssetOffset[2];
        w.runs[3].length = 500;  // a run that does not fit its asset
        CHECK(Install(w, w.pak, rec).status == SOTB_PROMPTS_TABLE && rec.overlay_calls == 0);
        w.runs[3].length = 56;
        w.runs[1].offset = 25;  // overlaps the run before it
        CHECK(Install(w, w.pak, rec).status == SOTB_PROMPTS_TABLE && rec.overlay_calls == 0);
        w.runs[1].offset = 100;
        w.stored[2] = 0xFF;  // a damaged data stream
        w.stored[3] = 0x00;
        CHECK(Install(w, w.pak, rec).status == SOTB_PROMPTS_REPLACEMENT && rec.overlay_calls == 0);
    }
    {
        World w = MakeWorld();
        Bind(w);
        Recorder rec;
        rec.refuse = 1;
        const SotbPromptResult r = Install(w, w.pak, rec);
        CHECK(r.status == SOTB_PROMPTS_OVERLAY_REFUSED && rec.overlay_calls == 1 && rec.patches.empty());
        CHECK(Install(w, w.pak, rec, 0, nullptr).status == SOTB_PROMPTS_OVERLAY_REFUSED);
    }
    {
        Recorder rec;
        g_rec = &rec;
        MemoryPak memory{nullptr};
        const SotbPromptSource source{ReadMemory, 10, &memory};
        CHECK(sotb_prompts_install(nullptr, &source, FakeOverlay).status == SOTB_PROMPTS_UNSUPPORTED_BUILD && rec.overlay_calls == 0);
    }
    CHECK(std::string(sotb_prompts_status_text(SOTB_PROMPTS_OK)).size() > 0);
    for (int s = SOTB_PROMPTS_OK; s <= SOTB_PROMPTS_OVERLAY_REFUSED; ++s)
        CHECK(std::string(sotb_prompts_status_text(static_cast<SotbPromptStatus>(s))) != "unknown");
}

SotbGamePromptsOutcome Apply(const fs::path& ini, const fs::path& pak, const World& w, Recorder& rec, const SotbPromptSet* set) {
    g_rec = &rec;
    const std::string ini_text = ini.string(), pak_text = pak.string();
    SotbGamePromptsRequest request{};
    request.settings_file = ini_text.c_str();
    request.pak_path = pak_text.c_str();
    request.build_key = "TEST-1.00";
    request.set = set;
    request.probe_startup_kind = FakeProbe;
    request.overlay = FakeOverlay;
    request.log = FakeLog;
    (void)w;
    return sotb_game_prompts_apply(&request);
}

void TestModeParsing(const fs::path& base) {
    CHECK(sotb_game_prompts_mode((base / "missing.ini").string().c_str()) == SOTB_PROMPT_AUTO);
    CHECK(sotb_game_prompts_mode(nullptr) == SOTB_PROMPT_AUTO);
    struct Case { const char* text; SotbPromptMode mode; } cases[] = {
        {"controller_prompts=auto\n", SOTB_PROMPT_AUTO},
        {"controller_prompts=xbox\n", SOTB_PROMPT_XBOX},
        {"controller_prompts=playstation\n", SOTB_PROMPT_PLAYSTATION},
        {"controller_prompts = Xbox # note\n", SOTB_PROMPT_XBOX},
        {"controller_prompts=PLAYSTATION\n", SOTB_PROMPT_PLAYSTATION},
        {"controller_prompts=nintendo\n", SOTB_PROMPT_AUTO},
        {"controller_prompts=\n", SOTB_PROMPT_AUTO},
        {"other=xbox\n", SOTB_PROMPT_AUTO},
        {"# controller_prompts=xbox\n", SOTB_PROMPT_AUTO},
        {"controller_prompts=garbage\r\ncontroller_prompts=xbox\r\n", SOTB_PROMPT_XBOX},
        {"controller_prompts=xbox\ncontroller_prompts=bogus\n", SOTB_PROMPT_AUTO},
    };
    int n = 0;
    for (const auto& c : cases) CHECK(sotb_game_prompts_mode(WriteFile(base, "mode" + std::to_string(n++) + ".ini", c.text).string().c_str()) == c.mode);
}

void TestStylePolicy(const fs::path& base) {
    World w = MakeWorld();
    Bind(w);
    const fs::path pak = WritePak(base, w.pak);
    const struct { SotbPadKind kind; SotbPromptFamily family; } auto_cases[] = {
        {SOTB_PAD_KIND_XBOX, SOTB_PROMPT_FAMILY_XBOX},
        {SOTB_PAD_KIND_DUALSHOCK4, SOTB_PROMPT_FAMILY_PLAYSTATION},
        {SOTB_PAD_KIND_DUALSENSE, SOTB_PROMPT_FAMILY_PLAYSTATION},
        {SOTB_PAD_KIND_DUALSHOCK3, SOTB_PROMPT_FAMILY_PLAYSTATION},
        {SOTB_PAD_KIND_GENERIC, SOTB_PROMPT_FAMILY_XBOX},
        {SOTB_PAD_KIND_UNKNOWN, SOTB_PROMPT_FAMILY_XBOX},
    };
    const fs::path auto_ini = WriteFile(base, "auto.ini", "controller_prompts=auto\n");
    for (const auto& c : auto_cases) {
        Recorder rec;
        rec.probe_kind = c.kind;
        const SotbGamePromptsOutcome out = Apply(auto_ini, pak, w, rec, &w.set);
        CHECK(out.requested == SOTB_PROMPT_AUTO && out.effective == c.family && out.startup_kind == c.kind);
        CHECK(rec.probe_calls == 1);
        CHECK(out.installed == (c.family == SOTB_PROMPT_FAMILY_XBOX ? 1 : 0));
        CHECK(rec.overlay_calls == (c.family == SOTB_PROMPT_FAMILY_XBOX ? 1 : 0));
    }
    // A missing setting is Auto.
    {
        Recorder rec;
        rec.probe_kind = SOTB_PAD_KIND_DUALSENSE;
        const SotbGamePromptsOutcome out = Apply(base / "absent.ini", pak, w, rec, &w.set);
        CHECK(out.requested == SOTB_PROMPT_AUTO && out.effective == SOTB_PROMPT_FAMILY_PLAYSTATION && rec.overlay_calls == 0);
    }
    // Explicit Xbox: always Xbox, whatever is connected, and nothing is probed.
    const fs::path xbox_ini = WriteFile(base, "xbox.ini", "controller_prompts=xbox\n");
    for (SotbPadKind kind : {SOTB_PAD_KIND_XBOX, SOTB_PAD_KIND_DUALSENSE, SOTB_PAD_KIND_UNKNOWN}) {
        Recorder rec;
        rec.probe_kind = kind;
        const SotbGamePromptsOutcome out = Apply(xbox_ini, pak, w, rec, &w.set);
        CHECK(out.requested == SOTB_PROMPT_XBOX && out.effective == SOTB_PROMPT_FAMILY_XBOX && out.installed == 1);
        CHECK(rec.probe_calls == 0 && rec.overlay_calls == 1 && rec.guest == kGuest);
    }
    // Explicit PlayStation: the original art, no overlay, nothing probed.
    const fs::path ps_ini = WriteFile(base, "ps.ini", "controller_prompts=playstation\n");
    for (SotbPadKind kind : {SOTB_PAD_KIND_XBOX, SOTB_PAD_KIND_DUALSENSE, SOTB_PAD_KIND_UNKNOWN}) {
        Recorder rec;
        rec.probe_kind = kind;
        const SotbGamePromptsOutcome out = Apply(ps_ini, pak, w, rec, &w.set);
        CHECK(out.requested == SOTB_PROMPT_PLAYSTATION && out.effective == SOTB_PROMPT_FAMILY_PLAYSTATION && out.installed == 0);
        CHECK(rec.probe_calls == 0 && rec.overlay_calls == 0);
    }
    // Invalid setting falls back to Auto.
    {
        Recorder rec;
        rec.probe_kind = SOTB_PAD_KIND_DUALSHOCK4;
        const SotbGamePromptsOutcome out = Apply(WriteFile(base, "bad.ini", "controller_prompts=switch\n"), pak, w, rec, &w.set);
        CHECK(out.requested == SOTB_PROMPT_AUTO && out.effective == SOTB_PROMPT_FAMILY_PLAYSTATION && rec.overlay_calls == 0);
    }
}

void TestDiagnosticsAndFallback(const fs::path& base) {
    World w = MakeWorld();
    Bind(w);
    const fs::path pak = WritePak(base, w.pak);
    const fs::path xbox_ini = WriteFile(base, "xbox2.ini", "controller_prompts=xbox\n");
    {
        Recorder rec;
        Apply(xbox_ini, pak, w, rec, &w.set);
        CHECK(rec.lines.size() == 2);
        CHECK(rec.lines[0] == "Controller prompts: requested=xbox, startup controller=unknown, effective=xbox, build=TEST-1.00");
        CHECK(rec.lines[1] == "Controller prompts: Xbox overlay installed, 3 resources, 4 patches");
    }
    {
        Recorder rec;
        rec.probe_kind = SOTB_PAD_KIND_DUALSENSE;
        Apply(WriteFile(base, "auto2.ini", "controller_prompts=auto\n"), pak, w, rec, &w.set);
        CHECK(rec.lines.size() == 2);
        CHECK(rec.lines[0] == "Controller prompts: requested=auto, startup controller=DualSense, effective=playstation, build=TEST-1.00");
        CHECK(rec.lines[1] == "Controller prompts: PlayStation originals kept, no overlay");
    }
    // An asset that is not the expected original: the original art stays, the log names the asset, nothing is installed.
    {
        std::vector<uint8_t> altered = w.pak;
        altered[kAssetOffset[1] + 300] ^= 1;
        const fs::path bad = WriteFile(base, "bad.pak", std::string(altered.begin(), altered.end()));
        Recorder rec;
        const SotbGamePromptsOutcome out = Apply(xbox_ini, bad, w, rec, &w.set);
        CHECK(out.requested == SOTB_PROMPT_XBOX && out.effective == SOTB_PROMPT_FAMILY_XBOX && out.installed == 0);
        CHECK(out.status == SOTB_PROMPTS_ASSET_CRC && rec.overlay_calls == 0);
        CHECK(rec.lines.size() == 2 && Contains(rec.lines[1], kNames[1]) && Contains(rec.lines[1], "using PlayStation original"));
    }
    // Missing pak, unknown build, refused overlay: the same fallback, with a reason.
    {
        Recorder rec;
        const SotbGamePromptsOutcome out = Apply(xbox_ini, base / "missing.pak", w, rec, &w.set);
        CHECK(out.installed == 0 && out.status == SOTB_PROMPTS_READ_FAILED && rec.overlay_calls == 0);
        CHECK(rec.lines.back() == "Controller prompts: pak.pak could not be opened, using PlayStation original");
    }
    {
        Recorder rec;
        const SotbGamePromptsOutcome out = Apply(xbox_ini, pak, w, rec, nullptr);
        CHECK(out.installed == 0 && out.status == SOTB_PROMPTS_UNSUPPORTED_BUILD && rec.overlay_calls == 0);
        CHECK(Contains(rec.lines.back(), "no Xbox art for build TEST-1.00") && Contains(rec.lines.back(), "using PlayStation original"));
    }
    {
        Recorder rec;
        rec.refuse = 1;
        const SotbGamePromptsOutcome out = Apply(xbox_ini, pak, w, rec, &w.set);
        CHECK(out.installed == 0 && out.status == SOTB_PROMPTS_OVERLAY_REFUSED);
        CHECK(Contains(rec.lines.back(), "overlay refused") && Contains(rec.lines.back(), "using PlayStation original"));
    }
    {
        std::vector<uint8_t> shorter(w.pak.begin(), w.pak.end() - 100);
        const fs::path small = WriteFile(base, "short.pak", std::string(shorter.begin(), shorter.end()));
        Recorder rec;
        const SotbGamePromptsOutcome out = Apply(xbox_ini, small, w, rec, &w.set);
        CHECK(out.installed == 0 && out.status == SOTB_PROMPTS_FILE_SIZE && rec.overlay_calls == 0);
    }
    // No line carries a local path or binary data.
    {
        Recorder rec;
        Apply(xbox_ini, pak, w, rec, &w.set);
        Apply(xbox_ini, base / "missing.pak", w, rec, &w.set);
        for (const std::string& line : rec.lines) {
            CHECK(line.rfind("Controller prompts: ", 0) == 0 && line.size() < 200);
            CHECK(!Contains(line, base.string()) && !Contains(line, "\\") && !Contains(line, ":/"));
            CHECK(std::all_of(line.begin(), line.end(), [](unsigned char c) { return c >= 32 && c < 127; }));
        }
    }
}

void TestRealGeneratedSetAgainstGame(const fs::path& base) {
    const fs::path pak = fs::path(SOTB_SOURCE_DIR) / "game" / "prepared" / "CUSA03762-1.00" / "beast" / "content" / "paks" / "pakchunk0-ps4.pak";
    std::error_code ec;
    if (!fs::exists(pak, ec)) {
        std::printf("real pak: not found, skipped\n");
        return;
    }
    Recorder rec;
    g_rec = &rec;
    const std::string ini = WriteFile(base, "real.ini", "controller_prompts=xbox\n").string();
    const std::string pak_text = pak.string();
    SotbGamePromptsRequest request{};
    request.settings_file = ini.c_str();
    request.pak_path = pak_text.c_str();
    request.build_key = "CUSA03762-1.00";
    request.overlay = FakeOverlay;
    request.log = FakeLog;
    const SotbGamePromptsOutcome out = sotb_game_prompts_apply(&request);
    CHECK(out.installed == 1 && out.status == SOTB_PROMPTS_OK);
    const SotbPromptSet* set = sotb_prompt_set_for_build("CUSA03762-1.00");
    CHECK(rec.overlay_calls == 1 && rec.patches.size() == set->run_count && rec.length == set->file_size);
    CHECK(rec.lines.size() == 2 && Contains(rec.lines[1], "Xbox overlay installed"));
    std::printf("real pak: %s\n", rec.lines.empty() ? "" : rec.lines.back().c_str());
}

size_t Count(const std::string& text, const std::string& part) {
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++n;
    return n;
}

void TestWiringAndTiming() {
    const std::string loader = Source("src/loader.c");
    const size_t configure = loader.find("runtime_file_configure(app0");
    const size_t install = loader.find("install_controller_prompts(settings_file, app0, &game_build);");
    const size_t entry = loader.find("sotb_startup_phase(\"guest entry\")");
    const size_t mapped = loader.find("sotb_startup_phase(\"guest image mapped\")");
    CHECK(configure != std::string::npos && install != std::string::npos && entry != std::string::npos);
    // After the file layer is configured and before the guest exists: no guest asset is read before the style is chosen.
    CHECK(configure < install && install < mapped && mapped < entry);
    CHECK(Count(loader, "install_controller_prompts(settings_file") == 1);
    CHECK(Contains(loader, "runtime_file_overlay(guest, length, (const RuntimeFilePatch *)patches, count)"));
    CHECK(Contains(loader, "sizeof(SotbPromptPatch) == sizeof(RuntimeFilePatch)"));
    CHECK(Contains(loader, "pakchunk0-ps4.pak") && Contains(loader, "\"%s-%s\", build->title_id, build->version"));

    // The decision is taken once, in the loader; nothing in the frame, input or render path takes or repeats it.
    const char* const per_frame[] = {"src/runtime_pad.c", "src/sotb_pad.c", "src/sotb_pad_prompts.c", "src/runtime_file.c", "src/runtime_memory.c",
                                     "src/guest_display.c", "src/runtime_services.c", "src/runtime_kernel.c"};
    for (const char* file : per_frame) {
        const std::string text = Source(file);
        CHECK(!Contains(text, "sotb_game_prompts_apply") && !Contains(text, "sotb_prompts_install"));
        if (std::string(file) != "src/runtime_file.c") CHECK(!Contains(text, "runtime_file_overlay("));
    }
    CHECK(Count(Source("src/runtime_pad.c"), "runtime_pad_startup_kind") == 1);
    CHECK(Count(loader, "runtime_pad_startup_kind") == 1);
    const std::string pad = Source("src/runtime_pad.c");
    const size_t track = pad.find("static void track_prompt_family");
    const size_t track_end = pad.find("\n}\n", track);
    const std::string tracker = pad.substr(track, track_end - track);
    CHECK(!Contains(tracker, "overlay") && !Contains(tracker, "install") && Contains(tracker, "printf"));
    // The startup probe never opens a gamepad or logs one in; the later scan still does that.
    const size_t probe = pad.find("int runtime_pad_startup_kind(void)");
    const std::string probe_body = pad.substr(probe, pad.find("\n}\n", probe) - probe);
    CHECK(!Contains(probe_body, "SDL_OpenGamepad") && !Contains(probe_body, "runtime_user_login") && Contains(probe_body, "load_user_gamepad_mappings()"));
    CHECK(Contains(probe_body, "if (!sdl_ready)"));

    // The prompt module never reads the pak outside the install, and the Xbox art never writes a game file.
    const std::string assets = Source("src/sotb_prompt_assets.c");
    CHECK(!Contains(assets, "fopen") && !Contains(assets, "fwrite") && !Contains(assets, "remove(") && !Contains(assets, "rename("));
    const std::string game_prompts = Source("src/sotb_game_prompts.c");
    CHECK(Contains(game_prompts, "\"rb\"") && !Contains(game_prompts, "\"wb\"") && !Contains(game_prompts, "\"r+b\"") && !Contains(game_prompts, "fwrite"));

    // Without Xbox the guest path is not touched: PlayStation returns before any file or overlay call.
    const size_t ps = game_prompts.find("effective != SOTB_PROMPT_FAMILY_XBOX");
    const size_t first_open = game_prompts.find("fopen");
    CHECK(ps != std::string::npos && first_open != std::string::npos && ps < first_open);
}

void TestLauncherWording() {
    const std::string settings = Source("launcher/settings.h");
    const size_t at = settings.find("kControllerPromptsHelp");
    const std::string help = settings.substr(at, settings.find("\";", at) - at);
    CHECK(Contains(help, "button icons") || Contains(help, "button art"));
    CHECK(Contains(help, "next game start"));
    CHECK(!Contains(help, "only changes the names") && !Contains(help, "not changed"));
    const std::string launcher = Source("launcher/launcher.cpp");
    CHECK(!Contains(launcher, "the game's own button art is not changed") && !Contains(launcher, "These names are for this page"));
    CHECK(Contains(launcher, "next game start"));
    CHECK(!Contains(Source("src/sotb_pad_prompts.h"), "is not changed here"));
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / ("sotb-controller-prompts-tests-" + std::to_string(std::rand()));
    fs::create_directories(base);
    TestCrcAndInflate();
    TestGeneratedTable();
    TestInstallSucceedsWithExactBytes();
    TestInstallRefusals();
    TestModeParsing(base);
    TestStylePolicy(base);
    TestDiagnosticsAndFallback(base);
    TestRealGeneratedSetAgainstGame(base);
    TestWiringAndTiming();
    TestLauncherWording();
    std::error_code ignored;
    fs::remove_all(base, ignored);
    std::printf("controller prompts tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

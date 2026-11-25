// Aspect Ratio: the six shapes and their setting, the Window size choices, the render targets (width alignment and the
// pixel budget), 16:9 left as it was, the six display size sites on the three real builds, the guard on the real
// target, and the presenter, FSR and SMAA reading the game's own image size. No window, no game, no GPU.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include "guest_patches.h"
}
#include "guest_display.h"
#include "incident_report.h"
#include "options.h"
#include "psn_notice.h"
#include "resolution_guard.h"
#include "settings.h"
#include "skip_intro.h"
#include "sotb_exit_game.h"
#include "sotb_graphics.h"
#include "sotb_launcher.h"
#include "sotb_menu_buttons.h"
#include "test_progress.h"

namespace fs = std::filesystem;
using namespace Sotb;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(condition)) {                                                       \
            ++g_failures;                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                         \
        sotb_test_progress(g_checks);                                             \
    } while (0)

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

void WriteAll(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

bool Contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, std::string_view needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

const fs::path Root() { return SOTB_SOURCE_DIR; }

constexpr std::array<const char*, SOTB_ASPECT_COUNT> kKeys = {"16:9", "16:10", "21:9", "32:9", "4:3", "5:4"};
constexpr std::array<const char*, SOTB_ASPECT_COUNT> kNames = {"16:9 Original", "16:10", "21:9 Ultrawide",
                                                             "32:9 Super Ultrawide", "4:3", "5:4"};
constexpr std::array<const char*, SOTB_RESOLUTION_COUNT> kResolutions = {"native", "480p", "720p", "1440p", "1800p", "4K"};

struct Size {
    int width, height;
    bool operator==(const Size&) const = default;
};

// Computed apart from the code: rows are native, 480p, 720p, 1440p, 1800p and 4K, columns follow kKeys.
constexpr Size kTargets[SOTB_RESOLUTION_COUNT][SOTB_ASPECT_COUNT] = {
    {{1920, 1080}, {1728, 1080}, {2520, 1080}, {3840, 1080}, {1440, 1080}, {1352, 1080}},
    {{854, 480}, {768, 480}, {1120, 480}, {1704, 480}, {640, 480}, {600, 480}},
    {{1280, 720}, {1152, 720}, {1680, 720}, {2560, 720}, {960, 720}, {904, 720}},
    {{2560, 1440}, {2304, 1440}, {3360, 1440}, {5120, 1440}, {1920, 1440}, {1800, 1440}},
    {{3200, 1800}, {2880, 1800}, {4200, 1800}, {5408, 1520}, {2400, 1800}, {2248, 1800}},
    {{3840, 2160}, {3456, 2160}, {4384, 1880}, {5408, 1520}, {2880, 2160}, {2704, 2160}},
};

constexpr bool kHeavy[SOTB_RESOLUTION_COUNT][SOTB_ASPECT_COUNT] = {
    {false, false, false, true, false, false}, {false, false, false, false, false, false},
    {false, false, false, false, false, false}, {false, false, true, true, false, false},
    {true, true, true, true, true, true},      {true, true, true, true, true, true},
};

constexpr int kSafeResolution[SOTB_ASPECT_COUNT] = {3, 3, 0, 2, 3, 3};

constexpr int kHeights[] = {720, 900, 1080, 1440, 2160};
constexpr int kWindowWidths[SOTB_ASPECT_COUNT][5] = {
    {1280, 1600, 1920, 2560, 3840}, {1152, 1440, 1728, 2304, 3456}, {1680, 2100, 2520, 3360, 5040},
    {2560, 3200, 3840, 5120, 7680}, {960, 1200, 1440, 1920, 2880},  {900, 1125, 1350, 1800, 2700},
};

SotbRenderTarget TargetOf(int resolution, int aspect) {
    return sotb_render_target(&sotb_resolutions[resolution], &sotb_aspect_ratios[aspect]);
}

void TestSettingValues() {
    CHECK(SOTB_ASPECT_COUNT == 6);
    for (int i = 0; i < SOTB_ASPECT_COUNT; ++i) {
        CHECK(std::string(sotb_aspect_ratios[i].key) == kKeys[i]);
        CHECK(std::string(sotb_aspect_ratios[i].name) == kNames[i]);
        CHECK(sotb_aspect_index(kKeys[i]) == i);
        CHECK(IsValidSetting(kAspectRatioKey, kKeys[i]));
    }
    CHECK(std::string(kAspectRatioKey) == "aspect_ratio" && std::string(kDefaultAspectRatio) == "16:9");
    for (const char* bad : {"", "16/9", "21:10", "ultrawide", "16:9 ", " 16:9", "4:3\n", "32:9,", "0:0"}) {
        CHECK(sotb_aspect_index(bad) == 0 && !IsValidSetting(kAspectRatioKey, bad));
    }
    CHECK(sotb_aspect_index(nullptr) == 0);

    bool managed = false;
    for (const ManagedSetting& setting : ManagedSettings()) {
        if (setting.key == kAspectRatioKey) {
            managed = setting.default_value == "16:9";
        }
    }
    CHECK(managed);
}

void TestPersistence() {
    const fs::path dir = fs::temp_directory_path() / "sotb-aspect-ratio-tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "sotb.ini";

    {
        LauncherSettings settings(file);
        CHECK(settings.Get(kAspectRatioKey, kDefaultAspectRatio) == "16:9");
        CHECK(sotb_aspect_ratio(file.string().c_str()) == 0);
        for (int i = 0; i < SOTB_ASPECT_COUNT; ++i) {
            settings.Set(kAspectRatioKey, kKeys[i]);
            CHECK(settings.Save());
            LauncherSettings reopened(file);
            CHECK(reopened.Get(kAspectRatioKey, kDefaultAspectRatio) == kKeys[i]);
            CHECK(sotb_aspect_ratio(file.string().c_str()) == i);
        }
    }

    WriteAll(file, "aspect_ratio = 21:10\nrender_resolution = 720p\n");
    {
        const LauncherSettings invalid(file);
        CHECK(invalid.Get(kAspectRatioKey, kDefaultAspectRatio) == "16:9");
        CHECK(sotb_aspect_ratio(file.string().c_str()) == 0);
    }
    WriteAll(file, "aspect_ratio =\n");
    CHECK(sotb_aspect_ratio(file.string().c_str()) == 0);
    CHECK(sotb_aspect_ratio(nullptr) == 0 && sotb_aspect_ratio("") == 0);
    CHECK(sotb_aspect_ratio((dir / "missing.ini").string().c_str()) == 0);

    WriteAll(file, "aspect_ratio = 32:9\ntrue_ultrawide = 1\nrender_resolution = 1440p\n");
    LauncherSettings settings(file);
    CHECK(settings.Get(kAspectRatioKey, kDefaultAspectRatio) == "32:9");
    settings.ResetToDefaults();
    CHECK(settings.Get(kAspectRatioKey, "x") == "16:9");
    CHECK(settings.Save());
    const std::string text = ReadAll(file);
    CHECK(Contains(text, "aspect_ratio = 16:9") && Contains(text, "true_ultrawide = 1"));

    const std::string summary = SettingsSummary(settings, nullptr);
    CHECK(Contains(summary, "Settings: Render resolution native 1920x1080, aspect ratio 16:9\n"));
    settings.Set(kAspectRatioKey, "21:9");
    settings.Set(kRenderResolutionKey, "1440p");
    CHECK(Contains(SettingsSummary(settings, nullptr), "Settings: Render resolution 1440p 3360x1440, aspect ratio 21:9\n"));
    settings.Set(kAspectRatioKey, "32:9");
    settings.Set(kRenderResolutionKey, "4K");
    CHECK(Contains(SettingsSummary(settings, nullptr), "Settings: Render resolution 4K 5408x1520, aspect ratio 32:9\n"));

    // The Bug Report reads the same line, with the real target and the shape, inside its 64 character limit.
    const LogDiagnostics diagnostics = ReadLogDiagnostics(SettingsSummary(settings, nullptr));
    CHECK(diagnostics.render_resolution == "4K 5408x1520, aspect ratio 32:9" && diagnostics.render_resolution.size() <= 64);
    fs::remove_all(dir, ec);
}

void TestWindowSizes() {
    for (int a = 0; a < SOTB_ASPECT_COUNT; ++a) {
        const std::vector<WindowSize> sizes = WindowSizesFor(sotb_aspect_ratios[a]);
        CHECK(sizes.size() == 5);
        for (size_t i = 0; i < sizes.size() && i < 5; ++i) {
            CHECK(sizes[i].height == kHeights[i] && sizes[i].width == kWindowWidths[a][i]);
            CHECK(WindowSizeOf(kHeights[i], sotb_aspect_ratios[a]) == sizes[i]);
        }
    }
    const std::vector<WindowSize> original = WindowSizesFor(sotb_aspect_ratios[0]);
    CHECK((original[0] == WindowSize{1280, 720} && original[2] == kGamePictureSize && original[4] == WindowSize{3840, 2160}));
    for (const int height : kHeights) {
        CHECK(WindowSizeOf(height, sotb_aspect_ratios[0]).width == (height * 16 + 4) / 9);
    }
    CHECK(WindowSizeLabel(WindowSizeOf(1080, sotb_aspect_ratios[2])) == "2520 \xC3\x97 1080");
    CHECK(WindowSizeLabel(kGamePictureSize).find("the game's own size") != std::string::npos);
}

void TestRenderTargets() {
    for (int r = 0; r < SOTB_RESOLUTION_COUNT; ++r) {
        CHECK(std::string(sotb_resolutions[r].key) == kResolutions[r]);
        for (int a = 0; a < SOTB_ASPECT_COUNT; ++a) {
            const SotbRenderTarget target = TargetOf(r, a);
            CHECK((Size{target.width, target.height} == kTargets[r][a]));
            CHECK(sotb_target_is_heavy(target) == kHeavy[r][a]);
            CHECK((int64_t)target.width * target.height <= std::max<int64_t>(SOTB_TARGET_PIXEL_BUDGET, (int64_t)sotb_resolutions[r].width * sotb_resolutions[r].height));
            if (a == 0) {
                // 16:9 hands the resolution back as it is, 854 x 480 included.
                CHECK(target.width == sotb_resolutions[r].width && target.height == sotb_resolutions[r].height);
                continue;
            }
            CHECK(target.width % SOTB_TARGET_ALIGNMENT == 0 && target.height % SOTB_TARGET_ALIGNMENT == 0);
            CHECK(target.height <= sotb_resolutions[r].height);
            const double ratio = double(sotb_aspect_ratios[a].width) / sotb_aspect_ratios[a].height;
            CHECK(std::abs(double(target.width) / target.height - ratio) / ratio < 0.006);
            const int64_t pixels = (int64_t)target.width * target.height;
            CHECK(pixels <= SOTB_TARGET_PIXEL_BUDGET);
            if (target.height == sotb_resolutions[r].height) continue;
            // Reduced: one more step of height would not have fitted.
            const SotbResolution taller = {"", "", 0, target.height + SOTB_TARGET_ALIGNMENT};
            CHECK(sotb_render_target(&taller, &sotb_aspect_ratios[a]).height == target.height);
        }
    }

    // The examples of the request.
    CHECK((Size{TargetOf(0, 1).width, TargetOf(0, 1).height} == Size{1728, 1080}));
    CHECK((Size{TargetOf(0, 2).width, TargetOf(0, 2).height} == Size{2520, 1080}));
    CHECK((Size{TargetOf(0, 3).width, TargetOf(0, 3).height} == Size{3840, 1080}));
    CHECK((Size{TargetOf(0, 4).width, TargetOf(0, 4).height} == Size{1440, 1080}));

    // Never the 7680 x 2160 target of 32:9 at 4K: the height drops, the ratio stays, and it fits 3840 x 2160 pixels.
    const SotbRenderTarget wide = TargetOf(5, 3);
    CHECK(wide.width < 7680 && wide.height < 2160 && (int64_t)wide.width * wide.height <= SOTB_TARGET_PIXEL_BUDGET);
    CHECK(SOTB_TARGET_PIXEL_BUDGET == 8294400);
    CHECK((int64_t)wide.width * wide.height > SOTB_TARGET_PIXEL_BUDGET * 99 / 100);

    // An aspect ratio that is not listed behaves as 16:9; a null one too.
    CHECK(sotb_render_target(&sotb_resolutions[5], nullptr).width == 3840);
}

void TestGuardOnRealTarget() {
    CHECK(std::string(SOTB_RESOLUTION_GUARD_SAFE_KEY) == "1440p");
    for (int a = 0; a < SOTB_ASPECT_COUNT; ++a) {
        const int safe = sotb_safe_resolution_index(&sotb_aspect_ratios[a]);
        CHECK(safe == kSafeResolution[a]);
        CHECK(!sotb_target_is_heavy(TargetOf(safe, a)));
    }
    CHECK(sotb_safe_resolution_index(&sotb_aspect_ratios[0]) == sotb_resolution_index("1440p"));

    const fs::path dir = fs::temp_directory_path() / "sotb-aspect-guard-tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string user = dir.string();
    char key[64], aspect[32];

    // 16:9 is what it was: 1800p and 4K arm, 1440p and below do not.
    for (const char* resolution : {"native", "480p", "720p", "1440p"}) {
        CHECK(!sotb_resolution_guard_arm_target(user.c_str(), resolution, "16:9"));
        CHECK(!sotb_resolution_guard_pending_target(user.c_str(), key, sizeof(key), aspect, sizeof(aspect)));
    }
    for (const char* resolution : {"1800p", "4K"}) {
        CHECK(sotb_resolution_guard_arm_target(user.c_str(), resolution, "16:9"));
        CHECK(sotb_resolution_guard_pending_target(user.c_str(), key, sizeof(key), aspect, sizeof(aspect)));
        CHECK(std::string(key) == resolution && std::string(aspect) == "16:9");
    }

    // A wide target is heavy at 1440p, a 4:3 one is not; the file keeps both.
    for (int r = 0; r < SOTB_RESOLUTION_COUNT; ++r) {
        for (int a = 0; a < SOTB_ASPECT_COUNT; ++a) {
            const bool armed = sotb_resolution_guard_arm_target(user.c_str(), kResolutions[r], kKeys[a]) != 0;
            CHECK(armed == kHeavy[r][a]);
            const bool pending = sotb_resolution_guard_pending_target(user.c_str(), key, sizeof(key), aspect, sizeof(aspect)) != 0;
            CHECK(pending == armed);
            if (pending) {
                CHECK(std::string(key) == kResolutions[r] && std::string(aspect) == kKeys[a]);
            }
            sotb_resolution_guard_clear(user.c_str());
        }
    }

    // Arming a safe target removes an earlier attempt; older marker files read as 16:9; damaged ones guard nothing.
    CHECK(sotb_resolution_guard_arm_target(user.c_str(), "1440p", "21:9"));
    CHECK(!sotb_resolution_guard_arm_target(user.c_str(), "1440p", "4:3"));
    CHECK(!sotb_resolution_guard_pending_target(user.c_str(), key, sizeof(key), aspect, sizeof(aspect)));
    const fs::path marker = dir / SOTB_RESOLUTION_GUARD_FILE;
    WriteAll(marker, "4K\n");
    CHECK(sotb_resolution_guard_pending_target(user.c_str(), key, sizeof(key), aspect, sizeof(aspect)));
    CHECK(std::string(key) == "4K" && std::string(aspect) == "16:9");
    WriteAll(marker, "4K\r\n32:9\r\n");
    CHECK(sotb_resolution_guard_pending_target(user.c_str(), key, sizeof(key), aspect, sizeof(aspect)));
    CHECK(std::string(key) == "4K" && std::string(aspect) == "32:9");
    WriteAll(marker, "4K\nbogus\n");
    CHECK(sotb_resolution_guard_pending_target(user.c_str(), key, sizeof(key), aspect, sizeof(aspect)));
    CHECK(std::string(aspect) == "16:9");
    WriteAll(marker, "720p\n32:9\n");
    CHECK(!sotb_resolution_guard_pending_target(user.c_str(), key, sizeof(key), aspect, sizeof(aspect)));
    WriteAll(marker, "not-a-resolution\n21:9\n");
    CHECK(!sotb_resolution_guard_pending_target(user.c_str(), key, sizeof(key), aspect, sizeof(aspect)));
    WriteAll(marker, "");
    CHECK(!sotb_resolution_guard_pending_target(user.c_str(), key, sizeof(key), aspect, sizeof(aspect)));
    CHECK(!sotb_resolution_guard_arm_target(user.c_str(), "not-a-resolution", "21:9"));

    // The key-only functions stay 16:9.
    CHECK(sotb_resolution_guard_arm(user.c_str(), "4K") && sotb_resolution_guard_pending(user.c_str(), key, sizeof(key)));
    CHECK(std::string(key) == "4K");
    sotb_resolution_guard_clear(user.c_str());
    fs::remove_all(dir, ec);

    const std::string recovery = ReadAll(Root() / "launcher/resolution_recovery.cpp");
    CHECK(Contains(recovery, "sotb_safe_resolution_index(") && !Contains(recovery, "SOTB_RESOLUTION_GUARD_SAFE_KEY"));
}

struct SiteSpec {
    uint32_t offset;
    std::array<uint8_t, 10> original;
    int immediate;
    bool height_only;
};

constexpr SiteSpec kSites[] = {
    {0x607f0c, {0x48, 0xb8, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, false},
    {0x6080b7, {0x48, 0xb8, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, false},
    {0x609011, {0x48, 0xb8, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, false},
    {0x609290, {0x48, 0xba, 0x80, 0x07, 0, 0, 0x38, 0x04, 0, 0}, 2, false},
    {0x609037, {0xc7, 0x85, 0x98, 0xdd, 0xff, 0xff, 0x80, 0x07, 0, 0}, 6, false},
    {0x609041, {0xc7, 0x85, 0x9c, 0xdd, 0xff, 0xff, 0x38, 0x04, 0, 0}, 6, true},
};

constexpr const char* kBuilds[] = {"CUSA03762-1.00", "CUSA02871-1.00", "CUSA00951-1.00"};

uint64_t Read64(const std::vector<uint8_t>& data, size_t offset) {
    uint64_t value = 0;
    std::memcpy(&value, data.data() + offset, 8);
    return value;
}

std::vector<uint8_t> LoadImage(const char* build) {
    const fs::path path = Root() / "build" / "image" / build / "boot.bin";
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 128 || std::memcmp(data.data(), "SOTBBOT1", 8)) return {};
    const uint64_t size = Read64(data, 8), segments = Read64(data, 24), relocations = Read64(data, 32), imports = Read64(data, 40);
    size_t offset = 8 + 6 * 8 + 8 + 4 * 8;
    offset += 8 + Read64(data, offset) * 56;
    offset += 8 + Read64(data, offset) * 24;
    offset += segments * 24 + imports * 128 + relocations * 32;
    if (offset + size > data.size()) return {};
    return std::vector<uint8_t>(data.begin() + long(offset), data.begin() + long(offset + size));
}

bool InSite(size_t index) {
    for (const SiteSpec& site : kSites) {
        if (index >= site.offset && index < site.offset + site.original.size()) return true;
    }
    return false;
}

std::vector<size_t> Changed(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after, size_t limit) {
    std::vector<size_t> changed;
    for (size_t i = 0; i < limit; ++i) {
        if (before[i] != after[i]) changed.push_back(i);
    }
    return changed;
}

uint32_t Read32(const std::vector<uint8_t>& image, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, image.data() + offset, 4);
    return value;
}

void TestRealBuilds() {
    std::vector<std::vector<uint8_t>> images;
    for (const char* build : kBuilds) {
        std::vector<uint8_t> image = LoadImage(build);
        if (image.empty()) {
            std::printf("%s not prepared: its fingerprint checks are skipped\n", build);
            continue;
        }
        images.push_back(std::move(image));
    }
    if (images.size() != std::size(kBuilds)) return;

    // The six sites are the same bytes at the same offsets on all three builds, so one table serves them.
    for (const std::vector<uint8_t>& image : images) {
        for (const SiteSpec& site : kSites) {
            CHECK(std::equal(site.original.begin(), site.original.end(), image.begin() + site.offset));
        }
    }
    for (size_t i = 1; i < images.size(); ++i) {
        for (const SiteSpec& site : kSites) {
            CHECK(std::equal(images[0].begin() + site.offset, images[0].begin() + site.offset + 10, images[i].begin() + site.offset));
        }
    }
    CHECK(images[0].size() == images[1].size() && images[1].size() == images[2].size());

    for (size_t build = 0; build < images.size(); ++build) {
        const std::vector<uint8_t>& original = images[build];
        char error[200];
        for (int r = 0; r < SOTB_RESOLUTION_COUNT; ++r) {
            for (int a = 0; a < SOTB_ASPECT_COUNT; ++a) {
                const SotbRenderTarget target = TargetOf(r, a);
                std::vector<uint8_t> image = original;
                CHECK(guest_apply_display_size(image.data(), image.size(), target.width, target.height, error, sizeof(error)));
                for (const size_t index : Changed(original, image, image.size())) CHECK(InSite(index));
                for (const SiteSpec& site : kSites) {
                    if (site.immediate == 2) {
                        CHECK(Read32(image, site.offset + 2) == uint32_t(target.width) &&
                              Read32(image, site.offset + 6) == uint32_t(target.height));
                    } else {
                        CHECK(Read32(image, site.offset + 6) == uint32_t(site.height_only ? target.height : target.width));
                    }
                    CHECK(image[site.offset] == original[site.offset]);
                }
                // A patched image is no longer the build's fingerprint (the native 16:9 target patches nothing).
                const std::vector<uint8_t> patched = image;
                CHECK(guest_apply_display_size(image.data(), image.size(), target.width, target.height, error, sizeof(error)) ==
                      (image == original ? 1 : 0));
                CHECK(image == patched);
            }
        }

        // The native 16:9 target is never patched; the same size through the patch is the original bytes.
        std::vector<uint8_t> image = original;
        CHECK(guest_apply_display_size(image.data(), image.size(), 1920, 1080, error, sizeof(error)));
        CHECK(image == original);

        // Any altered site refuses the whole patch and writes nothing.
        for (const SiteSpec& site : kSites) {
            for (const size_t at : {size_t(site.offset), size_t(site.offset + 2), size_t(site.offset + 9)}) {
                image = original;
                image[at] ^= 1;
                const std::vector<uint8_t> damaged = image;
                CHECK(!guest_apply_display_size(image.data(), image.size(), 3360, 1440, error, sizeof(error)));
                CHECK(image == damaged);
            }
        }
        image = original;
        CHECK(!guest_apply_display_size(image.data(), 0x609290, 3360, 1440, error, sizeof(error)) && image == original);
        CHECK(!guest_apply_display_size(image.data(), image.size(), 4, 1440, error, sizeof(error)) && image == original);

        // No other guest patch writes a byte of a display size site, and the display patch writes nothing else.
        const std::string build_key = kBuilds[build];
        const size_t size = original.size();
        const auto fresh = [&] {
            std::vector<uint8_t> buffer = original;
            buffer.resize(size + 8192, 0);
            return buffer;
        };
        struct Other {
            const char* name;
            std::vector<size_t> changed;
        };
        std::vector<Other> others;
        {
            std::vector<uint8_t> buffer = fresh();
            guest_patches_apply(buffer.data(), size, buffer.data() + size, GUEST_PATCH_CAVE_SIZE);
            others.push_back({"guest_patches", Changed(fresh(), buffer, size)});
        }
        {
            std::vector<uint8_t> buffer = fresh();
            sotb_menu_buttons_apply(buffer.data(), size, build_key.c_str(), SOTB_MENU_BUTTON_LAYOUT_WESTERN);
            others.push_back({"menu buttons", Changed(fresh(), buffer, size)});
        }
        {
            std::vector<uint8_t> buffer = fresh();
            sotb_exit_game_apply(buffer.data(), size, build_key.c_str(), buffer.data() + size + 2048, 2048, error, sizeof(error));
            others.push_back({"exit game", Changed(fresh(), buffer, size)});
        }
        {
            std::vector<uint8_t> buffer = fresh();
            psn_notice_apply(buffer.data(), size, buffer.data() + size + 3072, 1024, error, sizeof(error));
            others.push_back({"psn notice", Changed(fresh(), buffer, size)});
        }
        {
            std::vector<uint8_t> buffer = fresh();
            skip_intro_apply(buffer.data(), size, error, sizeof(error));
            others.push_back({"skip intro", Changed(fresh(), buffer, size)});
        }
        for (const Other& other : others) {
            CHECK(!other.changed.empty() || std::string(other.name) == "menu buttons");
            for (const size_t index : other.changed) CHECK(!InSite(index));
        }
        // The loading hint hook is a vtable entry the loader relocates first, so it is checked by address.
        constexpr size_t kLoadingVtableEntry = 0x3d42610;
        for (size_t i = 0; i < 8; ++i) CHECK(!InSite(kLoadingVtableEntry + i));
    }
}

void TestLoadingHintIndependentOfShape() {
    for (int a = 0; a < SOTB_ASPECT_COUNT; ++a) {
        const SotbRenderTarget target = TargetOf(3, a);
        GuestSlateGeometry allotted{};
        allotted.size_x = float(target.width);
        allotted.size_y = float(target.height);
        allotted.scale = 1.0f;
        allotted.render_m00 = allotted.render_m11 = 1.0f;
        const GuestSlateGeometry out = guest_scale_loading_geometry(&allotted);
        const float scale = std::min(allotted.size_x / 1920.0f, allotted.size_y / 1080.0f);
        CHECK(std::abs(out.scale - scale) < 1e-4f && out.size_x == 1920.0f && out.size_y == 1080.0f);
        CHECK(std::abs(out.position_x - (allotted.size_x - 1920.0f * scale) * 0.5f) < 1e-2f);
        CHECK(std::abs(out.position_y - (allotted.size_y - 1080.0f * scale) * 0.5f) < 1e-2f);
        CHECK(out.position_x * 2.0f + 1920.0f * scale <= allotted.size_x + 1e-2f);
    }
}

void TestPresentationAndPasses() {
    const fs::path gpu = Root() / "gpu";
    const std::string presenter = ReadAll(gpu / "shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    CHECK(!presenter.empty());
    CHECK(Contains(presenter, "expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);"));
    CHECK(Contains(presenter, "frame_width = static_cast<u32>(height * image_ratio);"));
    CHECK(Contains(presenter, "frame_height = static_cast<u32>(width / image_ratio);"));
    CHECK(Contains(presenter, "smaa_pass.Render(cmdbuf, draw_scheduler, image_view, image_size,"));
    CHECK(Contains(presenter, "fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},"));
    CHECK(Contains(presenter, "pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);"));
    CHECK(!Contains(presenter, "1920") && !Contains(presenter, "1080") && !Contains(presenter, "16.0f"));

    int passes = 0;
    for (const fs::directory_entry& pass : fs::directory_iterator(gpu / "shadps4/video_core/renderer_vulkan/host_passes")) {
        const std::string extension = pass.path().extension().string();
        if (extension != ".cpp" && extension != ".h") continue;
        ++passes;
        const std::string text = ReadAll(pass.path());
        CHECK(!Contains(text, "1920") && !Contains(text, "1080"));
    }
    CHECK(passes >= 3);

    // FSR 1 and RCAS follow the real image and window sizes, whatever the shape.
    for (int a = 1; a < SOTB_ASPECT_COUNT; ++a) {
        const SotbRenderTarget in = TargetOf(2, a);
        const Size window = {kWindowWidths[a][2], 1080};
        CHECK(SotbGraphics::FsrPassRuns(true, false, in.width, in.height, window.width, window.height));
        CHECK(!SotbGraphics::FsrPassRuns(false, false, in.width, in.height, window.width, window.height));
        CHECK(SotbGraphics::FsrPassRuns(false, true, in.width, in.height, in.width, in.height));
        CHECK(!SotbGraphics::FsrPassRuns(false, true, in.width, in.height, window.width, window.height));
    }
}

void TestWiring() {
    const std::string loader = ReadAll(Root() / "src/loader.c");
    CHECK(Contains(loader, "const SotbRenderTarget render_target = sotb_render_target(render_resolution, aspect_ratio);"));
    CHECK(Contains(loader, "const int native_target = render_target.width == 1920 && render_target.height == 1080;"));
    CHECK(Contains(loader, "guest_apply_display_size(image, size, render_target.width, render_target.height, error, sizeof(error))"));
    CHECK(Contains(loader, "printf(\"Display: render resolution %dx%d (%s), aspect ratio %s\\n\", render_target.width, render_target.height,"));
    CHECK(Count(loader, "guest_apply_display_size(") == 1);
    CHECK(Contains(loader, "sotb_resolution_guard_arm_target(user_dir, sotb_resolutions[resolution].key, sotb_aspect_ratios[aspect].key);"));

    const std::string launcher = ReadAll(Root() / "launcher/launcher.cpp");
    const size_t display = launcher.find("void DrawDisplay(trg::Ui& ui) {");
    const size_t graphics = launcher.find("void DrawGraphics(trg::Ui& ui) {");
    CHECK(display != std::string::npos && graphics != std::string::npos);
    const std::string page = launcher.substr(display, graphics - display);
    const size_t mode = page.find("\"Display mode\""), window = page.find("\"Window size\""), monitor = page.find("\"Monitor\""),
                 aspect = page.find("\"Aspect Ratio\""), renderer = page.find("\"Renderer\"");
    CHECK(mode < window && window < monitor && monitor < aspect && aspect < renderer);
    CHECK(Count(page, "\"Aspect Ratio\"") == 1 && Contains(page, "kAspectRatioKey, kDefaultAspectRatio, aspects"));
    CHECK(Contains(page, "WindowSizeOf(height, SavedAspectRatio()).width"));
    CHECK(Contains(launcher, "WindowSizesFor(SavedAspectRatio())"));
    CHECK(!Contains(launcher, "WindowSizes()"));
    CHECK(std::string(kAspectRatioHelp).rfind("Sets the game's target rendering format and the Window size choices.", 0) == 0);

    // This round changes the shape of the render target and nothing about the camera, the HUD or the pictures.
    for (const char* name : {"src/guest_display.c", "src/loader.c", "launcher/launcher.cpp", "launcher/settings.cpp"}) {
        const std::string text = ReadAll(Root() / name);
        for (const char* forbidden : {"true_ultrawide", "True Ultrawide", "Hor+", "ortho"}) {
            CHECK(!Contains(text, forbidden));
        }
        CHECK(!Contains(text, std::string("DEBUG_") + "SAULO"));
    }

    const std::string display_source = ReadAll(Root() / "src/guest_display.c");
    CHECK(Count(display_source, "10, {0x") == 6 && Contains(display_source, "static const SizeSite size_sites[]"));
}

} // namespace

int main() {
    TestSettingValues();
    TestPersistence();
    TestWindowSizes();
    TestRenderTargets();
    TestGuardOnRealTarget();
    TestRealBuilds();
    TestLoadingHintIndependentOfShape();
    TestPresentationAndPasses();
    TestWiring();
    std::printf("aspect ratio tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

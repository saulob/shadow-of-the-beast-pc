// True ultrawide: Hor+ for constrained perspective views (gameplay and the title) and the centered 16:9 safe area of the
// game's widgets while such a view is drawn. The projection function and the bytes that prove its conventions, the
// field of view math, which shapes and views are touched, the safe area geometry and its hold time, both hooks on the
// three real builds (identical bytes, nothing else written, no overlap with the other guest patches), refusal of
// anything else, and the wiring of the setting. No window, no game, no GPU.
#include "test_platform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include "guest_patches.h"
}
#include "guest_display.h"
#include "guest_hud.h"
#include "guest_ultrawide.h"
#include "psn_notice.h"
#include "settings.h"
#include "skip_intro.h"
#include "sotb_side_fill.h"
#include "sotb_exit_game.h"
#include "sotb_launcher.h"
#include "sotb_menu_buttons.h"
#include "test_progress.h"

namespace fs = std::filesystem;

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
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

const fs::path Root() { return SOTB_SOURCE_DIR; }

constexpr double kPi = 3.14159265358979323846;
constexpr double kOriginalAspect = 1.777778;
constexpr const char* kBuilds[] = {"CUSA03762-1.00", "CUSA02871-1.00", "CUSA00951-1.00"};

// The projection function's own conventions, as FReversedZPerspectiveMatrix(half, aspect, 1, near) builds them.
double MatrixX(double fov) { return 1.0 / std::tan(fov * kPi / 360.0); }
double MatrixY(double fov, double aspect) { return aspect / std::tan(fov * kPi / 360.0); }

struct View {
    std::array<uint8_t, 0x40> bytes{};
    View(float fov, float aspect, bool constrained, uint8_t mode) {
        std::memcpy(&bytes[SOTB_VIEW_FOV], &fov, 4);
        std::memcpy(&bytes[SOTB_VIEW_ASPECT_RATIO], &aspect, 4);
        bytes[SOTB_VIEW_CONSTRAIN] = constrained ? 1 : 0;
        bytes[SOTB_VIEW_PROJECTION_MODE] = mode;
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (i < SOTB_VIEW_FOV || (i > SOTB_VIEW_FOV + 3 && i < SOTB_VIEW_ASPECT_RATIO) ||
                (i > SOTB_VIEW_ASPECT_RATIO + 3 && i < SOTB_VIEW_CONSTRAIN)) {
                bytes[i] = uint8_t(0xa0 + i);
            }
        }
    }
    float fov() const { float v; std::memcpy(&v, &bytes[SOTB_VIEW_FOV], 4); return v; }
    float aspect() const { float v; std::memcpy(&v, &bytes[SOTB_VIEW_ASPECT_RATIO], 4); return v; }
};

struct ProjectionData {
    std::array<uint8_t, 0xb0> bytes{};
    ProjectionData(int width, int height) {
        const int32_t rect[4] = {0, 0, width, height};
        std::memcpy(&bytes[SOTB_PROJECTION_VIEW_RECT], rect, sizeof(rect));
    }
};

uint64_t g_clock = 1000;
uint64_t FakeClock() { return g_clock; }

bool Adjust(View& view, int width, int height) {
    const std::array<uint8_t, 0x40> before = view.bytes;
    ProjectionData data(width, height);
    sotb_ultrawide_adjust_view(view.bytes.data(), data.bytes.data());
    return view.bytes != before;
}

void TestWhichShapes() {
    for (const char* key : {"21:9", "32:9"}) CHECK(sotb_ultrawide_wanted(key));
    for (const char* key : {"16:9", "16:10", "4:3", "5:4", "", "21:10", "32:9 ", "ultrawide"}) CHECK(!sotb_ultrawide_wanted(key));
    CHECK(!sotb_ultrawide_wanted(nullptr));
    for (int i = 0; i < SOTB_ASPECT_COUNT; ++i) {
        const std::string key = sotb_aspect_ratios[i].key;
        const bool ultrawide = key == "21:9" || key == "32:9";
        CHECK(sotb_ultrawide_wanted(key.c_str()) == ultrawide);
        CHECK(sotb_ultrawide_enabled(key.c_str(), 1) == ultrawide);
        CHECK(!sotb_ultrawide_enabled(key.c_str(), 0));
    }
}

void TestHorPlusMath() {
    // The vertical view of the 16:9 original is kept and the horizontal one grows with the width.
    for (const double fov : {30.0, 39.5, 60.0, 61.5, 90.0, 100.0, 120.0}) {
        double previous = fov;
        for (const double target : {1920.0 / 1080, 4384.0 / 1880, 2520.0 / 1080, 3840.0 / 1080, 5408.0 / 1520}) {
            const double aspect = std::max(target, kOriginalAspect);
            const double wide = sotb_ultrawide_fov(fov, kOriginalAspect, aspect);
            CHECK(std::abs(MatrixY(wide, aspect) - MatrixY(fov, kOriginalAspect)) < 1e-9);
            CHECK(std::abs(MatrixX(wide) * aspect - MatrixX(fov) * kOriginalAspect) < 1e-9);
            CHECK(wide >= previous - 1e-12 && wide < 180.0);
            previous = wide;
        }
        CHECK(std::abs(sotb_ultrawide_fov(fov, kOriginalAspect, kOriginalAspect) - fov) < 1e-9);
    }
    CHECK(std::abs(sotb_ultrawide_fov(90.0, 16.0 / 9, 21.0 / 9) - 105.4) < 0.1);
    CHECK(std::abs(sotb_ultrawide_fov(90.0, 16.0 / 9, 32.0 / 9) - 126.87) < 0.1);
    // The gameplay view of the approved test: 61.5 degrees in 4384 x 1880 became 75.94.
    CHECK(std::abs(sotb_ultrawide_fov(61.5, 1.7778, 4384.0 / 1880) - 75.94) < 0.01);
    CHECK(std::abs(sotb_ultrawide_fov(90.0, 16.0 / 9, 21.0 / 9) - 90.0 * 21.0 / 16.0) > 5.0);
}

void TestWhichViews() {
    sotb_ultrawide_set_clock(FakeClock);

    // Every target the two shapes produce is widened; the vertical view stays the original's, whatever the camera's
    // own field of view (gameplay's 61.5, the title's 39.5, the 90 of a default camera).
    for (const float fov : {39.5f, 61.5f, 90.0f}) {
        for (int r = 0; r < SOTB_RESOLUTION_COUNT; ++r) {
            for (const char* key : {"21:9", "32:9"}) {
                const SotbRenderTarget target = sotb_render_target(&sotb_resolutions[r], &sotb_aspect_ratios[sotb_aspect_index(key)]);
                View view(fov, 1.777778f, true, 0);
                CHECK(Adjust(view, target.width, target.height));
                const double viewport = double(target.width) / target.height;
                CHECK(std::abs(view.aspect() - viewport) < 1e-4 && view.fov() > fov && view.fov() < 180.0f);
                CHECK(std::abs(MatrixY(view.fov(), view.aspect()) - MatrixY(fov, 1.777778)) < 1e-3);
                CHECK(std::abs(MatrixX(view.fov()) - MatrixX(fov) * 1.777778 / viewport) < 1e-3);
                CHECK(view.bytes[SOTB_VIEW_CONSTRAIN] == 1 && view.bytes[SOTB_VIEW_PROJECTION_MODE] == 0);
            }
        }
    }

    // Nothing else of the view is written, and a second call changes nothing.
    {
        View view(75.0f, 1.777778f, true, 0);
        const View original = view;
        CHECK(Adjust(view, 4384, 1880));
        for (size_t i = 0; i < view.bytes.size(); ++i) {
            const bool field = (i >= SOTB_VIEW_FOV && i < SOTB_VIEW_FOV + 4) || (i >= SOTB_VIEW_ASPECT_RATIO && i < SOTB_VIEW_ASPECT_RATIO + 4);
            CHECK(field || view.bytes[i] == original.bytes[i]);
        }
        CHECK(!Adjust(view, 4384, 1880));
    }

    // The other shapes (and 16:9 itself) never get a wider view: their targets are not wider than the camera's.
    for (int r = 0; r < SOTB_RESOLUTION_COUNT; ++r) {
        for (const char* key : {"16:9", "16:10", "4:3", "5:4"}) {
            const SotbRenderTarget target = sotb_render_target(&sotb_resolutions[r], &sotb_aspect_ratios[sotb_aspect_index(key)]);
            View view(90.0f, 1.777778f, true, 0);
            CHECK(!Adjust(view, target.width, target.height));
        }
    }

    // Views that are not a constrained perspective camera are left alone: the map, the options and the default camera.
    {
        View unconstrained(90.0f, 1.777778f, false, 0);
        CHECK(!Adjust(unconstrained, 4384, 1880));
        View orthographic(90.0f, 1.777778f, true, 1);
        CHECK(!Adjust(orthographic, 4384, 1880));
        View default_camera(90.0f, 1.3333f, false, 0);
        CHECK(!Adjust(default_camera, 4384, 1880));
        for (const float aspect : {0.0f, -1.0f, 0.4f, 5.0f}) {
            View odd(90.0f, aspect, true, 0);
            CHECK(!Adjust(odd, 4384, 1880));
        }
        for (const float fov : {0.0f, -10.0f, 175.0f, 400.0f}) {
            View odd(fov, 1.777778f, true, 0);
            CHECK(!Adjust(odd, 4384, 1880));
        }
        View empty(90.0f, 1.777778f, true, 0);
        CHECK(!Adjust(empty, 0, 0) && !Adjust(empty, 4384, 0) && !Adjust(empty, -5, 1080));
        sotb_ultrawide_adjust_view(nullptr, nullptr);
        View view(90.0f, 1.777778f, true, 0);
        sotb_ultrawide_adjust_view(view.bytes.data(), nullptr);
    }
    sotb_ultrawide_set_clock(nullptr);
}

void TestSafeAreaGeometry() {
    GuestSlateGeometry wide{};
    wide.size_x = 4384.0f;
    wide.size_y = 1880.0f;
    wide.scale = 1.0f;
    wide.render_m00 = wide.render_m11 = 1.0f;

    const GuestSlateGeometry safe = guest_safe_area_geometry(&wide);
    CHECK(std::abs(safe.size_x - 3342.222f) < 0.01f && safe.size_y == 1880.0f);
    CHECK(std::abs(safe.position_x - 520.889f) < 0.01f && std::abs(safe.abs_pos_x - 520.889f) < 0.01f);
    CHECK(std::abs(safe.render_tx - 520.889f) < 0.01f && safe.scale == 1.0f);
    // Left of the safe area, its centre and its right are the same points the screen's own centre and margins give.
    CHECK(std::abs((safe.abs_pos_x + safe.size_x / 2) - wide.size_x / 2) < 0.01f);
    CHECK(std::abs((safe.abs_pos_x + safe.size_x) - (wide.size_x - safe.abs_pos_x)) < 0.01f);

    GuestSlateGeometry super = wide;
    super.size_x = 5408.0f;
    super.size_y = 1520.0f;
    const GuestSlateGeometry super_safe = guest_safe_area_geometry(&super);
    CHECK(std::abs(super_safe.size_x - 2702.222f) < 0.01f && std::abs(super_safe.abs_pos_x - 1352.889f) < 0.01f);

    // A scaled and offset parent: the offset follows its scale in absolute space and its transform in render space.
    GuestSlateGeometry scaled = wide;
    scaled.scale = 2.0f;
    scaled.abs_pos_x = 100.0f;
    scaled.position_x = 10.0f;
    scaled.render_m00 = 2.0f;
    scaled.render_tx = 100.0f;
    const GuestSlateGeometry scaled_safe = guest_safe_area_geometry(&scaled);
    CHECK(std::abs(scaled_safe.abs_pos_x - (100.0f + 520.889f * 2.0f)) < 0.01f);
    CHECK(std::abs(scaled_safe.position_x - 530.889f) < 0.01f && std::abs(scaled_safe.render_tx - (100.0f + 520.889f * 2.0f)) < 0.01f);

    // 16:9 and narrower shapes are returned unchanged, and so is an empty geometry.
    for (const std::pair<float, float> size : {std::pair{1920.0f, 1080.0f}, {854.0f, 480.0f}, {1728.0f, 1080.0f}, {1440.0f, 1080.0f},
                                              {1352.0f, 1080.0f}, {3840.0f, 2160.0f}, {0.0f, 0.0f}, {-5.0f, 100.0f}}) {
        GuestSlateGeometry geometry = wide;
        geometry.size_x = size.first;
        geometry.size_y = size.second;
        const GuestSlateGeometry same = guest_safe_area_geometry(&geometry);
        CHECK(std::memcmp(&same, &geometry, sizeof(geometry)) == 0);
    }
}

void TestSafeAreaHold() {
    sotb_ultrawide_set_clock(FakeClock);
    g_clock = 5000;
    CHECK(!sotb_ultrawide_safe_area_active());

    // Views that are not widened never turn it on.
    View unconstrained(90.0f, 1.777778f, false, 0);
    View orthographic(90.0f, 1.777778f, true, 1);
    View narrow(90.0f, 1.777778f, true, 0);
    CHECK(!Adjust(unconstrained, 4384, 1880) && !Adjust(orthographic, 4384, 1880) && !Adjust(narrow, 1920, 1080));
    CHECK(!sotb_ultrawide_safe_area_active());

    // A widened view turns it on for the hold time and for no longer.
    View title(39.5f, 1.777778f, true, 0);
    CHECK(Adjust(title, 4384, 1880));
    CHECK(sotb_ultrawide_safe_area_active());
    g_clock += SOTB_SAFE_AREA_HOLD_MS - 1;
    CHECK(sotb_ultrawide_safe_area_active());
    g_clock += 1;
    CHECK(sotb_ultrawide_safe_area_active());
    g_clock += 1;
    CHECK(!sotb_ultrawide_safe_area_active());

    // Each widened view starts the hold again; a clock that went backwards turns it off.
    View gameplay(61.5f, 1.777778f, true, 0);
    CHECK(Adjust(gameplay, 4384, 1880));
    g_clock += 500;
    CHECK(sotb_ultrawide_safe_area_active());
    View again(61.5f, 1.777778f, true, 0);
    CHECK(Adjust(again, 4384, 1880));
    g_clock += 500;
    CHECK(sotb_ultrawide_safe_area_active());
    g_clock -= 10000;
    CHECK(!sotb_ultrawide_safe_area_active());

    // Setting the clock again forgets the last widened view.
    g_clock = 20000;
    View last(61.5f, 1.777778f, true, 0);
    CHECK(Adjust(last, 4384, 1880) && sotb_ultrawide_safe_area_active());
    sotb_ultrawide_set_clock(FakeClock);
    CHECK(!sotb_ultrawide_safe_area_active());
    sotb_ultrawide_set_clock(nullptr);
    CHECK(!sotb_ultrawide_safe_area_active());
}

std::vector<uint8_t> LoadImage(const char* build, std::vector<std::pair<uint64_t, uint64_t>>* relocations = nullptr) {
    const fs::path path = Root() / "build" / "image" / build / "boot.bin";
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto u64 = [&](size_t offset) {
        uint64_t value = 0;
        std::memcpy(&value, data.data() + offset, 8);
        return value;
    };
    if (data.size() < 128 || std::memcmp(data.data(), "SOTBBOT1", 8)) return {};
    const uint64_t size = u64(8), segments = u64(24), relocs = u64(32), imports = u64(40);
    size_t offset = 8 + 6 * 8 + 8 + 4 * 8;
    offset += 8 + u64(offset) * 56;
    offset += 8 + u64(offset) * 24;
    offset += segments * 24 + imports * 128;
    const size_t relocation_table = offset;
    offset += relocs * 32;
    if (offset + size > data.size()) return {};
    if (relocations) {
        for (uint64_t i = 0; i < relocs; ++i) {
            if (u64(relocation_table + i * 32 + 8) == 0) {
                relocations->emplace_back(u64(relocation_table + i * 32), u64(relocation_table + i * 32 + 16));
            }
        }
    }
    return std::vector<uint8_t>(data.begin() + long(offset), data.begin() + long(offset + size));
}

void Relocate(std::vector<uint8_t>& image, const std::vector<std::pair<uint64_t, uint64_t>>& relocations) {
    for (const auto& [target, value] : relocations) {
        const uint64_t pointer = uint64_t(reinterpret_cast<uintptr_t>(image.data())) + value;
        std::memcpy(image.data() + target, &pointer, sizeof(pointer));
    }
}

size_t CountOf(const std::vector<uint8_t>& image, const uint8_t* needle, size_t size) {
    size_t count = 0;
    const auto end = image.end();
    for (auto at = std::search(image.begin(), end, needle, needle + size); at != end;
         at = std::search(at + 1, end, needle, needle + size)) {
        ++count;
    }
    return count;
}

float FloatAtRipTarget(const std::vector<uint8_t>& image, size_t instruction, size_t length, size_t disp_offset) {
    int32_t disp = 0;
    std::memcpy(&disp, image.data() + instruction + disp_offset, 4);
    float value = 0.0f;
    std::memcpy(&value, image.data() + instruction + length + disp, 4);
    return value;
}

bool BytesAt(const std::vector<uint8_t>& image, size_t at, std::initializer_list<uint8_t> bytes) {
    return at + bytes.size() <= image.size() && std::equal(bytes.begin(), bytes.end(), image.begin() + long(at));
}

// What the constrained perspective path of the function does, read from its own instructions.
void TestProjectionConventions(const std::vector<uint8_t>& image) {
    const size_t f = SOTB_ULTRAWIDE_SITE;
    CHECK(BytesAt(image, f, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53}));
    CHECK(BytesAt(image, f + 0x24, {0x41, 0x0f, 0xb7, 0x46, 0x2c, 0xa8, 0x01, 0x0f, 0x84}));
    CHECK(BytesAt(image, f + 0x66, {0x41, 0x0f, 0xb6, 0x46, 0x2d, 0x83, 0xf8, 0x01, 0x0f, 0x85}));
    CHECK(BytesAt(image, 0x127ff39, {0xc4, 0xc1, 0x7a, 0x10, 0x46, 0x18}));
    CHECK(BytesAt(image, 0x127ff3f, {0xc4, 0xc1, 0x7a, 0x10, 0x4e, 0x28}));
    CHECK(BytesAt(image, 0x127ff4d, {0xc5, 0xfa, 0x59, 0x0d}));
    CHECK(std::abs(FloatAtRipTarget(image, 0x127ff4d, 8, 4) - float(kPi / 360.0)) < 1e-9f);
    CHECK(BytesAt(image, 0x127ff85, {0xe8}));
    CHECK(BytesAt(image, 0x127ff92, {0xc5, 0xf2, 0x5e, 0xc8}));
    CHECK(BytesAt(image, 0x127ff96, {0xc5, 0xfa, 0x10, 0x95, 0x48, 0xff, 0xff, 0xff, 0xc5, 0xea, 0x5e, 0xc0}));
    CHECK(BytesAt(image, 0x127ffb7, {0xc5, 0xfa, 0x11, 0x4b, 0x50}));
    CHECK(BytesAt(image, 0x127ffcc, {0xc5, 0xfa, 0x11, 0x43, 0x64}));
    CHECK(FloatAtRipTarget(image, 0x127ff8a, 8, 4) == 1.0f);
    CHECK(BytesAt(image, 0x127fdb1, {0xc4, 0xc1, 0x7a, 0x10, 0x46, 0x28}));
    CHECK(BytesAt(image, 0x127fdb7, {0x48, 0x8d, 0xb3, 0x90, 0x00, 0x00, 0x00}));
    CHECK(BytesAt(image, 0x127fecb, {0xc4, 0xe2, 0x79, 0x35, 0x8b, 0x98, 0x00, 0x00, 0x00}));
    int callers = 0;
    for (size_t i = 0; i + 5 <= image.size() && i < 0x3be8018; ++i) {
        if (image[i] != 0xe8 && image[i] != 0xe9) continue;
        int32_t disp = 0;
        std::memcpy(&disp, image.data() + i + 1, 4);
        callers += int64_t(i) + 5 + disp == SOTB_ULTRAWIDE_SITE ? 1 : 0;
    }
    CHECK(callers == 2);
}

// The game layer manager: its constructor stores the vtable object at +0x10, and slot 62 is the arrangement it inherits.
void TestGameLayerConventions(const std::vector<uint8_t>& image) {
    const size_t c = SOTB_GAME_LAYER_CONSTRUCTOR;
    CHECK(BytesAt(image, c, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb}));
    int32_t disp = 0;
    std::memcpy(&disp, image.data() + c + 0x11, 4);
    CHECK(BytesAt(image, c + 0x0e, {0x48, 0x8d, 0x05}) && int64_t(c) + 0x15 + disp + 0x10 == SOTB_GAME_LAYER_VTABLE);
    CHECK(BytesAt(image, c + 0x15, {0x48, 0x8d, 0x48, 0x10, 0x48, 0x89, 0x0b}));
    CHECK(SOTB_GAME_LAYER_ARRANGE_SLOT == 62);
    // The loading screen's vtable has SCompoundWidget::OnArrangeChildren in the same slot.
    CHECK(BytesAt(image, 0x808a50, {0x48, 0x8d, 0x87, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x8f, 0x38, 0x01}));
}

// UFrontendManager: the constructor stores the vtable object at +0x10 and clears the stack array, the top reader takes the
// last 16-byte entry's widget, and the per-frame function sits in slot 80.
void TestFrontendConventions(const std::vector<uint8_t>& image) {
    const size_t c = SOTB_FRONTEND_CONSTRUCTOR;
    CHECK(BytesAt(image, c, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb}));
    int32_t disp = 0;
    std::memcpy(&disp, image.data() + c + 0x11, 4);
    CHECK(BytesAt(image, c + 0x0e, {0x48, 0x8d, 0x05}) && int64_t(c) + 0x15 + disp + 0x10 == SOTB_FRONTEND_VTABLE);
    CHECK(BytesAt(image, c + 0x15, {0x48, 0x8d, 0x48, 0x10, 0x48, 0x89, 0x0b}));
    CHECK(SOTB_FRONTEND_STACK_DATA == 0x280 && SOTB_FRONTEND_STACK_NUM == 0x288 && SOTB_FRONTEND_STACK_ENTRY == 16);
    CHECK(BytesAt(image, c + 0x59, {0x48, 0xc7, 0x83, 0x88, 0x02, 0x00, 0x00}));
    CHECK(BytesAt(image, c + 0x64, {0x48, 0xc7, 0x83, 0x80, 0x02, 0x00, 0x00}));

    const size_t t = SOTB_FRONTEND_TOP_WIDGET;
    CHECK(BytesAt(image, t + 6, {0x8b, 0x8f, 0x88, 0x02, 0x00, 0x00}));
    CHECK(BytesAt(image, t + 0x17, {0x48, 0x8b, 0x8f, 0x80, 0x02, 0x00, 0x00, 0x48, 0xc1, 0xe0, 0x04, 0x48, 0x8b, 0x5c, 0x01, 0x08}));

    CHECK(BytesAt(image, SOTB_FRONTEND_TICK, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}));
    CHECK(SOTB_FRONTEND_VTABLE + SOTB_FRONTEND_TICK_SLOT * 8 == 0x3c0a510);
}

// The values the game gives the FrontendScreen enumerators are their order of registration, which is the order of their
// names in the image.
std::vector<std::wstring> FrontendScreenNames(const std::vector<uint8_t>& image) {
    const std::string first = "FrontendScreen::None";
    std::vector<uint8_t> needle;
    for (const char c : first) {
        needle.push_back(uint8_t(c));
        needle.push_back(0);
    }
    std::vector<std::wstring> names;
    auto at = std::search(image.begin(), image.end(), needle.begin(), needle.end());
    if (at == image.end()) return names;
    for (size_t cursor = size_t(at - image.begin()); names.size() < 80;) {
        std::wstring name;
        while (cursor + 1 < image.size() && (image[cursor] || image[cursor + 1])) {
            name.push_back(wchar_t(image[cursor] | (image[cursor + 1] << 8)));
            cursor += 2;
        }
        cursor += 2;
        if (name.rfind(L"FrontendScreen::", 0) != 0) break;
        names.push_back(name.substr(16));
        if (names.back() == L"FrontendScreen_MAX") break;
    }
    return names;
}

void TestFrontendScreens(const std::vector<uint8_t>& image) {
    const std::vector<std::wstring> names = FrontendScreenNames(image);
    CHECK(names.size() == 39 && names.front() == L"None" && names.back() == L"FrontendScreen_MAX");
    const auto value = [&](const wchar_t* name) {
        const auto at = std::find(names.begin(), names.end(), name);
        return at == names.end() ? -1 : int(at - names.begin());
    };
    CHECK(value(L"MainMenu") == SOTB_SCREEN_MAIN_MENU && value(L"Map") == SOTB_SCREEN_MAP);
    CHECK(value(L"MapLiveFeed") == SOTB_SCREEN_MAP_LIVE_FEED && value(L"MapConfirmationOverlayScreen") == SOTB_SCREEN_MAP_CONFIRMATION);
    CHECK(value(L"MapInfoOverlayScreen") == SOTB_SCREEN_MAP_INFO);
    CHECK(value(L"TimelineInfoOverlayScreen") == SOTB_SCREEN_TIMELINE_INFO && value(L"ManaOverlayScreen") == SOTB_SCREEN_MANA_OVERLAY);
    CHECK(value(L"Options") == SOTB_SCREEN_OPTIONS && value(L"ShopContainer") == SOTB_SCREEN_SHOP_CONTAINER);
    for (size_t i = 0; i < names.size(); ++i) {
        const int screen = int(i);
        const int expected = screen == SOTB_SCREEN_MAIN_MENU ? SOTB_CONTEXT_TITLE
                             : (screen == SOTB_SCREEN_MAP || screen == SOTB_SCREEN_MAP_LIVE_FEED || screen == SOTB_SCREEN_MAP_CONFIRMATION ||
                                screen == SOTB_SCREEN_MAP_INFO || screen == SOTB_SCREEN_TIMELINE_INFO || screen == SOTB_SCREEN_MANA_OVERLAY) ? SOTB_CONTEXT_MAP
                             : (screen == SOTB_SCREEN_OPTIONS || screen == SOTB_SCREEN_SHOP_CONTAINER) ? SOTB_CONTEXT_MENU : SOTB_CONTEXT_OTHER;
        CHECK(sotb_frontend_context_of_screen(screen) == expected);
    }
    CHECK(value(L"Pause") == 11 && sotb_frontend_context_of_screen(11) == SOTB_CONTEXT_OTHER);
    for (const wchar_t* other : {L"ControllerSettings", L"Beast89OptionsScreen", L"Beast89PauseScreen", L"DebugOptions", L"EndOfLevel",
                                 L"DeathScreen", L"Credits", L"SaveFlow"}) {
        CHECK(value(other) > 0 && sotb_frontend_context_of_screen(value(other)) == SOTB_CONTEXT_OTHER);
    }
    CHECK(sotb_frontend_context_of_screen(-1) == SOTB_CONTEXT_OTHER && sotb_frontend_context_of_screen(255) == SOTB_CONTEXT_OTHER);
}

struct HudClass {
    const char* name;
    uint32_t vtable;
    int anchor;
};

// The classes whose Draw is wrapped; the full-screen effects (blood, hit flash, fades), the tutorial sheets and the menu and
// debug screens are drawn by direct calls or keep the whole canvas, and are not in this list.
const HudClass kHudClasses[] = {
    {"Health", 0x3c26a60, SOTB_HUD_LEFT}, {"ComboLadder", 0x3c285b0, SOTB_HUD_LEFT}, {"TalismanActivate", 0x3c2ab00, SOTB_HUD_LEFT},
    {"EnemiesRemaining", 0x3c28d30, SOTB_HUD_RIGHT}, {"JetpackBar", 0x3c276e0, SOTB_HUD_RIGHT}, {"PostCombat", 0x3c29840, SOTB_HUD_RIGHT},
    {"PreCombat", 0x3c29c00, SOTB_HUD_RIGHT}, {"Elixir", 0x3c28970, SOTB_HUD_RIGHT}, {"Unlock", 0x3c2ba00, SOTB_HUD_RIGHT},
    {"NetworkPresent", 0x3c29480, SOTB_HUD_RIGHT}, {"TalismanExplanation", 0x3c2aec0, SOTB_HUD_RIGHT},
    {"CutsceneSkip", 0x3c25b60, SOTB_HUD_RIGHT}, {"SaveDataThrobber", 0x3c27e60, SOTB_HUD_RIGHT},
    {"TimeRemaining", 0x3c2b640, SOTB_HUD_CENTRE}, {"RageChain", 0x3c29fc0, SOTB_HUD_CENTRE}, {"TalismanSelect", 0x3c2b280, SOTB_HUD_CENTRE},
    {"EnemyHealthBar", 0x3c262e0, SOTB_HUD_CENTRE}, {"Megaton", 0x3c27aa0, SOTB_HUD_CENTRE}, {"ActionButtonPrompt", 0x3c24670, SOTB_HUD_CENTRE},
    {"TutorialMessage", 0x3c2c540, SOTB_HUD_CENTRE}, {"ActivityRejected", 0x3c24a30, SOTB_HUD_CENTRE}, {"Subtitle", 0x3c2bdc0, SOTB_HUD_CENTRE},
    {"Transmat", 0x3c2c180, SOTB_HUD_CENTRE}, {"SideGlow", 0x3c2a740, SOTB_HUD_SIDE_GLOW},
};
const uint32_t kHudFullScreen[] = {0x3c28220, 0x3c26e20, 0x3c290f0, 0x3c2c900, 0x3c253e0};

struct Changed {
    std::string name;
    std::vector<size_t> indices;
};

std::vector<size_t> Diff(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after) {
    std::vector<size_t> changed;
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i] != after[i]) changed.push_back(i);
    }
    return changed;
}

void TestRealBuilds() {
    std::vector<std::vector<uint8_t>> images;
    std::vector<std::vector<std::pair<uint64_t, uint64_t>>> relocations(std::size(kBuilds));
    for (size_t i = 0; i < std::size(kBuilds); ++i) {
        std::vector<uint8_t> image = LoadImage(kBuilds[i], &relocations[i]);
        if (image.empty()) {
            std::printf("%s not prepared: its fingerprint checks are skipped\n", kBuilds[i]);
            continue;
        }
        images.push_back(std::move(image));
    }
    if (images.size() != std::size(kBuilds)) return;

    // One function, one place, the same bytes on the three builds; the same vtable pointers after relocation.
    for (size_t build = 0; build < images.size(); ++build) {
        const std::vector<uint8_t>& image = images[build];
        CHECK(std::equal(images[0].begin() + SOTB_ULTRAWIDE_SITE, images[0].begin() + SOTB_ULTRAWIDE_SITE + SOTB_ULTRAWIDE_SIZE,
                         image.begin() + SOTB_ULTRAWIDE_SITE));
        CHECK(CountOf(image, image.data() + SOTB_ULTRAWIDE_SITE, SOTB_ULTRAWIDE_SIZE) == 1);
        CHECK(CountOf(image, image.data() + SOTB_GAME_LAYER_CONSTRUCTOR, 56) == 1);
        CHECK(std::equal(images[0].begin() + SOTB_GAME_LAYER_CONSTRUCTOR, images[0].begin() + SOTB_GAME_LAYER_CONSTRUCTOR + 56,
                         image.begin() + SOTB_GAME_LAYER_CONSTRUCTOR));
        CHECK(image.size() == images[0].size() && relocations[build].size() == relocations[0].size());
        TestProjectionConventions(image);
        TestGameLayerConventions(image);
        TestFrontendConventions(image);
        TestFrontendScreens(image);
    }

    for (size_t build = 0; build < images.size(); ++build) {
        const std::vector<uint8_t>& original = images[build];
        const size_t size = original.size();
        const std::string build_key = kBuilds[build];
        char error[200];
        const auto fresh = [&] {
            std::vector<uint8_t> buffer = original;
            buffer.resize(size + 8192, 0);
            return buffer;
        };

        // The projection hook: a jump at the function's entry, a cave stub, and nothing else.
        std::vector<uint8_t> hooked = fresh();
        uint8_t* cave = hooked.data() + size + 3584;
        CHECK(guest_ultrawide_apply(hooked.data(), size, cave, 512, error, sizeof(error)));
        const std::vector<size_t> changed = Diff(fresh(), hooked);
        size_t in_site = 0, in_cave = 0;
        for (const size_t index : changed) {
            in_site += index >= SOTB_ULTRAWIDE_SITE && index < SOTB_ULTRAWIDE_SITE + SOTB_ULTRAWIDE_PATCH_SIZE;
            in_cave += index >= size + 3584 && index < size + 3584 + SOTB_ULTRAWIDE_MIN_CAVE_SIZE;
        }
        CHECK(in_site + in_cave == changed.size() && in_site >= 2 && in_site <= SOTB_ULTRAWIDE_PATCH_SIZE);
        CHECK(hooked[SOTB_ULTRAWIDE_SITE] == 0xe9 && hooked[SOTB_ULTRAWIDE_SITE + 5] == 0x90);
        int32_t disp = 0;
        std::memcpy(&disp, &hooked[SOTB_ULTRAWIDE_SITE + 1], 4);
        CHECK(int64_t(SOTB_ULTRAWIDE_SITE) + 5 + disp == int64_t(size + 3584));
        CHECK(std::equal(original.begin() + SOTB_ULTRAWIDE_SITE + 6, original.begin() + SOTB_ULTRAWIDE_SITE + SOTB_ULTRAWIDE_SIZE,
                         hooked.begin() + SOTB_ULTRAWIDE_SITE + 6));

        const uint8_t save[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x48, 0x89, 0xce, 0x48, 0xb8};
        CHECK(std::memcmp(cave, save, sizeof(save)) == 0);
        uint64_t callee = 0, resume = 0;
        std::memcpy(&callee, cave + sizeof(save), 8);
        CHECK(callee == uint64_t(reinterpret_cast<uintptr_t>(sotb_ultrawide_adjust_view)));
        const uint8_t restore[] = {0xff, 0xd0, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};
        CHECK(std::memcmp(cave + sizeof(save) + 8, restore, sizeof(restore)) == 0);
        const uint8_t* displaced = cave + sizeof(save) + 8 + sizeof(restore);
        CHECK(std::memcmp(displaced, &original[SOTB_ULTRAWIDE_SITE], SOTB_ULTRAWIDE_PATCH_SIZE) == 0);
        CHECK(displaced[6] == 0x49 && displaced[7] == 0xba && displaced[16] == 0x41 && displaced[17] == 0xff && displaced[18] == 0xe2);
        std::memcpy(&resume, displaced + 8, 8);
        CHECK(resume == uint64_t(reinterpret_cast<uintptr_t>(hooked.data() + SOTB_ULTRAWIDE_SITE + 6)));
        CHECK(((8 + 5 * 8) % 16) == 0);

        for (const size_t at : {size_t(0), size_t(5), size_t(0x24), size_t(0x1b9), size_t(SOTB_ULTRAWIDE_SIZE - 1)}) {
            std::vector<uint8_t> altered = fresh();
            altered[SOTB_ULTRAWIDE_SITE + at] ^= 1;
            const std::vector<uint8_t> damaged = altered;
            CHECK(!guest_ultrawide_apply(altered.data(), size, altered.data() + size + 3584, 512, error, sizeof(error)));
            CHECK(altered == damaged);
        }
        {
            std::vector<uint8_t> buffer = fresh();
            const std::vector<uint8_t> copy = buffer;
            CHECK(!guest_ultrawide_apply(buffer.data(), SOTB_ULTRAWIDE_SITE + 100, buffer.data() + size, 512, error, sizeof(error)));
            CHECK(!guest_ultrawide_apply(buffer.data(), size, buffer.data() + size, SOTB_ULTRAWIDE_MIN_CAVE_SIZE - 1, error, sizeof(error)));
            CHECK(!guest_ultrawide_apply(nullptr, size, buffer.data() + size, 512, error, sizeof(error)));
            CHECK(!guest_ultrawide_apply(buffer.data(), size, nullptr, 512, error, sizeof(error)));
            std::vector<uint8_t> blank(size + 4096, 0);
            CHECK(!guest_ultrawide_apply(blank.data(), size, blank.data() + size, 512, error, sizeof(error)));
            CHECK(std::all_of(blank.begin(), blank.end(), [](uint8_t v) { return v == 0; }));
            CHECK(buffer == copy);
        }
        {
            std::vector<uint8_t> again = hooked;
            CHECK(!guest_ultrawide_apply(again.data(), size, again.data() + size + 3584, 512, error, sizeof(error)));
        }

        // The safe area hook: one pointer in the vtable of the game layer manager, on the relocated image only.
        std::vector<uint8_t> raw = fresh();
        const std::vector<uint8_t> raw_copy = raw;
        CHECK(!guest_ultrawide_apply_safe_area(raw.data(), size, error, sizeof(error)) && raw == raw_copy);

        std::vector<uint8_t> relocated = fresh();
        Relocate(relocated, relocations[build]);
        const std::vector<uint8_t> before_safe_area = relocated;
        CHECK(guest_ultrawide_apply_safe_area(relocated.data(), size, error, sizeof(error)));
        const std::vector<size_t> slot = Diff(before_safe_area, relocated);
        const size_t slot_address = SOTB_GAME_LAYER_VTABLE + SOTB_GAME_LAYER_ARRANGE_SLOT * 8;
        CHECK(!slot.empty() && slot.front() >= slot_address && slot.back() < slot_address + 8);
        uint64_t before_pointer = 0, after_pointer = 0;
        std::memcpy(&before_pointer, &before_safe_area[slot_address], 8);
        std::memcpy(&after_pointer, &relocated[slot_address], 8);
        CHECK(before_pointer == uint64_t(reinterpret_cast<uintptr_t>(relocated.data())) + 0x808a50 && after_pointer != before_pointer);
        {
            const std::vector<uint8_t> copy = relocated;
            CHECK(!guest_ultrawide_apply_safe_area(relocated.data(), size, error, sizeof(error)) && relocated == copy);
        }

        // Anything unexpected in the constructor, the vtable or the original arrangement refuses and writes nothing.
        const auto refused = [&](auto&& damage) {
            std::vector<uint8_t> buffer = fresh();
            Relocate(buffer, relocations[build]);
            damage(buffer);
            const std::vector<uint8_t> copy = buffer;
            return !guest_ultrawide_apply_safe_area(buffer.data(), buffer.size(), error, sizeof(error)) && buffer == copy;
        };
        CHECK(!refused([](std::vector<uint8_t>&) {}));
        CHECK(refused([&](std::vector<uint8_t>& b) { b[SOTB_GAME_LAYER_CONSTRUCTOR + 20] ^= 1; }));
        CHECK(refused([&](std::vector<uint8_t>& b) { b[SOTB_GAME_LAYER_CONSTRUCTOR] ^= 1; }));
        CHECK(refused([&](std::vector<uint8_t>& b) { b[0x808a50] ^= 1; }));
        CHECK(refused([&](std::vector<uint8_t>& b) { b[SOTB_GAME_LAYER_VTABLE + 62 * 8] ^= 1; }));
        CHECK(refused([&](std::vector<uint8_t>& b) { b[SOTB_GAME_LAYER_VTABLE + 8] ^= 1; }));
        CHECK(refused([&](std::vector<uint8_t>& b) { b[SOTB_GAME_LAYER_VTABLE + 64 * 8] ^= 1; }));
        CHECK(refused([&](std::vector<uint8_t>& b) { b.resize(SOTB_GAME_LAYER_VTABLE + 100); }));
        CHECK(!guest_ultrawide_apply_safe_area(nullptr, size, error, sizeof(error)));

        // The frontend hook writes one vtable slot, refuses any altered byte it relies on and writes nothing then.
        std::vector<uint8_t> frontend = fresh();
        Relocate(frontend, relocations[build]);
        const std::vector<uint8_t> frontend_before = frontend;
        CHECK(guest_ultrawide_apply_frontend(frontend.data(), size, error, sizeof(error)));
        const std::vector<size_t> frontend_changed = Diff(frontend_before, frontend);
        CHECK(!frontend_changed.empty() && frontend_changed.size() <= 8);
        for (const size_t index : frontend_changed) {
            CHECK(index >= SOTB_FRONTEND_VTABLE + SOTB_FRONTEND_TICK_SLOT * 8 && index < SOTB_FRONTEND_VTABLE + SOTB_FRONTEND_TICK_SLOT * 8 + 8);
        }
        const auto refused_frontend = [&](auto&& damage) {
            std::vector<uint8_t> buffer = fresh();
            Relocate(buffer, relocations[build]);
            damage(buffer);
            const std::vector<uint8_t> copy = buffer;
            return !guest_ultrawide_apply_frontend(buffer.data(), buffer.size() - 8192, error, sizeof(error)) && buffer == copy;
        };
        CHECK(!refused_frontend([](std::vector<uint8_t>&) {}));
        for (const size_t at : {size_t(SOTB_FRONTEND_CONSTRUCTOR), size_t(SOTB_FRONTEND_CONSTRUCTOR + 0x59), size_t(SOTB_FRONTEND_CONSTRUCTOR + SOTB_FRONTEND_CONSTRUCTOR_SIZE - 1),
                                size_t(SOTB_FRONTEND_TOP_WIDGET + 7), size_t(SOTB_FRONTEND_TOP_WIDGET + SOTB_FRONTEND_TOP_WIDGET_SIZE - 1),
                                size_t(SOTB_FRONTEND_TICK), size_t(SOTB_FRONTEND_TICK + 0x200), size_t(SOTB_FRONTEND_TICK + SOTB_FRONTEND_TICK_SIZE - 1)}) {
            CHECK(refused_frontend([&](std::vector<uint8_t>& b) { b[at] ^= 1; }));
        }
        CHECK(refused_frontend([&](std::vector<uint8_t>& b) { b[SOTB_FRONTEND_VTABLE + SOTB_FRONTEND_TICK_SLOT * 8] ^= 1; }));
        CHECK(refused_frontend([&](std::vector<uint8_t>& b) { b[SOTB_FRONTEND_VTABLE + (SOTB_FRONTEND_TICK_SLOT + 1) * 8 + 1] ^= 0; b.resize(SOTB_FRONTEND_VTABLE + 100 + 8192); }));
        CHECK(!guest_ultrawide_apply_frontend(nullptr, size, error, sizeof(error)));

        // The HUD hook replaces one vtable slot per wrapped class, the target of the cutscene fade's DrawItem call and a stub in
        // its cave, and nothing else; any altered byte it relies on refuses it.
        std::vector<uint8_t> hud = fresh();
        Relocate(hud, relocations[build]);
        const std::vector<uint8_t> hud_before = hud;
        CHECK(guest_hud_apply(hud.data(), size, hud.data() + size + 3840, 256, error, sizeof(error)));
        const std::vector<size_t> hud_changed = Diff(hud_before, hud);
        CHECK(!hud_changed.empty() && hud_changed.size() <= 24 * 8 + 4 + SOTB_HUD_FADE_STUB_SIZE);
        for (const size_t index : hud_changed) {
            bool inside = (index > SOTB_HUD_FADE_CALL && index < SOTB_HUD_FADE_CALL + 5) ||
                          (index >= size + 3840 && index < size + 3840 + SOTB_HUD_FADE_STUB_SIZE);
            for (const HudClass& c : kHudClasses) inside = inside || (index >= c.vtable + SOTB_HUD_DRAW_SLOT_OFFSET && index < c.vtable + SOTB_HUD_DRAW_SLOT_OFFSET + 8);
            CHECK(inside);
        }
        {
            int32_t rel = 0;
            std::memcpy(&rel, hud.data() + SOTB_HUD_FADE_CALL + 1, 4);
            CHECK(hud[SOTB_HUD_FADE_CALL] == 0xe8 && int64_t(SOTB_HUD_FADE_CALL) + 5 + rel == int64_t(size + 3840));
            CHECK(hud[size + 3840] == 0x48 && hud[size + 3841] == 0xb8 && hud[size + 3850] == 0xff && hud[size + 3851] == 0xe0);
            int32_t original_rel = 0;
            std::memcpy(&original_rel, hud_before.data() + SOTB_HUD_FADE_CALL + 1, 4);
            CHECK(int64_t(SOTB_HUD_FADE_CALL) + 5 + original_rel == int64_t(SOTB_HUD_DRAW_ITEM));
            // The fade tile: GWhiteTexture at the canvas origin, the canvas's own SizeX and SizeY, black with the fade's alpha.
            CHECK(BytesAt(hud_before, 0x2e3773, {0xc4, 0xc1, 0x7a, 0x2a, 0x47, 0x40, 0xc4, 0xc1, 0x7a, 0x2a, 0x4f, 0x44}));
            CHECK(BytesAt(hud_before, 0x2e373b, {0xc7, 0x45, 0xc0, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x45, 0xc4, 0x00, 0x00, 0x00, 0x00}));
            int32_t ctor = 0;
            std::memcpy(&ctor, hud_before.data() + 0x2e37ac, 4);
            CHECK(hud_before[0x2e37ab] == 0xe8 && 0x2e37b0 + int64_t(ctor) == 0x147d6d0);
            CHECK(BytesAt(hud_before, 0x147d6de, {0x48, 0x8b, 0x06, 0x48, 0x89, 0x47, 0x08}));
            CHECK(BytesAt(hud_before, 0x147d723, {0x48, 0x8b, 0x01, 0x48, 0x89, 0x47, 0x38}));
        }
        for (const uint32_t vtable : kHudFullScreen) {
            for (size_t i = 0; i < 0x400; ++i) CHECK(hud[vtable + i] == hud_before[vtable + i]);
        }
        const auto refused_hud = [&](auto&& damage) {
            std::vector<uint8_t> buffer = fresh();
            Relocate(buffer, relocations[build]);
            damage(buffer);
            const std::vector<uint8_t> copy = buffer;
            return !guest_hud_apply(buffer.data(), size, buffer.data() + size + 3840, 256, error, sizeof(error)) && buffer == copy;
        };
        for (const size_t at : {size_t(SOTB_HUD_FADE_BLOCK), size_t(0x2e3775), size_t(SOTB_HUD_FADE_CALL - 6), size_t(SOTB_HUD_FADE_CALL),
                                size_t(SOTB_HUD_FADE_CALL + 1), size_t(SOTB_HUD_FADE_CALL + 4), size_t(SOTB_HUD_DRAW_ITEM),
                                size_t(SOTB_HUD_DRAW_ITEM + SOTB_HUD_DRAW_ITEM_SIZE - 1)}) {
            CHECK(refused_hud([&](std::vector<uint8_t>& b) { b[at] ^= 1; }));
        }
        CHECK(!refused_hud([](std::vector<uint8_t>&) {}));
        for (const size_t at : {size_t(SOTB_HUD_POST_RENDER), size_t(SOTB_HUD_POST_RENDER + SOTB_HUD_POST_RENDER_SIZE - 1), size_t(SOTB_HUD_PUSH_TRANSFORM),
                                size_t(SOTB_HUD_PUSH_TRANSFORM + SOTB_HUD_PUSH_TRANSFORM_SIZE - 1), size_t(SOTB_HUD_POP_TRANSFORM),
                                size_t(SOTB_HUD_POP_TRANSFORM + SOTB_HUD_POP_TRANSFORM_SIZE - 1)}) {
            CHECK(refused_hud([&](std::vector<uint8_t>& b) { b[at] ^= 1; }));
        }
        for (const HudClass& c : kHudClasses) {
            uint64_t draw_pointer = 0;
            std::memcpy(&draw_pointer, hud_before.data() + c.vtable + SOTB_HUD_DRAW_SLOT_OFFSET, 8);
            const size_t draw = size_t(draw_pointer - uint64_t(reinterpret_cast<uintptr_t>(hud.data())));
            CHECK(refused_hud([&](std::vector<uint8_t>& b) { b[draw] ^= 1; }));
            CHECK(refused_hud([&](std::vector<uint8_t>& b) { b[draw + 0x7f] ^= 1; }));
            CHECK(refused_hud([&](std::vector<uint8_t>& b) { b[c.vtable + SOTB_HUD_DRAW_SLOT_OFFSET] ^= 1; }));
        }
        CHECK(!guest_hud_apply(nullptr, size, hud.data() + size + 3840, 256, error, sizeof(error)));
        {
            std::vector<uint8_t> small = fresh();
            Relocate(small, relocations[build]);
            const std::vector<uint8_t> small_copy = small;
            CHECK(!guest_hud_apply(small.data(), 0x2000000, small.data() + size + 3840, 256, error, sizeof(error)) && small == small_copy);
            CHECK(!guest_hud_apply(small.data(), size, nullptr, 256, error, sizeof(error)) && small == small_copy);
            CHECK(!guest_hud_apply(small.data(), size, small.data() + size + 3840, SOTB_HUD_FADE_STUB_SIZE - 1, error, sizeof(error)) && small == small_copy);
        }

        // No overlap with any other guest patch, in the image or in the cave, and all of them still apply together.
        std::vector<Changed> patches;
        const auto record = [&](const char* name, auto&& apply) {
            std::vector<uint8_t> buffer = fresh();
            Relocate(buffer, relocations[build]);
            const std::vector<uint8_t> before = buffer;
            apply(buffer);
            patches.push_back({name, Diff(before, buffer)});
        };
        record("guest_patches", [&](std::vector<uint8_t>& b) { guest_patches_apply(b.data(), size, b.data() + size, GUEST_PATCH_CAVE_SIZE); });
        record("menu buttons", [&](std::vector<uint8_t>& b) { sotb_menu_buttons_apply(b.data(), size, build_key.c_str(), SOTB_MENU_BUTTON_LAYOUT_WESTERN); });
        record("exit game", [&](std::vector<uint8_t>& b) { sotb_exit_game_apply(b.data(), size, build_key.c_str(), b.data() + size + 2048, 2048, error, sizeof(error)); });
        record("psn notice", [&](std::vector<uint8_t>& b) { psn_notice_apply(b.data(), size, b.data() + size + 3072, 512, error, sizeof(error)); });
        record("skip intro", [&](std::vector<uint8_t>& b) { skip_intro_apply(b.data(), size, error, sizeof(error)); });
        record("loading screen", [&](std::vector<uint8_t>& b) { guest_apply_loading_screen_layout(b.data(), size, error, sizeof(error)); });
        const SotbRenderTarget wide = sotb_render_target(&sotb_resolutions[3], &sotb_aspect_ratios[2]);
        record("display size", [&](std::vector<uint8_t>& b) { guest_apply_display_size(b.data(), size, wide.width, wide.height, error, sizeof(error)); });
        record("projection", [&](std::vector<uint8_t>& b) { guest_ultrawide_apply(b.data(), size, b.data() + size + 3584, 512, error, sizeof(error)); });
        record("safe area", [&](std::vector<uint8_t>& b) { guest_ultrawide_apply_safe_area(b.data(), size, error, sizeof(error)); });
        record("frontend", [&](std::vector<uint8_t>& b) { guest_ultrawide_apply_frontend(b.data(), size, error, sizeof(error)); });
        record("hud", [&](std::vector<uint8_t>& b) { guest_hud_apply(b.data(), size, b.data() + size + 3840, 256, error, sizeof(error)); });
        std::vector<size_t> all;
        for (const Changed& patch : patches) {
            CHECK(!patch.indices.empty() || patch.name == "menu buttons");
            all.insert(all.end(), patch.indices.begin(), patch.indices.end());
        }
        std::vector<size_t> sorted = all;
        std::sort(sorted.begin(), sorted.end());
        CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

        std::vector<uint8_t> together = fresh();
        Relocate(together, relocations[build]);
        const std::vector<uint8_t> baseline = together;
        guest_patches_apply(together.data(), size, together.data() + size, GUEST_PATCH_CAVE_SIZE);
        sotb_menu_buttons_apply(together.data(), size, build_key.c_str(), SOTB_MENU_BUTTON_LAYOUT_WESTERN);
        CHECK(sotb_exit_game_apply(together.data(), size, build_key.c_str(), together.data() + size + 2048, 2048, error, sizeof(error)));
        CHECK(psn_notice_apply(together.data(), size, together.data() + size + 3072, 512, error, sizeof(error)));
        CHECK(guest_ultrawide_apply(together.data(), size, together.data() + size + 3584, 512, error, sizeof(error)));
        CHECK(guest_ultrawide_apply_safe_area(together.data(), size, error, sizeof(error)));
        CHECK(guest_ultrawide_apply_frontend(together.data(), size, error, sizeof(error)));
        CHECK(guest_hud_apply(together.data(), size, together.data() + size + 3840, 256, error, sizeof(error)));
        CHECK(skip_intro_apply(together.data(), size, error, sizeof(error)));
        CHECK(guest_apply_display_size(together.data(), size, wide.width, wide.height, error, sizeof(error)));
        CHECK(guest_apply_loading_screen_layout(together.data(), size, error, sizeof(error)));
        // Every byte the combined run wrote lies inside the span of one patch's own writes: pointers inside stubs may
        // equal the byte they replace, so the counts are not compared.
        for (const size_t index : Diff(baseline, together)) {
            bool inside = false;
            for (const Changed& patch : patches) {
                inside = inside || (!patch.indices.empty() && index >= patch.indices.front() && index <= patch.indices.back());
            }
            CHECK(inside);
        }
    }
}

int g_loading_marks = 0;
std::vector<int> g_title_calls;
void RecordLoading() { ++g_loading_marks; }
void RecordFrontend(int context) { g_title_calls.push_back(context); }
int LastFill() { return g_title_calls.empty() ? -1 : g_title_calls.back(); }

// The frontend manager's hook, run for real. The three functions and the vtable slot the hook checks are copied from a real
// image into a small synthetic one, the per-frame function itself is replaced by a return after the patch is applied,
// and the manager is a block of memory holding a vtable pointer and a screen stack, as the game's is.
struct FrontendRig {
    using Tick = void(GUEST_ABI*)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, float);
    uint8_t* image = nullptr;
    Tick hook = nullptr;
    alignas(16) uint8_t manager[0x400]{};
    uint8_t entries[16 * 80]{};

    void SetVtable(bool valid) {
        const uint64_t vtable = uint64_t(reinterpret_cast<uintptr_t>(image)) + SOTB_FRONTEND_VTABLE + (valid ? 0 : 8);
        std::memcpy(manager, &vtable, 8);
    }
    void Set(std::initializer_list<int> screens) { Set(std::vector<int>(screens)); }
    void Set(const std::vector<int>& screens) {
        std::memset(entries, 0, sizeof(entries));
        int32_t count = 0;
        for (const int screen : screens) {
            entries[count * 16] = uint8_t(screen);
            const uint64_t widget = 0x1234000 + uint64_t(count);
            std::memcpy(entries + count * 16 + 8, &widget, 8);
            ++count;
        }
        SetRaw(count, uint64_t(reinterpret_cast<uintptr_t>(entries)));
    }
    void SetRaw(int32_t count, uint64_t data) {
        std::memcpy(manager + SOTB_FRONTEND_STACK_NUM, &count, 4);
        std::memcpy(manager + SOTB_FRONTEND_STACK_DATA, &data, 8);
    }
    void Frame() { hook(manager, 1, 2, 3, 4, 5, 0.016f); }
};
FrontendRig g_rig;

bool MakeFrontendRig() {
    const std::vector<uint8_t> real = LoadImage(kBuilds[0]);
    if (real.empty()) {
        std::printf("%s not prepared: the frontend hook run is skipped\n", kBuilds[0]);
        return false;
    }
    constexpr size_t kSize = 0x3c0b000;
    uint8_t* image = static_cast<uint8_t*>(test_allocate(kSize, 1));
    CHECK(image != nullptr);
    if (!image) return false;
    std::memcpy(image + SOTB_FRONTEND_CONSTRUCTOR, real.data() + SOTB_FRONTEND_CONSTRUCTOR, SOTB_FRONTEND_CONSTRUCTOR_SIZE);
    std::memcpy(image + SOTB_FRONTEND_TOP_WIDGET, real.data() + SOTB_FRONTEND_TOP_WIDGET, SOTB_FRONTEND_TOP_WIDGET_SIZE);
    std::memcpy(image + SOTB_FRONTEND_TICK, real.data() + SOTB_FRONTEND_TICK, SOTB_FRONTEND_TICK_SIZE);
    const uint64_t tick = uint64_t(reinterpret_cast<uintptr_t>(image)) + SOTB_FRONTEND_TICK;
    std::memcpy(image + SOTB_FRONTEND_VTABLE + SOTB_FRONTEND_TICK_SLOT * 8, &tick, 8);
    char error[200];
    CHECK(guest_ultrawide_apply_frontend(image, kSize, error, sizeof(error)));
    image[SOTB_FRONTEND_TICK] = 0xc3;
    std::memcpy(&g_rig.hook, image + SOTB_FRONTEND_VTABLE + SOTB_FRONTEND_TICK_SLOT * 8, sizeof(g_rig.hook));
    g_rig.image = image;
    g_rig.SetVtable(true);
    g_rig.Set({});
    CHECK(g_rig.hook != nullptr && uint64_t(reinterpret_cast<uintptr_t>(g_rig.hook)) != tick);
    return true;
}

struct HudSize {
    int width, height;
};
constexpr HudSize kWide[] = {{4384, 1880}, {5408, 1520}, {3440, 1440}, {2560, 1080}, {5120, 1440}};
constexpr HudSize kNotWide[] = {{3840, 2160}, {1920, 1080}, {1280, 720}, {854, 480}, {1440, 1080}, {1728, 1080}, {1350, 1080}};

// Design points of the 1920 x 1080 layout the HUD uses, and where the game would put them on a 16:9 screen of the same height.
double ScreenX(const SotbHudPlacement& placement, double design_x, int size_y) {
    const double scale = size_y / 1080.0;
    return placement.shift_x + design_x * scale;
}

void TestHudPlacement() {
    for (const HudSize size : kNotWide) {
        for (const int anchor : {SOTB_HUD_LEFT, SOTB_HUD_CENTRE, SOTB_HUD_RIGHT, SOTB_HUD_SIDE_GLOW}) {
            const SotbHudPlacement placement = sotb_hud_placement(anchor, size.width, size.height);
            CHECK(!placement.active && !placement.override_size && placement.shift_x == 0.0f && placement.shift_y == 0.0f);
        }
    }
    CHECK(!sotb_hud_placement(SOTB_HUD_RIGHT, 0, 0).active && !sotb_hud_placement(SOTB_HUD_RIGHT, -5, 1080).active);

    for (const HudSize size : kWide) {
        const double scale = size.height / 1080.0, width = size.height * 16.0 / 9.0;
        const SotbHudPlacement left = sotb_hud_placement(SOTB_HUD_LEFT, size.width, size.height);
        const SotbHudPlacement centre = sotb_hud_placement(SOTB_HUD_CENTRE, size.width, size.height);
        const SotbHudPlacement right = sotb_hud_placement(SOTB_HUD_RIGHT, size.width, size.height);
        for (const SotbHudPlacement& placement : {left, centre, right}) {
            CHECK(placement.active && placement.override_size && std::abs(placement.scale - scale) < 1e-5 &&
                  std::abs(placement.virtual_width - width) <= 0.5 && placement.shift_y == 0.0f);
        }
        // Left anchored: the original margin in height-proportional units, from the real left edge.
        CHECK(left.shift_x == 0.0f && std::abs(ScreenX(left, 17, size.height) - 17 * scale) < 1e-6);
        // Right anchored: the original right margin, from the real right edge, whatever the width.
        for (const double design : {1903.0, 1920.0, 1848.0, 1500.0}) {
            CHECK(std::abs(ScreenX(right, design, size.height) - (size.width - (1920.0 - design) * scale)) <= 0.5);
        }
        // Centre anchored: the design centre on the centre of the real screen.
        for (const double design : {960.0, 700.0, 1200.0}) {
            CHECK(std::abs(ScreenX(centre, design, size.height) - (size.width / 2.0 + (design - 960.0) * scale)) <= 0.5);
        }
        // Nothing leaves the screen: the bottom anchored block (design y 1060) and both margins.
        CHECK(1060 * scale <= size.height && ScreenX(left, 0, size.height) >= 0 && ScreenX(right, 1920, size.height) <= size.width + 0.5);
        // The size is the 16:9 one, not the width's.
        CHECK(scale < size.width / 1920.0 && std::abs(scale - size.height / 1080.0) < 1e-9);

        const SotbHudPlacement glow = sotb_hud_placement(SOTB_HUD_SIDE_GLOW, size.width, size.height);
        CHECK(glow.active && !glow.override_size && glow.shift_x == 0.0f);
        CHECK(std::abs(glow.shift_y + 540.0 * size.width / 1920.0 - size.height / 2.0) < 1e-3);
    }
    CHECK(sotb_hud_placement(SOTB_HUD_RIGHT, 4384, 1880).shift_x == 4384 - 3342 && sotb_hud_placement(SOTB_HUD_CENTRE, 4384, 1880).shift_x == 521.0f);
    CHECK(sotb_hud_placement(SOTB_HUD_RIGHT, 5408, 1520).shift_x == 5408 - 2702 && sotb_hud_placement(SOTB_HUD_CENTRE, 5408, 1520).shift_x == 1353.0f);
}

// The wrapper run for real. Copies of the functions and vtable slots it checks go into a small synthetic image; the Draw
// functions and the canvas transform functions are then replaced by jumps to recorders.
struct HudCall {
    float scale = 0.0f;
    int size_x = 0, size_y = 0, debug_size_x = 0;
    float tx = 0.0f, ty = 0.0f, debug_tx = 0.0f;
    int pushes = 0, pops = 0;
    bool aligned = true;
    void* component = nullptr;
};
HudCall g_hud_call;
std::map<void*, std::vector<std::pair<float, float>>> g_hud_stacks;
uint8_t* g_hud_main_canvas = nullptr;
uint8_t* g_hud_debug_canvas = nullptr;

float StackSum(void* fcanvas, bool x) {
    float sum = 0.0f;
    for (const auto& entry : g_hud_stacks[fcanvas]) sum += x ? entry.first : entry.second;
    return sum;
}

void GUEST_ABI RecordHudDraw(void* component, void* hud, void* canvas) {
    uint8_t* const h = static_cast<uint8_t*>(hud);
    uint8_t* main_canvas = nullptr;
    std::memcpy(&main_canvas, h + 0x340, sizeof(main_canvas));
    std::memcpy(&g_hud_call.scale, h + 0x518, 4);
    std::memcpy(&g_hud_call.size_x, main_canvas + 0x40, 4);
    std::memcpy(&g_hud_call.size_y, main_canvas + 0x44, 4);
    void* fcanvas = nullptr;
    std::memcpy(&fcanvas, main_canvas + 0x90, sizeof(fcanvas));
    g_hud_call.tx = StackSum(fcanvas, true);
    g_hud_call.ty = StackSum(fcanvas, false);
    if (canvas != main_canvas) {
        uint8_t* const debug = static_cast<uint8_t*>(canvas);
        std::memcpy(&g_hud_call.debug_size_x, debug + 0x40, 4);
        void* debug_fcanvas = nullptr;
        std::memcpy(&debug_fcanvas, debug + 0x90, sizeof(debug_fcanvas));
        g_hud_call.debug_tx = StackSum(debug_fcanvas, true);
    }
    g_hud_call.component = component;
}

void GUEST_ABI RecordHudPush(void* fcanvas, const float* matrix) {
    g_hud_call.aligned = g_hud_call.aligned && reinterpret_cast<uintptr_t>(matrix) % 16 == 0;
    const bool identity = matrix[0] == 1 && matrix[5] == 1 && matrix[10] == 1 && matrix[15] == 1 && matrix[14] == 0 && matrix[1] == 0 &&
                          matrix[2] == 0 && matrix[4] == 0 && matrix[6] == 0 && matrix[8] == 0 && matrix[9] == 0;
    g_hud_call.aligned = g_hud_call.aligned && identity;
    g_hud_stacks[fcanvas].emplace_back(matrix[12], matrix[13]);
    ++g_hud_call.pushes;
}

void GUEST_ABI RecordHudPop(void* fcanvas) {
    if (!g_hud_stacks[fcanvas].empty()) g_hud_stacks[fcanvas].pop_back();
    ++g_hud_call.pops;
}

// What the cutscene fade's tile reaches UCanvas::DrawItem with, and the translation the canvas has then.
struct FadeTile {
    int calls = 0;
    float x = 0, y = 0, width = 0, height = 0, tx = 0;
};
FadeTile g_fade;
using DrawItemCall = void(GUEST_ABI*)(void*, void*);
DrawItemCall g_fade_call_target = nullptr;

void GUEST_ABI RecordDrawItem(void* canvas, void* item) {
    const uint8_t* const tile = static_cast<const uint8_t*>(item);
    std::memcpy(&g_fade.x, tile + SOTB_HUD_TILE_POSITION, 4);
    std::memcpy(&g_fade.y, tile + SOTB_HUD_TILE_POSITION + 4, 4);
    std::memcpy(&g_fade.width, tile + SOTB_HUD_TILE_SIZE, 4);
    std::memcpy(&g_fade.height, tile + SOTB_HUD_TILE_SIZE + 4, 4);
    void* fcanvas = nullptr;
    std::memcpy(&fcanvas, static_cast<uint8_t*>(canvas) + 0x90, sizeof(fcanvas));
    g_fade.tx = StackSum(fcanvas, true);
    ++g_fade.calls;
}

// CutsceneSkip's Draw as far as the fade goes: a tile at the origin the size the canvas reports, through the game's own call.
void GUEST_ABI FadeDraw(void* component, void* hud, void* canvas) {
    RecordHudDraw(component, hud, canvas);
    alignas(16) uint8_t tile[0x80] = {};
    int width = 0, height = 0;
    std::memcpy(&width, static_cast<uint8_t*>(canvas) + 0x40, 4);
    std::memcpy(&height, static_cast<uint8_t*>(canvas) + 0x44, 4);
    const float size[2] = {float(width), float(height)};
    std::memcpy(tile + SOTB_HUD_TILE_SIZE, size, sizeof(size));
    g_fade_call_target(canvas, tile);
}

void JumpTo(uint8_t* at, const void* target) {
    const uint8_t jump[6] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(at, jump, sizeof(jump));
    const uint64_t address = uint64_t(reinterpret_cast<uintptr_t>(target));
    std::memcpy(at + sizeof(jump), &address, sizeof(address));
}

void TestHudHookRuns() {
    std::vector<std::pair<uint64_t, uint64_t>> relocations;
    const std::vector<uint8_t> real = LoadImage(kBuilds[0], &relocations);
    if (real.empty()) {
        std::printf("%s not prepared: the HUD hook run is skipped\n", kBuilds[0]);
        return;
    }
    constexpr size_t kSize = 0x3c2e000;
    uint8_t* image = static_cast<uint8_t*>(test_allocate(kSize, 1));
    CHECK(image != nullptr);
    if (!image) return;
    const auto copy = [&](size_t at, size_t size) { std::memcpy(image + at, real.data() + at, size); };
    copy(SOTB_HUD_POST_RENDER, SOTB_HUD_POST_RENDER_SIZE);
    copy(SOTB_HUD_PUSH_TRANSFORM, SOTB_HUD_PUSH_TRANSFORM_SIZE);
    copy(SOTB_HUD_POP_TRANSFORM, SOTB_HUD_POP_TRANSFORM_SIZE);
    copy(SOTB_HUD_FADE_BLOCK, SOTB_HUD_FADE_CALL + 5 - SOTB_HUD_FADE_BLOCK);
    copy(SOTB_HUD_DRAW_ITEM, SOTB_HUD_DRAW_ITEM_SIZE);
    const uint64_t base = uint64_t(reinterpret_cast<uintptr_t>(image));
    std::vector<uint32_t> draws;
    for (const HudClass& c : kHudClasses) {
        uint64_t entry = 0;
        for (const auto& [target, value] : relocations) {
            if (target == c.vtable + SOTB_HUD_DRAW_SLOT_OFFSET) entry = value;
        }
        CHECK(entry != 0);
        const uint32_t draw = uint32_t(entry);
        draws.push_back(draw);
        copy(draw, 0x80);
        const uint64_t slot = base + draw;
        std::memcpy(image + c.vtable + SOTB_HUD_DRAW_SLOT_OFFSET, &slot, 8);
    }
    char error[200];
    uint8_t* const cave = image + kSize - 0x1000;
    const bool applied = guest_hud_apply(image, kSize - 0x1000, cave, 256, error, sizeof(error));
    if (!applied) std::printf("HUD apply: %s\n", error);
    CHECK(applied);
    JumpTo(image + SOTB_HUD_PUSH_TRANSFORM, reinterpret_cast<const void*>(RecordHudPush));
    JumpTo(image + SOTB_HUD_POP_TRANSFORM, reinterpret_cast<const void*>(RecordHudPop));
    int32_t fade_rel = 0;
    std::memcpy(&fade_rel, image + SOTB_HUD_FADE_CALL + 1, 4);
    g_fade_call_target = reinterpret_cast<DrawItemCall>(image + SOTB_HUD_FADE_CALL + 5 + fade_rel);
    JumpTo(image + SOTB_HUD_DRAW_ITEM, reinterpret_cast<const void*>(RecordDrawItem));
    for (const HudClass& c : kHudClasses) {
        const bool fade = std::string(c.name) == "CutsceneSkip";
        JumpTo(image + draws[&c - kHudClasses], reinterpret_cast<const void*>(fade ? FadeDraw : RecordHudDraw));
    }

    using Draw = void(GUEST_ABI*)(void*, void*, void*);
    alignas(16) static uint8_t hud[0x600];
    alignas(16) static uint8_t canvas[0x100];
    alignas(16) static uint8_t debug_canvas[0x100];
    int fcanvas_main = 0, fcanvas_debug = 0;
    const auto setup = [&](int width, int height, float scale) {
        std::memset(hud, 0, sizeof(hud));
        std::memset(canvas, 0, sizeof(canvas));
        std::memset(debug_canvas, 0, sizeof(debug_canvas));
        void* const main_fcanvas = &fcanvas_main;
        void* const second_fcanvas = &fcanvas_debug;
        uint8_t* const main_pointer = canvas;
        std::memcpy(hud + 0x340, &main_pointer, sizeof(main_pointer));
        std::memcpy(hud + 0x518, &scale, 4);
        std::memcpy(canvas + 0x40, &width, 4);
        std::memcpy(canvas + 0x44, &height, 4);
        std::memcpy(canvas + 0x90, &main_fcanvas, sizeof(main_fcanvas));
        std::memcpy(debug_canvas + 0x40, &width, 4);
        std::memcpy(debug_canvas + 0x44, &height, 4);
        std::memcpy(debug_canvas + 0x90, &second_fcanvas, sizeof(second_fcanvas));
        g_hud_stacks.clear();
        g_hud_call = HudCall();
    };
    const auto read_int = [](const uint8_t* at) { int v = 0; std::memcpy(&v, at, 4); return v; };
    const auto read_float = [](const uint8_t* at) { float v = 0; std::memcpy(&v, at, 4); return v; };

    for (const HudClass& c : kHudClasses) {
        uint64_t slot = 0;
        std::memcpy(&slot, image + c.vtable + SOTB_HUD_DRAW_SLOT_OFFSET, 8);
        CHECK(slot != base + uint64_t(draws[&c - kHudClasses]));
        const Draw wrapper = reinterpret_cast<Draw>(slot);
        alignas(16) uint8_t component[0x40] = {};
        const uint64_t vptr = base + c.vtable + 0x10;
        std::memcpy(component, &vptr, 8);

        const bool fade = std::string(c.name) == "CutsceneSkip";
        for (const HudSize size : kNotWide) {
            setup(size.width, size.height, 3.5f);
            g_fade = FadeTile();
            wrapper(component, hud, canvas);
            CHECK(g_hud_call.pushes == 0 && g_hud_call.pops == 0 && g_hud_call.scale == 3.5f && g_hud_call.size_x == size.width);
            // 16:9 and narrower: the fade reaches DrawItem exactly as the game built it.
            if (fade) CHECK(g_fade.calls == 1 && g_fade.x == 0 && g_fade.y == 0 && g_fade.width == size.width && g_fade.height == size.height && g_fade.tx == 0);
        }
        for (const HudSize size : kWide) {
            const SotbHudPlacement placement = sotb_hud_placement(c.anchor, size.width, size.height);
            setup(size.width, size.height, 3.5f);
            g_fade = FadeTile();
            wrapper(component, hud, canvas);
            CHECK(g_hud_call.component == component && g_hud_call.aligned);
            // The cutscene fade covers the whole real canvas, not the right-anchored 16:9 part of it.
            if (fade) {
                CHECK(g_fade.calls == 1 && std::abs(g_fade.x + g_fade.tx) < 0.01f && std::abs(g_fade.x + g_fade.tx + g_fade.width - size.width) < 0.01f);
                CHECK(g_fade.y == 0 && g_fade.height == size.height);
            }
            CHECK(g_hud_call.tx == placement.shift_x && g_hud_call.ty == placement.shift_y);
            if (c.anchor == SOTB_HUD_SIDE_GLOW) {
                CHECK(g_hud_call.scale == 3.5f && g_hud_call.size_x == size.width && g_hud_call.pushes == 1);
            } else {
                CHECK(g_hud_call.scale == placement.scale && g_hud_call.size_x == placement.virtual_width);
                CHECK(g_hud_call.pushes == (c.anchor == SOTB_HUD_LEFT ? 0 : 1));
            }
            // Everything the wrapper touched is put back: scale, sizes, and every pushed transform popped.
            CHECK(g_hud_call.pushes == g_hud_call.pops && g_hud_stacks[&fcanvas_main].empty());
            CHECK(read_float(hud + 0x518) == 3.5f && read_int(canvas + 0x40) == size.width && read_int(canvas + 0x44) == size.height);
        }
        // A second canvas (the debug canvas some components are drawn with) gets the same size and shift.
        setup(5408, 1520, 3.5f);
        wrapper(component, hud, debug_canvas);
        const SotbHudPlacement second = sotb_hud_placement(c.anchor, 5408, 1520);
        CHECK(g_hud_call.debug_tx == second.shift_x && g_hud_call.pushes == g_hud_call.pops);
        CHECK(read_int(debug_canvas + 0x40) == 5408 && read_int(canvas + 0x40) == 5408);
        if (c.anchor != SOTB_HUD_SIDE_GLOW) CHECK(g_hud_call.debug_size_x == second.virtual_width);
    }

    // A component of any other class never reaches the wrapper; one that does with an unknown vtable is not drawn.
    {
        alignas(16) uint8_t stranger[0x40] = {};
        uint64_t slot = 0;
        std::memcpy(&slot, image + kHudClasses[0].vtable + SOTB_HUD_DRAW_SLOT_OFFSET, 8);
        setup(5408, 1520, 1.0f);
        reinterpret_cast<Draw>(slot)(stranger, hud, canvas);
        CHECK(g_hud_call.component == nullptr && g_hud_call.pushes == 0);
        reinterpret_cast<Draw>(slot)(nullptr, hud, canvas);
        CHECK(g_hud_call.component == nullptr);
    }
    for (const uint32_t vtable : kHudFullScreen) {
        for (const HudClass& c : kHudClasses) CHECK(c.vtable != vtable);
    }
    test_release(image, kSize);
}

// The safe area hook running: a synthetic image with the constructor, the vtable slots and an arrangement that records
// the geometry it is given.
void TestSafeAreaHookRuns() {
    const std::array<uint8_t, 56> constructor = {
        0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0xe8, 0x62, 0xd8, 0x3a, 0xff, 0x48, 0x8d, 0x05, 0xeb, 0x83, 0x8b,
        0x02, 0x48, 0x8d, 0x48, 0x10, 0x48, 0x89, 0x0b, 0x48, 0x05, 0x48, 0x02, 0x00, 0x00, 0x48, 0x89, 0x83, 0xa8, 0x01, 0x00,
        0x00, 0x48, 0x8d, 0xbb, 0xb0, 0x01, 0x00, 0x00, 0xe8, 0x1b, 0xf3, 0x33, 0xff, 0x48, 0xc7, 0x83};
    const std::array<uint8_t, 17> arrange_prefix = {0x48, 0x8d, 0x87, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x8d,
                                                    0x8f, 0x38, 0x01, 0x00, 0x00, 0x48, 0x89, 0xf7};
    constexpr size_t kSize = 0x3d14000;
    uint8_t* image = static_cast<uint8_t*>(test_allocate(kSize, 1));
    CHECK(image != nullptr);
    if (!image) return;

    static GuestSlateGeometry captured;
    std::memset(&captured, 0, sizeof(captured));
    std::memcpy(image + SOTB_GAME_LAYER_CONSTRUCTOR, constructor.data(), constructor.size());
    uint8_t* arrange = image + 0x808a50;
    std::memcpy(arrange, arrange_prefix.data(), arrange_prefix.size());
    uint8_t* p = arrange + arrange_prefix.size();
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xfe;                    // mov rsi, rdi (the geometry)
    *p++ = 0x48; *p++ = 0xbf;                                // mov rdi, &captured
    const uint64_t target = uint64_t(reinterpret_cast<uintptr_t>(&captured));
    std::memcpy(p, &target, 8); p += 8;
    *p++ = 0xb9; *p++ = 0x07; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;  // mov ecx, 7
    *p++ = 0xf3; *p++ = 0x48; *p++ = 0xa5;                    // rep movsq
    *p++ = 0xc3;
    const struct { uint32_t slot, offset; } slots[] = {{0, 0x279acd0}, {1, 0x279ace0}, {2, 0x145d9f0}, {3, 0x1de5100}, {4, 0x80aa00},
                                                      {25, 0x145da20}, {61, 0x1de5130}, {62, 0x808a50}, {63, 0x145c760}, {64, 0x145c8d0}};
    for (const auto& slot : slots) {
        const uint64_t pointer = uint64_t(reinterpret_cast<uintptr_t>(image)) + slot.offset;
        std::memcpy(image + SOTB_GAME_LAYER_VTABLE + slot.slot * 8, &pointer, 8);
    }

    char error[200];
    CHECK(guest_ultrawide_apply_safe_area(image, kSize, error, sizeof(error)));
    using Arrange = void(GUEST_ABI*)(void*, const GuestSlateGeometry*, void*);
    Arrange hook = nullptr;
    std::memcpy(&hook, image + SOTB_GAME_LAYER_VTABLE + SOTB_GAME_LAYER_ARRANGE_SLOT * 8, sizeof(hook));
    CHECK(hook != nullptr && uint64_t(reinterpret_cast<uintptr_t>(hook)) != uint64_t(reinterpret_cast<uintptr_t>(arrange)));

    GuestSlateGeometry wide{};
    wide.size_x = 4384.0f;
    wide.size_y = 1880.0f;
    wide.scale = 1.0f;
    wide.render_m00 = wide.render_m11 = 1.0f;
    GuestSlateGeometry narrow = wide;
    narrow.size_x = 1920.0f;
    narrow.size_y = 1080.0f;
    int self = 0, arranged = 0;

    // No widened view drawn: the geometry the game gives is the geometry the arrangement gets, as before.
    sotb_ultrawide_set_clock(FakeClock);
    g_clock = 9000;
    hook(&self, &wide, &arranged);
    CHECK(std::memcmp(&captured, &wide, sizeof(wide)) == 0);

    // A widened view (the title or gameplay) was drawn: the arrangement gets the centered 16:9 part, in the 16:9 case too.
    View view(61.5f, 1.777778f, true, 0);
    CHECK(Adjust(view, 4384, 1880));
    hook(&self, &wide, &arranged);
    const GuestSlateGeometry expected = guest_safe_area_geometry(&wide);
    CHECK(std::memcmp(&captured, &expected, sizeof(expected)) == 0 && std::abs(captured.size_x - 3342.222f) < 0.01f);
    hook(&self, &narrow, &arranged);
    CHECK(std::memcmp(&captured, &narrow, sizeof(narrow)) == 0);

    // Past the hold time the options and the menus get the whole screen again.
    g_clock += SOTB_SAFE_AREA_HOLD_MS + 1;
    hook(&self, &wide, &arranged);
    CHECK(std::memcmp(&captured, &wide, sizeof(wide)) == 0);

    // The title and the map are told by the screen stack, not by the hold: both lay out in the centered safe area with no widened
    // view at all, from the frame they open, and they stay there however long the clock runs.
    if (g_rig.hook) {
        g_rig.Set({SOTB_SCREEN_MAIN_MENU});
        g_rig.Frame();
        g_clock += 100000;
        hook(&self, &wide, &arranged);
        CHECK(std::memcmp(&captured, &expected, sizeof(expected)) == 0);
        View fresh(61.5f, 1.777778f, true, 0);
        CHECK(Adjust(fresh, 4384, 1880));
        g_rig.Set({SOTB_SCREEN_MAP});
        g_rig.Frame();
        g_clock += 5 * SOTB_SAFE_AREA_HOLD_MS;
        hook(&self, &wide, &arranged);
        CHECK(std::memcmp(&captured, &expected, sizeof(expected)) == 0);
        hook(&self, &narrow, &arranged);
        CHECK(std::memcmp(&captured, &narrow, sizeof(narrow)) == 0);
        // Map, the score and mana overlay over it, the map again: every frame lays out in the same safe area, whatever the clock.
        const std::vector<std::vector<int>> sequence = {
            {SOTB_SCREEN_MAIN_MENU, SOTB_SCREEN_MAP}, {SOTB_SCREEN_MAIN_MENU, SOTB_SCREEN_MAP, SOTB_SCREEN_MANA_OVERLAY},
            {SOTB_SCREEN_MAIN_MENU, SOTB_SCREEN_MAP, SOTB_SCREEN_MANA_OVERLAY}, {SOTB_SCREEN_MAIN_MENU, SOTB_SCREEN_MAP}};
        for (const auto& stack : sequence) {
            g_rig.Set(stack);
            g_rig.Frame();
            hook(&self, &wide, &arranged);
            CHECK(std::memcmp(&captured, &expected, sizeof(expected)) == 0);
            g_clock += 3 * SOTB_SAFE_AREA_HOLD_MS;
            hook(&self, &wide, &arranged);
            CHECK(std::memcmp(&captured, &expected, sizeof(expected)) == 0);
        }
        // Options and Wisdom of Shadows: the 1920x1080 composition in the centred 16:9 part of 21:9 and 32:9, with no widened
        // view and whatever the clock; 16:9 is left as the game gives it.
        GuestSlateGeometry super = wide;
        super.size_x = 5408.0f;
        super.size_y = 1520.0f;
        for (const auto& stack : std::vector<std::vector<int>>{{SOTB_SCREEN_MAIN_MENU, SOTB_SCREEN_OPTIONS},
                                                               {SOTB_SCREEN_MAIN_MENU, SOTB_SCREEN_SHOP_CONTAINER},
                                                               {11, SOTB_SCREEN_OPTIONS}}) {
            g_rig.Set(stack);
            g_rig.Frame();
            for (const GuestSlateGeometry* screen : {&wide, &super}) {
                g_clock += 10 * SOTB_SAFE_AREA_HOLD_MS;
                hook(&self, screen, &arranged);
                const float safe = screen->size_y * 16.0f / 9.0f, side = (screen->size_x - safe) * 0.5f;
                CHECK(std::abs(captured.size_x - safe) < 0.01f && captured.size_y == screen->size_y);
                CHECK(std::abs(captured.position_x - side) < 0.01f && std::abs(captured.render_tx - side) < 0.01f);
                CHECK(std::abs((screen->size_x - (captured.position_x + captured.size_x)) - captured.position_x) < 0.01f);
                CHECK(captured.scale == screen->scale && captured.position_y == screen->position_y);
            }
            hook(&self, &narrow, &arranged);
            CHECK(std::memcmp(&captured, &narrow, sizeof(narrow)) == 0);
        }
        g_rig.Set({11});
        g_rig.Frame();
        hook(&self, &wide, &arranged);
        CHECK(std::memcmp(&captured, &wide, sizeof(wide)) == 0);
        View pause(61.5f, 1.777778f, true, 0);
        CHECK(Adjust(pause, 4384, 1880));
        hook(&self, &wide, &arranged);
        CHECK(std::memcmp(&captured, &expected, sizeof(expected)) == 0);
        g_clock += SOTB_SAFE_AREA_HOLD_MS + 1;
        hook(&self, &wide, &arranged);
        CHECK(std::memcmp(&captured, &wide, sizeof(wide)) == 0);
        g_rig.Set({});
        g_rig.Frame();
    }
    sotb_ultrawide_set_clock(nullptr);
    test_release(image, kSize);
}

void TestSetting() {
    const fs::path dir = fs::temp_directory_path() / "sotb-true-ultrawide-tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "sotb.ini";
    const std::string path = file.string();

    CHECK(sotb_true_ultrawide(path.c_str()) == 1 && sotb_true_ultrawide(nullptr) == 1 && sotb_true_ultrawide("") == 1);
    for (const char* on : {"", "aspect_ratio = 21:9\n", "true_ultrawide = 1\n", "true_ultrawide = on\n", "true_ultrawide =\n",
                           "true_ultrawide = banana\n"}) {
        WriteAll(file, on);
        CHECK(sotb_true_ultrawide(path.c_str()) == 1);
    }
    for (const char* off : {"true_ultrawide = 0\n", "true_ultrawide = off\n", "true_ultrawide = false\n", "true_ultrawide = no\n"}) {
        WriteAll(file, off);
        CHECK(sotb_true_ultrawide(path.c_str()) == 0);
    }

    Sotb::LauncherSettings settings(file);
    settings.Set(Sotb::kTrueUltrawideKey, "0");
    CHECK(settings.Save());
    CHECK(sotb_true_ultrawide(path.c_str()) == 0);
    settings.ResetToDefaults();
    CHECK(settings.Save() && sotb_true_ultrawide(path.c_str()) == 1);
    CHECK(std::string(Sotb::kTrueUltrawideKey) == "true_ultrawide");
    bool managed = false;
    for (const Sotb::ManagedSetting& setting : Sotb::ManagedSettings()) {
        managed = managed || (setting.key == Sotb::kTrueUltrawideKey && setting.default_value == "1");
    }
    CHECK(managed);

    // Off turns every effect off whatever the shape; on needs 21:9 or 32:9.
    for (int i = 0; i < SOTB_ASPECT_COUNT; ++i) {
        const std::string key = sotb_aspect_ratios[i].key;
        CHECK(!sotb_ultrawide_enabled(key.c_str(), sotb_true_ultrawide("")) == !(key == "21:9" || key == "32:9"));
        WriteAll(file, "true_ultrawide = 0\n");
        CHECK(!sotb_ultrawide_enabled(key.c_str(), sotb_true_ultrawide(path.c_str())));
    }
    WriteAll(file, "aspect_ratio = 21:9\ntrue_ultrawide = 0\n");
    const Sotb::LauncherSettings off(file);
    CHECK(Contains(Sotb::SettingsSummary(off, nullptr), "Settings: True Ultrawide off\n"));
    WriteAll(file, "aspect_ratio = 4:3\n");
    CHECK(Contains(Sotb::SettingsSummary(Sotb::LauncherSettings(file), nullptr), "Settings: True Ultrawide on (no effect in this shape)\n"));
    WriteAll(file, "aspect_ratio = 32:9\n");
    CHECK(Contains(Sotb::SettingsSummary(Sotb::LauncherSettings(file), nullptr), "Settings: True Ultrawide on\n"));
    fs::remove_all(dir, ec);
}

void TestWiring() {
    const std::string loader = ReadAll(Root() / "src/loader.c");
    const size_t call = loader.find("if (sotb_ultrawide_wanted(aspect_ratio->key)) {");
    CHECK(call != std::string::npos && Count(loader, "guest_ultrawide_apply(") == 1 && Count(loader, "guest_ultrawide_apply_safe_area(") == 1);
    CHECK(Count(loader, "guest_hud_apply(") == 1 && Contains(loader, "guest_hud_apply(image, size, cave + GUEST_CAVE_HUD, cave_size - GUEST_CAVE_HUD, reason, sizeof(reason))"));
    CHECK(loader.find("guest_hud_apply(") > loader.find("guest_ultrawide_apply(image, size, cave + GUEST_CAVE_ULTRAWIDE") &&
          loader.find("guest_hud_apply(") < loader.find("Ultrawide: Hor+ unavailable"));
    CHECK(!Contains(loader, "hud_canvas") && !Contains(ReadAll(Root() / "src/guest_ultrawide.c"), "hud_canvas"));
    CHECK(Count(loader, "guest_ultrawide_apply_frontend(") == 1);
    CHECK(Contains(loader, "guest_ultrawide_apply_frontend(image, size, reason, sizeof(reason))"));
    CHECK(Contains(loader, "if (!sotb_ultrawide_enabled(aspect_ratio->key, sotb_true_ultrawide(settings_file))) {"));
    CHECK(Contains(loader, "guest_ultrawide_apply(image, size, cave + GUEST_CAVE_ULTRAWIDE, cave_size - GUEST_CAVE_ULTRAWIDE, reason, sizeof(reason))"));
    CHECK(Contains(loader, "guest_ultrawide_apply_safe_area(image, size, reason, sizeof(reason))"));
    CHECK(call > loader.find("psn_notice_apply(image, size, cave + GUEST_CAVE_PSN_NOTICE") &&call < loader.find("protect(cave, round_page(cave_size + lzcnt_stubs), 5);"));
    CHECK(Contains(loader, "const SotbRenderTarget render_target = sotb_render_target(render_resolution, aspect_ratio);"));
    CHECK(Count(loader, "sotb_ultrawide_enable_side_fill(") == 1);
    CHECK(Contains(loader, "sotb_ultrawide_enable_side_fill(sotbgpu_mark_loading_fill, sotbgpu_set_frontend_fill);"));
    CHECK(!Contains(loader, "gameplay_started"));
    CHECK(loader.find("guest_ultrawide_apply_frontend(") > loader.find("guest_ultrawide_apply_safe_area(") &&
          loader.find("sotb_ultrawide_enable_side_fill(") > loader.find("guest_ultrawide_apply_frontend(") &&
          loader.find("sotb_ultrawide_enable_side_fill(") < loader.find("Ultrawide: Hor+ unavailable"));
    // The relocations are applied before the patches, which the safe area hook needs.
    CHECK(loader.find("memcpy(image + relocs[i].target, &value, 8);") < call);

    const std::string source = ReadAll(Root() / "src/guest_ultrawide.c");
    CHECK(Count(source, "image[") == 2 && Count(source, "memcpy(image") == 3);
    CHECK(!Contains(source, "1920") && !Contains(source, "1080") && !Contains(source, "printf(\"") && !Contains(source, "DEBUG_" "SAULO"));
    CHECK(!Contains(source, "gameplay_reached") && !Contains(source, "gameplay_probe") && Count(source, "loading_sink()") == 1);
    CHECK(Contains(source, "const int widen = constrained && perspective && wider && fov > 0.0 && fov < 170.0;\n    if (!widen) return;\n    const float new_fov"));
    // The projection hook knows nothing of the title, and neither the title nor the map is read from a clock or a view.
    const size_t adjust = source.find("void GUEST_ABI sotb_ultrawide_adjust_view("), adjust_end = source.find("static void set_frontend_context");
    CHECK(adjust != std::string::npos && adjust < adjust_end && !Contains(source.substr(adjust, adjust_end - adjust), "title"));
    const size_t observe = source.find("static GUEST_ABI void frontend_tick("), observe_end = source.find("int guest_ultrawide_apply_frontend(");
    CHECK(observe != std::string::npos && observe < observe_end);
    const std::string tick = source.substr(observe, observe_end - observe);
    CHECK(!Contains(tick, "now_ms") && !Contains(tick, "last_widened") && !Contains(tick, "widened_once") && !Contains(tick, "clock"));
    CHECK(Contains(source, "if (!sotb_ultrawide_use_safe_area(sotb_frontend_context(), sotb_ultrawide_safe_area_active())) {"));
    CHECK(Contains(source, "if (context == SOTB_CONTEXT_TITLE || context == SOTB_CONTEXT_MAP || context == SOTB_CONTEXT_MENU) return 1;\n    return widened_recently;"));

    // The safe area is applied to the layer manager only, never to the map, the options or the loading screen.
    const std::string display = ReadAll(Root() / "src/guest_display.c");
    CHECK(Count(display, "LOADING_VTABLE_ARRANGE_OFFSET 0x3d42610u") == 1 && Count(source, "0x3d42610") == 0);
    CHECK(Contains(ReadAll(Root() / "src/guest_ultrawide.h"), "#define SOTB_GAME_LAYER_VTABLE 0x3d136a0u"));

    const std::string launcher = ReadAll(Root() / "launcher/launcher.cpp");
    const size_t display_page = launcher.find("void DrawDisplay(trg::Ui& ui) {");
    const size_t graphics_page = launcher.find("void DrawGraphics(trg::Ui& ui) {");
    const std::string page = launcher.substr(display_page, graphics_page - display_page);
    const size_t aspect = page.find("\"Aspect Ratio\""), toggle = page.find("\"True Ultrawide\""), renderer = page.find("\"Renderer\"");
    CHECK(aspect < toggle && toggle < renderer && Count(page, "\"True Ultrawide\"") == 1);
    CHECK(Contains(page, "ImGui::BeginDisabled(!sotb_ultrawide_wanted(SavedAspectRatio().key));"));
    CHECK(Contains(page, "ui.Toggle(\"True Ultrawide\", kTrueUltrawideHelp, kTrueUltrawideKey, true, \"Off\", \"On\");"));

    CHECK(!Contains(ReadAll(Root() / "src/resolution_guard.c"), "gameplay_started") && !Contains(ReadAll(Root() / "src/resolution_guard.h"), "gameplay_started"));
    CHECK(Contains(ReadAll(Root() / "CMakeLists.txt"), "src/psn_notice.c src/guest_ultrawide.c src/guest_hud.c)"));

    // Nothing else is touched by this work: the presenter and the other guest patches know nothing of it.
    for (const char* name : {"gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp", "src/skip_intro.c", "src/psn_notice.c",
                             "src/sotb_exit_game.c", "src/sotb_menu_buttons.c"}) {
        CHECK(!Contains(ReadAll(Root() / name), "ultrawide"));
    }
}

// Which screen is open decides the title and the map, read from the game's own screen stack on each of its frames:
// nothing in it looks at a clock, a camera or a view.
void TestFrontendHookRuns() {
    CHECK(sotb_frontend_context_of_screen(SOTB_SCREEN_MAIN_MENU) == SOTB_CONTEXT_TITLE);
    for (const int widened : {0, 1}) {
        CHECK(sotb_ultrawide_use_safe_area(SOTB_CONTEXT_TITLE, widened) == 1);
        CHECK(sotb_ultrawide_use_safe_area(SOTB_CONTEXT_MAP, widened) == 1);
        CHECK(sotb_ultrawide_use_safe_area(SOTB_CONTEXT_MENU, widened) == 1);
        CHECK(sotb_ultrawide_use_safe_area(SOTB_CONTEXT_OTHER, widened) == widened);
    }
    // The menus keep the presenter's side fill off (sotbgpu_set_frontend_fill: 1 title, 2 map, anything else none).
    CHECK(SOTB_CONTEXT_TITLE == 1 && SOTB_CONTEXT_MAP == 2 && SOTB_CONTEXT_MENU != 1 && SOTB_CONTEXT_MENU != 2);
    if (!MakeFrontendRig()) return;

    sotb_ultrawide_set_clock(FakeClock);
    g_clock = 5;
    g_title_calls.clear();
    sotb_ultrawide_enable_side_fill(RecordLoading, RecordFrontend);

    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_OTHER && g_title_calls.empty());

    // The title: the main menu screen on top, the very frame it is pushed, and gone the frame it is popped.
    g_rig.Set({SOTB_SCREEN_MAIN_MENU});
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_TITLE && g_title_calls.size() == 1 && LastFill() == SOTB_CONTEXT_TITLE);
    g_rig.Frame();
    CHECK(g_title_calls.size() == 1);
    g_clock += 1000000;
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_TITLE && g_title_calls.size() == 1);
    g_rig.Set({SOTB_SCREEN_MAIN_MENU, SOTB_SCREEN_OPTIONS});
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_MENU && LastFill() == SOTB_CONTEXT_MENU && g_title_calls.size() == 2);
    g_rig.Set({SOTB_SCREEN_MAIN_MENU});
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_TITLE && LastFill() == SOTB_CONTEXT_TITLE && g_title_calls.size() == 3);
    g_rig.Set({SOTB_SCREEN_MAIN_MENU, SOTB_SCREEN_SHOP_CONTAINER});
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_MENU && LastFill() == SOTB_CONTEXT_MENU && g_title_calls.size() == 4);
    g_rig.Set({SOTB_SCREEN_MAIN_MENU, SOTB_SCREEN_OPTIONS});
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_MENU && g_title_calls.size() == 4);
    g_rig.Set({SOTB_SCREEN_MAIN_MENU, 11});
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_OTHER && LastFill() == SOTB_CONTEXT_OTHER && g_title_calls.size() == 5);

    // The map and what opens over it, from the first frame, whatever the camera did before.
    for (const int screen : {SOTB_SCREEN_MAP, SOTB_SCREEN_MAP_LIVE_FEED, SOTB_SCREEN_MAP_CONFIRMATION, SOTB_SCREEN_MAP_INFO,
                             SOTB_SCREEN_TIMELINE_INFO, SOTB_SCREEN_MANA_OVERLAY}) {
        g_rig.Set({SOTB_SCREEN_MAIN_MENU, screen});
        g_rig.Frame();
        CHECK(sotb_frontend_context() == SOTB_CONTEXT_MAP);
        CHECK(sotb_ultrawide_use_safe_area(sotb_frontend_context(), 0) == 1);
    }
    CHECK(LastFill() == SOTB_CONTEXT_MAP);
    g_rig.Set({SOTB_SCREEN_MAP});
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_MAP && LastFill() == SOTB_CONTEXT_MAP);

    // The pause and the other screens keep what they had: a widened view decides.
    for (const int screen : {11, 10, 6, 26, 18, 21, 31}) {
        g_rig.Set({screen});
        g_rig.Frame();
        CHECK(sotb_frontend_context() == SOTB_CONTEXT_OTHER);
    }
    // The menus from the pause in game: the options and Wisdom of Shadows by the stack, not by the camera.
    for (const int screen : {SOTB_SCREEN_OPTIONS, SOTB_SCREEN_SHOP_CONTAINER}) {
        g_rig.Set({11, screen});
        g_rig.Frame();
        CHECK(sotb_frontend_context() == SOTB_CONTEXT_MENU && sotb_ultrawide_use_safe_area(sotb_frontend_context(), 0) == 1);
    }
    g_rig.Set({});
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_OTHER);

    // An object or a stack that cannot be trusted changes nothing.
    g_rig.Set({SOTB_SCREEN_MAP});
    g_rig.Frame();
    const size_t calls = g_title_calls.size();
    g_rig.SetRaw(-1, uint64_t(reinterpret_cast<uintptr_t>(g_rig.entries)));
    g_rig.Frame();
    g_rig.SetRaw(65, uint64_t(reinterpret_cast<uintptr_t>(g_rig.entries)));
    g_rig.Frame();
    g_rig.SetRaw(1, 0x10);
    g_rig.Frame();
    g_rig.SetRaw(1, 0);
    g_rig.Frame();
    g_rig.Set({SOTB_SCREEN_MAIN_MENU});
    g_rig.SetVtable(false);
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_MAP && g_title_calls.size() == calls);
    g_rig.SetVtable(true);
    uint8_t* const none = nullptr;
    g_rig.hook(none, 0, 0, 0, 0, 0, 0.0f);
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_MAP);

    g_rig.Set({});
    g_rig.Frame();
    CHECK(sotb_frontend_context() == SOTB_CONTEXT_OTHER);
    g_title_calls.clear();
    sotb_ultrawide_enable_side_fill(nullptr, nullptr);
    g_rig.Set({SOTB_SCREEN_MAIN_MENU});
    g_rig.Frame();
    CHECK(g_title_calls.empty() && sotb_frontend_context() == SOTB_CONTEXT_TITLE);
    g_rig.Set({});
    g_rig.Frame();
    sotb_ultrawide_set_clock(nullptr);
}

// The loading screen's own hook, run for real on a synthetic image: it tells the observer the geometry it was given
// and hands the original arrangement exactly the geometry it handed it before.
void TestLoadingScreenObserver() {
    constexpr size_t kSize = 0x3d43000;
    uint8_t* image = static_cast<uint8_t*>(test_allocate(kSize, 1));
    CHECK(image != nullptr);
    if (!image) return;
    static GuestSlateGeometry captured;
    std::memset(&captured, 0, sizeof(captured));
    const uint8_t prefix[17] = {0x48, 0x8d, 0x87, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x8f, 0x38, 0x01, 0x00, 0x00, 0x48, 0x89, 0xf7};
    const uint8_t hint[10] = {0x48, 0xb8, 0x00, 0x00, 0x48, 0x43, 0x00, 0x00, 0x57, 0x44};
    uint8_t* arrange = image + 0x808a50;
    std::memcpy(arrange, prefix, sizeof(prefix));
    uint8_t* p = arrange + sizeof(prefix);
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xfe;
    *p++ = 0x48; *p++ = 0xbf;
    const uint64_t target = uint64_t(reinterpret_cast<uintptr_t>(&captured));
    std::memcpy(p, &target, 8); p += 8;
    *p++ = 0xb9; *p++ = 0x07; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0xf3; *p++ = 0x48; *p++ = 0xa5;
    *p++ = 0xc3;
    std::memcpy(image + 0x1e0963a, hint, sizeof(hint));
    const uint64_t entry = uint64_t(reinterpret_cast<uintptr_t>(arrange));
    std::memcpy(image + 0x3d42610, &entry, 8);
    char error[200];
    CHECK(guest_apply_loading_screen_layout(image, kSize, error, sizeof(error)));
    using Arrange = void(GUEST_ABI*)(void*, const GuestSlateGeometry*, void*);
    Arrange hook = nullptr;
    std::memcpy(&hook, image + 0x3d42610, sizeof(hook));
    CHECK(hook != nullptr);

    GuestSlateGeometry wide{};
    wide.size_x = 4384.0f;
    wide.size_y = 1880.0f;
    wide.scale = 1.0f;
    wide.render_m00 = wide.render_m11 = 1.0f;
    GuestSlateGeometry original = wide;
    original.size_x = 1920.0f;
    original.size_y = 1080.0f;
    int self = 0, arranged = 0;
    const GuestSlateGeometry scaled_wide = guest_scale_loading_geometry(&wide);
    const GuestSlateGeometry scaled_original = guest_scale_loading_geometry(&original);

    // Off, or 16:9: no observer, and the original arrangement gets the same geometry as ever.
    g_loading_marks = 0;
    hook(&self, &wide, &arranged);
    CHECK(std::memcmp(&captured, &scaled_wide, sizeof(captured)) == 0 && g_loading_marks == 0);

    sotb_ultrawide_enable_side_fill(RecordLoading, RecordFrontend);
    hook(&self, &wide, &arranged);
    CHECK(g_loading_marks == 1);
    CHECK(std::memcmp(&captured, &scaled_wide, sizeof(captured)) == 0);
    CHECK(captured.size_x == 1920.0f && captured.size_y == 1080.0f && std::abs(captured.scale - 1880.0f / 1080.0f) < 1e-4f);
    for (const std::pair<float, float> size : {std::pair{1920.0f, 1080.0f}, {1728.0f, 1080.0f}, {1440.0f, 1080.0f}, {854.0f, 480.0f}}) {
        g_loading_marks = 0;
        GuestSlateGeometry other = original;
        other.size_x = size.first;
        other.size_y = size.second;
        hook(&self, &other, &arranged);
        CHECK(g_loading_marks == 0);
    }
    g_loading_marks = 0;
    hook(&self, &original, &arranged);
    CHECK(g_loading_marks == 0 && std::memcmp(&captured, &scaled_original, sizeof(captured)) == 0);

    // A wide loading screen ends the title and the map at once: the next world's frontend manager says what is open.
    if (g_rig.hook) {
        for (const int screen : {SOTB_SCREEN_MAIN_MENU, SOTB_SCREEN_MAP, SOTB_SCREEN_OPTIONS, SOTB_SCREEN_SHOP_CONTAINER}) {
            g_title_calls.clear();
            g_rig.Set({screen});
            g_rig.Frame();
            CHECK(sotb_frontend_context() != SOTB_CONTEXT_OTHER);
            hook(&self, &original, &arranged);
            CHECK(sotb_frontend_context() != SOTB_CONTEXT_OTHER);
            hook(&self, &wide, &arranged);
            CHECK(sotb_frontend_context() == SOTB_CONTEXT_OTHER);
            CHECK(g_title_calls.size() == 2 && LastFill() == SOTB_CONTEXT_OTHER);
        }
        g_rig.Set({});
        g_rig.Frame();

        // Whole sessions with the presenter's own side fill state: title, loading, gameplay, the map, more loads and levels.
        // Every first gameplay frame past the loading hold has no fill at all, on the tenth level as on the first.
        using namespace SotbSideFill;
        static const auto mark = [] { MarkLoading(g_clock); };
        static const auto screen = [](int context) { SetScreen(context == 1 ? TitleVignette : context == 2 ? MapVignette : None); };
        sotb_ultrawide_enable_side_fill([] { mark(); }, [](int context) { screen(context); });
        const auto loading = [&](int frames) {
            g_rig.Set({});
            for (int i = 0; i < frames; ++i) {
                g_clock += 16;
                hook(&self, &wide, &arranged);
                g_rig.Frame();
                CHECK(Current(g_clock) == LoadingBlur && sotb_frontend_context() == SOTB_CONTEXT_OTHER);
            }
        };
        const auto gameplay = [&](int frames) {
            g_rig.Set({});
            g_clock += kLoadingHoldMs + 1;
            for (int i = 0; i < frames; ++i) {
                g_rig.Frame();
                CHECK(Current(g_clock) == None && sotb_frontend_context() == SOTB_CONTEXT_OTHER);
                g_clock += 16;
            }
        };
        const auto open = [&](std::vector<int> stack, int mode) {
            g_rig.Set(stack);
            g_rig.Frame();
            g_clock += kLoadingHoldMs + 1;
            CHECK(Current(g_clock) == mode);
        };
        open({SOTB_SCREEN_MAIN_MENU}, TitleVignette);
        loading(30);
        gameplay(5);
        for (int level = 0; level < 10; ++level) {
            open({SOTB_SCREEN_MAP}, MapVignette);
            open({SOTB_SCREEN_MAP, SOTB_SCREEN_MANA_OVERLAY}, MapVignette);
            loading(20 + level);
            gameplay(5);
            open({11}, None);
            loading(10);
            gameplay(3);
        }
        open({SOTB_SCREEN_MAIN_MENU}, TitleVignette);
        loading(12);
        gameplay(2);
        g_rig.Set({});
        g_rig.Frame();
        SetScreen(None);
    }

    sotb_ultrawide_enable_side_fill(nullptr, nullptr);
    g_loading_marks = 0;
    hook(&self, &wide, &arranged);
    CHECK(g_loading_marks == 0 && std::memcmp(&captured, &scaled_wide, sizeof(captured)) == 0);
    test_release(image, kSize);
}

// The state the presenter reads: the mode, how long it is held and the part of the picture the game draws.
void TestSideFillState() {
    using namespace SotbSideFill;
    SetScreen(None);
    MarkLoading(1);
    CHECK(Current(1) == LoadingBlur && Current(1 + kLoadingHoldMs) == LoadingBlur && Current(2 + kLoadingHoldMs) == None);
    CHECK(Current(0) == None && Current(100000) == None);

    // The title and the map have no hold: each is on until it is cleared, and gone at that moment.
    SetScreen(TitleVignette);
    CHECK(Current(100000) == TitleVignette && Current(1) == LoadingBlur && Current(1000000000) == TitleVignette);
    SetScreen(MapVignette);
    CHECK(Current(100000) == MapVignette && Current(1000000000) == MapVignette);
    SetScreen(None);
    CHECK(Current(100000) == None && Current(1000000000) == None);
    SetScreen(LoadingBlur);
    CHECK(Current(100000) == None);
    SetScreen(77);
    CHECK(Current(100000) == None);

    // The loading blur wins while the loading screen is arranged, and the title's black is never the blur.
    MarkLoading(5000);
    SetScreen(TitleVignette);
    CHECK(Current(5100) == LoadingBlur && Current(5000 + kLoadingHoldMs + 1) == TitleVignette);
    SetScreen(None);
    CHECK(Current(5000 + kLoadingHoldMs + 1) == None && NowMs() > 0);
    MarkLoading(1);

    CHECK(std::abs(CentralFraction(4384, 1880) - 0.76235f) < 1e-3f);
    CHECK(std::abs(CentralFraction(5408, 1520) - 0.49970f) < 1e-3f);
    for (const std::pair<uint32_t, uint32_t> size : {std::pair<uint32_t, uint32_t>{1920, 1080}, {854, 480}, {3840, 2160}, {1728, 1080},
                                                    {1440, 1080}, {1352, 1080}, {0, 0}, {100, 0}, {0, 100}}) {
        CHECK(CentralFraction(size.first, size.second) == 0.0f);
    }
    // Every target the two ultrawide shapes make gets a fraction that is the safe area's share of the width.
    for (int r = 0; r < SOTB_RESOLUTION_COUNT; ++r) {
        for (const char* key : {"21:9", "32:9"}) {
            const SotbRenderTarget target = sotb_render_target(&sotb_resolutions[r], &sotb_aspect_ratios[sotb_aspect_index(key)]);
            const float fraction = CentralFraction(uint32_t(target.width), uint32_t(target.height));
            CHECK(fraction > 0.4f && fraction < 0.8f);
            GuestSlateGeometry wide{};
            wide.size_x = float(target.width);
            wide.size_y = float(target.height);
            wide.scale = 1.0f;
            const GuestSlateGeometry safe = guest_safe_area_geometry(&wide);
            CHECK(std::abs(safe.size_x / wide.size_x - fraction) < 1e-4f);
        }
    }
    SetScreen(None);
}

// The fill is in the post process pass of the Vulkan presenter only, off unless a mode is marked, and the settings it
// reads keep the layout the shader declares.
void TestPresenterWiring() {
    const std::string shader = ReadAll(Root() / "gpu/shadps4/video_core/host_shaders/post_process.frag");
    CHECK(Contains(shader, "    bool srgb_input;\n    uint side_fill;\n    float side_fraction;\n} pp;"));
    CHECK(Contains(shader, "vec4 color_linear = SampleArea(texSampler, uv);\n    if (pp.side_fill != 0u) {"));
    CHECK(Contains(shader, "const int blur_taps = 32;"));
    CHECK(!Contains(shader, "clamp(uv.x") && !Contains(shader, "inset") && !Contains(shader, "side_fill == 1u"));
    CHECK(!Contains(shader, "vec4(0.0, 0.0, 0.0, 1.0)"));
    // The title and the map read only the source under their own pixel; the loading blur is the only code that taps the
    // picture elsewhere. The strip the background leaves at the central part's right edge is darkened too.
    CHECK(Contains(shader, "        if (pp.side_fill >= 2u) {\n            float strip = 1.92188 / 1920.0 * pp.side_fraction;\n"
                           "            color_linear = FrontendSides(color_linear, lo, hi, strip, pp.side_fill == 3u ? 0.5 : 1.0, footprint);\n"
                           "        } else if (uv.x < lo || uv.x > hi) {\n            color_linear = SideFill(lo, hi);\n        }"));
    CHECK(Contains(shader, "float first_dark = ceil(lo * size.x - 0.5), end_dark = ceil((hi - strip) * size.x - 0.5);"));
    CHECK(Contains(shader, "float a = uv.x * size.x - 0.5 * footprint.x, b = a + footprint.x;\n    if (a >= first_dark && b <= end_dark) return color;\n"
                           "    if (b <= first_dark || a >= end_dark) return vec4(VignetteSide(color.rgb, strength), color.a);"));
    CHECK(Contains(shader, "for (float column = floor(a); column < b; column += 1.0) {"));
    CHECK(Contains(shader, "sum += (min(b, column + 1.0) - max(a, column)) * (side ? VignetteSide(c, strength) : c);"));
    CHECK(Contains(shader, "vec2 footprint = max(abs(vec2(dFdx(uv.x), dFdy(uv.y))) * vec2(textureSize(texSampler, 0)), vec2(1e-4));\n"
                           "    vec4 color_linear = SampleArea(texSampler, uv);"));
    const size_t title_begin = shader.find("float FrontendVignette(float v) {"), title_end = shader.find("vec4 SideFill(float lo, float hi) {");
    CHECK(title_begin != std::string::npos && title_begin < title_end);
    const std::string title_code = shader.substr(title_begin, title_end - title_begin);
    CHECK(!Contains(title_code, "SampleArea") && !Contains(title_code, "center") && !Contains(title_code, "dFd") && Count(title_code, "textureLod(") == 1);
    CHECK(Count(shader, "textureLod(texSampler") == 2);
    CHECK(Contains(shader, "return pp.srgb_input ? rgb * keep : degamma(encode(rgb) * keep);"));

    // The vignette is the edge of the title's own background texture: 108 rows, strongest at the top and bottom.
    const size_t table_begin = shader.find("const float title_vignette[108] = float[108]("), table_end = shader.find(");", table_begin);
    CHECK(table_begin != std::string::npos);
    std::vector<float> table;
    const size_t numbers_begin = shader.find('(', table_begin) + 1;
    std::istringstream numbers(shader.substr(numbers_begin, table_end - numbers_begin));
    for (std::string token; std::getline(numbers, token, ',');) table.push_back(std::strtof(token.c_str(), nullptr));
    CHECK(table.size() == 108);
    if (table.size() == 108) {
        CHECK(std::abs(table[0] - 0.8216f) < 1e-4f && std::abs(table[107] - 0.8353f) < 1e-4f && std::abs(table[54] - 0.4608f) < 1e-4f);
        for (const float alpha : table) CHECK(alpha > 0.45f && alpha < 0.85f);
        for (size_t i = 0; i < 54; ++i) CHECK(table[i] >= table[i + 1] - 1e-4f && table[107 - i] >= table[106 - i] - 1e-4f);
    }
    CHECK(Contains(shader, "return vec4(sum / float(blur_taps) * 0.55, 1.0);"));
    CHECK(!Contains(shader, "DEBUG_" "SAULO"));

    const std::string pass = ReadAll(Root() / "gpu/shadps4/video_core/renderer_vulkan/host_passes/pp_pass.h");
    CHECK(Contains(pass, "        u32 srgb_input = 0;\n        u32 side_fill = 0;\n        float side_fraction = 1.0f;\n"));
    const std::string presenter = ReadAll(Root() / "gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");
    CHECK(Contains(presenter, "pp_settings.side_fill = side_fraction > 0.0f ? u32(SotbSideFill::Current(SotbSideFill::NowMs())) : 0u;"));
    CHECK(Contains(presenter, "pp_settings.side_fraction = side_fraction > 0.0f ? side_fraction : 1.0f;"));
    CHECK(presenter.find("pp_settings.side_fill =") < presenter.find("pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);"));
    // No edge readback, no automatic frame capture: the presenter reads the game's image only through its passes.
    const fs::path vulkan = Root() / "gpu/shadps4/video_core/renderer_vulkan";
    CHECK(!fs::exists(vulkan / "vk_presenter_edges.h"));
    for (const char* name : {"vk_presenter.cpp", "vk_presenter.h", "vk_frame_capture.cpp", "vk_frame_capture.h"}) {
        const std::string text = ReadAll(vulkan / name);
        CHECK(!text.empty() && !Contains(text, "DEBUG_" "SAULO") && !Contains(text, "debug_saulo") && !Contains(text, "ArmNow"));
    }
    CHECK(!Contains(presenter, "copyImageToBuffer") && !Contains(presenter, "FrameCapture::"));
    // The frame analyzer reads a ring-bound buffer from its ring copy: flattened user data has guest address 0.
    const std::string rasterizer = ReadAll(vulkan / "vk_rasterizer.cpp");
    CHECK(Contains(rasterizer, "if (FrameCapture::Active()) {\n                FrameCapture::Buffer(stage.pgm_hash, buffer_index, ring->address,\n"
                               "                                     constant_ring->Data(ring->offset), ring->size);"));
    CHECK(!Contains(rasterizer, "reinterpret_cast<const void*>(ring->address)"));
    CHECK(Contains(rasterizer, "copy(index, stage.flattened_ud_buf.data(), 0,"));

    for (const char* name : {"gpu/shadps4/video_core/renderer_vulkan/host_passes/fsr_pass.cpp", "gpu/shadps4/video_core/renderer_vulkan/host_passes/smaa_pass.cpp"}) {
        CHECK(!Contains(ReadAll(Root() / name), "side_fill"));
    }
    const std::string gpu = ReadAll(Root() / "gpu/shim/sotbgpu.cpp");
    CHECK(Contains(gpu, "SotbSideFill::MarkLoading(SotbSideFill::NowMs());") && Contains(gpu, "SotbSideFill::SetScreen(context == 1 ? SotbSideFill::TitleVignette : context == 2 ? SotbSideFill::MapVignette : SotbSideFill::None);"));
    CHECK(!Contains(ReadAll(Root() / "gpu/shim/sotb_side_fill.h"), "Edge") && !Contains(ReadAll(Root() / "gpu/shim/sotb_side_fill.h"), "kHoldMs"));

    const std::string display = ReadAll(Root() / "src/guest_display.c");
    CHECK(Contains(display, "    if (loading_observer) loading_observer(allotted);\n    const GuestSlateGeometry scaled = guest_scale_loading_geometry(allotted);\n    original_loading_arrange(self, &scaled, arranged);"));
}

// The title's side vignette at the edges of the span the game darkens itself, on one row of a uniform scene. The game darkens
// the source columns whose centres its background covers by `keep`; the sides must get the same, so every frame pixel,
// box filtered from the source like the post process pass, should come out at `keep`. `per_column` is post_process.frag's
// FrontendSides; without it the pixel is vignetted or not as a whole by where its centre falls.
double TitleSeamError(uint32_t width, uint32_t height, uint32_t frame_width, bool per_column) {
    const double fraction = SotbSideFill::CentralFraction(width, height);
    const double lo = (1.0 - fraction) * 0.5, hi = lo + fraction, strip = 1.92188 / 1920.0 * fraction;
    const double first_dark = std::ceil(lo * width - 0.5), end_dark = std::ceil((hi - strip) * width - 0.5);
    const double keep = 0.4, footprint = double(width) / frame_width;
    const auto dark = [&](double column) { return column >= first_dark && column < end_dark; };
    double worst = 0.0;
    for (uint32_t pixel = 0; pixel < frame_width; ++pixel) {
        const double u = (pixel + 0.5) / frame_width, a = u * width - 0.5 * footprint, b = a + footprint;
        double game = 0.0, ours = 0.0;
        for (double column = std::floor(a); column < b; column += 1.0) {
            const double weight = (std::min(b, column + 1.0) - std::max(a, column)) / footprint;
            const double clamped = std::clamp(column, 0.0, double(width) - 1.0);
            game += weight * (dark(clamped) ? keep : 1.0);
            ours += weight * (dark(clamped) ? keep : 1.0 * keep);
        }
        double value = game;
        if (!per_column) {
            if (u < lo || u > hi - strip) value *= keep;
        } else if (!(a >= first_dark && b <= end_dark)) {
            value = (b <= first_dark || a >= end_dark) ? game * keep : ours;
        }
        worst = std::max(worst, std::abs(value - keep));
    }
    return worst;
}

void TestTitleSeam() {
    struct Case {
        uint32_t width, height;
        int32_t window_width, window_height;
    };
    const Case cases[] = {{4384, 1880, 1920, 823}, {4384, 1880, 1920, 824}, {4384, 1880, 2560, 1080}, {4384, 1880, 3440, 1440},
                          {2560, 1080, 1920, 823},  {3440, 1440, 2560, 1080}, {5408, 1520, 1920, 540},  {5408, 1520, 3840, 1080},
                          {5408, 1520, 5120, 1440}, {3840, 1080, 1920, 540}};
    double before = 0.0;
    for (const Case& c : cases) {
        CHECK(SotbSideFill::CentralFraction(c.width, c.height) > 0.0f);
        // The presenter's frame: the window narrowed to the picture's shape (Presenter::SetExpectedGameSize).
        const float image_ratio = float(c.width) / float(c.height), ratio = float(c.window_width) / float(c.window_height);
        const uint32_t frame_width = ratio > image_ratio ? uint32_t(c.window_height * image_ratio) : uint32_t(c.window_width);
        before = std::max(before, TitleSeamError(c.width, c.height, frame_width, false));
        CHECK(TitleSeamError(c.width, c.height, frame_width, true) < 1e-9);
    }
    // The reported line: 4384 x 1880 in a 1920 x 823 window, frame 1919 wide, pixel 1689 over a third unshaded.
    CHECK(TitleSeamError(4384, 1880, 1919, false) > 0.2 && before > 0.2);
    // 16:9 and narrower have no sides at all.
    CHECK(SotbSideFill::CentralFraction(3840, 2160) == 0.0f && SotbSideFill::CentralFraction(1920, 1080) == 0.0f);
}

} // namespace

int main() {
    TestWhichShapes();
    TestHorPlusMath();
    TestWhichViews();
    TestSafeAreaGeometry();
    TestSafeAreaHold();
    TestHudPlacement();
    TestHudHookRuns();
    TestFrontendHookRuns();
    TestSafeAreaHookRuns();
    TestLoadingScreenObserver();
    TestSideFillState();
    TestPresenterWiring();
    TestTitleSeam();
    TestSetting();
    TestRealBuilds();
    TestWiring();
    std::printf("true ultrawide tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

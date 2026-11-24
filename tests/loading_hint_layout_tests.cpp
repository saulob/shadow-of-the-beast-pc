#include "guest_display.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* expr, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

GuestSlateGeometry MakeViewportGeometry(float w, float h) {
    GuestSlateGeometry g{};
    g.size_x = w;
    g.size_y = h;
    g.scale = 1.0f;
    g.render_m00 = 1.0f;
    g.render_m11 = 1.0f;
    return g;
}

float ProjectX(const GuestSlateGeometry& g, float local_x) { return g.render_tx + local_x * g.render_m00; }
float ProjectY(const GuestSlateGeometry& g, float local_y) { return g.render_ty + local_y * g.render_m11; }

void VerifyHintBarCentered(float viewport_w, float viewport_h, float expected_scale) {
    const GuestSlateGeometry vp = MakeViewportGeometry(viewport_w, viewport_h);
    const GuestSlateGeometry g = guest_scale_loading_geometry(&vp);

    CHECK(Near(g.size_x, SOTB_LOADING_DESIGN_WIDTH));
    CHECK(Near(g.size_y, SOTB_LOADING_DESIGN_HEIGHT));
    CHECK(Near(g.scale, expected_scale));
    CHECK(Near(g.render_m00, expected_scale));
    CHECK(Near(g.render_m11, expected_scale));

    // SBeastLoadingScreen SCanvas hint bar slots: LB x=100, text x=200 w=1520, RB x=1820.
    const float lb_x = ProjectX(g, 100.0f);
    const float text_left = ProjectX(g, 200.0f);
    const float text_right = ProjectX(g, 200.0f + 1520.0f);
    const float rb_x = ProjectX(g, 1820.0f);
    const float text_center = (text_left + text_right) * 0.5f;
    const float bar_center = (lb_x + rb_x) * 0.5f;

    CHECK(Near(text_center, viewport_w * 0.5f));
    CHECK(Near(bar_center, viewport_w * 0.5f));
    CHECK(Near(lb_x, viewport_w - rb_x));
    CHECK(Near(text_left, viewport_w - text_right));
    CHECK(Near(text_right - text_left, 1520.0f * expected_scale));

    // Vertical placement preserves 1920x1080 proportions without horizontal stretch.
    const float text_y = ProjectY(g, 860.0f);
    const float button_y = ProjectY(g, 894.0f);
    CHECK(Near(text_y, 860.0f * expected_scale));
    CHECK(Near(button_y, 894.0f * expected_scale));
}

void TestResolutionsAndAspects() {
    VerifyHintBarCentered(1280.0f, 720.0f, 720.0f / 1080.0f);
    VerifyHintBarCentered(1920.0f, 1080.0f, 1.0f);
    VerifyHintBarCentered(2560.0f, 1440.0f, 1440.0f / 1080.0f);
    VerifyHintBarCentered(3840.0f, 2160.0f, 2160.0f / 1080.0f);
    VerifyHintBarCentered(3200.0f, 1800.0f, 1800.0f / 1080.0f);

    // Ultrawide 21:9 and 32:9 keep the 16:9 logical area centered with no horizontal stretch.
    VerifyHintBarCentered(2560.0f, 1080.0f, 1.0f);
    VerifyHintBarCentered(3440.0f, 1440.0f, 1440.0f / 1080.0f);
    VerifyHintBarCentered(3840.0f, 1080.0f, 1.0f);
    VerifyHintBarCentered(5120.0f, 1440.0f, 1440.0f / 1080.0f);

    // 1920x1080 is an exact identity passthrough.
    const GuestSlateGeometry native = MakeViewportGeometry(1920.0f, 1080.0f);
    const GuestSlateGeometry native_out = guest_scale_loading_geometry(&native);
    CHECK(std::memcmp(&native, &native_out, sizeof(native)) == 0);

    // Degenerate sizes pass through safely.
    const GuestSlateGeometry zero = MakeViewportGeometry(0.0f, 0.0f);
    const GuestSlateGeometry zero_out = guest_scale_loading_geometry(&zero);
    CHECK(std::memcmp(&zero, &zero_out, sizeof(zero)) == 0);

    // Non-zero parent origin / scale composes properly.
    GuestSlateGeometry nested = MakeViewportGeometry(2560.0f, 1440.0f);
    nested.abs_pos_x = 10.0f;
    nested.abs_pos_y = 20.0f;
    nested.render_tx = 10.0f;
    nested.render_ty = 20.0f;
    const GuestSlateGeometry nested_out = guest_scale_loading_geometry(&nested);
    CHECK(Near(nested_out.abs_pos_x, 10.0f));
    CHECK(Near(nested_out.abs_pos_y, 20.0f));
    CHECK(Near(nested_out.render_tx, 10.0f));
    CHECK(Near(nested_out.render_ty, 20.0f));
}

void TestVtablePatchValidation() {
    char error[160] = {};
    constexpr size_t kVtableSlot = 0x3d42610u;
    constexpr size_t kArrangeFn = 0x808a50u;
    constexpr size_t kHintPos = 0x1e0963au;
    static const uint8_t kArrangeBytes[] = {
        0x48, 0x8d, 0x87, 0xf0, 0x00, 0x00, 0x00,
        0x48, 0x8d, 0x8f, 0x38, 0x01, 0x00, 0x00,
        0x48, 0x89, 0xf7,
    };
    static const uint8_t kHintBytes[] = {0x48, 0xb8, 0x00, 0x00, 0x48, 0x43, 0x00, 0x00, 0x57, 0x44};

    std::vector<uint8_t> image(kVtableSlot + 16, 0);
    CHECK(!guest_apply_loading_screen_layout(image.data(), 0x1000, error, sizeof(error)));
    CHECK(!guest_apply_loading_screen_layout(image.data(), image.size(), error, sizeof(error)));

    std::memcpy(image.data() + kArrangeFn, kArrangeBytes, sizeof(kArrangeBytes));
    std::memcpy(image.data() + kHintPos, kHintBytes, sizeof(kHintBytes));
    const uint64_t expected_fn = reinterpret_cast<uint64_t>(image.data() + kArrangeFn);
    std::memcpy(image.data() + kVtableSlot, &expected_fn, sizeof(expected_fn));

    // Only the SBeastLoadingScreen vtable slot changes.
    CHECK(guest_apply_loading_screen_layout(image.data(), image.size(), error, sizeof(error)));
    uint64_t patched_fn = 0;
    std::memcpy(&patched_fn, image.data() + kVtableSlot, sizeof(patched_fn));
    CHECK(patched_fn != 0 && patched_fn != expected_fn);

    // Re-applying rejects already-patched slot.
    CHECK(!guest_apply_loading_screen_layout(image.data(), image.size(), error, sizeof(error)));
}

} // namespace

int main() {
    TestResolutionsAndAspects();
    TestVtablePatchValidation();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("loading_hint_layout_tests: OK");
    return 0;
}

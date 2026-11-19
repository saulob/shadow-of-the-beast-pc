#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>

#include "common/number_utils.h"
#include "video_core/amdgpu/pixel_format.h"

namespace Common::Log {
bool ShouldLog(Level) {
    return false;
}
void Write(std::string_view, Level, const char*, int, const char*, std::string&&) {}
} // namespace Common::Log

void assert_fail_impl() {
    std::abort();
}
[[noreturn]] void unreachable_impl() {
    std::abort();
}

namespace {
int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

u32 EncodeUfloat(float value, u32 mantissa_bits) {
    if (std::isnan(value) || !(value > 0.f)) {
        return 0;
    }
    if (std::isinf(value)) {
        return 31u << mantissa_bits;
    }
    int exponent = std::ilogb(value);
    if (exponent < -14) {
        u32 mantissa = u32(std::nearbyint(std::ldexp(value, 14 + int(mantissa_bits))));
        return (mantissa >> mantissa_bits) ? (1u << mantissa_bits) : mantissa;
    }
    u32 mantissa = u32(std::nearbyint(std::ldexp(value, int(mantissa_bits) - exponent)));
    if (mantissa >= (2u << mantissa_bits)) {
        mantissa >>= 1;
        ++exponent;
    }
    return (u32(exponent + 15) << mantissa_bits) | (mantissa & ((1u << mantissa_bits) - 1));
}

u32 PackR11G11B10(const std::array<float, 3>& rgb) {
    return EncodeUfloat(rgb[0], 6) | (EncodeUfloat(rgb[1], 6) << 11) | (EncodeUfloat(rgb[2], 5) << 22);
}

std::array<float, 3> UnpackR11G11B10(u32 packed) {
    return {NumberUtils::Uf11ToF32(packed & 0x7ff), NumberUtils::Uf11ToF32((packed >> 11) & 0x7ff),
            NumberUtils::Uf10ToF32((packed >> 22) & 0x3ff)};
}

float ClampBlue(float value) {
    return value > 0.f ? std::fmin(value, 64512.f) : 0.f;
}

void TestFormatRemap() {
    CHECK(AmdGpu::RemapDataFormat(AmdGpu::DataFormat::Format11_11_10) ==
          AmdGpu::DataFormat::Format10_11_11);
    CHECK(AmdGpu::RemapDataFormat(AmdGpu::DataFormat::Format10_11_11) ==
          AmdGpu::DataFormat::Format10_11_11);
}

void TestComponentSwap() {
    using S = AmdGpu::CompSwizzle;
    const std::array<float, 4> rgba = {1.f, 2.f, 3.f, 4.f};
    const AmdGpu::CompMapping standard{.r = S::Red, .g = S::Green, .b = S::Blue, .a = S::Alpha};
    const AmdGpu::CompMapping alt{.r = S::Blue, .g = S::Green, .b = S::Red, .a = S::Alpha};
    const AmdGpu::CompMapping standard_rev{.r = S::Alpha, .g = S::Blue, .b = S::Green, .a = S::Red};
    const AmdGpu::CompMapping alt_rev{.r = S::Alpha, .g = S::Red, .b = S::Green, .a = S::Blue};
    CHECK(standard.Apply(rgba) == (std::array<float, 4>{1.f, 2.f, 3.f, 4.f}));
    CHECK(alt.Apply(rgba) == (std::array<float, 4>{3.f, 2.f, 1.f, 4.f}));
    CHECK(standard_rev.Apply(rgba) == (std::array<float, 4>{4.f, 3.f, 2.f, 1.f}));
    CHECK(alt_rev.Apply(rgba) == (std::array<float, 4>{4.f, 1.f, 2.f, 3.f}));
    CHECK(standard.ApplyMask(0x7) == 0x7);
    CHECK(alt.ApplyMask(0x1) == 0x4);
    CHECK(alt.ApplyMask(0x4) == 0x1);
    CHECK(alt.Inverse() == alt);
    for (const auto& mapping : {standard, alt, standard_rev, alt_rev}) {
        CHECK(mapping.Inverse().Apply(mapping.Apply(rgba)) == rgba);
    }
}

void TestRoundTrip() {
    const std::array<std::array<float, 3>, 5> colors = {{
        {1.f, 0.f, 0.f},
        {0.f, 1.f, 0.f},
        {0.f, 0.f, 1.f},
        {1.f, 1.f, 1.f},
        {0.25f, 0.5f, 0.75f},
    }};
    for (const auto& color : colors) {
        const auto result = UnpackR11G11B10(PackR11G11B10(color));
        for (u32 i = 0; i < 3; ++i) {
            CHECK(std::fabs(result[i] - color[i]) <= 0.02f * color[i] + 1e-6f);
        }
    }
    CHECK(PackR11G11B10({1.f, 0.f, 0.f}) == 0x3c0u);
    CHECK(PackR11G11B10({0.f, 1.f, 0.f}) == (0x3c0u << 11));
    CHECK(PackR11G11B10({0.f, 0.f, 1.f}) == (0x1e0u << 22));
}

void TestOverflow() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(ClampBlue(-1.f) == 0.f);
    CHECK(ClampBlue(nan) == 0.f);
    CHECK(ClampBlue(1e30f) == 64512.f);
    CHECK(std::isfinite(NumberUtils::Uf10ToF32(EncodeUfloat(ClampBlue(65024.f), 5))));
    CHECK(NumberUtils::Uf10ToF32(EncodeUfloat(ClampBlue(65024.f), 5)) == 64512.f);
    CHECK(!std::isfinite(NumberUtils::Uf10ToF32(EncodeUfloat(65024.f, 5))));
    CHECK(NumberUtils::Uf11ToF32(EncodeUfloat(65024.f, 6)) == 65024.f);
}
} // namespace

int main() {
    TestFormatRemap();
    TestComponentSwap();
    TestRoundTrip();
    TestOverflow();
    if (!failures) {
        std::puts("hdr_format_test: ok");
    }
    return failures ? 1 : 0;
}

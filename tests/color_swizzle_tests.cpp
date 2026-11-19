#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/amdgpu/regs_color.h"

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
using Rgba = std::array<float, 4>;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

Rgba Export(const AmdGpu::CompMapping& swizzle, const Rgba& guest) {
    Rgba memory{};
    for (u32 i = 0; i < 4; ++i) {
        memory[i] = guest[swizzle.Map(i)];
    }
    return memory;
}

Rgba Sample(const AmdGpu::CompMapping& view, const Rgba& memory) {
    return view.Apply(memory);
}

void RoundTrip(AmdGpu::ColorBuffer::SwapMode swap) {
    AmdGpu::ColorBuffer cb{};
    cb.info.format = u32(AmdGpu::DataFormat::Format8_8_8_8);
    cb.info.number_type = u32(AmdGpu::NumberFormat::Unorm);
    cb.info.comp_swap = swap;
    const auto export_swizzle = cb.Swizzle();
    const auto read_view = export_swizzle.Inverse();
    const std::array<Rgba, 5> colors{{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1},
                                      {1, 1, 1, 1}, {1.0f, 0.5f, 0.25f, 1}}};
    for (const auto& color : colors) {
        CHECK(Sample(read_view, Export(export_swizzle, color)) == color);
    }
}
} // namespace

int main() {
    using Swap = AmdGpu::ColorBuffer::SwapMode;
    AmdGpu::ColorBuffer cb{};
    cb.info.format = u32(AmdGpu::DataFormat::Format8_8_8_8);
    cb.info.number_type = u32(AmdGpu::NumberFormat::Unorm);
    cb.info.comp_swap = Swap::Alternate;
    const auto swizzle = cb.Swizzle();
    CHECK(swizzle.r == AmdGpu::CompSwizzle::Blue);
    CHECK(swizzle.g == AmdGpu::CompSwizzle::Green);
    CHECK(swizzle.b == AmdGpu::CompSwizzle::Red);
    CHECK(swizzle.a == AmdGpu::CompSwizzle::Alpha);
    CHECK(cb.GetDataFmt() == AmdGpu::DataFormat::Format8_8_8_8);
    CHECK(cb.GetNumberFmt() == AmdGpu::NumberFormat::Unorm);

    const Rgba red{1, 0, 0, 1};
    const Rgba stored = Export(swizzle, red);
    CHECK((stored == Rgba{0, 0, 1, 1}));

    for (const auto swap : {Swap::Standard, Swap::Alternate, Swap::StandardReverse,
                            Swap::AlternateReverse}) {
        RoundTrip(swap);
    }
    std::printf(failures ? "color_swizzle_tests FAILED\n" : "color_swizzle_tests OK\n");
    return failures != 0;
}

#include <cstdio>

#include "video_core/renderer_vulkan/vk_blend_min_max.h"

using namespace Vulkan;

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static bool Same(const BlendEquationVk& a, const BlendEquationVk& b) {
    return a.op == b.op && a.src == b.src && a.dst == b.dst;
}

int main() {
    using Op = vk::BlendOp;
    using F = vk::BlendFactor;
    CHECK(Same(ResolveZeroingMinMax({Op::eMin, F::eOne, F::eZero}), {Op::eAdd, F::eZero, F::eZero}));
    CHECK(Same(ResolveZeroingMinMax({Op::eMax, F::eOne, F::eZero}), {Op::eAdd, F::eOne, F::eZero}));
    CHECK(Same(ResolveZeroingMinMax({Op::eMin, F::eZero, F::eOne}), {Op::eAdd, F::eZero, F::eZero}));
    CHECK(Same(ResolveZeroingMinMax({Op::eMax, F::eZero, F::eOne}), {Op::eAdd, F::eZero, F::eOne}));
    CHECK(Same(ResolveZeroingMinMax({Op::eMin, F::eSrcColor, F::eDstColor}),
               {Op::eMin, F::eSrcColor, F::eDstColor}));
    CHECK(Same(ResolveZeroingMinMax({Op::eMax, F::eOne, F::eOne}), {Op::eMax, F::eOne, F::eOne}));
    CHECK(Same(ResolveZeroingMinMax({Op::eAdd, F::eOne, F::eZero}), {Op::eAdd, F::eOne, F::eZero}));
    if (!failures) {
        std::puts("blend_min_max_test: ok");
    }
    return failures ? 1 : 0;
}

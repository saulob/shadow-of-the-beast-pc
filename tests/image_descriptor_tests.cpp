#include <cstdio>
#include <cstdlib>
#include <string_view>
#include "video_core/amdgpu/resource.h"

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

AmdGpu::Image Make(AmdGpu::ImageType type, u32 width, u32 height, u32 depth) {
    AmdGpu::Image image = AmdGpu::Image::Null(false);
    image.type = u64(type);
    image.width = width - 1;
    image.height = height - 1;
    image.depth = depth - 1;
    return image;
}
} // namespace

int main() {
    using AmdGpu::ImageType;
    CHECK(Make(ImageType::Color1D, 64, 1, 5).NumLayers() == 1);
    CHECK(Make(ImageType::Color2D, 64, 64, 7).NumLayers() == 1);
    CHECK(Make(ImageType::Color2DArray, 64, 64, 7).NumLayers() == 7);
    CHECK(Make(ImageType::Color1DArray, 64, 1, 3).NumLayers() == 3);
    CHECK(Make(ImageType::Cube, 64, 64, 1).NumLayers() == 6);
    CHECK(Make(ImageType::Cube, 64, 64, 2).NumLayers() == 12);
    CHECK(Make(ImageType::Color3D, 16, 16, 8).NumLayers() == 1);

    CHECK(Make(ImageType::Cube, 8, 8, 1).GetBaseType() == ImageType::Color2D);
    CHECK(Make(ImageType::Color1DArray, 8, 1, 2).GetBaseType() == ImageType::Color1D);
    CHECK(Make(ImageType::Color2DArray, 8, 8, 2).GetViewType(false) == ImageType::Color2D);
    CHECK(Make(ImageType::Color2DArray, 8, 8, 2).GetViewType(true) == ImageType::Color2DArray);
    CHECK(Make(ImageType::Color1DArray, 8, 1, 2).GetViewType(false) == ImageType::Color1D);
    CHECK(Make(ImageType::Cube, 8, 8, 1).GetViewType(true) == ImageType::Cube);
    CHECK(Make(ImageType::Color2DMsaaArray, 8, 8, 2).GetViewType(false) == ImageType::Color2DMsaa);

    CHECK(Make(ImageType::Color2D, 8, 8, 1).Valid());
    CHECK(!Make(ImageType::Color2D, 8, 8, 1).HasReservedFields());
    AmdGpu::Image mipped = Make(ImageType::Color2D, 8, 8, 1);
    mipped.last_level = 7;
    CHECK(mipped.NumLevels() == 4);
    CHECK(Make(ImageType::Color1D, 8, 1, 1).NumLevels() == 1);

    AmdGpu::Image reserved = Make(ImageType::Color2D, 8, 8, 1);
    reserved.dst_sel_x = 2;
    CHECK(reserved.HasReservedFields());

    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("image descriptor tests passed\n");
    return 0;
}

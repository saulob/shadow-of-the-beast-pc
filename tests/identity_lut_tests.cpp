#include <cstdio>

#include "shader_recompiler/backend/spirv/identity_lut.h"

using namespace Shader::Backend::SPIRV;

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

int main() {
    CHECK(IsIdentityLutShape(32, 32, 32));
    CHECK(IsIdentityLutShape(16, 16, 16));
    CHECK(IsIdentityLutShape(64, 64, 64));
    CHECK(!IsIdentityLutShape(15, 15, 15));
    CHECK(!IsIdentityLutShape(65, 65, 65));
    CHECK(!IsIdentityLutShape(32, 32, 16));
    CHECK(!IsIdentityLutShape(160, 90, 64));
    if (!failures) {
        std::puts("identity_lut_test: ok");
    }
    return failures ? 1 : 0;
}

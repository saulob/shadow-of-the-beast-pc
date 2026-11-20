// Shader interface hardening tests: the clip-distance attribute the vertex side writes and the fragment side reads must
// follow one rule, a pipeline without a pixel shader takes it from its own registers, and cached shader binaries of the
// old rule are retired. Pure rule checks and source wiring; no Vulkan device, no shader compilation.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#include "shader_recompiler/runtime_info.h"
#pragma clang diagnostic pop

namespace {

using namespace Shader;

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

std::string ReadSource(const std::string& relative) {
    std::ifstream file(std::string(SOTB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    std::string source = text.str();
    source.erase(std::remove(source.begin(), source.end(), char(13)), source.end());
    return source;
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

void TestHardwareVsProgram() {
    CHECK(HardwareVsProgram(false, false) == SwStage::Vertex);
    CHECK(HardwareVsProgram(true, false) == SwStage::TessellationEval);
    CHECK(HardwareVsProgram(false, true) == SwStage::Geometry);
    CHECK(HardwareVsProgram(true, true) == SwStage::Geometry);
}

void TestEmulationOnlyForTheGuestVertexShaderOnTheHardwareVs() {
    for (const HwStage hw : {HwStage::Fragment, HwStage::Vertex, HwStage::Geometry, HwStage::Export,
                             HwStage::Hull, HwStage::Local, HwStage::Compute}) {
        for (const SwStage sw : {SwStage::Fragment, SwStage::TessellationControl, SwStage::TessellationEval,
                                 SwStage::Vertex, SwStage::Geometry, SwStage::Compute}) {
            const bool expected = hw == HwStage::Vertex && sw == SwStage::Vertex;
            CHECK(EmulatesClipDistances(hw, sw, true) == expected);
            CHECK(!EmulatesClipDistances(hw, sw, false));
        }
    }
}

void TestFragmentSideFollowsTheVertexSide() {
    for (int mask = 0; mask < 32; ++mask) {
        const bool tessellation = mask & 1;
        const bool geometry = mask & 2;
        const bool clip_distances = mask & 4;
        const bool user_planes = mask & 8;
        const bool needs = mask & 16;
        const bool fragment = FragmentClipDistanceEmulation(tessellation, geometry, clip_distances, user_planes, needs);
        const bool vertex_writes =
            EmulatesClipDistances(HwStage::Vertex, HardwareVsProgram(tessellation, geometry), needs);
        // Never the fragment shader reading an attribute the vertex side does not write.
        CHECK(!fragment || vertex_writes);
        CHECK(fragment == (vertex_writes && (clip_distances || user_planes)));
        if (tessellation || geometry || !needs) {
            CHECK(!fragment);
        }
    }
    CHECK(FragmentClipDistanceEmulation(false, false, true, false, true));
    CHECK(FragmentClipDistanceEmulation(false, false, false, true, true));
    CHECK(!FragmentClipDistanceEmulation(false, false, false, false, true));
    CHECK(!FragmentClipDistanceEmulation(true, false, true, true, true));
    CHECK(!FragmentClipDistanceEmulation(false, true, true, true, true));
    CHECK(!FragmentClipDistanceEmulation(false, false, true, true, false));
}

void TestEmitContextUsesOneRule() {
    const auto context = ReadSource("gpu/shadps4/shader_recompiler/backend/spirv/spirv_emit_context.cpp");
    CHECK(Contains(context, "EmulatesClipDistances(hw_stage, sw_stage, profile.needs_clip_distance_emulation)"));
    CHECK(Contains(context, "EmitsEmulatedClipDistances(&info, profile.needs_clip_distance_emulation)"));
    CHECK(!Contains(context, "hw_stage == HwStage::Vertex && profile.needs_clip_distance_emulation"));
    CHECK(!Contains(context, "sw_stage == SwStage::Vertex &&\n                                               hw_stage == HwStage::Vertex"));
    const auto info = ReadSource("gpu/shadps4/shader_recompiler/info.h");
    CHECK(Contains(info, "inline bool EmitsEmulatedClipDistances(const Info* stage, bool needs_emulation)"));
    CHECK(Contains(info, "EmulatesClipDistances(stage->hw_stage, stage->sw_stage, needs_emulation)"));
    CHECK(Contains(info, "stage->stores.GetAny(IR::Attribute::ClipDistance)"));
}

void TestPipelineWithoutPixelShaderUsesItsOwnState() {
    const auto cache = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");
    const auto refresh = cache.find("bool PipelineCache::RefreshGraphicsStages");
    CHECK(refresh != std::string::npos);
    const auto bind = cache.find("!bind_stage(HwStage::Fragment, SwStage::Fragment)", refresh);
    const auto initialize = cache.find("fragment.Initialize(HwStage::Fragment, SwStage::Fragment)", refresh);
    const auto emulation = cache.find("fragment.hw.fs.clip_distance_emulation = FragmentClipDistanceEmulation(regs)",
                                      refresh);
    CHECK(bind != std::string::npos && bind < initialize && initialize < emulation);
    CHECK(Contains(cache, "info.hw.fs.clip_distance_emulation = FragmentClipDistanceEmulation(regs);"));
    CHECK(Contains(cache, "regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))"));
    CHECK(Contains(cache, "regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry))"));
    CHECK(!Contains(cache, "lowers_user_clip_planes"));
    const auto header = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.h");
    CHECK(Contains(header, "bool FragmentClipDistanceEmulation(const AmdGpu::Regs& regs) const;"));
}

void TestOldShaderBinariesAreRetired() {
    const auto serialization = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    CHECK(Contains(serialization, "ShaderBinaryVersion = 21u;"));
}

void TestAlreadyPresentDecodedFrameCopy() {
    const auto video = ReadSource("gpu/shadps4/core/libraries/videodec/video_utils.cpp");
    CHECK(Contains(video, "src.data[0] + u64{y} * src.linesize[0]"));
    CHECK(Contains(video, "src.data[1] + u64{y} * src.linesize[1]"));
    CHECK(!Contains(video, "src.width != dst_pitch"));
}

} // namespace

int main() {
    TestHardwareVsProgram();
    TestEmulationOnlyForTheGuestVertexShaderOnTheHardwareVs();
    TestFragmentSideFollowsTheVertexSide();
    TestEmitContextUsesOneRule();
    TestPipelineWithoutPixelShaderUsesItsOwnState();
    TestOldShaderBinariesAreRetired();
    TestAlreadyPresentDecodedFrameCopy();
    std::printf("nvidia hardening tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

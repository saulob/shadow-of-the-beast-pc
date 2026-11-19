// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>
#include <span>
#include <boost/container/static_vector.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/identity_lut.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/microinstruction.h"

namespace Shader::Backend::SPIRV {

template <bool is_float>
static Id FixImageCoords(EmitContext& ctx, Id coords, AmdGpu::ImageType image_type) {
    const auto coord_type = is_float ? ctx.F32 : ctx.U32;
    const auto zero = is_float ? ctx.f32_zero_value : ctx.u32_zero_value;

    switch (image_type) {
    case AmdGpu::ImageType::Color1D: {
        // Lowered to 2D with height 1
        return ctx.OpCompositeConstruct(coord_type[2], coords, zero);
    }
    case AmdGpu::ImageType::Color1DArray: {
        // Lowered to 2D array with height 1
        const auto x = ctx.OpCompositeExtract(coord_type[1], coords, 0U);
        const auto slice = ctx.OpCompositeExtract(coord_type[1], coords, 1U);
        return ctx.OpCompositeConstruct(coord_type[3], x, zero, slice);
    }
    case AmdGpu::ImageType::Cube: {
        // Lowered to 2D array
        if (is_float) {
            const auto x = ctx.OpCompositeExtract(coord_type[1], coords, 0U);
            const auto y = ctx.OpCompositeExtract(coord_type[1], coords, 1U);
            const auto face = ctx.OpCompositeExtract(coord_type[1], coords, 2U);

            // AMD cube math results in coordinates in the range [1.0, 2.0]. We need
            // to convert this to the range [0.0, 1.0] to get correct results.
            const auto one = ctx.ConstF32(1.f);
            const auto fixed_x = ctx.OpFSub(coord_type[1], x, one);
            const auto fixed_y = ctx.OpFSub(coord_type[1], y, one);
            // The face operand is the face index plus 8 per cube; 6 layers per cube.
            const auto fixed_face = ctx.OpFma(
                coord_type[1],
                ctx.OpFloor(coord_type[1], ctx.OpFDiv(coord_type[1], face, ctx.ConstF32(8.f))),
                ctx.ConstF32(-2.f), face);
            return ctx.OpCompositeConstruct(coord_type[3], fixed_x, fixed_y, fixed_face);
        }
        return coords;
    }
    default:
        return coords;
    }
}

template <bool is_float>
static Id PadImageOperand(EmitContext& ctx, Id operand, AmdGpu::ImageType image_type) {
    if (image_type == AmdGpu::ImageType::Color1D || image_type == AmdGpu::ImageType::Color1DArray) {
        const auto coord_type = is_float ? ctx.F32 : ctx.U32;
        const auto zero = is_float ? ctx.f32_zero_value : ctx.u32_zero_value;
        return ctx.OpCompositeConstruct(coord_type[2], operand, zero);
    }
    return operand;
}

static bool IsCubeImage(AmdGpu::ImageType image_type) {
    return image_type == AmdGpu::ImageType::Cube;
}

struct ImageOperands {
    void Add(spv::ImageOperandsMask new_mask, Id value) {
        if (!Sirit::ValidId(value)) {
            return;
        }
        mask = static_cast<spv::ImageOperandsMask>(static_cast<u32>(mask) |
                                                   static_cast<u32>(new_mask));
        operands.push_back(value);
    }
    void Add(spv::ImageOperandsMask new_mask, Id value1, Id value2) {
        mask = static_cast<spv::ImageOperandsMask>(static_cast<u32>(mask) |
                                                   static_cast<u32>(new_mask));
        operands.push_back(value1);
        operands.push_back(value2);
    }

    void AddOffset(EmitContext& ctx, AmdGpu::ImageType image_type, const IR::Value& offset,
                   bool is_gather = false) {
        // Cube sampling ignores texel offsets on hardware.
        if (offset.IsEmpty() || IsCubeImage(image_type)) {
            return;
        }
        const bool is_1d = image_type == AmdGpu::ImageType::Color1D ||
                           image_type == AmdGpu::ImageType::Color1DArray;
        if (offset.IsImmediate()) {
            const s32 operand = offset.U32();
            Add(spv::ImageOperandsMask::ConstOffset,
                is_1d ? ctx.ConstS32(operand, 0) : ctx.ConstS32(operand));
            return;
        }
        IR::Inst* const inst{offset.Inst()};
        if (inst->AreAllArgsImmediates()) {
            switch (inst->GetOpcode()) {
            case IR::Opcode::CompositeConstructU32x2:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                 static_cast<s32>(inst->Arg(1).U32())));
                return;
            case IR::Opcode::CompositeConstructU32x3:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                 static_cast<s32>(inst->Arg(1).U32()),
                                 static_cast<s32>(inst->Arg(2).U32())));
                return;
            default:
                break;
            }
        }
        if (is_gather) {
            Add(spv::ImageOperandsMask::Offset,
                PadImageOperand<false>(ctx, ctx.Def(offset), image_type));
        }
    }

    void AddDerivatives(EmitContext& ctx, Id derivatives_dx, Id derivatives_dy) {
        if (!Sirit::ValidId(derivatives_dx) || !Sirit::ValidId(derivatives_dy)) {
            return;
        }
        Add(spv::ImageOperandsMask::Grad, derivatives_dx, derivatives_dy);
    }

    spv::ImageOperandsMask mask{};
    boost::container::static_vector<Id, 4> operands;
};

namespace {

// Shapes are those of the lowered Vulkan image: 1D images are 2D with height 1.
struct OffsetShape {
    u32 dims;
    bool is_array;
    u32 offset_dims;
    u32 Total() const {
        return dims + (is_array ? 1 : 0);
    }
};

std::optional<OffsetShape> GetOffsetShape(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return OffsetShape{2, false, 1};
    case AmdGpu::ImageType::Color1DArray:
        return OffsetShape{2, true, 1};
    case AmdGpu::ImageType::Color2D:
        return OffsetShape{2, false, 2};
    case AmdGpu::ImageType::Color2DArray:
        return OffsetShape{2, true, 2};
    case AmdGpu::ImageType::Color3D:
        return OffsetShape{3, false, 3};
    default:
        return std::nullopt;
    }
}

bool IsRuntimeOffset(const IR::Value& offset) {
    if (offset.IsEmpty() || offset.IsImmediate()) {
        return false;
    }
    const IR::Inst* const inst{offset.Inst()};
    if (!inst->AreAllArgsImmediates()) {
        return true;
    }
    const IR::Opcode op = inst->GetOpcode();
    return op != IR::Opcode::CompositeConstructU32x2 && op != IR::Opcode::CompositeConstructU32x3;
}

Id LeadingComponents(EmitContext& ctx, const VectorIds& types, Id value, u32 count, u32 total) {
    if (count == total) {
        return value;
    }
    if (count == 1) {
        return ctx.OpCompositeExtract(types[1], value, 0u);
    }
    if (count == 2) {
        return ctx.OpVectorShuffle(types[2], value, value, 0u, 1u);
    }
    return ctx.OpVectorShuffle(types[3], value, value, 0u, 1u, 2u);
}

enum class LodSource { Implicit, Bias, Explicit, Gradient };

struct LodInfo {
    LodSource source;
    Id value{};
    Id grad_x{};
    Id grad_y{};
};

Id SelectedLevel(EmitContext& ctx, Id image, Id sampled_image, Id coords, const OffsetShape& shape,
                 const LodInfo& lod) {
    const u32 dims = shape.dims;
    const u32 total = shape.Total();
    const Id levels = ctx.OpBitcast(ctx.S32[1], ctx.OpImageQueryLevels(ctx.U32[1], image));
    const Id max_level = ctx.OpISub(ctx.S32[1], levels, ctx.ConstS32(1));
    Id lambda{};
    switch (lod.source) {
    case LodSource::Implicit:
    case LodSource::Bias: {
        const Id query_coords = LeadingComponents(ctx, ctx.F32, coords, dims, total);
        const Id query = ctx.OpImageQueryLod(ctx.F32[2], sampled_image, query_coords);
        if (lod.source == LodSource::Bias) {
            lambda = ctx.OpFAdd(ctx.F32[1], ctx.OpCompositeExtract(ctx.F32[1], query, 1u),
                                lod.value);
        } else {
            lambda = ctx.OpCompositeExtract(ctx.F32[1], query, 0u);
        }
        break;
    }
    case LodSource::Explicit:
        lambda = lod.value;
        break;
    case LodSource::Gradient: {
        const Id base_size = ctx.OpImageQuerySizeLod(ctx.U32[total], image, ctx.ConstS32(0));
        const Id size_f = ctx.OpConvertUToF(
            ctx.F32[dims], LeadingComponents(ctx, ctx.U32, base_size, dims, total));
        const Id len_x = ctx.OpLength(ctx.F32[1], ctx.OpFMul(ctx.F32[dims], lod.grad_x, size_f));
        const Id len_y = ctx.OpLength(ctx.F32[1], ctx.OpFMul(ctx.F32[dims], lod.grad_y, size_f));
        const Id longest = ctx.OpFMax(ctx.F32[1], len_x, len_y);
        lambda = ctx.OpLog2(ctx.F32[1], ctx.OpFMax(ctx.F32[1], longest, ctx.ConstF32(1.0f)));
        break;
    }
    }
    const Id floored = ctx.OpConvertFToS(ctx.S32[1], ctx.OpFloor(ctx.F32[1], lambda));
    return ctx.OpSClamp(ctx.S32[1], floored, ctx.ConstS32(0), max_level);
}

Id ApplyRuntimeOffset(EmitContext& ctx, const EmitContext::TextureDefinition& texture, Id image,
                      Id sampled_image, Id coords, const IR::Value& offset, const LodInfo& lod) {
    if (!IsRuntimeOffset(offset)) {
        return coords;
    }
    if (IsCubeImage(texture.view_type)) {
        return coords;
    }
    const auto shape = GetOffsetShape(texture.view_type);
    if (!shape) {
        LOG_WARNING(Render_Vulkan, "Runtime offset is not supported for image type {}",
                    static_cast<u32>(texture.view_type));
        return coords;
    }
    const u32 dims = shape->dims;
    const u32 total = shape->Total();
    const Id level = SelectedLevel(ctx, image, sampled_image, coords, *shape, lod);
    const Id size = ctx.OpImageQuerySizeLod(ctx.U32[total], image, level);
    const Id size_f = ctx.OpConvertUToF(ctx.F32[dims],
                                        LeadingComponents(ctx, ctx.U32, size, dims, total));
    const u32 offset_dims = shape->offset_dims;
    Id texel_offset = ctx.OpConvertSToF(ctx.F32[offset_dims],
                                        ctx.OpBitcast(ctx.S32[offset_dims], ctx.Def(offset)));
    if (offset_dims < dims) {
        texel_offset = ctx.OpCompositeConstruct(ctx.F32[dims], texel_offset, ctx.f32_zero_value);
    }
    const Id delta = ctx.OpFDiv(ctx.F32[dims], texel_offset, size_f);
    if (total == dims) {
        return ctx.OpFAdd(ctx.F32[total], coords, delta);
    }
    boost::container::static_vector<Id, 4> parts;
    if (dims == 1) {
        parts.push_back(delta);
    } else {
        for (u32 i = 0; i < dims; ++i) {
            parts.push_back(ctx.OpCompositeExtract(ctx.F32[1], delta, i));
        }
    }
    parts.push_back(ctx.f32_zero_value);
    const Id delta_full = ctx.OpCompositeConstruct(ctx.F32[total], std::span<const Id>{parts});
    return ctx.OpFAdd(ctx.F32[total], coords, delta_full);
}

// Cubic 3D textures of 16..64 texels per side are color grading LUTs; they sample as an identity LUT.
Id IdentityLutOrSample(EmitContext& ctx, const auto& texture, Id image, Id coords, Id sample) {
    if (ctx.sw_stage != Shader::SwStage::Fragment ||
        texture.view_type != AmdGpu::ImageType::Color3D || texture.is_storage ||
        texture.is_integer || texture.data_types != &ctx.F32) {
        return sample;
    }
    const Id size = ctx.OpImageQuerySizeLod(ctx.U32[3], image, ctx.ConstS32(0));
    const Id width = ctx.OpCompositeExtract(ctx.U32[1], size, 0U);
    const Id height = ctx.OpCompositeExtract(ctx.U32[1], size, 1U);
    const Id depth = ctx.OpCompositeExtract(ctx.U32[1], size, 2U);
    Id cubic = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpIEqual(ctx.U1[1], width, height),
                                ctx.OpIEqual(ctx.U1[1], height, depth));
    cubic = ctx.OpLogicalAnd(ctx.U1[1], cubic,
                             ctx.OpUGreaterThanEqual(ctx.U1[1], width, ctx.ConstU32(IdentityLutMinSize)));
    cubic = ctx.OpLogicalAnd(ctx.U1[1], cubic,
                             ctx.OpULessThanEqual(ctx.U1[1], width, ctx.ConstU32(IdentityLutMaxSize)));
    const Id texels = ctx.OpConvertUToF(ctx.F32[1], width);
    const Id last = ctx.OpFSub(ctx.F32[1], texels, ctx.ConstF32(1.f));
    std::array<Id, 4> identity;
    for (u32 i = 0; i < 3; ++i) {
        const Id coord = ctx.OpCompositeExtract(ctx.F32[1], coords, i);
        const Id texel = ctx.OpFSub(ctx.F32[1], ctx.OpFMul(ctx.F32[1], coord, texels),
                                    ctx.ConstF32(0.5f));
        identity[i] = ctx.OpNClamp(ctx.F32[1], ctx.OpFDiv(ctx.F32[1], texel, last),
                                   ctx.f32_zero_value, ctx.f32_one_value);
    }
    identity[3] = ctx.f32_one_value;
    const Id identity_color = ctx.OpCompositeConstruct(ctx.F32[4], identity);
    const Id use_identity = ctx.OpCompositeConstruct(ctx.U1[4], cubic, cubic, cubic, cubic);
    return ctx.OpSelect(ctx.F32[4], use_identity, identity_color, sample);
}

} // namespace

Id EmitImageHandle(EmitContext& ctx, Id, Id) {
    UNREACHABLE_MSG("Unreachable instruction");
}

Id EmitImageSampleRaw(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address1, Id address2,
                      Id address3, Id address4) {
    UNREACHABLE_MSG("Unreachable instruction");
}

Id EmitImageSampleImplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id bias,
                              const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    const Id adjusted = ApplyRuntimeOffset(
        ctx, texture, image, sampled_image, fixed_coords, offset,
        LodInfo{Sirit::ValidId(bias) ? LodSource::Bias : LodSource::Implicit, bias});
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    operands.AddOffset(ctx, texture.view_type, offset);
    Id sample = ctx.OpImageSampleImplicitLod(result_type, sampled_image, adjusted, operands.mask,
                                             operands.operands);
    sample = IdentityLutOrSample(ctx, texture, image, fixed_coords, sample);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageSampleExplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod,
                              const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    const Id adjusted = ApplyRuntimeOffset(ctx, texture, image, sampled_image, fixed_coords,
                                           offset, LodInfo{LodSource::Explicit, lod});
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, lod);
    operands.AddOffset(ctx, texture.view_type, offset);
    Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, adjusted, operands.mask,
                                             operands.operands);
    sample = IdentityLutOrSample(ctx, texture, image, fixed_coords, sample);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageSampleDrefImplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id dref,
                                  Id bias, const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(1);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    const Id adjusted = ApplyRuntimeOffset(
        ctx, texture, image, sampled_image, fixed_coords, offset,
        LodInfo{Sirit::ValidId(bias) ? LodSource::Bias : LodSource::Implicit, bias});
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    operands.AddOffset(ctx, texture.view_type, offset);
    const Id sample = ctx.OpImageSampleDrefImplicitLod(result_type, sampled_image, adjusted, dref,
                                                       operands.mask, operands.operands);
    const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
    return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, ctx.f32_zero_value,
                                    ctx.f32_zero_value, ctx.f32_zero_value);
}

Id EmitImageSampleDrefExplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id dref,
                                  Id lod, const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(1);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    const Id adjusted = ApplyRuntimeOffset(ctx, texture, image, sampled_image, fixed_coords,
                                           offset, LodInfo{LodSource::Explicit, lod});
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, lod);
    operands.AddOffset(ctx, texture.view_type, offset);
    const Id sample = ctx.OpImageSampleDrefExplicitLod(result_type, sampled_image, adjusted, dref,
                                                       operands.mask, operands.operands);
    const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
    return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, ctx.f32_zero_value,
                                    ctx.f32_zero_value, ctx.f32_zero_value);
}

Id EmitImageGather(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                   const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    const u32 comp = inst->Flags<IR::TextureInstInfo>().gather_comp.Value();
    ImageOperands operands;
    operands.AddOffset(ctx, texture.view_type, offset, true);
    const Id texels = ctx.OpImageGather(result_type, sampled_image, fixed_coords,
                                        ctx.ConstU32(comp), operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageGatherDref(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                       const IR::Value& offset, Id dref) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.AddOffset(ctx, texture.view_type, offset, true);
    const Id texels = ctx.OpImageDrefGather(result_type, sampled_image, fixed_coords, dref,
                                            operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageQueryDimensions(EmitContext& ctx, IR::Inst* inst, u32 handle, Id lod, bool has_mips) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id zero = ctx.u32_zero_value;
    const auto mips{[&] { return has_mips ? ctx.OpImageQueryLevels(ctx.U32[1], image) : zero; }};
    const bool is_msaa = texture.view_type == AmdGpu::ImageType::Color2DMsaa ||
                         texture.view_type == AmdGpu::ImageType::Color2DMsaaArray;
    const bool uses_lod{!is_msaa && !texture.is_storage};
    const auto query{[&](Id type) {
        return uses_lod ? ctx.OpImageQuerySizeLod(type, image, lod)
                        : ctx.OpImageQuerySize(type, image);
    }};
    switch (texture.view_type) {
    case AmdGpu::ImageType::Color1D: {
        const auto width = ctx.OpCompositeExtract(ctx.U32[1], query(ctx.U32[2]), 0U);
        return ctx.OpCompositeConstruct(ctx.U32[4], width, zero, zero, mips());
    }
    case AmdGpu::ImageType::Color1DArray: {
        const auto base = query(ctx.U32[3]);
        const auto width = ctx.OpCompositeExtract(ctx.U32[1], base, 0U);
        const auto slices = ctx.OpCompositeExtract(ctx.U32[1], base, 2U);
        return ctx.OpCompositeConstruct(ctx.U32[4], width, slices, zero, mips());
    }
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[2]), zero, mips());
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaaArray:
    case AmdGpu::ImageType::Cube:
    case AmdGpu::ImageType::Color3D:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[3]), mips());
    default:
        UNREACHABLE_MSG("SPIR-V Instruction");
    }
}

Id EmitImageQueryLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    return ctx.OpImageQueryLod(ctx.F32[2], sampled_image, fixed_coords);
}

Id EmitImageGradient(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id derivatives_dx,
                     Id derivatives_dy, const IR::Value& offset, const IR::Value& lod_clamp) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id fixed_coords = FixImageCoords<true>(ctx, coords, texture.view_type);
    const Id grad_x = PadImageOperand<true>(ctx, derivatives_dx, texture.view_type);
    const Id grad_y = PadImageOperand<true>(ctx, derivatives_dy, texture.view_type);
    const Id adjusted = ApplyRuntimeOffset(ctx, texture, image, sampled_image, fixed_coords,
                                           offset, LodInfo{LodSource::Gradient, {}, grad_x, grad_y});
    ImageOperands operands;
    operands.AddDerivatives(ctx, grad_x, grad_y);
    operands.AddOffset(ctx, texture.view_type, offset);
    const Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, adjusted,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageRead(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id color_type = texture.data_types->Get(4);
    const Id fixed_coords = FixImageCoords<false>(ctx, coords, texture.view_type);
    ImageOperands operands;
    Id texel;
    if (!texture.is_storage) {
        const Id image = ctx.OpLoad(texture.image_type, texture.id);
        if (texture.view_type == AmdGpu::ImageType::Color2DMsaa ||
            texture.view_type == AmdGpu::ImageType::Color2DMsaaArray) {
            // GCN hardware wraps out-of-range MSAA sample indices
            if (Sirit::ValidId(ms)) {
                const Id sample_count = ctx.OpImageQuerySamples(ctx.U32[1], image);
                const Id wrapped_ms = ctx.OpUMod(ctx.U32[1], ms, sample_count);
                operands.Add(spv::ImageOperandsMask::Sample, wrapped_ms);
            }
        } else {
            if (Sirit::ValidId(ms)) {
                LOG_ERROR(Render_Recompiler, "image is not MS but ms operand is provided");
            }
            operands.Add(spv::ImageOperandsMask::Lod, lod);
            operands.Add(spv::ImageOperandsMask::Sample, ms);
        }
        texel = ctx.OpImageFetch(color_type, image, fixed_coords, operands.mask, operands.operands);
    } else {
        Id image_ptr = texture.id;
        if (ctx.profile.supports_image_load_store_lod) {
            operands.Add(spv::ImageOperandsMask::Lod, lod);
        } else if (Sirit::ValidId(lod)) {
#if 1
            // It's confusing what interactions will cause this code path so leave it as
            // unreachable until a case is found.
            // Normally IMAGE_LOAD_MIP should translate -> OpImageFetch
            UNREACHABLE_MSG("Unsupported ImageRead with Lod");
#else
            LOG_WARNING(Render, "Fallback for ImageRead with LOD");
            ASSERT(texture.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex);
            const Id single_image_ptr_type =
                ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type);
            image_ptr = ctx.OpAccessChain(single_image_ptr_type, image_ptr, std::array{lod});
#endif
        }
        const Id image = ctx.OpLoad(texture.image_type, image_ptr);
        texel = ctx.OpImageRead(color_type, image, fixed_coords, operands.mask, operands.operands);
    }
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texel) : texel;
}

void EmitImageWrite(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms,
                    Id color) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    Id image_ptr = texture.id;
    const Id color_type = texture.data_types->Get(4);
    const Id fixed_coords = FixImageCoords<false>(ctx, coords, texture.view_type);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Sample, ms);
    if (ctx.profile.supports_image_load_store_lod) {
        operands.Add(spv::ImageOperandsMask::Lod, lod);
    } else if (Sirit::ValidId(lod)) {
        LOG_WARNING(Render, "Fallback for ImageWrite with LOD");
        ASSERT(texture.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex);
        const Id single_image_ptr_type =
            ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type);
        image_ptr = ctx.OpAccessChain(single_image_ptr_type, image_ptr, std::array{lod});
    }
    const Id image = ctx.OpLoad(texture.image_type, image_ptr);
    Id texel = texture.is_integer ? ctx.OpBitcast(color_type, color) : color;
    const auto sharp = ctx.info.images[handle & 0xFFFF].GetSharp(ctx.info);
    const auto dfmt = sharp.GetDataFmt();
    if (!texture.is_integer && sharp.GetNumberFmt() == AmdGpu::NumberFormat::Float &&
        (dfmt == AmdGpu::DataFormat::Format10_11_11 || dfmt == AmdGpu::DataFormat::Format11_11_10)) {
        std::array<Id, 4> parts;
        for (u32 i = 0; i < 4; ++i) {
            const Id part = ctx.OpCompositeExtract(ctx.F32[1], texel, i);
            parts[i] = i == 3 ? part
                              : ctx.OpSelect(ctx.F32[1],
                                             ctx.OpFOrdGreaterThan(ctx.U1[1], part, ctx.ConstF32(0.f)),
                                             ctx.OpFMin(ctx.F32[1], part,
                                                        ctx.ConstF32(i == 2 ? 64512.f : 65024.f)),
                                             ctx.ConstF32(0.f));
        }
        texel = ctx.OpCompositeConstruct(ctx.F32[4], parts);
    }
    ctx.OpImageWrite(image, fixed_coords, texel, operands.mask, operands.operands);
}

static Id SelectCubeResult(EmitContext& ctx, Id x, Id y, Id z, Id x_res, Id y_res, Id z_res) {
    const auto abs_x = ctx.OpFAbs(ctx.F32[1], x);
    const auto abs_y = ctx.OpFAbs(ctx.F32[1], y);
    const auto abs_z = ctx.OpFAbs(ctx.F32[1], z);

    const auto z_face_cond{ctx.OpLogicalAnd(ctx.U1[1],
                                            ctx.OpFOrdGreaterThanEqual(ctx.U1[1], abs_z, abs_x),
                                            ctx.OpFOrdGreaterThanEqual(ctx.U1[1], abs_z, abs_y))};
    const auto y_face_cond{ctx.OpFOrdGreaterThanEqual(ctx.U1[1], abs_y, abs_x)};

    return ctx.OpSelect(ctx.F32[1], z_face_cond, z_res,
                        ctx.OpSelect(ctx.F32[1], y_face_cond, y_res, x_res));
}

Id EmitCubeFaceIndex(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    if (ctx.profile.supports_native_cube_calc) {
        return ctx.OpCubeFaceIndexAMD(ctx.F32[1], ctx.OpCompositeConstruct(ctx.F32[3], x, y, z));
    }

    const auto x_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], x, ctx.f32_zero_value)};
    const auto y_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], y, ctx.f32_zero_value)};
    const auto z_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], z, ctx.f32_zero_value)};
    const auto x_face{ctx.OpSelect(ctx.F32[1], x_neg_cond, ctx.ConstF32(1.f), ctx.ConstF32(0.f))};
    const auto y_face{ctx.OpSelect(ctx.F32[1], y_neg_cond, ctx.ConstF32(3.f), ctx.ConstF32(2.f))};
    const auto z_face{ctx.OpSelect(ctx.F32[1], z_neg_cond, ctx.ConstF32(5.f), ctx.ConstF32(4.f))};

    return SelectCubeResult(ctx, x, y, z, x_face, y_face, z_face);
}

Id EmitCubeFaceCoordS(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    const auto x_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], x, ctx.f32_zero_value)};
    const auto z_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], z, ctx.f32_zero_value)};
    const auto x_sc{ctx.OpSelect(ctx.F32[1], x_neg_cond, z, ctx.OpFNegate(ctx.F32[1], z))};
    const auto y_sc{x};
    const auto z_sc{ctx.OpSelect(ctx.F32[1], z_neg_cond, ctx.OpFNegate(ctx.F32[1], x), x)};

    return SelectCubeResult(ctx, x, y, z, x_sc, y_sc, z_sc);
}

Id EmitCubeFaceCoordT(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    const auto y_neg_cond{ctx.OpFOrdLessThan(ctx.U1[1], y, ctx.f32_zero_value)};
    const auto x_z_tc{ctx.OpFNegate(ctx.F32[1], y)};
    const auto y_tc{ctx.OpSelect(ctx.F32[1], y_neg_cond, ctx.OpFNegate(ctx.F32[1], z), z)};

    return SelectCubeResult(ctx, x, y, z, x_z_tc, y_tc, x_z_tc);
}

Id EmitCubeFaceMajorAxis(EmitContext& ctx, IR::Inst* inst, Id x, Id y, Id z) {
    const auto two{ctx.ConstF32(2.f)};
    const auto x_major_axis{ctx.OpFMul(ctx.F32[1], x, two)};
    const auto y_major_axis{ctx.OpFMul(ctx.F32[1], y, two)};
    const auto z_major_axis{ctx.OpFMul(ctx.F32[1], z, two)};

    return SelectCubeResult(ctx, x, y, z, x_major_axis, y_major_axis, z_major_axis);
}

} // namespace Shader::Backend::SPIRV

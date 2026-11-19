// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/div_ceil.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/microinstruction.h"
#include "shader_recompiler/ir/shared_memory.h"

namespace Shader::Backend::SPIRV {
namespace {

using PointerType = EmitContext::PointerType;
using SharedFlags = SharedMemory::Flags;

bool IsScratch(const IR::Inst* inst) {
    return inst->Flags<SharedFlags>().is_scratch.Value() != 0;
}

// GCN reads 0 past the end of the LDS allocation and drops such writes; Vulkan leaves
// out-of-range workgroup memory accesses undefined. Scratch accesses are in range by
// construction.
u32 NumSharedElements(const EmitContext& ctx, PointerType type) {
    const u32 element_size = 1U << static_cast<u32>(EmitContext::GetPointerSize(type));
    return Common::DivCeil(ctx.shared_memory_size, element_size);
}

Id LoadShared(EmitContext& ctx, IR::Inst* inst, Id offset, PointerType pointer_type, Id type,
              Id zero) {
    if (IsScratch(inst)) {
        return ctx.OpLoad(type, ctx.EmitSharedMemoryAccess(offset, pointer_type, true));
    }
    const u32 num_elements = NumSharedElements(ctx, pointer_type);
    const Id shift{ctx.ConstU32(static_cast<u32>(EmitContext::GetPointerSize(pointer_type)))};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift)};
    const Id in_range{ctx.OpULessThan(ctx.U1[1], index, ctx.ConstU32(num_elements))};
    const Id last{ctx.ConstU32(num_elements > 0 ? num_elements - 1 : 0U)};
    const Id safe_index{ctx.OpUMin(ctx.U32[1], index, last)};
    const Id pointer{ctx.EmitSharedMemoryElement(safe_index, pointer_type)};
    return ctx.OpSelect(type, in_range, ctx.OpLoad(type, pointer), zero);
}

void WriteShared(EmitContext& ctx, IR::Inst* inst, Id offset, PointerType pointer_type,
                 Id value) {
    if (IsScratch(inst)) {
        ctx.OpStore(ctx.EmitSharedMemoryAccess(offset, pointer_type, true), value);
        return;
    }
    const u32 num_elements = NumSharedElements(ctx, pointer_type);
    const Id shift{ctx.ConstU32(static_cast<u32>(EmitContext::GetPointerSize(pointer_type)))};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift)};
    const Id in_range{ctx.OpULessThan(ctx.U1[1], index, ctx.ConstU32(num_elements))};
    const Id store_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(in_range, store_label, merge_label);
    ctx.AddLabel(store_label);
    ctx.OpStore(ctx.EmitSharedMemoryElement(index, pointer_type), value);
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

} // Anonymous namespace

Id EmitLoadSharedU16(EmitContext& ctx, IR::Inst* inst, Id offset) {
    return LoadShared(ctx, inst, offset, PointerType::U16, ctx.U16, ctx.u16_zero_value);
}

Id EmitLoadSharedU32(EmitContext& ctx, IR::Inst* inst, Id offset) {
    return LoadShared(ctx, inst, offset, PointerType::U32, ctx.U32[1], ctx.u32_zero_value);
}

Id EmitLoadSharedU64(EmitContext& ctx, IR::Inst* inst, Id offset) {
    return LoadShared(ctx, inst, offset, PointerType::U64, ctx.U64, ctx.u64_zero_value);
}

void EmitWriteSharedU16(EmitContext& ctx, IR::Inst* inst, Id offset, Id value) {
    WriteShared(ctx, inst, offset, PointerType::U16, value);
}

void EmitWriteSharedU32(EmitContext& ctx, IR::Inst* inst, Id offset, Id value) {
    WriteShared(ctx, inst, offset, PointerType::U32, value);
}

void EmitWriteSharedU64(EmitContext& ctx, IR::Inst* inst, Id offset, Id value) {
    WriteShared(ctx, inst, offset, PointerType::U64, value);
}

} // namespace Shader::Backend::SPIRV

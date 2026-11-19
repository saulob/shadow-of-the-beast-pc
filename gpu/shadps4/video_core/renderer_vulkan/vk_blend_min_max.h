// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

struct BlendEquationVk {
    vk::BlendOp op;
    vk::BlendFactor src;
    vk::BlendFactor dst;
};

// Vulkan ignores blend factors for min/max. A factor that zeroes one operand makes the operation a plain
// selection, which an add with the matching factors reproduces exactly.
constexpr BlendEquationVk ResolveZeroingMinMax(BlendEquationVk equation) {
    if (equation.op != vk::BlendOp::eMin && equation.op != vk::BlendOp::eMax) {
        return equation;
    }
    const bool takes_max = equation.op == vk::BlendOp::eMax;
    if (equation.src == vk::BlendFactor::eOne && equation.dst == vk::BlendFactor::eZero) {
        return {vk::BlendOp::eAdd, takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero,
                vk::BlendFactor::eZero};
    }
    if (equation.src == vk::BlendFactor::eZero && equation.dst == vk::BlendFactor::eOne) {
        return {vk::BlendOp::eAdd, vk::BlendFactor::eZero,
                takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero};
    }
    return equation;
}

} // namespace Vulkan

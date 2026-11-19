// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Shader::Backend::SPIRV {

constexpr uint32_t IdentityLutMinSize = 16;
constexpr uint32_t IdentityLutMaxSize = 64;

constexpr bool IsIdentityLutShape(uint32_t width, uint32_t height, uint32_t depth) {
    return width == height && height == depth && width >= IdentityLutMinSize &&
           width <= IdentityLutMaxSize;
}

} // namespace Shader::Backend::SPIRV

// The launcher's palette, separate from the in-game palette in src/sotb_accent.h.
#pragma once

#include <cstdint>

namespace Sotb::palette {

struct Rgb {
    uint8_t r, g, b;
};

inline constexpr Rgb kActionFill{0x52, 0x61, 0x6C};
inline constexpr Rgb kActionHover{0x61, 0x72, 0x7E};
inline constexpr Rgb kActionPressed{0x3D, 0x4C, 0x56};
inline constexpr Rgb kActionEdge{0x62, 0xD5, 0xEF};
inline constexpr Rgb kActionLink{0x82, 0xDE, 0xF2};
inline constexpr Rgb kActionText{0xFF, 0xFF, 0xFF};

// Graphite on the title side; dark steel with a little olive under the art.
inline constexpr Rgb kHeaderTopLeft{0x10, 0x15, 0x1B};
inline constexpr Rgb kHeaderTopRight{0x24, 0x31, 0x2C};
inline constexpr Rgb kHeaderBottomRight{0x12, 0x1A, 0x1C};

} // namespace Sotb::palette

// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    uint side_fill;
    float side_fraction;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

// One bilinear tap per pixel skips source texels once the picture is reduced more than 2x, so thin
// strokes such as small text break apart; average taps spaced at most one texel apart instead.
vec4 SampleArea(sampler2D tex, vec2 uv) {
    vec2 size = vec2(textureSize(tex, 0));
    vec2 footprint = abs(vec2(dFdx(uv.x), dFdy(uv.y))) * size;
    ivec2 taps = clamp(ivec2(ceil(footprint - 0.001)), ivec2(1), ivec2(8));
    if (taps.x * taps.y == 1) return textureLod(tex, uv, 0.0);
    vec2 spacing = footprint / vec2(taps) / size;
    vec2 first = uv - 0.5 * footprint / size + 0.5 * spacing;
    vec4 sum = vec4(0.0);
    for (int y = 0; y < taps.y; ++y)
        for (int x = 0; x < taps.x; ++x)
            sum += textureLod(tex, first + vec2(x, y) * spacing, 0.0);
    return sum / float(taps.x * taps.y);
}

const int blur_taps = 32;

float Noise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// The picture is only drawn in its central 16:9 part. On the loading screen the sides show it zoomed to the full width,
// blurred and darkened. On the title and the map the sides keep the scene the game draws there and get the black vignette
// their background image (FullScreenDarken, drawn at full alpha on the title and at half on the map) lays over the central
// part, at the alpha of that texture's left and right edge columns per row, every 10 rows of its 1080. That image stops
// 1.92188 of its 1920 units short of the central part's right edge, so the strip there gets the vignette as well.
const float title_vignette[108] = float[108](
    0.8216, 0.8133, 0.8020, 0.7937, 0.7824, 0.7737, 0.7608, 0.7490, 0.7353,
    0.7263, 0.7118, 0.7027, 0.6882, 0.6796, 0.6627, 0.6537, 0.6412, 0.6294,
    0.6176, 0.6090, 0.5980, 0.5898, 0.5784, 0.5706, 0.5588, 0.5522, 0.5431,
    0.5388, 0.5314, 0.5235, 0.5157, 0.5114, 0.5059, 0.5016, 0.4961, 0.4922,
    0.4882, 0.4843, 0.4804, 0.4784, 0.4745, 0.4722, 0.4706, 0.4686, 0.4667,
    0.4667, 0.4627, 0.4627, 0.4627, 0.4627, 0.4627, 0.4627, 0.4608, 0.4608,
    0.4608, 0.4627, 0.4627, 0.4627, 0.4647, 0.4647, 0.4671, 0.4686, 0.4686,
    0.4725, 0.4745, 0.4784, 0.4796, 0.4824, 0.4859, 0.4922, 0.4961, 0.4996,
    0.5035, 0.5078, 0.5137, 0.5192, 0.5255, 0.5314, 0.5404, 0.5467, 0.5561,
    0.5627, 0.5725, 0.5800, 0.5886, 0.5941, 0.6075, 0.6176, 0.6298, 0.6388,
    0.6525, 0.6620, 0.6761, 0.6843, 0.6976, 0.7102, 0.7224, 0.7322, 0.7447,
    0.7545, 0.7682, 0.7765, 0.7910, 0.7976, 0.8106, 0.8192, 0.8290, 0.8353
);

float FrontendVignette(float v) {
    float at = clamp(v * 108.0, 0.0, 107.0);
    int i = int(floor(at));
    return mix(title_vignette[i], title_vignette[min(i + 1, 107)], fract(at));
}

vec3 encode(vec3 rgb) {
    return mix(a * pow(rgb, vec3(1.0 / 2.4)) - b, d * rgb, lessThan(rgb, vec3(cutoff)));
}

// The vignette is blended by the game in the encoded values of its picture.
vec3 VignetteSide(vec3 rgb, float strength) {
    float keep = 1.0 - strength * FrontendVignette(uv.y);
    return pp.srgb_input ? rgb * keep : degamma(encode(rgb) * keep);
}

// One source column at its texel centre, averaged down the pixel's height.
vec3 SampleColumn(float column, vec2 size, float footprint_y) {
    int taps = clamp(int(ceil(footprint_y - 0.001)), 1, 8);
    float spacing = footprint_y / float(taps) / size.y;
    float first = uv.y - 0.5 * footprint_y / size.y + 0.5 * spacing;
    vec3 sum = vec3(0.0);
    for (int y = 0; y < taps; ++y)
        sum += textureLod(texSampler, vec2((column + 0.5) / size.x, first + float(y) * spacing), 0.0).rgb;
    return sum / float(taps);
}

// The game darkens the source columns whose centres its background covers. A frame pixel that straddles either edge of
// that span vignettes its source columns one by one, so the edge stays continuous.
vec4 FrontendSides(vec4 color, float lo, float hi, float strip, float strength, vec2 footprint) {
    vec2 size = vec2(textureSize(texSampler, 0));
    float first_dark = ceil(lo * size.x - 0.5), end_dark = ceil((hi - strip) * size.x - 0.5);
    float a = uv.x * size.x - 0.5 * footprint.x, b = a + footprint.x;
    if (a >= first_dark && b <= end_dark) return color;
    if (b <= first_dark || a >= end_dark) return vec4(VignetteSide(color.rgb, strength), color.a);
    vec3 sum = vec3(0.0);
    for (float column = floor(a); column < b; column += 1.0) {
        vec3 c = SampleColumn(clamp(column, 0.0, size.x - 1.0), size, footprint.y);
        bool side = column < first_dark || column >= end_dark;
        sum += (min(b, column + 1.0) - max(a, column)) * (side ? VignetteSide(c, strength) : c);
    }
    return vec4(sum / footprint.x, color.a);
}

vec4 SideFill(float lo, float hi) {
    vec2 size = vec2(textureSize(texSampler, 0));
    vec2 center = vec2(lo + uv.x * pp.side_fraction, 0.5 + (uv.y - 0.5) * pp.side_fraction);
    float radius = 0.09 * size.y;
    float angle = 6.2831853 * Noise(gl_FragCoord.xy);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < blur_taps; ++i) {
        float r = sqrt((float(i) + 0.5) / float(blur_taps));
        vec2 p = center + vec2(cos(angle), sin(angle)) * r * radius / size;
        sum += textureLod(texSampler, clamp(p, vec2(lo, 0.0), vec2(hi, 1.0)), 0.0).rgb;
        angle += 2.39996323;
    }
    return vec4(sum / float(blur_taps) * 0.55, 1.0);
}

void main() {
    vec2 footprint = max(abs(vec2(dFdx(uv.x), dFdy(uv.y))) * vec2(textureSize(texSampler, 0)), vec2(1e-4));
    vec4 color_linear = SampleArea(texSampler, uv);
    if (pp.side_fill != 0u) {
        float lo = (1.0 - pp.side_fraction) * 0.5;
        float hi = lo + pp.side_fraction;
        if (pp.side_fill >= 2u) {
            float strip = 1.92188 / 1920.0 * pp.side_fraction;
            color_linear = FrontendSides(color_linear, lo, hi, strip, pp.side_fill == 3u ? 0.5 : 1.0, footprint);
        } else if (uv.x < lo || uv.x > hi) {
            color_linear = SideFill(lo, hi);
        }
    }
    if (pp.hdr) {
        color = color_linear;
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}

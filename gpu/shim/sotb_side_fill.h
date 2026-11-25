// SPDX-License-Identifier: GPL-2.0-or-later
// What the sides of a picture wider than 16:9 get when the game only lays out its central 16:9 part. The loading screen
// shows a blurred, darkened copy of that part for a short hold; the title and the map get the vignette of their own
// background over the scene the game draws there, for as long as they are the open screen. The presenter reads the mode once per frame.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace SotbSideFill {

enum Mode : int { None = 0, LoadingBlur = 1, TitleVignette = 2, MapVignette = 3 };

inline constexpr uint64_t kLoadingHoldMs = 200;

struct State {
    std::atomic<uint64_t> loading_marked_ms{0};
    std::atomic<int> screen{None};
};

inline State& Get() {
    static State state;
    return state;
}

inline uint64_t NowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count()) + 1;
}

inline void MarkLoading(uint64_t now_ms) {
    Get().loading_marked_ms.store(now_ms, std::memory_order_release);
}

/// The screen the guest says is open: TitleVignette, MapVignette, or anything else for none.
inline void SetScreen(int mode) {
    Get().screen.store(mode == TitleVignette || mode == MapVignette ? mode : int(None), std::memory_order_release);
}

/// The loading blur while the loading screen was arranged within the hold time, else the vignette of the open title or
/// map, else None. The title and the map have no hold: they end the moment the guest clears them.
inline int Current(uint64_t now_ms) {
    const State& state = Get();
    const uint64_t marked = state.loading_marked_ms.load(std::memory_order_acquire);
    if (marked != 0 && now_ms >= marked && now_ms - marked <= kLoadingHoldMs) return LoadingBlur;
    return state.screen.load(std::memory_order_acquire);
}

/// Width of the central 16:9 part as a fraction of the picture, or 0 when the picture is not wider than 16:9.
inline float CentralFraction(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return 0.0f;
    const double central = double(height) * 16.0 / 9.0;
    return double(width) > central * 1.01 ? float(central / double(width)) : 0.0f;
}

} // namespace SotbSideFill

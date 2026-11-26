// When the launcher asks Vulkan for the Graphics card list, and what the row shows meanwhile. Pure policy: no Vulkan.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace Sotb {

// The Vulkan probe runs on the UI thread and holds the window while it does, so it waits until the launcher has shown
// a few complete frames.
inline constexpr int kGraphicsProbeAfterFrames = 3;
inline constexpr double kGraphicsProbeAfterSeconds = 0.5;

enum class GraphicsCardsState { kPending, kSkipped, kProbed };

inline bool GraphicsProbeDue(int frames_shown, double seconds_shown) {
    return frames_shown >= kGraphicsProbeAfterFrames && seconds_shown >= kGraphicsProbeAfterSeconds;
}

// The saved choice, shown before the probe has run.
inline std::string GraphicsCardSavedText(std::string_view saved) {
    return saved.empty() || saved == "automatic" ? std::string("First available") : std::string(saved);
}

// What the row shows when there is no card to choose; empty while pending or when there are cards.
inline const char* GraphicsCardsProblem(GraphicsCardsState state, size_t cards) {
    if (cards > 0) {
        return "";
    }
    switch (state) {
    case GraphicsCardsState::kPending: return "";
    case GraphicsCardsState::kSkipped: return "Graphics card detection is off while the launcher uses software rendering";
    case GraphicsCardsState::kProbed: return "No graphics card found";
    }
    return "";
}

inline std::string GraphicsCardsLine(GraphicsCardsState state, size_t cards) {
    if (state == GraphicsCardsState::kSkipped) {
        return "Launcher: graphics cards: detection skipped, software launcher renderer requested";
    }
    char line[96];
    std::snprintf(line, sizeof(line), "Launcher: graphics cards: %zu usable", cards);
    return line;
}

} // namespace Sotb

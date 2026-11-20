// Where the launcher window was left, so it reopens there.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "settings.h"
#include "sotb_window_fit.h"

namespace Sotb {

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool operator==(const Rect&) const = default;
};

// The restored rectangle in screen pixels, whether the window is maximized, and whether it was left in its own F11
// fullscreen. Fullscreen sits on top of the other two, which keep describing the window underneath.
struct WindowPlacement {
    Rect normal;
    bool maximized = false;
    bool fullscreen = false;

    bool operator==(const WindowPlacement&) const = default;
};

// What the window is doing right now. The rectangle means nothing while minimized.
struct WindowState {
    Rect rect;
    bool maximized = false;
    bool minimized = false;
};

inline constexpr int kMinPlacementWidth = 320;
inline constexpr int kMinPlacementHeight = 240;
inline constexpr int kMinVisibleWidth = 100;
inline constexpr int kMinVisibleHeight = 50;
inline constexpr double kPlacementSaveDelaySeconds = 0.5;

inline bool PlacementIsSet(const WindowPlacement& placement) {
    return placement.normal.width() >= kMinPlacementWidth && placement.normal.height() >= kMinPlacementHeight;
}

// A saved window is only restored if enough of it lies on a display to be grabbed.
inline bool PlacementIsVisible(const WindowPlacement& placement, std::span<const Rect> work_areas) {
    if (!PlacementIsSet(placement)) {
        return false;
    }
    for (const Rect& area : work_areas) {
        const int width = std::min(placement.normal.right, area.right) - std::max(placement.normal.left, area.left);
        const int height = std::min(placement.normal.bottom, area.bottom) - std::max(placement.normal.top, area.top);
        if (width >= kMinVisibleWidth && height >= kMinVisibleHeight) {
            return true;
        }
    }
    return false;
}

struct DisplayArea {
    Rect usable;
    float scale = 1.0f;
};

enum class PlacementSource { kCentered, kRestored, kClamped, kFallback };

// The window's client rectangle in screen pixels and the display it belongs to (0 is the primary). A centered choice is
// left to SDL's centering on that display; its rectangle is where SDL puts it.
struct PlacementChoice {
    Rect rect;
    size_t display = 0;
    PlacementSource source = PlacementSource::kCentered;
    bool centered = true;
    bool stored_invalid = false;
    bool maximized = false;
    bool fullscreen = false;
};

namespace placement_detail {

inline int Overlap(int a0, int a1, int b0, int b1) { return std::max(0, std::min(a1, b1) - std::max(a0, b0)); }

inline int64_t OverlapArea(const Rect& a, const Rect& b) {
    return int64_t(Overlap(a.left, a.right, b.left, b.right)) * Overlap(a.top, a.bottom, b.top, b.bottom);
}

inline int TitleBar(float scale) { return EstimatedBorders(scale).top; }

inline Rect Sized(int left, int top, int width, int height) { return {left, top, left + width, top + height}; }

inline Rect Centered(int width, int height, const Rect& usable) {
    return Sized(usable.left + (usable.width() - width) / 2, usable.top + (usable.height() - height) / 2, width, height);
}

inline int Share(int size, int room) {
    return room > 0 ? std::min(size, static_cast<int>(room * kWindowDisplayShare)) : size;
}

// The title bar sits above the client rectangle; half of it on the display is enough to drag the window.
inline bool Grabbable(const Rect& rect, const Rect& usable, int title) {
    return Overlap(rect.left, rect.right, usable.left, usable.right) >= kMinVisibleWidth &&
           rect.top - title / 2 >= usable.top && rect.top <= usable.bottom - kMinVisibleHeight;
}

inline Rect ClampInto(const Rect& rect, const Rect& usable, int title) {
    const int width = std::min(rect.width(), usable.width());
    const int height = std::min(rect.height(), std::max(1, usable.height() - title));
    const int left = std::clamp(rect.left, usable.left, std::max(usable.left, usable.right - width));
    const int top = std::clamp(rect.top, usable.top + title, std::max(usable.top + title, usable.bottom - height));
    return Sized(left, top, width, height);
}

} // namespace placement_detail

// The display holding most of `rect`; the primary when it lies on none.
inline size_t BestDisplay(const Rect& rect, std::span<const DisplayArea> displays) {
    size_t best = 0;
    int64_t most = 0;
    for (size_t i = 0; i < displays.size(); ++i) {
        const int64_t area = placement_detail::OverlapArea(rect, displays[i].usable);
        if (area > most) {
            most = area;
            best = i;
        }
    }
    return best;
}

// Where the launcher opens. A saved rectangle is kept while it fits its display and its title bar can be reached;
// one that sticks out of the top or is larger than the display is moved and shrunk into it; one on no display is
// centered on the primary with its size kept where it fits. With nothing saved the launcher is centered on the
// primary at its logical size times the display scale. The maximized and fullscreen states are always kept.
inline PlacementChoice ChoosePlacement(const std::optional<WindowPlacement>& saved, std::span<const DisplayArea> displays,
                                       int logical_width, int logical_height) {
    using namespace placement_detail;
    PlacementChoice choice;
    if (saved) {
        choice.maximized = saved->maximized;
        choice.fullscreen = saved->fullscreen;
    }
    if (displays.empty()) {
        return choice;
    }
    std::vector<Rect> areas;
    for (const DisplayArea& display : displays) {
        areas.push_back(display.usable);
    }
    const Rect& primary = displays.front().usable;
    if (saved && PlacementIsVisible(*saved, areas)) {
        const Rect& normal = saved->normal;
        choice.display = BestDisplay(normal, displays);
        const DisplayArea& display = displays[choice.display];
        const int title = TitleBar(display.scale);
        const bool fits = normal.width() <= display.usable.width() && normal.height() <= display.usable.height() - title / 2;
        choice.centered = false;
        if (fits && Grabbable(normal, display.usable, title)) {
            choice.rect = normal;
            choice.source = PlacementSource::kRestored;
        } else {
            choice.rect = ClampInto(normal, display.usable, title);
            choice.source = PlacementSource::kClamped;
        }
        return choice;
    }
    if (saved && PlacementIsSet(*saved)) {
        choice.stored_invalid = true;
        choice.source = PlacementSource::kFallback;
        choice.rect = Centered(Share(saved->normal.width(), primary.width()), Share(saved->normal.height(), primary.height()),
                               primary);
        return choice;
    }
    choice.stored_invalid = saved && saved->normal != Rect{};
    const float scale = displays.front().scale > 0.0f ? displays.front().scale : 1.0f;
    choice.rect = Centered(Share(static_cast<int>(logical_width * scale), primary.width()),
                           Share(static_cast<int>(logical_height * scale), primary.height()), primary);
    return choice;
}

inline const char* PlacementSourceName(PlacementSource source) {
    switch (source) {
    case PlacementSource::kCentered: return "centered";
    case PlacementSource::kRestored: return "restored";
    case PlacementSource::kClamped: return "clamped";
    case PlacementSource::kFallback: return "fallback";
    }
    return "unknown";
}

// What the window really got once it was created, for the one placement line.
struct PlacementResult {
    Rect final_rect;
    int pixel_width = 0;
    int pixel_height = 0;
    float window_scale = 0.0f;
};

// A display name for the log: printable ASCII only, spaces as underscores, at most 32 characters.
inline std::string DisplayNameForLog(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if (out.size() >= 32) {
            break;
        }
        out += c == ' ' ? '_' : (c > ' ' && c < 0x7f && c != '"' && c != '\\' ? c : '?');
    }
    return out.empty() ? "unknown" : out;
}

inline std::string PlacementLine(const PlacementChoice& choice, std::span<const DisplayArea> displays, std::string_view display_name,
                                 const PlacementResult& result) {
    const DisplayArea display = choice.display < displays.size() ? displays[choice.display] : DisplayArea{};
    const char* stored = choice.stored_invalid ? "invalid"
                         : choice.source == PlacementSource::kRestored || choice.source == PlacementSource::kClamped ? "valid"
                                                                                                                       : "none";
    const Rect& want = choice.rect;
    const Rect& got = result.final_rect;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "Launcher telemetry: window_placement source=%s stored=%s display=%zu/%zu primary=%d name=%s "
                  "usable=%d,%d %dx%d display_scale=%.2f requested=%d,%d %dx%d final=%d,%d %dx%d pixels=%dx%d "
                  "window_scale=%.2f maximized=%d fullscreen=%d",
                  PlacementSourceName(choice.source), stored, choice.display + 1, displays.size(), choice.display == 0 ? 1 : 0,
                  DisplayNameForLog(display_name).c_str(), display.usable.left, display.usable.top, display.usable.width(),
                  display.usable.height(), double(display.scale), want.left, want.top, want.width(), want.height(), got.left,
                  got.top, got.width(), got.height(), result.pixel_width, result.pixel_height, double(result.window_scale),
                  choice.maximized ? 1 : 0, choice.fullscreen ? 1 : 0);
    return line;
}

template <typename Settings>
WindowPlacement ReadPlacement(const Settings& settings) {
    WindowPlacement placement;
    placement.normal.left = settings.GetInt(kLauncherXKey, 0);
    placement.normal.top = settings.GetInt(kLauncherYKey, 0);
    placement.normal.right = placement.normal.left + settings.GetInt(kLauncherWidthKey, 0);
    placement.normal.bottom = placement.normal.top + settings.GetInt(kLauncherHeightKey, 0);
    placement.maximized = settings.GetBool(kLauncherMaximizedKey, false);
    placement.fullscreen = settings.GetBool(kLauncherFullscreenKey, false);
    return placement;
}

inline Values PlacementValues(const WindowPlacement& placement) {
    return {
        {kLauncherXKey, std::to_string(placement.normal.left)},
        {kLauncherYKey, std::to_string(placement.normal.top)},
        {kLauncherWidthKey, std::to_string(placement.normal.width())},
        {kLauncherHeightKey, std::to_string(placement.normal.height())},
        {kLauncherMaximizedKey, placement.maximized ? "1" : "0"},
        {kLauncherFullscreenKey, placement.fullscreen ? "1" : "0"},
    };
}

// Follows the window and says when its placement has changed and settled, so it is saved once and not on every move.
// A maximized window keeps the restored rectangle; a fullscreen one keeps both it and the maximized flag, and a
// minimized one keeps everything (SDL may clear the maximized flag while the window is minimized).
class PlacementTracker {
public:
    explicit PlacementTracker(std::optional<WindowPlacement> saved = std::nullopt) : saved_(saved), current_(saved) {}

    void Observe(const WindowState& state, bool fullscreen, double now) {
        WindowPlacement next = current_.value_or(WindowPlacement{});
        next.fullscreen = fullscreen;
        if (!fullscreen && !state.minimized) {
            next.maximized = state.maximized;
            if (!state.maximized) {
                next.normal = state.rect;
            }
        }
        if (!saved_) {
            saved_ = next;
        }
        if (!current_ || next != *current_) {
            current_ = next;
            changed_at_ = now;
        }
    }

    const std::optional<WindowPlacement>& Current() const { return current_; }

    std::optional<WindowPlacement> TakeSettled(double now) {
        if (!Dirty() || now - changed_at_ < kPlacementSaveDelaySeconds) {
            return std::nullopt;
        }
        return Flush();
    }

    std::optional<WindowPlacement> Flush() {
        if (!Dirty()) {
            return std::nullopt;
        }
        saved_ = current_;
        return saved_;
    }

private:
    bool Dirty() const { return current_ && current_ != saved_ && PlacementIsSet(*current_); }

    std::optional<WindowPlacement> saved_;
    std::optional<WindowPlacement> current_;
    double changed_at_ = 0.0;
};

} // namespace Sotb

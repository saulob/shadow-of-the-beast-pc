// SPDX-License-Identifier: GPL-2.0-or-later
// Cursor visibility from the last meaningful gameplay input device.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <unordered_map>

namespace SotbCursor {

enum class Device { Mouse, Keyboard, Controller };

class State {
public:
    // gameplay: the game window is accepting input (false in the launcher).
    // overlay: a mouse-driven UI is open and needs the cursor.
    // Returns the new visibility when it changes, otherwise nullopt.
    std::optional<bool> Update(bool gameplay, bool focused, bool overlay) {
        if (!gameplay) {
            device_ = Device::Mouse;
        }
        const bool visible = !gameplay || !focused || overlay || device_ == Device::Mouse;
        if (visible == visible_) {
            return std::nullopt;
        }
        visible_ = visible;
        return visible;
    }

    void MouseMoved(int dx, int dy) {
        if (dx != 0 || dy != 0) device_ = Device::Mouse;
    }
    void MouseButtonPressed() { device_ = Device::Mouse; }
    void MouseWheelMoved() { device_ = Device::Mouse; }
    void KeyboardActivity() { device_ = Device::Keyboard; }
    void ControllerActivity() { device_ = Device::Controller; }

    Device LastDevice() const { return device_; }
    bool Visible() const { return visible_; }

private:
    Device device_ = Device::Mouse;
    bool visible_ = true;
};

// A key press that counts as gameplay input: not a repeat, and not a host shortcut.
constexpr bool IsGameplayKey(bool repeat, bool host_shortcut) {
    return !repeat && !host_shortcut;
}

// Onset thresholds in the gamepad's raw -32768..32767 units; a stick or trigger rearms
// only after falling below the lower release level, so jitter at the edge is one onset.
inline constexpr int kStickDeadzone = 7849;
inline constexpr int kStickRelease = kStickDeadzone * 3 / 4;
inline constexpr int kTriggerThreshold = 3840; // 30 of 255 in the runtime's trigger units
inline constexpr int kTriggerRelease = kTriggerThreshold * 3 / 4;

enum class Axis { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger };

// Reports only the start of meaningful input on one controller, not a held button,
// stick, or trigger, so mouse movement is not overridden by input already down.
class ControllerActivityFilter {
public:
    bool ButtonChanged(int button, bool down) {
        if (button < 0 || button >= 32) return false;
        const std::uint32_t bit = 1u << button;
        const bool started = down && (buttons_ & bit) == 0;
        buttons_ = down ? (buttons_ | bit) : (buttons_ & ~bit);
        return started;
    }

    bool AxisMoved(Axis axis, int value) {
        const int index = static_cast<int>(axis);
        const bool trigger = axis == Axis::LeftTrigger || axis == Axis::RightTrigger;
        const long long magnitude = value >= 0 ? value : (trigger ? 0 : -static_cast<long long>(value));
        const long long enter = trigger ? kTriggerThreshold : kStickDeadzone;
        const long long release = trigger ? kTriggerRelease : kStickRelease;
        bool& active = active_[index];
        if (!active && magnitude >= enter) {
            active = true;
            return true;
        }
        if (active && magnitude < release) active = false;
        return false;
    }

private:
    std::uint32_t buttons_ = 0;
    bool active_[6] = {};
};

// One filter per connected controller; a hotplug or disconnect only creates or drops state.
class ControllerActivityTracker {
public:
    bool ButtonChanged(std::uint32_t id, int button, bool down) { return pads_[id].ButtonChanged(button, down); }
    bool AxisMoved(std::uint32_t id, Axis axis, int value) { return pads_[id].AxisMoved(axis, value); }
    void Forget(std::uint32_t id) { pads_.erase(id); }
    std::size_t Tracked() const { return pads_.size(); }

private:
    std::unordered_map<std::uint32_t, ControllerActivityFilter> pads_;
};

} // namespace SotbCursor

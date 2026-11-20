// Waiting for a controller button on the Controls page: which button or trigger the user presses next.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "sotb_pad.h"
#include "sotb_pad_prompts.h"

struct SDL_Gamepad;

namespace Sotb {

struct PadCaptureResult {
    enum class Kind { kWaiting, kAssigned, kCancelled, kNoController };
    Kind kind = Kind::kWaiting;
    int source = SOTB_PAD_SOURCE_NONE;
};

// A source must be the one held for this many updates in a row, so a one-frame glitch assigns nothing.
inline constexpr int kPadCaptureStableUpdates = 2;

class PadCapture {
public:
    // False when no controller is connected: nothing starts and the binding stays as it is.
    bool Begin(SotbPadAction action, size_t controllers);
    void Cancel();
    bool active() const { return action_.has_value(); }
    SotbPadAction action() const { return *action_; }

    // One frame of the controllers' state. A button held when the capture began (the one that opened the row) is
    // ignored until everything has been released. No controller left ends the capture.
    PadCaptureResult Update(std::span<const SotbPadInput> controllers, bool escape);

private:
    std::optional<SotbPadAction> action_;
    bool armed_ = false;
    int candidate_ = SOTB_PAD_SOURCE_NONE;
    int stable_ = 0;
};

// The controllers SDL reports, opened for the time a binding waits.
class ControllerReader {
public:
    ControllerReader() = default;
    ControllerReader(const ControllerReader&) = delete;
    ControllerReader& operator=(const ControllerReader&) = delete;
    ~ControllerReader() { Close(); }
    static size_t Connected();
    // `pressed_id` receives the id of the first controller with something held, 0 when none.
    std::vector<SotbPadInput> Read(uint32_t* pressed_id = nullptr);
    void Close();

private:
    std::vector<SDL_Gamepad*> open_;
};

// The connected gamepads in SDL's order with their detected kind, without opening them.
std::vector<SotbPadDevice> ConnectedPadDevices();

} // namespace Sotb

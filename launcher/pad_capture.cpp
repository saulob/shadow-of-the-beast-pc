#include "pad_capture.h"

#include <algorithm>

#include <SDL3/SDL.h>

namespace Sotb {
namespace {

uint8_t TriggerValue(SDL_Gamepad* gamepad, SDL_GamepadAxis axis) {
    return static_cast<uint8_t>(std::clamp(SDL_GetGamepadAxis(gamepad, axis) >> 7, 0, 255));
}

} // namespace

bool PadCapture::Begin(SotbPadAction action, size_t controllers) {
    if (controllers == 0) {
        return false;
    }
    action_ = action;
    armed_ = false;
    candidate_ = SOTB_PAD_SOURCE_NONE;
    stable_ = 0;
    return true;
}

void PadCapture::Cancel() {
    action_.reset();
    armed_ = false;
    candidate_ = SOTB_PAD_SOURCE_NONE;
    stable_ = 0;
}

PadCaptureResult PadCapture::Update(std::span<const SotbPadInput> controllers, bool escape) {
    using Kind = PadCaptureResult::Kind;
    if (!action_) {
        return {};
    }
    if (escape) {
        Cancel();
        return {Kind::kCancelled, SOTB_PAD_SOURCE_NONE};
    }
    if (controllers.empty()) {
        Cancel();
        return {Kind::kNoController, SOTB_PAD_SOURCE_NONE};
    }
    int pressed = SOTB_PAD_SOURCE_NONE;
    for (const SotbPadInput& controller : controllers) {
        pressed = sotb_pad_first_pressed(&controller);
        if (pressed != SOTB_PAD_SOURCE_NONE) {
            break;
        }
    }
    if (pressed == SOTB_PAD_SOURCE_NONE) {
        armed_ = true;
        candidate_ = SOTB_PAD_SOURCE_NONE;
        stable_ = 0;
        return {};
    }
    if (!armed_) {
        return {};
    }
    stable_ = pressed == candidate_ ? stable_ + 1 : 1;
    candidate_ = pressed;
    if (stable_ < kPadCaptureStableUpdates) {
        return {};
    }
    Cancel();
    return {Kind::kAssigned, pressed};
}

size_t ControllerReader::Connected() {
    int count = 0;
    SDL_free(SDL_GetGamepads(&count));
    return static_cast<size_t>(count);
}

std::vector<SotbPadInput> ControllerReader::Read(uint32_t* pressed_id) {
    if (pressed_id) {
        *pressed_id = 0;
    }
    for (auto it = open_.begin(); it != open_.end();) {
        if (SDL_GamepadConnected(*it)) {
            ++it;
        } else {
            SDL_CloseGamepad(*it);
            it = open_.erase(it);
        }
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        SDL_Gamepad* gamepad = SDL_OpenGamepad(ids[i]);
        if (!gamepad) {
            continue;
        }
        if (std::find(open_.begin(), open_.end(), gamepad) == open_.end()) {
            open_.push_back(gamepad);
        } else {
            SDL_CloseGamepad(gamepad);
        }
    }
    SDL_free(ids);
    std::vector<SotbPadInput> samples;
    for (SDL_Gamepad* gamepad : open_) {
        SotbPadInput sample{};
        for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
            if (SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(button))) {
                sample.buttons |= 1u << button;
            }
        }
        sample.left_trigger = TriggerValue(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        sample.right_trigger = TriggerValue(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        if (pressed_id && !*pressed_id && sotb_pad_first_pressed(&sample) != SOTB_PAD_SOURCE_NONE) {
            *pressed_id = SDL_GetGamepadID(gamepad);
        }
        samples.push_back(sample);
    }
    return samples;
}

void ControllerReader::Close() {
    for (SDL_Gamepad* gamepad : open_) {
        SDL_CloseGamepad(gamepad);
    }
    open_.clear();
}

std::vector<SotbPadDevice> ConnectedPadDevices() {
    std::vector<SotbPadDevice> devices;
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
        return devices;
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        devices.push_back({ids[i], sotb_pad_classify(static_cast<int>(SDL_GetGamepadTypeForID(ids[i])),
                                                     static_cast<int>(SDL_GetRealGamepadTypeForID(ids[i])),
                                                     SDL_GetGamepadVendorForID(ids[i]),
                                                     SDL_GetGamepadProductForID(ids[i]))});
    }
    SDL_free(ids);
    return devices;
}

} // namespace Sotb

// SPDX-License-Identifier: GPL-2.0-or-later
// Exit Game confirmation dialog state, layout metrics, and localized prompt formatting.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "sotb_exit_game_l10n.h"

namespace SotbExitConfirm {

constexpr uint32_t kButtonCircle = 0x2000u;
constexpr uint32_t kButtonCross = 0x4000u;

enum class Action { None, Cancelled, Confirmed };

struct PopupMetrics {
    float min_w = 0.0f;
    float max_w = 0.0f;
    float max_h = 0.0f;
    float pad_x = 0.0f;
    float pad_y = 0.0f;
    float item_spacing_x = 0.0f;
    float item_spacing_y = 0.0f;
    float section_gap = 0.0f;
    float title_scale = 1.12f;
    float body_scale = 1.05f;
    float hint_scale = 0.95f;
};

inline PopupMetrics ComputeMetrics(float viewport_w, float viewport_h, float base_scale) {
    const float scale = std::max(base_scale, 0.75f);
    return {
        .min_w = viewport_w * 0.36f,
        .max_w = viewport_w * 0.68f,
        .max_h = viewport_h * 0.55f,
        .pad_x = 24.0f * scale,
        .pad_y = 20.0f * scale,
        .item_spacing_x = 10.0f * scale,
        .item_spacing_y = 8.0f * scale,
        .section_gap = 6.0f * scale,
        .title_scale = 1.12f,
        .body_scale = 1.05f,
        .hint_scale = 0.95f,
    };
}

inline uint32_t ConfirmMask(bool original_layout) {
    return original_layout ? kButtonCircle : kButtonCross;
}

inline uint32_t CancelMask(bool original_layout) {
    return original_layout ? kButtonCross : kButtonCircle;
}

inline std::string FormatPromptHint(bool original_layout, bool xbox_prompts,
                                    const char* confirm_text, const char* cancel_text) {
    const char* confirm_btn = original_layout ? (xbox_prompts ? "B" : "Circle")
                                              : (xbox_prompts ? "A" : "Cross");
    const char* cancel_btn = original_layout ? (xbox_prompts ? "A" : "Cross")
                                             : (xbox_prompts ? "B" : "Circle");
    return std::string(confirm_btn) + " " + (confirm_text ? confirm_text : "Confirm") +
           "     " + cancel_btn + " " + (cancel_text ? cancel_text : "Cancel");
}

struct State {
    bool active = false;
    bool waiting_for_save = false;
    bool original_layout = false;
    bool xbox_prompts = true;
    int focus = 0; // 0 = Confirm, 1 = Cancel
    const SotbExitGameStrings* strings = nullptr;

    const SotbExitGameStrings& Text() const {
        return *(strings ? strings : sotb_exit_game_strings("en_US"));
    }

    std::string HintText() const {
        const auto& t = Text();
        return FormatPromptHint(original_layout, xbox_prompts, t.confirm, t.cancel);
    }

    void Open(bool original, bool xbox, const char* locale = "en_US") {
        active = true;
        waiting_for_save = false;
        original_layout = original;
        xbox_prompts = xbox;
        focus = 0;
        strings = sotb_exit_game_strings(locale);
    }

    void Close() {
        active = false;
        waiting_for_save = false;
        focus = 0;
    }

    Action Step(int move, bool confirm_pressed, bool cancel_pressed) {
        if (!active || waiting_for_save) {
            return Action::None;
        }
        if (move != 0) {
            focus = focus == 0 ? 1 : 0;
        }
        if (cancel_pressed) {
            Close();
            return Action::Cancelled;
        }
        if (confirm_pressed) {
            if (focus == 0) {
                return Action::Confirmed;
            }
            Close();
            return Action::Cancelled;
        }
        return Action::None;
    }

    Action StepPad(uint32_t pressed_bits) {
        constexpr uint32_t kMoveMask = 0x10u | 0x20u | 0x40u | 0x80u;
        const int move = (pressed_bits & kMoveMask) ? 1 : 0;
        const bool confirm = (pressed_bits & ConfirmMask(original_layout)) != 0;
        const bool cancel = (pressed_bits & CancelMask(original_layout)) != 0;
        return Step(move, confirm, cancel);
    }
};

} // namespace SotbExitConfirm

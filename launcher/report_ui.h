// Small drawing helpers of the Bug Report page and its My Reports section.
#pragma once

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <ctime>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"
#include "incident_report.h"
#include "localization/ui_text.h"
#include "trg/launcher.h"

namespace Sotb {

inline constexpr char kMiddleDot[] = " \xC2\xB7 ";

inline void AccentText(trg::Ui& ui, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().accent);
    ImGui::TextUnformatted(Tr(text));
    ImGui::PopStyleColor();
}

inline void WarningText(trg::Ui& ui, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
    ImGui::TextWrapped("%s", Tr(text));
    ImGui::PopStyleColor();
}

inline void HelpLine(const ImVec4& color, const std::string& text, bool wrap = false) {
    ImGui::PushFont(nullptr, trg::layout::kTextHelp);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    if (wrap) ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    if (wrap) ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Escape leaves the field like a click elsewhere: ImGui would revert to the text it had when editing began, and a long
// draft would be lost.
inline bool InputTextMultilineKeepOnEscape(const char* id, char* buf, size_t size, const ImVec2& box, ImGuiInputTextFlags flags,
                                           ImGuiInputTextCallback callback = nullptr) {
    const bool leaving = ImGui::GetActiveID() == ImGui::GetID(id) && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    std::string kept;
    if (leaving) kept = buf;
    const bool changed = ImGui::InputTextMultiline(id, buf, size, box, flags, callback);
    if (!leaving || kept == buf) return changed;
    std::memcpy(buf, kept.c_str(), kept.size() + 1);
    return false;
}

// Text that cannot change: ImGui never writes to a read-only buffer.
inline void ReadOnlyBox(const char* id, std::string& text, float height) {
    ImGui::InputTextMultiline(id, text.data(), text.size() + 1, ImVec2(-FLT_MIN, height),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
}

// The centred text and spinner of a page whose first content is still being prepared.
inline void DrawLoadingView(trg::Ui& ui, const char* english_text) {
    const float scale = ui.scale();
    const char* text = Tr(english_text);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 room = ImGui::GetContentRegionAvail();
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const float radius = 10.0f * scale, gap = 14.0f * scale;
    const float block = text_size.y + gap + radius * 2.0f;
    const ImVec2 center(min.x + room.x * 0.5f, min.y + std::max(0.0f, (room.y - block) * 0.5f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddText(ImVec2(center.x - text_size.x * 0.5f, center.y), ImGui::GetColorU32(ui.theme().text), text);
    const ImVec2 spinner(center.x, center.y + text_size.y + gap + radius);
    const float angle = float(ImGui::GetTime()) * 5.0f;
    draw->PathArcTo(spinner, radius, angle, angle + 4.2f, 24);
    draw->PathStroke(ImGui::GetColorU32(ui.theme().accent), 0, 3.0f * scale);
}

// `milliseconds` since the epoch, as the service counts them.
inline std::string DateText(int64_t milliseconds) { return LogDateText(std::time_t(milliseconds / 1000)); }

} // namespace Sotb

// Which SDL renderer the launcher window asks for, and how the answer is reported. Pure policy: no window, no SDL.
#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Sotb {

inline constexpr const char* kLauncherRendererVariable = "SOTB_LAUNCHER_RENDERER";

enum class RendererKind { kUnknown, kSoftware, kAccelerated };

inline RendererKind ClassifyRenderer(std::string_view name) {
    if (name == "software") {
        return RendererKind::kSoftware;
    }
    for (const std::string_view accelerated : {"direct3d11", "direct3d12", "direct3d", "opengl", "opengles2", "vulkan", "gpu", "metal"}) {
        if (name == accelerated) {
            return RendererKind::kAccelerated;
        }
    }
    return RendererKind::kUnknown;
}

inline const char* RendererKindName(RendererKind kind) {
    switch (kind) {
    case RendererKind::kSoftware: return "software";
    case RendererKind::kAccelerated: return "accelerated";
    default: return "unknown";
    }
}

inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

// Renderer names in the order they are tried, each at most once. An empty name lets SDL pick its first working one.
struct RendererPlan {
    std::array<std::string_view, 2> names{};
    size_t count = 0;
    bool ignored_request = false;  // the environment named something this platform does not allow
};

// Windows tries Direct3D 11 and falls back to software; Direct3D 11 is never the only choice. The environment can only
// ask for software, or repeat the default.
inline RendererPlan PlanLauncherRenderer(bool windows, const char* request) {
    RendererPlan plan;
    const std::string_view value = request ? std::string_view(request) : std::string_view();
    const bool software = EqualsIgnoreCase(value, "software");
    if (windows) {
        plan.names = software ? std::array<std::string_view, 2>{"software", ""} : std::array<std::string_view, 2>{"direct3d11", "software"};
        plan.count = software ? 1 : 2;
        plan.ignored_request = !value.empty() && !software && !EqualsIgnoreCase(value, "direct3d11");
        return plan;
    }
    plan.names = software ? std::array<std::string_view, 2>{"software", ""} : std::array<std::string_view, 2>{"", "software"};
    plan.count = software ? 1 : 2;
    plan.ignored_request = !value.empty() && !software;
    return plan;
}

// System modules the renderer loads that stay mapped until the process exits: SDL unloads d3d11.dll right after the
// device's last Release, while a driver thread can still be returning through it.
inline std::vector<std::string_view> RendererModulesToPin(std::string_view renderer) {
    if (renderer == "direct3d11") {
        return {"d3d11.dll", "dxgi.dll"};
    }
    return {};
}

inline bool LauncherSoftwareRequested(const char* request) {
    return request && EqualsIgnoreCase(request, "software");
}

// How far one renderer attempt got: a window, the pinned modules, the SDL renderer, a first frame (texture upload,
// clear, draw, present) and the ImGui backends.
enum class RendererStage { kWindow, kPin, kCreate, kFirstFrame, kBackend };

inline const char* RendererStageName(RendererStage stage) {
    switch (stage) {
    case RendererStage::kWindow: return "window";
    case RendererStage::kPin: return "pin";
    case RendererStage::kCreate: return "create";
    case RendererStage::kFirstFrame: return "first_frame";
    case RendererStage::kBackend: return "imgui_backend";
    }
    return "unknown";
}

inline constexpr size_t kRendererReasonMax = 120;

// One line of printable ASCII: SDL and driver messages can carry paths, control characters or any length.
inline std::string RendererReason(std::string_view text) {
    std::string reason;
    for (const char c : text) {
        if (reason.size() == kRendererReasonMax) {
            break;
        }
        reason += c == '\\' ? '/' : (c >= 0x20 && c < 0x7F ? c : ' ');
    }
    return reason.empty() ? "unknown error" : reason;
}

struct RendererAttempt {
    std::string requested;  // the plan entry; empty is the platform default
    std::string actual;     // the renderer created; empty when the attempt failed
    RendererStage stage = RendererStage::kCreate;
    std::string reason;

    bool ok() const { return !actual.empty(); }
    static RendererAttempt Opened(std::string_view requested, std::string_view actual) {
        RendererAttempt attempt;
        attempt.requested = std::string(requested);
        attempt.actual = actual.empty() ? std::string("unknown") : std::string(actual);
        return attempt;
    }
    static RendererAttempt Failed(std::string_view requested, RendererStage stage, std::string_view reason) {
        RendererAttempt attempt;
        attempt.requested = std::string(requested);
        attempt.stage = stage;
        attempt.reason = RendererReason(reason);
        return attempt;
    }
};

struct RendererSelection {
    bool created = false;
    size_t index = 0;
    std::string actual;
    bool fallback = false;  // an earlier renderer in the plan failed
    int failures = 0;
    std::vector<RendererAttempt> attempts;
};

// Every name in the plan is tried once, in order, and nothing is retried. After a failed attempt `discard` releases
// whatever it opened, before the next one starts.
template <class Open, class Discard>
RendererSelection SelectRenderer(const RendererPlan& plan, Open&& open, Discard&& discard) {
    RendererSelection selection;
    for (size_t i = 0; i < plan.count; ++i) {
        RendererAttempt attempt = open(plan.names[i]);
        attempt.requested = std::string(plan.names[i]);
        selection.attempts.push_back(attempt);
        if (attempt.ok()) {
            selection.created = true;
            selection.index = i;
            selection.actual = attempt.actual;
            selection.fallback = i > 0;
            return selection;
        }
        discard();
        ++selection.failures;
    }
    return selection;
}

// The startup stage while an attempt runs, so a driver that ends the process names the renderer it was creating.
inline const char* RendererAttemptPhase(std::string_view name) {
    if (name == "direct3d11") {
        return "launcher renderer direct3d11";
    }
    if (name == "software") {
        return "launcher renderer software";
    }
    return name.empty() ? "launcher renderer default" : "launcher renderer other";
}

inline std::string RendererLabel(std::string_view name) {
    std::string label;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        label += plain ? c : (c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : '_');
    }
    return label.empty() ? "unknown" : label;
}

inline std::string RendererRequestedLabel(const RendererPlan& plan) {
    return plan.count == 0 || plan.names[0].empty() ? "default" : RendererLabel(plan.names[0]);
}

inline std::string RendererAttemptLabel(std::string_view name) {
    return name.empty() ? "default" : RendererLabel(name);
}

// Which attempt failed first and at what stage: "direct3d11_first_frame"; "none" without a fallback.
inline std::string RendererFallbackReason(const RendererSelection& selection) {
    if (!selection.fallback || selection.attempts.empty() || selection.attempts.front().ok()) {
        return "none";
    }
    const RendererAttempt& first = selection.attempts.front();
    return RendererAttemptLabel(first.requested) + "_" + RendererStageName(first.stage);
}

inline std::string RendererSelectedLine(const RendererPlan& plan, const RendererSelection& selection, int pinned_modules) {
    char line[320];
    std::snprintf(line, sizeof(line),
                  "Launcher telemetry: renderer_selected requested=%s actual=%s class=%s fallback=%d failures=%d "
                  "ignored_request=%d pinned_modules=%d fallback_reason=%s",
                  RendererRequestedLabel(plan).c_str(), RendererLabel(selection.actual).c_str(),
                  RendererKindName(ClassifyRenderer(selection.actual)), selection.fallback ? 1 : 0, selection.failures,
                  plan.ignored_request ? 1 : 0, pinned_modules, RendererFallbackReason(selection).c_str());
    return line;
}

inline std::string RendererAttemptLine(const RendererAttempt& attempt) {
    char line[320];
    std::snprintf(line, sizeof(line), "Launcher telemetry: renderer_attempt requested=%s result=failed stage=%s reason=%s",
                  RendererAttemptLabel(attempt.requested).c_str(), RendererStageName(attempt.stage),
                  RendererReason(attempt.reason).c_str());
    return line;
}

inline std::string RendererFailedLine(const RendererPlan& plan, const RendererSelection& selection) {
    char line[128];
    std::snprintf(line, sizeof(line), "Launcher telemetry: renderer_failed requested=%s tried=%d",
                  RendererRequestedLabel(plan).c_str(), selection.failures);
    return line;
}

// What the player is told when no renderer opens: each renderer tried and why it failed.
inline std::string RendererFailureText(const RendererSelection& selection) {
    std::string text = "The launcher could not start its window renderer.";
    for (const RendererAttempt& attempt : selection.attempts) {
        if (!attempt.ok()) {
            text += "\n" + RendererAttemptLabel(attempt.requested) + " (" + RendererStageName(attempt.stage) +
                    "): " + RendererReason(attempt.reason);
        }
    }
    return text;
}

} // namespace Sotb

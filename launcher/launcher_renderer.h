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

struct RendererSelection {
    bool created = false;
    size_t index = 0;
    std::string actual;
    bool fallback = false;  // an earlier renderer in the plan failed
    int failures = 0;
};

// `create` returns the created renderer's name, or an empty string when it could not be created. Every name in the
// plan is tried once, in order, and nothing is retried.
template <class Create>
RendererSelection SelectRenderer(const RendererPlan& plan, Create&& create) {
    RendererSelection selection;
    for (size_t i = 0; i < plan.count; ++i) {
        std::string actual = create(plan.names[i]);
        if (!actual.empty()) {
            selection.created = true;
            selection.index = i;
            selection.actual = std::move(actual);
            selection.fallback = i > 0;
            return selection;
        }
        ++selection.failures;
    }
    return selection;
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

inline std::string RendererSelectedLine(const RendererPlan& plan, const RendererSelection& selection, int pinned_modules) {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "Launcher telemetry: renderer_selected requested=%s actual=%s class=%s fallback=%d failures=%d "
                  "ignored_request=%d pinned_modules=%d",
                  RendererRequestedLabel(plan).c_str(), RendererLabel(selection.actual).c_str(),
                  RendererKindName(ClassifyRenderer(selection.actual)), selection.fallback ? 1 : 0, selection.failures,
                  plan.ignored_request ? 1 : 0, pinned_modules);
    return line;
}

inline std::string RendererFailedLine(const RendererPlan& plan, const RendererSelection& selection) {
    char line[128];
    std::snprintf(line, sizeof(line), "Launcher telemetry: renderer_failed requested=%s tried=%d",
                  RendererRequestedLabel(plan).c_str(), selection.failures);
    return line;
}

} // namespace Sotb

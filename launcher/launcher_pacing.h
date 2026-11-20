// The launcher loop's frame cap, minimized behavior and loop telemetry. Pure policy on caller-supplied monotonic
// nanoseconds: no window, no clock, no sleeping.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "launcher_renderer.h"

namespace Sotb {

constexpr int kLauncherTargetHz = 60;
constexpr int64_t kNsPerSecond = 1'000'000'000;
constexpr int64_t kNsPerMs = 1'000'000;
constexpr int64_t kMinimizedIdleNs = 50 * kNsPerMs;

struct FrameWait {
    bool draw = false;
    int64_t wait_ns = 0;
};

// Deadline cap with an exact period: the remainder of 1e9 / hz is carried, so the cadence does not drift. A frame that
// is a full period late, or a clock that moves backwards, re-anchors instead of drawing a burst. 0 Hz never waits.
class FramePacer {
public:
    explicit FramePacer(int hz = kLauncherTargetHz)
        : hz_(hz), step_(hz > 0 ? kNsPerSecond / hz : 0), step_remainder_(hz > 0 ? kNsPerSecond % hz : 0) {}

    FrameWait Next(int64_t now_ns) {
        if (hz_ <= 0) {
            return {true, 0};
        }
        if (!scheduled_) {
            scheduled_ = true;
            Rebase(now_ns);
            return {true, 0};
        }
        if (deadline_ - now_ns > step_ + 1) {
            Rebase(now_ns);
            return {false, deadline_ - now_ns};
        }
        if (now_ns < deadline_) {
            return {false, deadline_ - now_ns};
        }
        Advance();
        if (deadline_ <= now_ns) {
            Rebase(now_ns);
        }
        return {true, 0};
    }

    void Reset() { scheduled_ = false; }
    int hz() const { return hz_; }
    int64_t step_ns() const { return step_; }

private:
    void Rebase(int64_t now_ns) {
        deadline_ = now_ns;
        fraction_ = 0;
        Advance();
    }
    void Advance() {
        deadline_ += step_;
        fraction_ += step_remainder_;
        if (fraction_ >= hz_) {
            fraction_ -= hz_;
            ++deadline_;
        }
    }

    int hz_;
    int64_t step_;
    int64_t step_remainder_;
    int64_t deadline_ = 0;
    int64_t fraction_ = 0;
    bool scheduled_ = false;
};

enum class GateAction { kDraw, kSleep, kIdle };

struct GateStep {
    GateAction action = GateAction::kDraw;
    int64_t sleep_ns = 0;
};

// One step of the launcher loop, after its events were handled. Minimized never draws: the caller does its
// background work and sleeps kMinimizedIdleNs. The pacer is reset there, so a restore draws at once and never catches up.
class LoopGate {
public:
    explicit LoopGate(int hz = kLauncherTargetHz) : pacer_(hz) {}

    GateStep Step(int64_t now_ns, bool minimized) {
        if (minimized) {
            pacer_.Reset();
            return {GateAction::kIdle, kMinimizedIdleNs};
        }
        const FrameWait wait = pacer_.Next(now_ns);
        return wait.draw ? GateStep{GateAction::kDraw, 0} : GateStep{GateAction::kSleep, wait.wait_ns};
    }

    const FramePacer& pacer() const { return pacer_; }

private:
    FramePacer pacer_;
};

// Wall-clock time of one launcher frame: building the UI, recording draw commands, and presenting, which also executes
// the recorded commands and may wait for VSync.
struct FrameTimes {
    int64_t build_ns = 0;
    int64_t submit_ns = 0;
    int64_t present_ns = 0;
};

struct LoopSample {
    int64_t frames = 0;
    int64_t elapsed_ns = 0;
    double hz = 0.0;
    FrameTimes total;
    int64_t wait_ns = 0;
    int64_t max_present_ns = 0;
    int64_t slow_presents = 0;
    int cpu_pct = -1;  // the UI thread's own CPU time over the window, -1 when unavailable
    bool first = false;
    bool excessive_rate = false;
    bool present_stall = false;
    bool high_cpu = false;
};

// One warning per incident: it fires once, is re-armed by a window without the condition, and never repeats within
// kRepeatNs of the previous one.
class Alarm {
public:
    static constexpr int64_t kRepeatNs = 60 * kNsPerSecond;

    bool Fire(bool condition, int64_t now_ns) {
        if (!condition) {
            armed_ = true;
            return false;
        }
        if (!armed_ || (fired_ && now_ns - last_ns_ < kRepeatNs)) {
            return false;
        }
        armed_ = false;
        fired_ = true;
        last_ns_ = now_ns;
        return true;
    }

private:
    bool armed_ = true;
    bool fired_ = false;
    int64_t last_ns_ = 0;
};

// Aggregates drawn frames into fixed windows once the launcher has been visible long enough to be stable. Constant
// time and no allocation per frame; the thread CPU time is read only at window boundaries.
class LoopMonitor {
public:
    static constexpr int64_t kWarmupNs = 5 * kNsPerSecond;
    static constexpr int64_t kWindowNs = 5 * kNsPerSecond;
    static constexpr double kExcessiveHz = kLauncherTargetHz * 1.5;
    static constexpr int64_t kSlowPresentNs = 50 * kNsPerMs;
    static constexpr int64_t kHardStallNs = 250 * kNsPerMs;
    static constexpr int64_t kStallMinFrames = 3;
    static constexpr int kHighCpuPct = 60;

    void Waited(int64_t ns) {
        if (windowed_) {
            wait_ += ns;
        }
    }

    // `thread_cpu_ns` is called for the window's start and end only; it returns -1 when the time is unavailable.
    template <class CpuFn>
    std::optional<LoopSample> FrameDrawn(int64_t now_ns, const FrameTimes& times, CpuFn&& thread_cpu_ns) {
        if (!visible_) {
            visible_ = true;
            visible_since_ = now_ns;
        }
        if (!windowed_) {
            if (now_ns - visible_since_ >= kWarmupNs) {
                StartWindow(now_ns, thread_cpu_ns());
            }
            return std::nullopt;
        }
        ++frames_;
        total_.build_ns += times.build_ns;
        total_.submit_ns += times.submit_ns;
        total_.present_ns += times.present_ns;
        max_present_ = std::max(max_present_, times.present_ns);
        slow_presents_ += times.present_ns > kSlowPresentNs ? 1 : 0;
        const int64_t elapsed = now_ns - window_start_;
        if (elapsed < kWindowNs) {
            return std::nullopt;
        }
        const int64_t cpu_now = thread_cpu_ns();
        LoopSample sample;
        sample.frames = frames_;
        sample.elapsed_ns = elapsed;
        sample.hz = double(frames_) * double(kNsPerSecond) / double(elapsed);
        sample.total = total_;
        sample.wait_ns = wait_;
        sample.max_present_ns = max_present_;
        sample.slow_presents = slow_presents_;
        if (cpu_now >= 0 && window_cpu_ >= 0) {
            sample.cpu_pct = int(std::clamp(double(cpu_now - window_cpu_) * 100.0 / double(elapsed), 0.0, 100.0) + 0.5);
        }
        sample.first = !sampled_;
        sampled_ = true;
        const bool stalled = max_present_ >= kHardStallNs || (frames_ >= kStallMinFrames && slow_presents_ * 2 >= frames_);
        sample.excessive_rate = rate_alarm_.Fire(sample.hz > kExcessiveHz, now_ns);
        sample.present_stall = stall_alarm_.Fire(stalled, now_ns);
        sample.high_cpu = cpu_alarm_.Fire(sample.cpu_pct >= kHighCpuPct, now_ns);
        StartWindow(now_ns, cpu_now);
        return sample;
    }

    // The launcher left the visible state: the partial window and the warm-up are discarded.
    void Reset() {
        visible_ = false;
        windowed_ = false;
    }

private:
    void StartWindow(int64_t now_ns, int64_t thread_cpu_ns) {
        windowed_ = true;
        window_start_ = now_ns;
        window_cpu_ = thread_cpu_ns;
        frames_ = 0;
        total_ = {};
        wait_ = 0;
        max_present_ = 0;
        slow_presents_ = 0;
    }

    bool visible_ = false;
    bool windowed_ = false;
    bool sampled_ = false;
    Alarm rate_alarm_;
    Alarm stall_alarm_;
    Alarm cpu_alarm_;
    int64_t visible_since_ = 0;
    int64_t window_start_ = 0;
    int64_t window_cpu_ = -1;
    int64_t frames_ = 0;
    FrameTimes total_;
    int64_t wait_ = 0;
    int64_t max_present_ = 0;
    int64_t slow_presents_ = 0;
};

struct VsyncState {
    bool requested = false;
    bool effective = false;
};

inline std::string PacingModeLine(std::string_view renderer, const VsyncState& vsync) {
    char line[192];
    std::snprintf(line, sizeof(line),
                  "Launcher telemetry: frame_pacing renderer=%s requested_vsync=%d effective_vsync=%d pacing=%s target_hz=%d",
                  RendererLabel(renderer).c_str(), vsync.requested ? 1 : 0, vsync.effective ? 1 : 0,
                  vsync.effective ? "vsync" : "cap", kLauncherTargetHz);
    return line;
}

namespace loop_detail {
inline double AverageMs(int64_t total_ns, int64_t frames) {
    return frames > 0 ? double(total_ns) / double(frames) / double(kNsPerMs) : 0.0;
}
inline int Percent(int64_t part_ns, int64_t whole_ns) {
    return whole_ns > 0 ? int(std::clamp(double(part_ns) * 100.0 / double(whole_ns), 0.0, 100.0)) : 0;
}
inline std::string Body(std::string_view renderer, const LoopSample& s) {
    char body[320];
    std::snprintf(body, sizeof(body),
                  "renderer=%s frames=%lld elapsed_ms=%lld effective_hz=%.1f avg_build_ms=%.1f avg_submit_ms=%.1f "
                  "avg_present_ms=%.1f avg_wait_ms=%.1f present_pct=%d wait_pct=%d",
                  RendererLabel(renderer).c_str(), static_cast<long long>(std::min<int64_t>(s.frames, 999'999'999)),
                  static_cast<long long>(s.elapsed_ns / kNsPerMs), std::clamp(s.hz, 0.0, 99999.0),
                  AverageMs(s.total.build_ns, s.frames), AverageMs(s.total.submit_ns, s.frames),
                  AverageMs(s.total.present_ns, s.frames), AverageMs(s.wait_ns, s.frames),
                  Percent(s.total.present_ns, s.elapsed_ns), Percent(s.wait_ns, s.elapsed_ns));
    std::string text = body;
    if (s.cpu_pct >= 0) {
        text += " ui_thread_cpu_pct=" + std::to_string(s.cpu_pct);
    }
    return text;
}
} // namespace loop_detail

inline std::string LoopSampleLine(std::string_view renderer, const LoopSample& sample) {
    return "Launcher telemetry: loop_sample " + loop_detail::Body(renderer, sample);
}

inline std::string ExcessiveRateLine(std::string_view renderer, const LoopSample& sample) {
    return "Launcher telemetry: event=launcher_frame_pacing_excessive target_hz=" + std::to_string(kLauncherTargetHz) + " " +
           loop_detail::Body(renderer, sample);
}

inline std::string PresentStallLine(std::string_view renderer, const LoopSample& sample) {
    char extra[96];
    std::snprintf(extra, sizeof(extra), " max_present_ms=%.1f slow_presents=%lld threshold_ms=%lld",
                  double(sample.max_present_ns) / double(kNsPerMs), static_cast<long long>(sample.slow_presents),
                  static_cast<long long>(LoopMonitor::kSlowPresentNs / kNsPerMs));
    return "Launcher telemetry: event=launcher_present_stall " + loop_detail::Body(renderer, sample) + extra;
}

inline std::string HighCpuLine(std::string_view renderer, const LoopSample& sample) {
    return "Launcher telemetry: event=launcher_ui_cpu_high threshold_pct=" + std::to_string(LoopMonitor::kHighCpuPct) + " " +
           loop_detail::Body(renderer, sample);
}

} // namespace Sotb

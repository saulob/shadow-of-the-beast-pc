#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace Sotb {

inline constexpr std::chrono::milliseconds BackgroundLoadingDelay{500};

// Remembers when a background load first reported work, so a short load never shows the note.
// Report comes from the loading thread, Shown from the one that draws.
class BackgroundLoadingClock {
public:
    using Rep = std::chrono::steady_clock::rep;

    void Report(std::uint32_t total, Rep now) {
        if (total == 0) {
            started_ = 0;
        } else if (started_ == 0) {
            started_ = now ? now : 1;
        }
        total_ = total;
    }

    bool Shown(Rep now) const {
        const Rep started = started_;
        if (total_ == 0 || started == 0) {
            return false;
        }
        return std::chrono::steady_clock::duration{now - started} >= BackgroundLoadingDelay;
    }

private:
    std::atomic<std::uint32_t> total_{0};
    std::atomic<Rep> started_{0};
};

} // namespace Sotb

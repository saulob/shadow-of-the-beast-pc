// SPDX-License-Identifier: GPL-2.0-or-later
// Keeps a repeating diagnostic line from flooding the log.
#pragma once

#include <cstdint>

namespace SotbLog {

// A log line that is allowed a short burst and then once per interval, counting what it held back.
class LogGate {
public:
    LogGate(unsigned burst, std::uint64_t interval_ns) : burst_(burst), interval_ns_(interval_ns) {}

    bool Allow(std::uint64_t now_ns) {
        if (burst_ != 0) {
            --burst_;
            last_ns_ = now_ns;
            return true;
        }
        if (now_ns >= last_ns_ && now_ns - last_ns_ >= interval_ns_) {
            last_ns_ = now_ns;
            return true;
        }
        ++suppressed_;
        return false;
    }

    unsigned TakeSuppressed() {
        const unsigned count = suppressed_;
        suppressed_ = 0;
        return count;
    }

private:
    unsigned burst_;
    std::uint64_t interval_ns_;
    std::uint64_t last_ns_ = 0;
    unsigned suppressed_ = 0;
};

} // namespace SotbLog

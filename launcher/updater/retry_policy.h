#pragma once

namespace trg::update {

constexpr int kMaxDownloadAttempts = 3;

inline int RetryDelayMs(int failed_attempt) {
  return failed_attempt <= 1 ? 1000 : 2500;
}

inline bool ShouldRetry(int failed_attempt, bool transient, bool cancelled) {
  return transient && !cancelled && failed_attempt < kMaxDownloadAttempts;
}

}  // namespace trg::update

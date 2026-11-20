// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include "common/types.h"

namespace AmdGpu {

/// One graphics submission's command words as they were submitted. The game may write that memory again while
/// the submission is still queued or being read, so the command processor reads this copy.
struct CommandSnapshot {
    std::vector<u32> words; ///< the DCB, then the CCB
    std::span<const u32> dcb;
    std::span<const u32> ccb;
    const u32* guest_dcb{};
};

enum class SnapshotStatus { Ok, TooLarge };

/// Per submission, DCB and CCB together. Real command buffers are far smaller; a larger length is
/// a corrupt or hostile size and must not become an allocation.
constexpr size_t MaxSnapshotWords = size_t{16} << 20;

/// Snapshots kept for reuse are trimmed to this capacity so one huge submission does not pin memory.
constexpr size_t MaxRetainedWords = size_t{1} << 20;

inline SnapshotStatus CheckSnapshotSize(size_t dcb_words, size_t ccb_words) {
    if (dcb_words > MaxSnapshotWords || ccb_words > MaxSnapshotWords - dcb_words) {
        return SnapshotStatus::TooLarge;
    }
    return SnapshotStatus::Ok;
}

/// Snapshot storage reused across submissions: no allocation once the largest submission has been seen.
class CommandSnapshots {
public:
    static constexpr size_t MaxSpare = 8;

    /// Null when the submission is larger than MaxSnapshotWords.
    std::unique_ptr<CommandSnapshot> Take(std::span<const u32> dcb, std::span<const u32> ccb) {
        if (CheckSnapshotSize(dcb.size(), ccb.size()) != SnapshotStatus::Ok) {
            return nullptr;
        }
        std::unique_ptr<CommandSnapshot> snapshot;
        {
            std::scoped_lock lock{mutex};
            if (!spare.empty()) {
                snapshot = std::move(spare.back());
                spare.pop_back();
            }
        }
        if (!snapshot) {
            snapshot = std::make_unique<CommandSnapshot>();
        }
        auto& words = snapshot->words;
        words.clear();
        words.reserve(dcb.size() + ccb.size());
        words.insert(words.end(), dcb.begin(), dcb.end());
        words.insert(words.end(), ccb.begin(), ccb.end());
        snapshot->dcb = std::span<const u32>{words.data(), dcb.size()};
        snapshot->ccb = ccb.empty() ? std::span<const u32>{}
                                    : std::span<const u32>{words.data() + dcb.size(), ccb.size()};
        snapshot->guest_dcb = dcb.data();
        return snapshot;
    }

    void Return(std::unique_ptr<CommandSnapshot> snapshot) {
        if (!snapshot || snapshot->words.capacity() > MaxRetainedWords) {
            return;
        }
        std::scoped_lock lock{mutex};
        if (spare.size() < MaxSpare) {
            spare.push_back(std::move(snapshot));
        }
    }

    size_t SpareCount() {
        std::scoped_lock lock{mutex};
        return spare.size();
    }

private:
    std::mutex mutex;
    std::vector<std::unique_ptr<CommandSnapshot>> spare;
};

/// The guest words at the position `rest` has reached in a snapshot that starts at `snapshot`.
inline std::span<const u32> GuestRest(std::span<const u32> rest, const u32* snapshot, const u32* guest) {
    return {guest + (rest.data() - snapshot), rest.size()};
}

/// The first word where the guest memory no longer holds what was submitted; `submitted.size()` when unchanged.
inline size_t FirstRewrittenWord(std::span<const u32> submitted, const u32* guest) {
    if (submitted.empty() || std::memcmp(submitted.data(), guest, submitted.size_bytes()) == 0) {
        return submitted.size();
    }
    for (size_t i = 0; i < submitted.size(); ++i) {
        if (submitted[i] != guest[i]) {
            return i;
        }
    }
    return submitted.size();
}

} // namespace AmdGpu

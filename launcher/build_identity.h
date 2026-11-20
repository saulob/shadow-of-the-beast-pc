// Compares what is known about a selected game build, before anything runs, with the supported builds, and writes the
// deterministic report a maintainer needs to add support. A comparison never makes a build supported: only an exact match
// of every recorded field does, and a close one is a candidate to study.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game_profile.h"

namespace Sotb {

struct BuildEvidence {
    std::string source; // where the facts come from, for the report
    std::optional<std::string> title_id, content_id, app_ver, sfo_version, eboot_sha256;
    std::optional<uint64_t> file_size, image_extent, entry;
    std::optional<ExecutableRange> code, data;
};

enum class BuildVerdict { kSupported, kUnsupported, kPossibleCandidate };

const char* VerdictName(BuildVerdict verdict);

enum class FieldState { kMatch, kDiffer, kNotRecorded };

struct FieldResult {
    std::string name;
    std::string expected;
    std::string found;
    FieldState state = FieldState::kNotRecorded;
};

struct BuildAssessment {
    BuildVerdict verdict = BuildVerdict::kUnsupported;
    const GameBuild* closest = nullptr; // the known build sharing the most recorded fields
    std::vector<FieldResult> fields;    // the closest build's, in report order
    size_t matched = 0;
    size_t differed = 0;
};

BuildAssessment AssessBuild(const BuildEvidence& evidence, std::span<const GameBuild> builds = SupportedBuilds());

// The delimited-free report lines, identical for identical evidence.
std::vector<std::string> DescribeAssessment(const BuildEvidence& evidence, const BuildAssessment& assessment);

// The facts a SELF image's headers give without decrypting or running it. The first 4 KiB of eboot.bin is enough.
struct ExecutableHeader {
    bool valid = false;
    std::string error;
    uint64_t entry = 0;
    uint64_t image_extent = 0;
    std::optional<ExecutableRange> code, data;
    size_t load_segments = 0;
};

ExecutableHeader ReadExecutableHeader(std::span<const uint8_t> head);

// Fills the executable fields of `evidence` from the header, the file size and the SHA-256.
void AddExecutable(BuildEvidence& evidence, const ExecutableHeader& header, uint64_t file_size, const std::string& sha256);

} // namespace Sotb

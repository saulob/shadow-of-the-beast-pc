#include "build_identity.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace Sotb {
namespace {

constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kFlagExecute = 1, kFlagWrite = 2;
constexpr uint8_t kSelfMagic[] = {'O', 0x15, '=', 0x1d};
constexpr uint8_t kElfIdent[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
constexpr size_t kProgramHeaderSize = 56;
constexpr size_t kMaxProgramHeaders = 256;

std::string Hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::string Decimal(uint64_t value) { return std::to_string(value); }

std::string RangeText(const ExecutableRange& range) { return Hex(range.start) + "+" + Hex(range.size); }

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Shown(const std::string& value) { return value.empty() ? "(missing)" : value; }

template <typename T> bool Read(std::span<const uint8_t> data, uint64_t offset, T& value) {
    if (offset > data.size() || sizeof(T) > data.size() - offset) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return true;
}

FieldResult Compare(const char* name, const std::optional<std::string>& found, const std::string& expected, bool exact_case = true) {
    FieldResult result{name, expected, found ? Shown(*found) : std::string{}, FieldState::kNotRecorded};
    if (found) {
        const bool same = exact_case ? *found == expected : Lower(*found) == Lower(expected);
        result.state = same ? FieldState::kMatch : FieldState::kDiffer;
    }
    return result;
}

FieldResult Compare(const char* name, const std::optional<uint64_t>& found, uint64_t expected, bool decimal) {
    const auto text = [&](uint64_t value) { return decimal ? Decimal(value) : Hex(value); };
    FieldResult result{name, text(expected), found ? text(*found) : std::string{}, FieldState::kNotRecorded};
    if (found) {
        result.state = *found == expected ? FieldState::kMatch : FieldState::kDiffer;
    }
    return result;
}

FieldResult Compare(const char* name, const std::optional<ExecutableRange>& found, const ExecutableRange& expected) {
    FieldResult result{name, RangeText(expected), found ? RangeText(*found) : std::string{}, FieldState::kNotRecorded};
    if (found) {
        result.state = found->start == expected.start && found->size == expected.size ? FieldState::kMatch : FieldState::kDiffer;
    }
    return result;
}

bool ExecutableMatches(const GameBuild& build, const std::optional<std::string>& sha) {
    return sha && IsBuildExecutable(build, Lower(*sha));
}

std::vector<FieldResult> CompareWithBuild(const BuildEvidence& evidence, const GameBuild& build) {
    const PackageLayer& layer = FinishedLayer(build);
    std::vector<FieldResult> fields;
    fields.push_back(Compare("TITLE_ID", evidence.title_id, build.title_id));
    fields.push_back(Compare("CONTENT_ID", evidence.content_id, build.content_id));
    fields.push_back(Compare("APP_VER", evidence.app_ver, layer.app_ver));
    fields.push_back(Compare("VERSION", evidence.sfo_version, layer.sfo_version));
    FieldResult sha = Compare("eboot SHA-256", evidence.eboot_sha256, build.eboot_sha256, false);
    if (evidence.eboot_sha256) {
        sha.state = ExecutableMatches(build, evidence.eboot_sha256) ? FieldState::kMatch : FieldState::kDiffer;
    }
    fields.push_back(std::move(sha));
    fields.push_back(Compare("eboot size", evidence.file_size, build.executable.file_size, true));
    fields.push_back(Compare("entry point", evidence.entry, build.executable.entry, false));
    fields.push_back(Compare("image extent", evidence.image_extent, build.executable.image_extent, true));
    fields.push_back(Compare("code range", evidence.code, build.executable.code));
    fields.push_back(Compare("data range", evidence.data, build.executable.data));
    return fields;
}

size_t Count(const std::vector<FieldResult>& fields, FieldState state) {
    return static_cast<size_t>(std::count_if(fields.begin(), fields.end(), [&](const FieldResult& f) { return f.state == state; }));
}

bool Matches(const std::vector<FieldResult>& fields, const char* name) {
    return std::any_of(fields.begin(), fields.end(), [&](const FieldResult& f) { return f.name == name && f.state == FieldState::kMatch; });
}

const char* StateName(FieldState state) {
    return state == FieldState::kMatch ? "match" : state == FieldState::kDiffer ? "differs" : "not recorded";
}

} // namespace

const char* VerdictName(BuildVerdict verdict) {
    switch (verdict) {
    case BuildVerdict::kSupported:
        return "SUPPORTED";
    case BuildVerdict::kPossibleCandidate:
        return "POSSIBLE CANDIDATE";
    default:
        return "UNSUPPORTED";
    }
}

BuildAssessment AssessBuild(const BuildEvidence& evidence, std::span<const GameBuild> builds) {
    BuildAssessment best;
    size_t best_matched = 0;
    bool found_any = false;
    for (const GameBuild& build : builds) {
        std::vector<FieldResult> fields = CompareWithBuild(evidence, build);
        const size_t matched = Count(fields, FieldState::kMatch);
        if (!found_any || matched > best_matched) {
            found_any = true;
            best_matched = matched;
            best.closest = &build;
            best.fields = std::move(fields);
        }
    }
    if (!best.closest) {
        return best;
    }
    best.matched = Count(best.fields, FieldState::kMatch);
    best.differed = Count(best.fields, FieldState::kDiffer);
    const bool exact = best.differed == 0 && Matches(best.fields, "TITLE_ID") && Matches(best.fields, "eboot SHA-256");
    const bool related = Matches(best.fields, "TITLE_ID") || Matches(best.fields, "CONTENT_ID") || Matches(best.fields, "eboot SHA-256");
    best.verdict = exact ? BuildVerdict::kSupported : related ? BuildVerdict::kPossibleCandidate : BuildVerdict::kUnsupported;
    return best;
}

std::vector<std::string> DescribeAssessment(const BuildEvidence& evidence, const BuildAssessment& assessment) {
    std::vector<std::string> lines;
    lines.push_back(std::string("Build verdict: ") + VerdictName(assessment.verdict) +
                    (assessment.verdict == BuildVerdict::kSupported ? ""
                     : assessment.verdict == BuildVerdict::kPossibleCandidate
                         ? " (a lead for maintainers, never treated as supported)"
                         : " (the game was not started)"));
    lines.push_back("  evidence from: " + (evidence.source.empty() ? std::string("unknown source") : evidence.source));
    lines.push_back("  TITLE_ID " + Shown(evidence.title_id.value_or("")) + ", CONTENT_ID " + Shown(evidence.content_id.value_or("")) +
                    ", APP_VER " + Shown(evidence.app_ver.value_or("")) + ", VERSION " + Shown(evidence.sfo_version.value_or("")));
    if (evidence.file_size || evidence.entry || evidence.image_extent || evidence.eboot_sha256) {
        lines.push_back("  eboot: " + (evidence.file_size ? Decimal(*evidence.file_size) + " bytes" : std::string("size not recorded")) +
                        ", entry " + (evidence.entry ? Hex(*evidence.entry) : std::string("not recorded")) + ", image extent " +
                        (evidence.image_extent ? Hex(*evidence.image_extent) : std::string("not recorded")));
        lines.push_back("  eboot SHA-256 " + Shown(evidence.eboot_sha256.value_or("")));
    }
    if (evidence.code || evidence.data) {
        lines.push_back(std::string("  code range ") + (evidence.code ? RangeText(*evidence.code) : "not recorded") + ", data range " +
                        (evidence.data ? RangeText(*evidence.data) : "not recorded"));
    }
    if (!assessment.closest) {
        lines.push_back("  no supported build to compare with");
        return lines;
    }
    lines.push_back(std::string("Closest supported build: ") + assessment.closest->label + " (region " + assessment.closest->region + "), " +
                    std::to_string(assessment.matched) + " fields match, " + std::to_string(assessment.differed) + " differ, " +
                    std::to_string(assessment.fields.size() - assessment.matched - assessment.differed) + " not recorded");
    for (const FieldResult& field : assessment.fields) {
        std::string line = std::string("  ") + field.name + ": " + StateName(field.state);
        if (field.state == FieldState::kDiffer) {
            line += " (expected " + field.expected + ", found " + field.found + ")";
        } else if (field.state == FieldState::kMatch) {
            line += " (" + field.expected + ")";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

ExecutableHeader ReadExecutableHeader(std::span<const uint8_t> head) {
    ExecutableHeader out;
    const auto fail = [&](const char* error) {
        out.error = error;
        return out;
    };
    if (head.size() < 4 || std::memcmp(head.data(), kSelfMagic, sizeof(kSelfMagic)) != 0) {
        return fail("not a PS4 SELF image");
    }
    uint16_t segment_count = 0;
    if (!Read(head, 4, out.self_version) || !Read(head, 5, out.self_mode) || !Read(head, 6, out.self_endian) ||
        !Read(head, 7, out.self_attributes) || !Read(head, 12, out.self_header_size) || !Read(head, 14, out.self_meta_size) ||
        !Read(head, 16, out.self_file_size) || !Read(head, 24, segment_count)) {
        return fail("truncated SELF header");
    }
    for (uint16_t i = 0; i < segment_count; ++i) {
        SelfEntry entry;
        const uint64_t at = 32 + uint64_t{i} * 32;
        if (!Read(head, at, entry.flags) || !Read(head, at + 8, entry.offset) || !Read(head, at + 16, entry.size) ||
            !Read(head, at + 24, entry.memory)) {
            return fail("SELF segment table outside the bytes read");
        }
        out.self_entries.push_back(entry);
    }
    const uint64_t base = 32 + uint64_t{segment_count} * 32;
    uint16_t entry_size = 0, count = 0;
    uint64_t entry = 0, table = 0;
    if (head.size() < base + 64 || std::memcmp(head.data() + base, kElfIdent, sizeof(kElfIdent)) != 0 ||
        !Read(head, base + 16, out.elf_type) || !Read(head, base + 18, out.machine) || out.machine != 62 ||
        !Read(head, base + 24, entry) || !Read(head, base + 32, table) || !Read(head, base + 54, entry_size) ||
        !Read(head, base + 56, count)) {
        return fail("not a little-endian x86-64 ELF image");
    }
    if (entry_size != kProgramHeaderSize || count == 0 || count >= kMaxProgramHeaders) {
        return fail("unsupported program header table");
    }
    if (table > head.size()) {
        return fail("program headers outside the bytes read");
    }
    out.entry = entry;
    for (uint16_t i = 0; i < count; ++i) {
        const uint64_t at = base + table + uint64_t{i} * kProgramHeaderSize;
        ProgramSegment segment;
        if (!Read(head, at, segment.type) || !Read(head, at + 4, segment.flags) || !Read(head, at + 8, segment.offset) ||
            !Read(head, at + 16, segment.vaddr) || !Read(head, at + 32, segment.filesz) || !Read(head, at + 40, segment.memsz) ||
            !Read(head, at + 48, segment.align)) {
            return fail("program headers outside the bytes read");
        }
        out.segments.push_back(segment);
        const uint32_t type = segment.type, flags = segment.flags;
        const uint64_t vaddr = segment.vaddr, memsz = segment.memsz;
        if (type != kPtLoad) {
            continue;
        }
        ++out.load_segments;
        if (vaddr > ~uint64_t{0} - memsz) {
            return fail("segment range overflows");
        }
        out.image_extent = std::max(out.image_extent, vaddr + memsz);
        if ((flags & kFlagExecute) && !out.code) {
            out.code = ExecutableRange{vaddr, memsz};
        } else if ((flags & kFlagWrite) && !(flags & kFlagExecute) && !out.data) {
            out.data = ExecutableRange{vaddr, memsz};
        }
    }
    out.valid = true;
    return out;
}

void AddExecutable(BuildEvidence& evidence, const ExecutableHeader& header, uint64_t file_size, const std::string& sha256) {
    evidence.file_size = file_size;
    if (!sha256.empty()) {
        evidence.eboot_sha256 = sha256;
    }
    if (header.valid) {
        evidence.entry = header.entry;
        evidence.image_extent = header.image_extent;
        evidence.code = header.code;
        evidence.data = header.data;
    }
}

} // namespace Sotb

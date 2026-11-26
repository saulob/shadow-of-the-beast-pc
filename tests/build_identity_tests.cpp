// Unsupported build diagnostics tests: the supported identity still matches exactly, every altered field leaves it
// unsupported, candidates are labelled and never supported, the report is deterministic, and an unknown build is refused
// before any guest code. Synthetic evidence, SELF headers and folders; the real prepared image is only read, and skipped
// when absent. No window, no game launch, no network.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "build_diagnostics.h"
#include "build_identity.h"
#include "crypto.h"
#include "game_files.h"
#include "sotb_launcher.h"

namespace {

using namespace Sotb;
namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

std::string ReadAll(const std::string& relative) {
    std::ifstream file(std::string(SOTB_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += line + "\n";
    }
    return text;
}

const GameBuild& Supported() {
    return SupportedBuilds().front();
}

BuildEvidence EvidenceFor(const GameBuild& build) {
    const PackageLayer& layer = FinishedLayer(build);
    BuildEvidence evidence;
    evidence.source = "synthetic";
    evidence.title_id = build.title_id;
    evidence.content_id = build.content_id;
    evidence.app_ver = layer.app_ver;
    evidence.sfo_version = layer.sfo_version;
    evidence.eboot_sha256 = build.eboot_sha256;
    evidence.file_size = build.executable.file_size;
    evidence.image_extent = build.executable.image_extent;
    evidence.entry = build.executable.entry;
    evidence.code = build.executable.code;
    evidence.data = build.executable.data;
    return evidence;
}

BuildEvidence SupportedEvidence() {
    return EvidenceFor(Supported());
}

std::string Sha(char digit) {
    return std::string(64, digit);
}

void Put(std::vector<uint8_t>& bytes, size_t at, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

struct SyntheticSegment {
    uint32_t type, flags;
    uint64_t vaddr, memsz;
};

// A SELF with one table entry per program header and the headers of the supported executable's shape.
std::vector<uint8_t> SyntheticSelf(uint64_t entry, const std::vector<SyntheticSegment>& segments) {
    const size_t table = 3;
    const size_t base = 32 + table * 32;
    std::vector<uint8_t> self(base + 64 + segments.size() * 56, 0);
    const uint8_t magic[] = {'O', 0x15, '=', 0x1d};
    std::memcpy(self.data(), magic, 4);
    Put(self, 24, table, 2);
    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    std::memcpy(self.data() + base, ident, sizeof(ident));
    Put(self, base + 18, 62, 2);
    Put(self, base + 24, entry, 8);
    Put(self, base + 32, 64, 8);
    Put(self, base + 54, 56, 2);
    Put(self, base + 56, segments.size(), 2);
    for (size_t i = 0; i < segments.size(); ++i) {
        const size_t at = base + 64 + i * 56;
        Put(self, at, segments[i].type, 4);
        Put(self, at + 4, segments[i].flags, 4);
        Put(self, at + 16, segments[i].vaddr, 8);
        Put(self, at + 40, segments[i].memsz, 8);
    }
    return self;
}

std::vector<SyntheticSegment> SupportedSegments() {
    const ExecutableProfile& exe = Supported().executable;
    return {{1, 5, exe.code.start, exe.code.size},
            {0x61000010, 4, 0x3bec000, 0x294000},
            {1, 6, exe.data.start, exe.data.size},
            {2, 6, 0, 0xdd0}};
}

bool Differs(const BuildAssessment& assessment, const std::string& field) {
    for (const FieldResult& result : assessment.fields) {
        if (result.name == field) {
            return result.state == FieldState::kDiffer;
        }
    }
    return false;
}

void TestSupportedIdentity() {
    const BuildEvidence evidence = SupportedEvidence();
    const BuildAssessment assessment = AssessBuild(evidence);
    CHECK(assessment.verdict == BuildVerdict::kSupported);
    CHECK(assessment.closest == &Supported());
    CHECK(assessment.matched == assessment.fields.size() && assessment.differed == 0);
    CHECK(std::string(VerdictName(assessment.verdict)) == "SUPPORTED");
    CHECK(std::string(Supported().title_id) == "CUSA03762" && std::string(Supported().version) == "1.00");
    CHECK(Supported().executable.entry == 0x19105d0 && Supported().executable.file_size == 75181963);

    // The same facts read from a synthetic SELF with the supported shape.
    BuildEvidence from_self = SupportedEvidence();
    from_self.entry.reset();
    from_self.image_extent.reset();
    from_self.code.reset();
    from_self.data.reset();
    const auto self = SyntheticSelf(Supported().executable.entry, SupportedSegments());
    const ExecutableHeader header = ReadExecutableHeader(self);
    CHECK(header.valid && header.load_segments == 2);
    CHECK(header.entry == Supported().executable.entry && header.image_extent == Supported().executable.image_extent);
    AddExecutable(from_self, header, Supported().executable.file_size, Supported().eboot_sha256);
    CHECK(AssessBuild(from_self).verdict == BuildVerdict::kSupported);

    CHECK(SupportedBuilds().size() == 3);
    CHECK(Has(SupportedBuildsText(), "CUSA03762 1.00 Korea"));
    CHECK(Has(SupportedBuildsText(), "CUSA02871 1.00 USA"));
    CHECK(Has(SupportedBuildsText(), "CUSA00951 1.00 Europe"));
    const std::string profiles_py = ReadAll("scripts/game_profiles.py");
    for (const GameBuild& build : SupportedBuilds()) {
        const BuildEvidence ev = EvidenceFor(build);
        const BuildAssessment assessed = AssessBuild(ev);
        CHECK(assessed.verdict == BuildVerdict::kSupported);
        CHECK(assessed.closest == &build);
        CHECK(assessed.matched == assessed.fields.size() && assessed.differed == 0);
        CHECK(BuildMetadataText(build) == "APP_VER 1.00 \xC2\xB7 VERSION 1.01");
        CHECK(Has(profiles_py, build.title_id) && Has(profiles_py, build.content_id) &&
              Has(profiles_py, build.eboot_sha256) && Has(profiles_py, build.folder));
        const bool expected_circle = std::string(build.title_id) == "CUSA03762";
        CHECK(build.original_circle_confirm == expected_circle);
    }

    // Cross-build executable mismatch (Europe metadata with Korea/USA SHA) is never SUPPORTED.
    const GameBuild* europe = nullptr;
    for (const GameBuild& build : SupportedBuilds()) {
        if (std::string(build.title_id) == "CUSA00951") {
            europe = &build;
        }
    }
    CHECK(europe != nullptr);
    if (europe) {
        BuildEvidence mixed = EvidenceFor(*europe);
        mixed.eboot_sha256 = Supported().eboot_sha256;
        const BuildAssessment mixed_result = AssessBuild(mixed);
        CHECK(mixed_result.verdict == BuildVerdict::kPossibleCandidate);
        CHECK(mixed_result.closest == europe);
    }

    // Evidence that records less is never SUPPORTED without the executable's fingerprint.
    BuildEvidence names_only = SupportedEvidence();
    names_only.eboot_sha256.reset();
    CHECK(AssessBuild(names_only).verdict == BuildVerdict::kPossibleCandidate);
    BuildEvidence fingerprint_only;
    fingerprint_only.eboot_sha256 = Supported().eboot_sha256;
    CHECK(AssessBuild(fingerprint_only).verdict != BuildVerdict::kSupported);
    CHECK(AssessBuild(BuildEvidence{}).verdict == BuildVerdict::kUnsupported);
}

void TestAlteredFields() {
    struct Case {
        const char* field;
        void (*alter)(BuildEvidence&);
    };
    const Case cases[] = {
        {"TITLE_ID", [](BuildEvidence& e) { e.title_id = "CUSA99999"; }},
        {"CONTENT_ID", [](BuildEvidence& e) { e.content_id = "HP9000-CUSA03762_00-SHADOWOFBEAST0EU"; }},
        {"APP_VER", [](BuildEvidence& e) { e.app_ver = "01.01"; }},
        {"VERSION", [](BuildEvidence& e) { e.sfo_version = "01.02"; }},
        {"eboot size", [](BuildEvidence& e) { e.file_size = *e.file_size + 4096; }},
        {"image extent", [](BuildEvidence& e) { e.image_extent = *e.image_extent + 0x1000; }},
        {"entry point", [](BuildEvidence& e) { e.entry = *e.entry + 0x10; }},
        {"eboot SHA-256", [](BuildEvidence& e) { e.eboot_sha256 = Sha('a'); }},
        {"code range", [](BuildEvidence& e) { e.code = ExecutableRange{0, e.code->size + 0x1000}; }},
        {"data range", [](BuildEvidence& e) { e.data = ExecutableRange{e.data->start + 0x10000, e.data->size}; }},
    };
    for (const Case& test : cases) {
        BuildEvidence evidence = SupportedEvidence();
        test.alter(evidence);
        const BuildAssessment assessment = AssessBuild(evidence);
        CHECK(assessment.verdict != BuildVerdict::kSupported);
        CHECK(assessment.verdict == BuildVerdict::kPossibleCandidate);
        CHECK(assessment.differed == 1 && Differs(assessment, test.field));
        CHECK(assessment.matched == assessment.fields.size() - 1);
        const std::string text = Join(DescribeAssessment(evidence, assessment));
        CHECK(Has(text, std::string("  ") + test.field + ": differs (expected "));
        CHECK(Has(text, "Build verdict: POSSIBLE CANDIDATE"));
    }
    // The fingerprint alone differing while everything else matches is the sharpest candidate.
    BuildEvidence rebuilt = SupportedEvidence();
    rebuilt.eboot_sha256 = Sha('b');
    CHECK(AssessBuild(rebuilt).closest == &Supported());
}

void TestCandidateAndUnknown() {
    BuildEvidence update = SupportedEvidence();
    update.app_ver = "01.05";
    update.sfo_version = "01.05";
    update.eboot_sha256 = Sha('c');
    update.file_size = 80000000;
    update.entry = 0x1a00000;
    const BuildAssessment candidate = AssessBuild(update);
    CHECK(candidate.verdict == BuildVerdict::kPossibleCandidate);
    const std::string text = Join(DescribeAssessment(update, candidate));
    CHECK(Has(text, "Build verdict: POSSIBLE CANDIDATE (a lead for maintainers, never treated as supported)"));
    CHECK(Has(text, "Closest supported build: CUSA03762 1.00 Korea (region Korea)"));
    CHECK(Has(text, "TITLE_ID: match (CUSA03762)"));
    CHECK(Has(text, "APP_VER: differs (expected 01.00, found 01.05)"));
    CHECK(Has(text, "entry point: differs (expected 0x19105d0, found 0x1a00000)"));
    CHECK(Has(text, "eboot size: differs (expected 75181963, found 80000000)"));
    CHECK(!Has(text, "Build verdict: SUPPORTED"));

    BuildEvidence unrelated;
    unrelated.source = "synthetic";
    unrelated.title_id = "CUSA00001";
    unrelated.content_id = "XX0000-CUSA00001_00-UNRELATEDGAME0000";
    unrelated.app_ver = "01.00";
    unrelated.sfo_version = "01.00";
    unrelated.eboot_sha256 = Sha('d');
    unrelated.file_size = 1000;
    unrelated.entry = 0x400000;
    const BuildAssessment unknown = AssessBuild(unrelated);
    CHECK(unknown.verdict == BuildVerdict::kUnsupported);
    const std::string unknown_text = Join(DescribeAssessment(unrelated, unknown));
    CHECK(Has(unknown_text, "Build verdict: UNSUPPORTED (the game was not started)"));
    CHECK(!Has(unknown_text, "POSSIBLE CANDIDATE") && !Has(unknown_text, "Build verdict: SUPPORTED"));

    // Only the fingerprint relates it: still a candidate, never supported.
    BuildEvidence same_binary = unrelated;
    same_binary.eboot_sha256 = Supported().eboot_sha256;
    CHECK(AssessBuild(same_binary).verdict == BuildVerdict::kPossibleCandidate);
}

void TestHeaderReader() {
    const auto good = SyntheticSelf(0x1234, SupportedSegments());
    const ExecutableHeader header = ReadExecutableHeader(good);
    CHECK(header.valid && header.entry == 0x1234);
    CHECK(header.code && header.code->start == 0 && header.code->size == Supported().executable.code.size);
    CHECK(header.data && header.data->start == Supported().executable.data.start);

    CHECK(!ReadExecutableHeader({}).valid);
    std::vector<uint8_t> bad_magic = good;
    bad_magic[0] ^= 0xff;
    CHECK(!ReadExecutableHeader(bad_magic).valid);
    std::vector<uint8_t> bad_machine = good;
    bad_machine[32 + 3 * 32 + 18] = 0;
    CHECK(!ReadExecutableHeader(bad_machine).valid);
    for (size_t cut : {size_t{3}, size_t{24}, size_t{100}, good.size() - 20}) {
        CHECK(!ReadExecutableHeader({good.data(), cut}).valid);
    }
    std::vector<uint8_t> bad_count = good;
    Put(bad_count, 32 + 3 * 32 + 56, 0, 2);
    CHECK(!ReadExecutableHeader(bad_count).valid);
    const auto overflow = SyntheticSelf(1, {{1, 5, ~uint64_t{0} - 4, 100}});
    const ExecutableHeader wrapped = ReadExecutableHeader(overflow);
    CHECK(!wrapped.valid && Has(wrapped.error, "overflows"));
}

void TestDeterministicOutput() {
    BuildEvidence evidence = SupportedEvidence();
    evidence.entry = 0x1111;
    evidence.eboot_sha256 = Sha('e');
    const std::string first = Join(DescribeAssessment(evidence, AssessBuild(evidence)));
    for (int i = 0; i < 3; ++i) {
        CHECK(Join(DescribeAssessment(evidence, AssessBuild(evidence))) == first);
    }
    CHECK(first.size() < 2500);
    CHECK(std::count(first.begin(), first.end(), '\n') < 30);
    // Same facts from a different path: the same report.
    BuildEvidence copy = evidence;
    CHECK(Join(DescribeAssessment(copy, AssessBuild(copy))) == first);
    CHECK(!Has(first, "(missing)"));
}

void TestNoForeignIdentifiers() {
    BuildEvidence evidence = SupportedEvidence();
    evidence.title_id = "CUSA77777";
    evidence.eboot_sha256 = Sha('f');
    std::string text = Join(DescribeAssessment(evidence, AssessBuild(evidence)));
    for (const std::string& source :
         {ReadAll("launcher/build_identity.h"), ReadAll("launcher/build_identity.cpp"), ReadAll("launcher/build_diagnostics.cpp")}) {
        text += source;
    }
    for (const char* other : {"Odin", "odin", "Leifthrasir", "Dragon", "Dragon's Crown", "Alienation", "13 Sentinels", "Sentinels", "Aegis", "osl_",
                              "OSL_", "dcp", "Vanillaware"}) {
        CHECK(!Has(text, other));
    }
}

std::string SfoWith(const std::vector<std::pair<std::string, std::string>>& values) {
    std::string keys, data;
    std::vector<std::vector<uint8_t>> entries;
    auto le = [](uint64_t value, size_t size) {
        std::string out;
        for (size_t i = 0; i < size; ++i) {
            out += static_cast<char>(value >> (8 * i));
        }
        return out;
    };
    std::string table;
    for (const auto& [key, value] : values) {
        table += le(keys.size(), 2) + le(0x0204, 2) + le(value.size() + 1, 4) + le(value.size() + 1, 4) + le(data.size(), 4);
        keys += key + '\0';
        data += value + '\0';
    }
    const uint32_t key_offset = 20 + static_cast<uint32_t>(values.size()) * 16;
    std::string sfo = std::string("\0PSF", 4) + le(0x101, 4) + le(key_offset, 4) + le(key_offset + keys.size(), 4) + le(values.size(), 4);
    return sfo + table + keys + data;
}

struct TempFolder {
    fs::path path;
    TempFolder() {
        path = fs::temp_directory_path() / ("sotb-build-identity-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void Write(const std::string& name, const std::string& bytes) const {
        fs::create_directories((path / name).parent_path());
        std::ofstream(path / name, std::ios::binary) << bytes;
    }
};

void TestFolderDiagnostics() {
    TempFolder folder;
    const auto self = SyntheticSelf(0x19105d1, SupportedSegments());
    folder.Write("eboot.bin", std::string(self.begin(), self.end()));
    const GameBuild& build = Supported();
    const std::string sfo = SfoWith({{"TITLE", "Shadow of the Beast"},
                                     {"TITLE_ID", build.title_id},
                                     {"CONTENT_ID", build.content_id},
                                     {"APP_VER", "01.02"},
                                     {"VERSION", FinishedLayer(build).sfo_version},
                                     {"CATEGORY", "gd"}});
    const auto source = OpenFolderSource(folder.path);
    const UnsupportedReport first = DiagnoseUnsupportedBuild(*source, sfo);
    CHECK(first.recognized);
    const std::string text = Join(first.lines);
    CHECK(Has(text, kDiagnosticsBegin) && Has(text, kDiagnosticsEnd));
    CHECK(Has(text, "Build verdict: POSSIBLE CANDIDATE"));
    CHECK(Has(text, "APP_VER: differs (expected 01.00, found 01.02)"));
    CHECK(Has(text, "entry point: differs (expected 0x19105d0, found 0x19105d1)"));
    CHECK(Has(text, "TITLE_ID: match (CUSA03762)"));
    CHECK(Has(text, "eboot SHA-256: differs"));
    CHECK(!Has(text, "Build verdict: SUPPORTED"));
    CHECK(first.lines.size() <= diagnostics::kMaxReportLines);
    CHECK(Join(DiagnoseUnsupportedBuild(*source, sfo).lines) == text);

    const std::string stranger = SfoWith({{"TITLE", "Another Game"}, {"TITLE_ID", "CUSA00001"}});
    CHECK(!DiagnoseUnsupportedBuild(*source, stranger).recognized);
}

void TestImageReport() {
    TempFolder folder;
    const GameBuild& build = Supported();
    const auto image_json = [&](const std::string& sha, const std::string& entry, uint64_t extent) {
        return std::string("{\n  \"title_id\": \"") + build.title_id + "\",\n  \"app_ver\": \"" + FinishedLayer(build).app_ver +
               "\",\n  \"eboot_sha256\": \"" + sha + "\",\n  \"entry\": \"" + entry + "\",\n  \"image_bytes\": 72139772,\n  \"eboot_image_bytes\": " +
               std::to_string(extent) + "\n}\n";
    };
    folder.Write("exact/image.json", image_json(build.eboot_sha256, "0x19105d0", build.executable.image_extent));
    SotbGameBuild found{};
    CHECK(sotb_game_build((folder.path / "exact").string().c_str(), &found) == 1);

    folder.Write("altered/image.json", image_json(Sha('1'), "0x19105d0", build.executable.image_extent));
    CHECK(sotb_game_build((folder.path / "altered").string().c_str(), &found) == 0);
    std::vector<std::string> lines;
    static std::vector<std::string>* sink;
    sink = &lines;
    sotb_game_build_report((folder.path / "altered").string().c_str(), [](const char* line) { sink->push_back(line); });
    const std::string text = Join(lines);
    CHECK(Has(text, "Build verdict: POSSIBLE CANDIDATE"));
    CHECK(Has(text, "eboot SHA-256: differs"));
    CHECK(Has(text, "entry point: match (0x19105d0)"));
    CHECK(Has(text, "image extent: match (69601336)"));

    lines.clear();
    sotb_game_build_report((folder.path / "exact").string().c_str(), [](const char* line) { sink->push_back(line); });
    CHECK(Has(Join(lines), "Build verdict: SUPPORTED"));

    for (const GameBuild& build : SupportedBuilds()) {
        const std::string sub = std::string("exact-") + build.title_id;
        folder.Write(sub + "/image.json",
                     std::string("{\n  \"title_id\": \"") + build.title_id + "\",\n  \"app_ver\": \"" +
                         FinishedLayer(build).app_ver + "\",\n  \"eboot_sha256\": \"" + build.eboot_sha256 +
                         "\",\n  \"entry\": \"0x19105d0\",\n  \"image_bytes\": 72139772,\n  \"eboot_image_bytes\": " +
                         std::to_string(build.executable.image_extent) + "\n}\n");
        SotbGameBuild build_info{};
        CHECK(sotb_game_build((folder.path / sub).string().c_str(), &build_info) == 1);
        CHECK(std::string(build_info.title_id) == build.title_id);
        CHECK(std::string(build_info.code_key) == build.code_build);
        CHECK(build_info.original_circle_confirm == (build.original_circle_confirm ? 1 : 0));
    }

    // Multi-build active routing: each build has its own folder and the most recently prepared build wins.
    {
        const GameRoots roots{folder.path / "prepared", folder.path / "image"};
        const GameBuild& kr = SupportedBuilds()[0];
        const GameBuild& us = SupportedBuilds()[1];
        const GameBuild& eu = SupportedBuilds()[2];
        const auto mark = [&](const GameBuild& b, int seconds_offset) {
            const fs::path marker = BuildPaths(roots, b).app0 / kPreparedMarker;
            fs::create_directories(marker.parent_path());
            std::ofstream(marker, std::ios::binary) << "{\"version\": \"1.00\", \"scope\": \"full\"}\n";
            fs::last_write_time(marker, fs::file_time_type::clock::now() + std::chrono::seconds(seconds_offset));
        };
        mark(kr, -20);
        mark(us, -10);
        CHECK(LastPreparedBuild(roots) == &us);
        CHECK(ActiveBuildPaths(roots).app0 == BuildPaths(roots, us).app0);
        mark(eu, 0);
        CHECK(LastPreparedBuild(roots) == &eu);
        CHECK(PreparedBuilds(roots).size() == 3);
        mark(kr, 10);
        CHECK(LastPreparedBuild(roots) == &kr);
    }

    char problem[512] = "";
    sotb_game_build_problem((folder.path / "altered").string().c_str(), problem, sizeof(problem));
    CHECK(Has(problem, "(POSSIBLE CANDIDATE)") && Has(problem, "is not the executable of " + SupportedBuildsText()));
    lines.clear();
    sotb_game_build_report((folder.path / "missing").string().c_str(), [](const char* line) { sink->push_back(line); });
    CHECK(Has(Join(lines), "Build verdict: UNSUPPORTED"));
}

void TestRefusedBeforeGuest() {
    const std::string loader = ReadAll("src/loader.c");
    const size_t refusal = loader.find("unsupported build: %s");
    CHECK(refusal != std::string::npos);
    const size_t block = loader.rfind("if (!known_build) {", refusal);
    CHECK(block != std::string::npos && block < refusal);
    CHECK(Has(loader.substr(refusal, 200), "fail(message);"));
    CHECK(Has(loader.substr(block, refusal - block), "sotb_game_build_report(paths.image_dir, print_build_line);"));
    for (const char* later : {"fopen(content_profile", "sotbgpu_init(&gpu)", "import_traps_build(traps", "Entering original x86-64 code",
                              "lzcnt_compat_install(", "((Entry)(image + entry))"}) {
        const size_t at = loader.find(later);
        CHECK(at != std::string::npos && at > refusal);
    }
    // The diagnostics themselves only read files.
    const std::string diagnostics = ReadAll("launcher/build_diagnostics.cpp") + ReadAll("launcher/build_identity.cpp");
    for (const char* writes : {"ofstream", "fopen(", "CreateProcess", "system(", "remove(", "WriteFile", "LoadLibrary"}) {
        CHECK(!Has(diagnostics, writes));
    }
}

void TestRealImage() {
    size_t checked = 0;
    for (const GameBuild& build : SupportedBuilds()) {
        const fs::path eboot = fs::path(SOTB_SOURCE_DIR) / "game" / "prepared" / build.folder / "eboot.bin";
        std::error_code ec;
        if (!fs::is_regular_file(eboot, ec)) {
            continue;
        }
        ++checked;
        std::ifstream in(eboot, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        uint8_t digest[32];
        CHECK(crypto::Sha256(bytes.data(), bytes.size(), digest));
        static const char digits[] = "0123456789abcdef";
        std::string sha;
        for (const uint8_t byte : digest) {
            sha += digits[byte >> 4];
            sha += digits[byte & 15];
        }
        BuildEvidence evidence = EvidenceFor(build);
        evidence.source = "real eboot.bin";
        evidence.entry.reset();
        evidence.image_extent.reset();
        evidence.code.reset();
        evidence.data.reset();
        const ExecutableHeader header = ReadExecutableHeader({bytes.data(), std::min<size_t>(bytes.size(), 16 * 1024)});
        CHECK(header.valid);
        AddExecutable(evidence, header, bytes.size(), sha);
        const BuildAssessment assessment = AssessBuild(evidence);
        CHECK(assessment.verdict == BuildVerdict::kSupported);
        CHECK(assessment.closest == &build);
        CHECK(assessment.matched == assessment.fields.size());
        std::printf("real image: %s %s, %zu fields match (entry 0x%llx, %zu bytes)\n", build.folder,
                    VerdictName(assessment.verdict), assessment.matched, static_cast<unsigned long long>(header.entry),
                    bytes.size());
    }
    if (checked == 0) {
        std::printf("real image: SKIPPED (no supported build is prepared)\n");
    }
}

} // namespace

int main() {
    TestSupportedIdentity();
    TestAlteredFields();
    TestCandidateAndUnknown();
    TestHeaderReader();
    TestDeterministicOutput();
    TestNoForeignIdentifiers();
    TestFolderDiagnostics();
    TestImageReport();
    TestRefusedBeforeGuest();
    TestRealImage();
    std::printf("build identity tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

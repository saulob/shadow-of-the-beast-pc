// Unsupported-build diagnostics: source classification, package metadata, base and update packages, the executable's
// SELF/ELF layout and the launcher's own image builder, the loader's patch checks run read-only, the exploratory site
// search, the bounded log block and install safety. Synthetic sources in memory and temporary folders; the prepared
// supported builds under game/prepared are only read, and skipped when absent. No window, no game launch, no network.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "guest_patches.h"
#include "isa_emulate.h"
}
#include "boot_image.h"
#include "build_diagnostics.h"
#include "crypto.h"
#include "game_files.h"
#include "game_install.h"
#include "guest_display.h"
#include "guest_hud.h"
#include "guest_ultrawide.h"
#include "incident_report.h"
#include "log_scrubber.h"
#include "lzcnt_compat.h"
#include "psn_notice.h"
#include "skip_intro.h"
#include "sotb_exit_game.h"
#include "sotb_menu_buttons.h"
#include "test_progress.h"

namespace {

using namespace Sotb;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

long long g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        sotb_test_progress(g_checks);                                                                          \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

const fs::path kRoot = SOTB_SOURCE_DIR;

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

std::string Join(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += line + "\n";
    }
    return text;
}

void Expect(const std::string& line, const std::string& expected) {
    CHECK(Has(line, expected));
    if (!Has(line, expected)) {
        std::printf("  expected '%s' in '%s'\n", expected.c_str(), line.c_str());
    }
}

std::string LineWith(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& line : lines) {
        if (Has(line, needle)) {
            return line;
        }
    }
    return {};
}

std::string Sha(const Bytes& data) {
    uint8_t digest[32];
    crypto::Sha256(data.data(), data.size(), digest);
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (const uint8_t byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

void Put(Bytes& bytes, size_t at, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void Append(Bytes& bytes, uint64_t value, size_t size) {
    bytes.resize(bytes.size() + size);
    Put(bytes, bytes.size() - size, value, size);
}

Bytes Text(const std::string& text) { return Bytes(text.begin(), text.end()); }

std::string SfoWith(const std::vector<std::pair<std::string, std::string>>& values) {
    std::string keys, data, table;
    auto le = [](uint64_t value, size_t size) {
        std::string out;
        for (size_t i = 0; i < size; ++i) {
            out += static_cast<char>(value >> (8 * i));
        }
        return out;
    };
    for (const auto& [key, value] : values) {
        table += le(keys.size(), 2) + le(0x0204, 2) + le(value.size() + 1, 4) + le(value.size() + 1, 4) + le(data.size(), 4);
        keys += key + '\0';
        data += value + '\0';
    }
    const uint32_t key_offset = 20 + static_cast<uint32_t>(values.size()) * 16;
    const std::string sfo = std::string("\0PSF", 4) + le(0x101, 4) + le(key_offset, 4) + le(key_offset + keys.size(), 4) + le(values.size(), 4);
    return sfo + table + keys + data;
}

// Synthetic executables in the PS4 SELF layout the launcher's image builder reads

struct Segment {
    uint32_t type, flags;
    uint64_t vaddr;
    Bytes data;
    uint64_t memsz;
    uint64_t align = 0x4000;
};

struct SelfSpec {
    uint64_t entry = 0x100;
    std::vector<Segment> segments;
    std::vector<std::pair<uint64_t, uint64_t>> relative; // R_X86_64_RELATIVE: target, addend
};

Bytes MakeSelf(const SelfSpec& spec) {
    std::vector<Segment> segments = spec.segments;
    Bytes blob(32, 0); // empty string table, then the null symbol
    const uint64_t rela = blob.size();
    for (const auto& [target, addend] : spec.relative) {
        Append(blob, target, 8);
        Append(blob, 8, 8);
        Append(blob, addend, 8);
    }
    const uint64_t rela_size = blob.size() - rela;
    Bytes dynamic;
    for (const auto& [tag, value] : std::vector<std::pair<uint64_t, uint64_t>>{{0x61000035, 0},
                                                                              {0x61000037, 1},
                                                                              {0x61000039, 8},
                                                                              {0x6100003f, 24},
                                                                              {0x61000029, rela},
                                                                              {0x6100002d, rela_size},
                                                                              {0x6100002f, blob.size()},
                                                                              {0x61000031, 0},
                                                                              {0, 0}}) {
        Append(dynamic, tag, 8);
        Append(dynamic, value, 8);
    }
    segments.push_back({2, 4, 0, dynamic, 0, 8});
    segments.push_back({0x61000000, 4, 0, blob, 0, 16});

    const size_t phnum = segments.size();
    const size_t header_end = 64 + phnum * 56;
    const size_t entries = phnum;
    const size_t base = 32 + entries * 32;
    Bytes self(base + header_end, 0);
    const uint8_t magic[] = {'O', 0x15, '=', 0x1d};
    std::memcpy(self.data(), magic, 4);
    self[4] = 0;
    self[5] = 1;
    self[6] = 1;
    self[7] = 0x12;
    Put(self, 12, base, 2);
    Put(self, 24, entries, 2);
    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    std::memcpy(self.data() + base, ident, sizeof(ident));
    Put(self, base + 16, 0xfe10, 2);
    Put(self, base + 18, 62, 2);
    Put(self, base + 24, spec.entry, 8);
    Put(self, base + 32, 64, 8);
    Put(self, base + 52, 64, 2);
    Put(self, base + 54, 56, 2);
    Put(self, base + 56, phnum, 2);
    uint64_t elf_offset = (header_end + 15) & ~uint64_t(15);
    for (size_t i = 0; i < phnum; ++i) {
        const Segment& s = segments[i];
        const size_t at = base + 64 + i * 56;
        Put(self, at, s.type, 4);
        Put(self, at + 4, s.flags, 4);
        Put(self, at + 8, elf_offset, 8);
        Put(self, at + 16, s.vaddr, 8);
        Put(self, at + 24, s.vaddr, 8);
        Put(self, at + 32, s.data.size(), 8);
        Put(self, at + 40, s.memsz ? s.memsz : s.data.size(), 8);
        Put(self, at + 48, s.align, 8);
        elf_offset += (s.data.size() + 15) & ~uint64_t(15);
    }
    for (size_t i = 0; i < phnum; ++i) {
        const Segment& s = segments[i];
        while (self.size() % 16) {
            self.push_back(0);
        }
        Put(self, 32 + i * 32, 0x800 | (uint64_t(i) << 20), 8);
        Put(self, 32 + i * 32 + 8, self.size(), 8);
        Put(self, 32 + i * 32 + 16, s.data.size(), 8);
        Put(self, 32 + i * 32 + 24, s.data.size(), 8);
        self.insert(self.end(), s.data.begin(), s.data.end());
    }
    Put(self, 16, self.size(), 8);
    return self;
}

SelfSpec SmallSpec(uint64_t code_size = 0x2000, uint64_t entry = 0x100) {
    SelfSpec spec;
    spec.entry = entry;
    Bytes code(code_size, 0xcc);
    spec.segments.push_back({1, 5, 0, code, code_size});
    spec.segments.push_back({1, 6, (code_size + 0x3fff) & ~uint64_t(0x3fff), Bytes(0x100, 0), 0x1000});
    spec.relative.push_back({spec.segments[1].vaddr, 0x40});
    return spec;
}

Bytes SmallEboot() { return MakeSelf(SmallSpec()); }
Bytes SmallModule() { return MakeSelf(SmallSpec(0x1000, 0x10)); }

// A UE4 pak: a body, an index (mount point and entry count) and the 44-byte footer.
Bytes MakePak(size_t body = 256, uint32_t entries = 3) {
    Bytes pak(body, 0x11);
    const uint64_t index_offset = pak.size();
    Append(pak, 10, 4);
    for (const char c : std::string("../../../")) {
        pak.push_back(uint8_t(c));
    }
    pak.push_back(0);
    Append(pak, entries, 4);
    pak.resize(pak.size() + 16, 0);
    const uint64_t index_size = pak.size() - index_offset;
    Append(pak, 0x5A6F12E1, 4);
    Append(pak, 3, 4);
    Append(pak, index_offset, 8);
    Append(pak, index_size, 8);
    pak.resize(pak.size() + 20, 0xab);
    return pak;
}

// Sources

// The package-shaped source of the tests: files in memory, with every read recorded.
class MemorySource final : public GameSource {
public:
    std::string kind = "package";
    std::map<std::string, Bytes> files;
    mutable std::map<std::string, uint64_t> whole_reads, range_bytes;

    std::string Kind() const override { return kind; }
    std::vector<SourceEntry> Files(size_t limit) const override {
        std::vector<SourceEntry> out;
        for (const auto& [path, data] : files) {
            if (out.size() >= limit) {
                break;
            }
            out.push_back({path, data.size()});
        }
        return out;
    }
    bool Read(const std::string& path, uint64_t max_bytes, Bytes& out) const override {
        const auto it = files.find(path);
        if (it == files.end() || it->second.size() > max_bytes) {
            return false;
        }
        whole_reads[path] += it->second.size();
        out = it->second;
        return true;
    }
    bool ReadRange(const std::string& path, uint64_t offset, size_t length, Bytes& out) const override {
        const auto it = files.find(path);
        if (it == files.end() || offset > it->second.size() || length > it->second.size() - offset) {
            return false;
        }
        range_bytes[path] += length;
        out.assign(it->second.begin() + std::ptrdiff_t(offset), it->second.begin() + std::ptrdiff_t(offset + length));
        return true;
    }
    std::string Sfo() const {
        const auto it = files.find("sce_sys/param.sfo");
        return it == files.end() ? std::string() : std::string(it->second.begin(), it->second.end());
    }
};

const GameBuild& Korea() { return SupportedBuilds().front(); }

std::vector<std::pair<std::string, std::string>> Metadata(const std::string& title_id, const std::string& content_id,
                                                          const std::string& app_ver, const std::string& category,
                                                          const std::string& title = "Shadow of the Beast") {
    return {{"TITLE", title},     {"TITLE_ID", title_id},     {"CONTENT_ID", content_id}, {"APP_VER", app_ver},
            {"VERSION", "01.01"}, {"CATEGORY", category}, {"ATTRIBUTE", "x"}};
}

void SetSfo(MemorySource& source, const std::string& sfo) { source.files["sce_sys/param.sfo"] = Text(sfo); }

// A complete base game of the supported layout with synthetic executables and archives.
MemorySource ShadowSource(const std::string& app_ver = "01.02", Bytes eboot = SmallEboot()) {
    MemorySource source;
    SetSfo(source, SfoWith(Metadata(Korea().title_id, Korea().content_id, app_ver, "gd")));
    source.files["eboot.bin"] = std::move(eboot);
    source.files["sce_module/libc.prx"] = SmallModule();
    source.files["sce_module/libSceFios2.prx"] = SmallModule();
    source.files["sce_module/libSceFace.prx"] = Bytes(64, 1);
    source.files["sce_discmap.plt"] = Bytes(32, 2);
    source.files["ue4commandline.txt"] = Text("-BuildID=synthetic -SKU=KR");
    source.files[Korea().regional_archive] = MakePak();
    for (const char* name : Korea().extra_files) {
        source.files[name] = MakePak(128, 2);
    }
    return source;
}

struct TempFolder {
    fs::path path;
    explicit TempFolder(const char* name) {
        path = fs::temp_directory_path() / (std::string("sotb-build-diagnostics-") + name + "-" +
                                            std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void Write(const fs::path& folder, const MemorySource& source) const {
        for (const auto& [name, data] : source.files) {
            fs::create_directories((folder / name).parent_path());
            std::ofstream(folder / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        }
    }
};

// Every file under `folder` with its hash, to prove nothing changed.
std::map<std::string, std::string> Snapshot(const fs::path& folder) {
    std::map<std::string, std::string> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().lexically_relative(folder).generic_string();
        if (it->is_directory()) {
            files[name] = "dir";
            continue;
        }
        std::ifstream in(it->path(), std::ios::binary);
        files[name] = Sha(Bytes(std::istreambuf_iterator<char>(in), {}));
    }
    return files;
}

UnsupportedReport Diagnose(const MemorySource& source) { return DiagnoseUnsupportedBuild(source, source.Sfo()); }


void TestClassification() {
    MemorySource unsupported = ShadowSource();
    CHECK(ClassifySource(unsupported, unsupported.Sfo()).kind == SourceClass::kUnsupported);

    MemorySource unknown_title = ShadowSource();
    SetSfo(unknown_title, SfoWith(Metadata("CUSA99999", "XP0000-CUSA99999_00-SOMEOTHERNAME000", "01.00", "gd")));
    const SourceClassification by_title = ClassifySource(unknown_title, unknown_title.Sfo());
    CHECK(by_title.kind == SourceClass::kUnsupported);
    CHECK(Has(Join(by_title.evidence), "TITLE names Shadow of the Beast"));
    CHECK(Has(Join(by_title.evidence), "with a valid UE4 pak footer"));

    MemorySource archive_only = ShadowSource();
    SetSfo(archive_only, SfoWith(Metadata("CUSA99999", "XP0000-CUSA99999_00-SOMEOTHERNAME000", "01.00", "gd", "Untitled")));
    CHECK(ClassifySource(archive_only, archive_only.Sfo()).Shadow());

    MemorySource other;
    SetSfo(other, SfoWith(Metadata("CUSA00001", "UP0000-CUSA00001_00-ANOTHERGAME00000", "01.00", "gd", "Another Game")));
    other.files["eboot.bin"] = SmallEboot();
    other.files["Game/Content/Paks/pakchunk0-ps4.pak"] = MakePak();
    const UnsupportedReport other_report = Diagnose(other);
    CHECK(other_report.classification.kind == SourceClass::kOtherGame);
    CHECK(!other_report.recognized && other_report.lines.empty());
    CHECK(other.whole_reads.empty() && other.range_bytes.empty()); // no Shadow scan of a different game

    MemorySource incomplete = ShadowSource();
    incomplete.files.erase(Korea().regional_archive);
    const UnsupportedReport incomplete_report = Diagnose(incomplete);
    CHECK(incomplete_report.classification.kind == SourceClass::kIncomplete);
    CHECK(incomplete_report.recognized);
    CHECK(Has(Join(incomplete_report.lines), std::string(Korea().regional_archive) + ": absent (missing from a base game)"));

    MemorySource malformed;
    malformed.files["sce_sys/param.sfo"] = Text("not a param.sfo at all");
    malformed.files["eboot.bin"] = Bytes(100, 0);
    CHECK(ClassifySource(malformed, malformed.Sfo()).kind == SourceClass::kInvalid);
    CHECK(!Diagnose(malformed).recognized);

    MemorySource empty;
    CHECK(ClassifySource(empty, "").kind == SourceClass::kInvalid);
}

void TestMetadata() {
    MemorySource known_version = ShadowSource("01.07");
    const std::vector<std::string> lines = Diagnose(known_version).lines;
    const std::string text = Join(lines);
    CHECK(Has(text, "  TITLE_ID: CUSA03762"));
    CHECK(Has(text, "  APP_VER: 01.07"));
    CHECK(Has(text, "  CATEGORY: gd"));
    CHECK(Has(text, "  region (CONTENT_ID prefix): Asia"));
    CHECK(Has(text, "APP_VER: differs (expected 01.00, found 01.07)"));
    CHECK(Has(text, "supported build of this TITLE_ID: CUSA03762 1.00 Korea, APP_VER 01.00, VERSION 01.01, eboot SHA-256 " +
                        std::string(Korea().eboot_sha256)));
    CHECK(Has(text, "other fields: ATTRIBUTE=x"));
    CHECK(!Has(text, "Build verdict: SUPPORTED"));

    MemorySource fresh = ShadowSource();
    SetSfo(fresh, SfoWith(Metadata("CUSA77777", "EP9000-CUSA77777_00-SHADOWOFBEAST0XX", "02.00", "gd")));
    const std::string fresh_text = Join(Diagnose(fresh).lines);
    CHECK(Has(fresh_text, "TITLE_ID CUSA77777 is not a supported title"));
    CHECK(Has(fresh_text, "CONTENT_ID names SHADOWOFBEAST"));
    CHECK(Has(fresh_text, "region (CONTENT_ID prefix): Europe"));

    MemorySource sparse = ShadowSource();
    SetSfo(sparse, SfoWith({{"TITLE", "Shadow of the Beast"}, {"TITLE_ID", Korea().title_id}}));
    const std::string sparse_text = Join(Diagnose(sparse).lines);
    CHECK(Has(sparse_text, "  APP_VER: (missing)"));
    CHECK(Has(sparse_text, "  Identity: incomplete"));
    CHECK(Has(sparse_text, "package type: unknown"));

    MemorySource broken = ShadowSource();
    broken.files["sce_sys/param.sfo"] = Text("\x01PSFbroken");
    const UnsupportedReport broken_report = Diagnose(broken);
    CHECK(broken_report.recognized); // the archives still identify the game
    CHECK(Has(Join(broken_report.lines), "not a PSF file"));
    CHECK(Has(Join(broken_report.lines), "  Identity: incomplete"));
}

void TestBaseAndUpdate() {
    MemorySource update;
    SetSfo(update, SfoWith(Metadata(Korea().title_id, Korea().content_id, "01.01", "gp")));
    update.files["eboot.bin"] = SmallEboot();
    const UnsupportedReport report = Diagnose(update);
    CHECK(report.classification.kind == SourceClass::kUpdate);
    CHECK(report.recognized);
    const std::string text = Join(report.lines);
    CHECK(Has(text, "package type: update (CATEGORY gp)"));
    CHECK(Has(text, "sce_module/libc.prx: absent (base package only; legitimate in an update)"));
    CHECK(Has(text, std::string(Korea().regional_archive) + ": absent (base package only; legitimate in an update)"));
    CHECK(Has(text, "not evaluable from this update"));
    CHECK(Has(text, "Executable mapping: executable only (modules not linked)"));
    CHECK(Has(text, "Controller prompts (pak texture overlay): NOT EVALUATED"));
    CHECK(Has(text, "obtain the base package's sce_module files"));
    CHECK(Has(text, "SHA-256: " + Sha(update.files["eboot.bin"])));

    MemorySource with_module = update;
    with_module.files["sce_module/libc.prx"] = SmallModule();
    CHECK(Has(Join(Diagnose(with_module).lines), "sce_module/libSceFios2.prx: absent (base package only"));

    MemorySource base = update;
    SetSfo(base, SfoWith(Metadata(Korea().title_id, Korea().content_id, "01.01", "gd")));
    const UnsupportedReport base_report = Diagnose(base);
    CHECK(base_report.classification.kind == SourceClass::kIncomplete);
    CHECK(Has(Join(base_report.lines), "(missing from a base game)"));
    CHECK(!Has(Join(base_report.lines), "legitimate in an update"));

    MemorySource no_eboot;
    SetSfo(no_eboot, SfoWith(Metadata(Korea().title_id, Korea().content_id, "01.01", "gp")));
    no_eboot.files[Korea().regional_archive] = MakePak();
    const std::string no_eboot_text = Join(Diagnose(no_eboot).lines);
    CHECK(Has(no_eboot_text, "absent from this package"));
    CHECK(Has(no_eboot_text, "obtain this build's eboot.bin"));
}

std::vector<std::string> Section(const std::vector<std::string>& lines, const std::string& first, const std::string& next) {
    std::vector<std::string> out;
    bool inside = false;
    for (const std::string& line : lines) {
        if (line.starts_with(first)) inside = true;
        else if (line.starts_with(next)) inside = false;
        if (inside) out.push_back(line);
    }
    return out;
}

void TestPackageAndFolder() {
    const MemorySource package = ShadowSource();
    const UnsupportedReport from_package = Diagnose(package);
    CHECK(from_package.recognized);
    CHECK(Has(Join(from_package.lines), "  kind: package"));
    // Archives are inspected by their footer and index head only; nothing large is read whole.
    for (const auto& [path, bytes] : package.whole_reads) {
        CHECK(!Has(path, ".pak"));
    }
    for (const auto& [path, bytes] : package.range_bytes) {
        CHECK(bytes <= 44 + 1024);
    }

    TempFolder folder("folder");
    folder.Write(folder.path, package);
    const auto before = Snapshot(folder.path);
    const UnsupportedReport from_folder = DiagnoseUnsupportedBuild(*OpenFolderSource(folder.path), package.Sfo());
    CHECK(from_folder.recognized);
    CHECK(Has(Join(from_folder.lines), "  kind: game folder"));
    CHECK(from_folder.classification.kind == from_package.classification.kind);
    for (const auto& [first, next] : std::vector<std::pair<std::string, std::string>>{
             {"Executable (eboot.bin):", "Modules"}, {"Mapped image", "Build-specific"},
             {"Production validators", "Exploratory"}, {"Overall diagnostic result:", "Still required"}}) {
        CHECK(Section(from_folder.lines, first, next) == Section(from_package.lines, first, next));
        CHECK(!Section(from_package.lines, first, next).empty());
    }
    CHECK(Snapshot(folder.path) == before);
    CHECK(!Has(Join(from_folder.lines), folder.path.string()));
    CHECK(!Has(Join(from_folder.lines), folder.path.generic_string()));

    // The package path of the installer diagnoses the opened package and never extracts an unknown build.
    std::ifstream in(kRoot / "launcher" / "game_install.cpp", std::ios::binary);
    const std::string install((std::istreambuf_iterator<char>(in)), {});
    const size_t identity = install.find("CheckPackageIdentity(package.sfo(), builds)");
    const size_t diagnose = install.find("UnsupportedOrFailure(identity, *OpenPackageSource(package), package.sfo())");
    const size_t foreign = install.find("RejectForeignExecutable(package, identity)");
    const size_t reuse = install.find("ReuseInstalledBuild(roots, builds, build, hooks.replace_active)", foreign);
    const size_t extract = install.find("package.Extract(");
    CHECK(identity != std::string::npos && diagnose > identity && diagnose != std::string::npos);
    CHECK(foreign != std::string::npos && reuse != std::string::npos && foreign < reuse);
    const size_t install_from_package = install.find("InstallResult InstallFromPackage(");
    CHECK(install_from_package != std::string::npos && install.find("package.Extract(", install_from_package) == std::string::npos);
    CHECK(extract != std::string::npos && extract < install_from_package);
}

void TestExecutable() {
    MemorySource valid = ShadowSource();
    const std::vector<std::string> lines = Diagnose(valid).lines;
    const std::string text = Join(lines);
    CHECK(Has(text, "SHA-256: " + Sha(valid.files["eboot.bin"])));
    CHECK(Has(text, "ELF: type 0xfe10, machine 62 (x86-64), entry 0x100, 4 program headers, 2 loadable"));
    CHECK(Has(text, "ph[0]: LOAD (0x1) r-x vaddr 0x0 memsz 0x2000 filesz 0x2000"));
    CHECK(Has(text, "ph[1]: LOAD (0x1) rw- vaddr 0x4000 memsz 0x1000 filesz 0x100"));
    CHECK(Has(text, "ph[2]: DYNAMIC"));
    CHECK(Has(text, "ph[3]: SCE_DYNLIBDATA"));
    CHECK(Has(text, "self[0]: flags 0x800"));
    CHECK(Has(text, "rva 0x0..0x2000 = file offset"));
    CHECK(Has(text, "executable: 0x5000 bytes at 0, entry 0x100"));
    CHECK(Has(text, "linked image: 0x25000 bytes with 2 modules"));
    CHECK(Has(text, "3 relocations (3 to the image itself)"));

    const auto broken = [&](const Bytes& eboot, const std::string& expected) {
        MemorySource source = ShadowSource("01.02", eboot);
        const UnsupportedReport report = Diagnose(source);
        const std::string t = Join(report.lines);
        CHECK(report.recognized);
        CHECK(Has(t, "SHA-256: " + Sha(eboot)));
        CHECK(Has(t, "TITLE_ID: CUSA03762"));
        CHECK(Has(t, expected));
        CHECK(Has(t, "Executable mapping: incomplete"));
        CHECK(Has(t, "the executable did not map"));
        if (!Has(t, expected)) {
            std::printf("  expected '%s' in:\n%s\n", expected.c_str(), t.c_str());
        }
    };
    Bytes not_self = SmallEboot();
    not_self[0] = 'X';
    broken(not_self, "structure: INVALID: not a PS4 SELF image");
    Bytes not_elf = SmallEboot();
    not_elf[32 + 4 * 32] = 0;
    broken(not_elf, "structure: INVALID: not a little-endian x86-64 ELF image");
    Bytes truncated = SmallEboot();
    truncated.resize(40);
    broken(truncated, "structure: INVALID: SELF segment table outside the bytes read");
    Bytes cut_segment = SmallEboot();
    cut_segment.resize(cut_segment.size() - 64);
    broken(cut_segment, "lies beyond the end of the file");

    SelfSpec huge = SmallSpec();
    huge.segments[1].memsz = 1ull << 30;
    broken(MakeSelf(huge), "maps beyond 0x10000000");
    SelfSpec overlapping = SmallSpec();
    overlapping.segments[1].vaddr = 0x1000;
    broken(MakeSelf(overlapping), "overlaps another loadable segment");
    SelfSpec aligned = SmallSpec();
    aligned.segments[0].align = 0x3000;
    broken(MakeSelf(aligned), "is not a power of two");
    SelfSpec wrapping = SmallSpec();
    wrapping.segments[1].vaddr = ~uint64_t(0) - 0x10;
    broken(MakeSelf(wrapping), "structure: INVALID: segment range overflows");

    // The image builder itself refuses a wrapped segment instead of allocating from it.
    LinkedImage image;
    CHECK(!LinkGameImage(MakeSelf(wrapping), [](const char*, Bytes&) { return false; }, image).empty());
    CHECK(image.bytes.empty());
}

void TestModules() {
    MemorySource source = ShadowSource();
    std::string text = Join(Diagnose(source).lines);
    CHECK(Has(text, "sce_module/libc.prx: " + std::to_string(source.files["sce_module/libc.prx"].size()) + " bytes, SHA-256 " +
                        Sha(source.files["sce_module/libc.prx"]) + ", valid SELF and ELF"));
    CHECK(Has(text, "mapped at 0x10000, size 0x5000"));
    CHECK(Has(text, "mapped at 0x20000, size 0x5000"));
    CHECK(Has(text, "Modules: 2 mapped, 0 absent, 0 invalid"));
    CHECK(Has(text, "other system modules (not linked): libSceFace.prx 64"));

    MemorySource invalid = ShadowSource();
    invalid.files["sce_module/libSceFios2.prx"] = Bytes(80, 7);
    text = Join(Diagnose(invalid).lines);
    CHECK(Has(text, "sce_module/libSceFios2.prx: 80 bytes"));
    CHECK(Has(text, "INVALID: not a PS4 SELF image"));
    CHECK(Has(text, "Modules: 0 mapped, 0 absent, 1 invalid"));
    CHECK(Has(text, "linked image: not built: expected PS4 SELF"));

    MemorySource absent = ShadowSource();
    absent.files.erase("sce_module/libc.prx");
    text = Join(Diagnose(absent).lines);
    CHECK(Has(text, "sce_module/libc.prx: absent\n"));
    CHECK(Has(text, "Modules: 0 mapped, 1 absent, 0 invalid"));
    CHECK(Has(text, "libc.prx is not available"));

    // The linked image is the loader's: the executable at 0, each module at the next 64 KiB boundary.
    LinkedImage image;
    const MemorySource full = ShadowSource();
    CHECK(LinkGameImage(full.files.at("eboot.bin"),
                        [&](const char* file, Bytes& bytes) {
                            bytes = full.files.at(std::string("sce_module/") + file);
                            return true;
                        },
                        image)
              .empty());
    CHECK(image.modules.size() == 2 && image.modules[0].file == "libc.prx" && image.modules[0].base == 0x10000);
    CHECK(image.module_error.empty() && image.bytes.size() == 0x25000);
    CHECK(!image.relative.empty() && image.relative.front() == std::make_pair(uint64_t(0x4000), uint64_t(0x40)));
}

void TestContent() {
    MemorySource source = ShadowSource();
    for (int i = 0; i < 300; ++i) {
        source.files["unrelated/file" + std::to_string(i) + ".bin"] = Bytes(4, 0);
    }
    const std::vector<std::string> lines = Diagnose(source).lines;
    const std::string text = Join(lines);
    CHECK(Has(text, "ue4commandline.txt: " + std::to_string(source.files["ue4commandline.txt"].size()) + " bytes, SHA-256"));
    CHECK(Has(text, "text: -BuildID=synthetic -SKU=KR"));
    CHECK(Has(text, std::string(Korea().regional_archive) + ": " + std::to_string(MakePak().size()) + " bytes, pak v3"));
    CHECK(Has(text, "mount '../../../', 3 entries"));
    CHECK(Has(text, "unrelated/ 300"));
    CHECK(!Has(text, "file17.bin"));

    MemorySource hostile = ShadowSource();
    Bytes pak = MakePak();
    Put(pak, pak.size() - 44 + 8, 1ull << 60, 8);
    hostile.files[Korea().regional_archive] = pak;
    Bytes footless = MakePak();
    footless[footless.size() - 44] = 0;
    hostile.files[Korea().extra_files[0]] = footless;
    hostile.files[Korea().extra_files[1]] = Bytes(10, 0);
    const std::string hostile_text = Join(Diagnose(hostile).lines);
    CHECK(Has(hostile_text, std::string(Korea().regional_archive) + ": " + std::to_string(pak.size()) + " bytes, index outside the archive"));
    CHECK(Has(hostile_text, std::string(Korea().extra_files[0]) + ": " + std::to_string(footless.size()) + " bytes, no UE4 pak footer"));
    CHECK(Has(hostile_text, std::string(Korea().extra_files[1]) + ": 10 bytes, footer unreadable"));
    for (const auto& [path, bytes] : hostile.range_bytes) {
        CHECK(bytes <= 44 + 1024);
    }
}

// The loader's checks: read-only, equal to what the installers decide, on synthetic and (when present) real images

std::vector<SotbPatchSite> Sites(size_t (*list)(const SotbPatchSite**)) {
    const SotbPatchSite* sites = nullptr;
    const size_t count = list(&sites);
    return {sites, sites + count};
}

std::vector<SotbPatchSite> KeyedSites(size_t (*list)(const char*, SotbPatchSite*, size_t)) {
    SotbPatchSite sites[8];
    const size_t count = list(CodeBuildKey(Korea()).c_str(), sites, 8);
    return {sites, sites + count};
}

void Place(Bytes& image, const std::vector<SotbPatchSite>& sites, int64_t shift = 0) {
    for (const SotbPatchSite& site : sites) {
        std::memcpy(image.data() + int64_t(site.offset) + shift, site.bytes, site.length);
    }
}

constexpr uint64_t kLoadingVtable = 0x3d42610;

// The code an image needs for the exact-byte checks to hold: Skip Intro, Exit Game, Menu Button Layout, display size
// and the loading hint layout (whose vtable entry is a relocation).
Bytes ExactSiteCode(int64_t skip_intro_shift = 0) {
    Bytes code(kLoadingVtable + 0x100, 0x90);
    Place(code, Sites(skip_intro_sites), skip_intro_shift);
    Place(code, Sites(guest_display_sites));
    Place(code, KeyedSites(sotb_exit_game_sites));
    Place(code, KeyedSites(sotb_menu_buttons_sites));
    return code;
}

using Apply = std::function<int(uint8_t* image, size_t size, uint8_t* cave, char* error)>;
using Checker = std::function<int(const uint8_t* image, size_t size, char* error)>;

// The check decides what the installer decides, with the same reason, and writes nothing.
void SameAsInstaller(const char* name, const Bytes& image, const Checker& check, const Apply& apply, bool expected) {
    Bytes copy = image;
    char check_error[200] = "", apply_error[200] = "";
    const std::string before = Sha(copy);
    const int checked = check(copy.data(), copy.size(), check_error);
    CHECK(Sha(copy) == before);
    std::vector<uint8_t> scratch(image.size() + GUEST_PATCH_CAVE_SIZE, 0);
    std::copy(image.begin(), image.end(), scratch.begin());
    const int applied = apply(scratch.data(), image.size(), scratch.data() + image.size(), apply_error);
    CHECK(checked == applied);
    CHECK((checked != 0) == expected);
    if (!checked) {
        CHECK(std::string(check_error) == apply_error);
    }
    if ((checked != 0) != expected || checked != applied) {
        std::printf("  %s: check %d (%s), apply %d (%s)\n", name, checked, check_error, applied, apply_error);
    }
}

void TestNativeChecks() {
    Bytes image = ExactSiteCode();
    // The relocated vtable entry, as the loader leaves it.
    const uint64_t compound = uint64_t(reinterpret_cast<uintptr_t>(image.data())) + Sites(guest_display_sites)[6].offset;
    std::memcpy(image.data() + kLoadingVtable, &compound, 8);
    const std::string key = CodeBuildKey(Korea());
    SameAsInstaller("skip intro", image, [](const uint8_t* p, size_t n, char* e) { return skip_intro_check(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t*, char* e) { return skip_intro_apply(p, n, e, 200); }, true);
    SameAsInstaller("exit game", image, [&](const uint8_t* p, size_t n, char* e) { return sotb_exit_game_check(p, n, key.c_str(), e, 200); },
                    [&](uint8_t* p, size_t n, uint8_t* c, char* e) {
                        return sotb_exit_game_apply(p, n, key.c_str(), c, GUEST_PATCH_CAVE_SIZE, e, 200);
                    },
                    true);
    SameAsInstaller("display size", image, [](const uint8_t* p, size_t n, char* e) { return guest_check_display_size(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t*, char* e) { return guest_apply_display_size(p, n, 2560, 1440, e, 200); }, true);
    {
        // The vtable entry is compared with the image's own address, so this one runs in place.
        char error[200] = "";
        const std::string before = Sha(image);
        CHECK(guest_check_loading_screen_layout(image.data(), image.size(), error, sizeof(error)) == 1);
        CHECK(Sha(image) == before);
        Bytes shifted = image;
        CHECK(guest_check_loading_screen_layout(shifted.data(), shifted.size(), error, sizeof(error)) == 0);
        CHECK(std::string(error) == "unexpected SBeastLoadingScreen vtable entry at image offset 0x3d42610");
    }
    {
        char error[200] = "";
        CHECK(sotb_menu_buttons_check(image.data(), image.size(), key.c_str(), error, sizeof(error)) == 1);
        Bytes twice = image;
        const SotbPatchSite context = KeyedSites(sotb_menu_buttons_sites)[0];
        std::memcpy(twice.data() + 0x100, context.bytes, context.length);
        CHECK(sotb_menu_buttons_check(twice.data(), twice.size(), key.c_str(), error, sizeof(error)) == 0);
        CHECK(std::string(error) == "the menu layout code is not unique");
        CHECK(sotb_menu_buttons_check(image.data(), image.size(), "CUSA99999-1.00", error, sizeof(error)) == 0);
        CHECK(std::string(error) == "not a supported build");
        Bytes western = image;
        CHECK(sotb_menu_buttons_apply(western.data(), western.size(), key.c_str(), SOTB_MENU_BUTTON_LAYOUT_WESTERN) == 1);
        CHECK(Sha(western) != Sha(image));
    }

    Bytes blank(image.size(), 0);
    SameAsInstaller("skip intro missing", blank, [](const uint8_t* p, size_t n, char* e) { return skip_intro_check(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t*, char* e) { return skip_intro_apply(p, n, e, 200); }, false);
    SameAsInstaller("psn missing", blank, [](const uint8_t* p, size_t n, char* e) { return psn_notice_check(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t* c, char* e) { return psn_notice_apply(p, n, c, GUEST_PATCH_CAVE_SIZE, e, 200); }, false);
    SameAsInstaller("ultrawide missing", blank, [](const uint8_t* p, size_t n, char* e) { return guest_ultrawide_check(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t* c, char* e) { return guest_ultrawide_apply(p, n, c, GUEST_PATCH_CAVE_SIZE, e, 200); },
                    false);
    SameAsInstaller("safe area missing", blank,
                    [](const uint8_t* p, size_t n, char* e) { return guest_ultrawide_check_safe_area(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t*, char* e) { return guest_ultrawide_apply_safe_area(p, n, e, 200); }, false);
    SameAsInstaller("frontend missing", blank,
                    [](const uint8_t* p, size_t n, char* e) { return guest_ultrawide_check_frontend(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t*, char* e) { return guest_ultrawide_apply_frontend(p, n, e, 200); }, false);
    SameAsInstaller("hud missing", blank, [](const uint8_t* p, size_t n, char* e) { return guest_hud_check(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t* c, char* e) { return guest_hud_apply(p, n, c, GUEST_PATCH_CAVE_SIZE, e, 200); }, false);
    SameAsInstaller("exit game missing", blank,
                    [&](const uint8_t* p, size_t n, char* e) { return sotb_exit_game_check(p, n, key.c_str(), e, 200); },
                    [&](uint8_t* p, size_t n, uint8_t* c, char* e) {
                        return sotb_exit_game_apply(p, n, key.c_str(), c, GUEST_PATCH_CAVE_SIZE, e, 200);
                    },
                    false);
    SameAsInstaller("display missing", blank, [](const uint8_t* p, size_t n, char* e) { return guest_check_display_size(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t*, char* e) { return guest_apply_display_size(p, n, 2560, 1440, e, 200); }, false);
    SameAsInstaller("loading missing", blank,
                    [](const uint8_t* p, size_t n, char* e) { return guest_check_loading_screen_layout(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t*, char* e) { return guest_apply_loading_screen_layout(p, n, e, 200); }, false);
    Bytes tiny(64, 0);
    SameAsInstaller("psn short", tiny, [](const uint8_t* p, size_t n, char* e) { return psn_notice_check(p, n, e, 200); },
                    [](uint8_t* p, size_t n, uint8_t* c, char* e) { return psn_notice_apply(p, n, c, GUEST_PATCH_CAVE_SIZE, e, 200); }, false);
    char error[200];
    CHECK(skip_intro_check(tiny.data(), tiny.size(), error, sizeof(error)) == 0 && std::string(error) == "image too short");
    CHECK(psn_notice_check(tiny.data(), tiny.size(), error, sizeof(error)) == 0 && std::string(error) == "image too short");

    const GuestPatchReport none = guest_patches_check(blank.data(), blank.size());
    CHECK(none.applied == 0 && none.skipped == 13);
    CHECK(guest_patches_check(nullptr, 0).skipped == 13);

    // LZCNT: every site of the table placed by its own bytes; the check counts them, writes nothing, and the installer
    // agrees once the image also has the table's size and entry.
    const LzcntBuild& known = lzcnt_builds[0];
    Bytes lzcnt(known.image.size, 0x90);
    for (size_t i = 0; i < known.count; ++i) {
        std::memcpy(lzcnt.data() + known.offsets[i], known.shapes[i].bytes, known.shapes[i].length);
    }
    size_t matched = 0;
    const std::string before = Sha(lzcnt);
    CHECK(lzcnt_compat_check(lzcnt.data(), lzcnt.size(), &known, &matched) == LZCNT_COMPAT_OK && matched == known.count);
    CHECK(Sha(lzcnt) == before);
    lzcnt[known.offsets[3]] ^= 0xff;
    CHECK(lzcnt_compat_check(lzcnt.data(), lzcnt.size(), &known, &matched) == LZCNT_COMPAT_BAD_SITE && matched == known.count - 1);
    CHECK(lzcnt_compat_check(blank.data(), blank.size(), &known, &matched) == LZCNT_COMPAT_BAD_SITE && matched == 0);
}

// A synthetic executable big enough for the production offsets, with the exact-byte sites in place.
Bytes SiteEboot(int64_t skip_intro_shift = 0, bool twice = false) {
    Bytes code = ExactSiteCode(skip_intro_shift);
    if (twice) {
        Place(code, Sites(skip_intro_sites), 0x2000);
        Place(code, Sites(skip_intro_sites), 0x4000);
        for (int i = 0; i < 6; ++i) {
            Place(code, Sites(skip_intro_sites), 0x10000 + i * 0x100);
        }
    }
    SelfSpec spec;
    spec.entry = 0x100;
    const uint64_t size = code.size();
    spec.segments.push_back({1, 5, 0, std::move(code), size});
    const uint64_t data = (size + 0x3fff) & ~uint64_t(0x3fff);
    spec.segments.push_back({1, 6, data, Bytes(0x100, 0), 0x1000});
    spec.relative.push_back({kLoadingVtable, Sites(guest_display_sites)[6].offset});
    return MakeSelf(spec);
}

void TestValidatorsInReport() {
    MemorySource source = ShadowSource("01.02", SiteEboot());
    const std::vector<std::string> lines = Diagnose(source).lines;
    const std::string text = Join(lines);
    CHECK(Has(LineWith(lines, "  Skip Intro:"), "accepted: boot sequence 0x28d5c21, 1 of 1 checked sites hold their bytes"));
    CHECK(Has(LineWith(lines, "  Exit Game:"), "accepted: tables of CUSA03762-1.00, CUSA02871-1.00, CUSA00951-1.00: holds"));
    CHECK(Has(LineWith(lines, "  Menu Button Layout:"), "accepted"));
    CHECK(Has(LineWith(lines, "  Display size"), "accepted: 6 of 6 checked sites hold their bytes"));
    CHECK(Has(LineWith(lines, "  Loading hint layout:"), "accepted: 2 of 2 checked sites hold their bytes, vtable entry checked"));
    CHECK(Has(LineWith(lines, "  PlayStation Network notice:"), "REJECTED: text lookup does not match a supported build"));
    CHECK(Has(LineWith(lines, "  True Ultrawide projection:"), "REJECTED: projection function does not match a supported build"));
    CHECK(Has(LineWith(lines, "  Frontend safe area:"), "REJECTED: game layer constructor does not match a supported build"));
    CHECK(Has(LineWith(lines, "  HUD anchoring and cutscene fade:"), "REJECTED: HUD canvas code does not match a supported build"));
    CHECK(Has(LineWith(lines, "  Frontend screen contexts:"), "REJECTED: frontend manager does not match a supported build"));
    CHECK(Has(LineWith(lines, "  Offline Social Features policy:"), "REJECTED: 0 of 13 sites hold their bytes"));
    CHECK(Has(LineWith(lines, "  CPU compatibility (LZCNT rewrite):"), "REJECTED: table Shadow of the Beast 1.00: 0 of " + std::to_string(lzcnt_builds[0].count) + " sites"));
    CHECK(Has(LineWith(lines, "  Controller prompts"), "REJECTED: set of CUSA03762-1.00: pak size differs"));
    CHECK(Has(text, "Production validators: 5 accepted out of 13 (13 evaluated)"));
    CHECK(Has(text, "Features: 7 mapped out of 20 investigated"));
    CHECK(Has(text, "skip_intro mapped") && Has(text, "true_ultrawide NOT mapped"));
    CHECK(Has(text, "Skip Intro: boot screen sequence (33 bytes, production 0x28d5c21): holds"));
    Expect(LineWith(lines, "PSN notice: GetDisplayString prologue"), "(70 bytes, production 0x5861a0): MISSING; partial: first 35 bytes x0, last 35 bytes x0");
    CHECK(Has(text, "  - locate or verify True Ultrawide projection: projection function does not match a supported build"));
    CHECK(Has(text, "safe area and loading hint share SCompoundWidget::OnArrangeChildren: unverified"));
    CHECK(Has(text, "shared code cave slices ordered and disjoint"));
    CHECK(Has(text, "moved sites: none found by exact search"));

    MemorySource moved = ShadowSource("01.02", SiteEboot(0x1000));
    const std::vector<std::string> moved_lines = Diagnose(moved).lines;
    CHECK(Has(LineWith(moved_lines, "  Skip Intro:"), "REJECTED: boot screen sequence does not match a supported build"));
    CHECK(Has(LineWith(moved_lines, "Skip Intro: boot screen sequence"), "MOVED, unique exact match at 0x28d6c21 (+0x1000)"));
    CHECK(Has(Join(moved_lines), "moved sites share 1 displacement: +0x1000 x1"));
    CHECK(Has(Join(moved_lines), "Missing sites: 1") || Has(Join(moved_lines), "Missing sites: 2"));

    MemorySource ambiguous = ShadowSource("01.02", SiteEboot(0x1000, true));
    const std::string ambiguous_line = LineWith(Diagnose(ambiguous).lines, "Skip Intro: boot screen sequence");
    Expect(ambiguous_line, "AMBIGUOUS, 9 exact matches, first: 0x28d6c21 0x28d7c21 0x28d9c21 0x28e5c21");
    CHECK(!Has(ambiguous_line, "0x28e5d21")); // bounded candidate list

    // A site shorter than two partial runs is never partially matched.
    const std::vector<std::string> short_lines = Diagnose(ShadowSource()).lines;
    CHECK(Has(LineWith(short_lines, "original first key (7 bytes"), "MISSING, no exact match (too short for partial matching)"));
    CHECK(Has(LineWith(short_lines, "display size 1 (10 bytes"), "too short for partial matching"));
}

void TestKnownMetadataDifferentSha() {
    MemorySource source = ShadowSource(FinishedLayer(Korea()).app_ver);
    SetSfo(source, SfoWith(Metadata(Korea().title_id, Korea().content_id, FinishedLayer(Korea()).app_ver, "gd")));
    const SourceClassification cls = ClassifySource(source, source.Sfo());
    CHECK(cls.kind == SourceClass::kUnsupported);
    CHECK(cls.build == &Korea());
    CHECK(cls.eboot_sha256 == Sha(source.files["eboot.bin"]));
    CHECK(Has(Join(cls.evidence), "exactly, but eboot.bin is another executable: possible revision"));
    const UnsupportedReport report = Diagnose(source);
    CHECK(report.recognized);
    const std::string text = Join(report.lines);
    CHECK(Has(text, "POSSIBLE REVISION: param.sfo is exactly CUSA03762 1.00 Korea, but eboot.bin differs"));
    CHECK(Has(text, "expected eboot SHA-256 " + std::string(Korea().eboot_sha256)));
    CHECK(Has(text, "actual eboot SHA-256   " + Sha(source.files["eboot.bin"])));
    CHECK(Has(text, "APP_VER: match (01.00)"));
    CHECK(Has(text, "eboot SHA-256: differs"));
    CHECK(!Has(text, "Build verdict: SUPPORTED"));

    // The installer refuses it: nothing is installed, the selected folder and the installed game stay as they were.
    TempFolder temp("revision");
    const fs::path selected = temp.path / "selected";
    temp.Write(selected, source);
    const GameRoots roots{temp.path / "prepared", temp.path / "image"};
    const fs::path installed = BuildPaths(roots, Korea()).app0;
    fs::create_directories(installed / "sce_sys");
    std::ofstream(installed / kPreparedMarker) << "{\"version\": \"1.00\", \"scope\": \"full\"}\n";
    std::ofstream(installed / "eboot.bin") << "the installed game";
    const auto selected_before = Snapshot(selected);
    const auto roots_before = Snapshot(temp.path / "prepared");
    const InstallResult result = InstallFromFolder(selected, roots);
    CHECK(result.status == InstallStatus::kUnsupportedBuild);
    CHECK(!result.ok());
    CHECK(!result.diagnostics.empty() && result.diagnostics.front() == kDiagnosticsBegin && result.diagnostics.back() == kDiagnosticsEnd);
    CHECK(Snapshot(selected) == selected_before);
    CHECK(Snapshot(temp.path / "prepared") == roots_before);
    CHECK(!fs::exists(temp.path / "image"));
    const InstallMessage message = DescribeInstall(result);
    CHECK(message.title == "Shadow of the Beast version not supported yet");
    CHECK(Has(message.text, "written to the current log") && Has(message.text, "installed game was not changed") &&
          Has(message.text, "Bug Report"));
    CHECK(!Has(message.text, "0x") && !Has(message.text, "REJECTED"));
}

void TestInstallSafety() {
    TempFolder temp("install");
    const GameRoots roots{temp.path / "prepared", temp.path / "image"};
    const MemorySource unsupported = ShadowSource();
    temp.Write(temp.path / "unsupported", unsupported);
    const auto before = Snapshot(temp.path / "unsupported");
    const InstallResult result = InstallFromFolder(temp.path / "unsupported", roots);
    CHECK(result.status == InstallStatus::kUnsupportedBuild);
    CHECK(Has(Join(result.diagnostics), "Classification: probable unsupported Shadow of the Beast build"));
    CHECK(Snapshot(temp.path / "unsupported") == before);
    CHECK(!fs::exists(roots.prepared) && !fs::exists(roots.image));

    MemorySource other;
    SetSfo(other, SfoWith(Metadata("CUSA00001", "UP0000-CUSA00001_00-ANOTHERGAME00000", "01.00", "gd", "Another Game")));
    other.files["eboot.bin"] = SmallEboot();
    temp.Write(temp.path / "other", other);
    const InstallResult other_result = InstallFromFolder(temp.path / "other", roots);
    CHECK(other_result.status == InstallStatus::kWrongGame);
    CHECK(other_result.diagnostics.empty());
    CHECK(Has(other_result.detail, "different game"));
    CHECK(!fs::exists(roots.prepared) && !fs::exists(roots.image));

    // The diagnostics only read.
    for (const char* file : {"launcher/build_diagnostics.cpp", "launcher/build_identity.cpp"}) {
        std::ifstream in(kRoot / file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        for (const char* writes : {"ofstream", "fopen(", "CreateProcess", "system(", "remove(", "WriteFile", "rename(", "_apply("}) {
            CHECK(!Has(text, writes));
        }
    }
}

void TestLog() {
    const MemorySource source = ShadowSource();
    const UnsupportedReport first = Diagnose(source);
    const std::vector<std::string>& lines = first.lines;
    CHECK(lines.front() == kDiagnosticsBegin);
    CHECK(lines.back() == kDiagnosticsEnd);
    CHECK(Join(Diagnose(source).lines) == Join(lines));
    const std::string text = Join(lines);
    CHECK(text.size() <= diagnostics::kMaxReportBytes);
    CHECK(lines.size() <= diagnostics::kMaxReportLines);
    for (const std::string& line : lines) {
        CHECK(line.size() <= diagnostics::kMaxLineLength);
        size_t run = 0, longest = 0;
        for (const char c : line) {
            run = std::isxdigit(static_cast<unsigned char>(c)) ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        CHECK(longest <= 64); // hashes only, never raw bytes
    }
    std::vector<std::string> headings;
    for (const std::string& line : lines) {
        if (!line.empty() && line[0] != ' ' && line.back() == ':') headings.push_back(line);
    }
    for (const char* heading : {"Source:", "Known-build comparison:", "Executable (eboot.bin):", "SELF/ELF layout:", "Important content:",
                                "Overall diagnostic result:", "Still required for support:", "Package limits:", "Relationships:"}) {
        CHECK(std::find(headings.begin(), headings.end(), heading) != headings.end());
    }
    CHECK(Has(text, "  - add a build to launcher/game_profile.cpp and scripts/game_profiles.py"));
    CHECK(Has(text, "  - prepare the build and pass tools/test.ps1 -Filter build-parity, then validate the runtime on hardware"));
    CHECK(Has(text, "This build stays unsupported"));

    // Bug Report's filter keeps the block whole, and its tail holds it.
    const PrivateFacts facts;
    CHECK(ScrubText(text, facts) == text);
    CHECK(LastBytes("earlier session lines\n" + text, kMostLogBytes).find(kDiagnosticsBegin) != std::string::npos);

    // A report bigger than the limits is cut, and still ends with its marker.
    MemorySource crowded = ShadowSource();
    for (int i = 0; i < 400; ++i) {
        crowded.files[std::string(i, 'x').substr(0, 40) + std::to_string(i) + ".bin"] = Bytes(1, 0);
        crowded.files["sce_module/extra" + std::to_string(i) + "_" + std::string(30, 'm') + ".prx"] = Bytes(1, 0);
    }
    const std::vector<std::string> cut = Diagnose(crowded).lines;
    CHECK(cut.back() == kDiagnosticsEnd);
    CHECK(Join(cut).size() <= diagnostics::kMaxReportBytes);
    CHECK(cut.size() <= diagnostics::kMaxReportLines);
}

// Every per-build feature of the parity contract is investigated by the diagnostics.
void TestParityFeatures() {
    std::ifstream in(kRoot / "tests" / "build_parity_tests.cpp", std::ios::binary);
    const std::string parity((std::istreambuf_iterator<char>(in)), {});
    const std::string text = Join(Diagnose(ShadowSource()).lines);
    const std::regex feature(R"re(\{"([a-z0-9_]+)", "[^"]*", true,)re");
    size_t features = 0;
    for (std::sregex_iterator it(parity.begin(), parity.end(), feature), end; it != end; ++it) {
        const std::string key = (*it)[1];
        ++features;
        CHECK(Has(text, key));
        if (!Has(text, key)) {
            std::printf("  parity feature %s is not in the diagnostics\n", key.c_str());
        }
    }
    CHECK(features >= 23);
}

// The prepared supported builds, read only: recognized as supported, linked exactly as their boot image, and a copy that
// differs by one executable byte in memory is diagnosed with every loader check accepting it.
class AlteredSource final : public GameSource {
public:
    explicit AlteredSource(const fs::path& folder) : inner_(OpenFolderSource(folder)) {}
    std::string Kind() const override { return inner_->Kind(); }
    std::vector<SourceEntry> Files(size_t limit) const override { return inner_->Files(limit); }
    bool Read(const std::string& path, uint64_t max_bytes, Bytes& out) const override {
        if (!inner_->Read(path, max_bytes, out)) return false;
        if (path == "eboot.bin" && !out.empty()) out.back() ^= 0x5a;
        return true;
    }
    bool ReadRange(const std::string& path, uint64_t offset, size_t length, Bytes& out) const override {
        return inner_->ReadRange(path, offset, length, out);
    }

private:
    std::unique_ptr<GameSource> inner_;
};

void TestRealBuilds() {
    size_t checked = 0;
    for (const GameBuild& build : SupportedBuilds()) {
        const fs::path folder = kRoot / "game" / "prepared" / build.folder;
        std::error_code ec;
        if (!fs::is_regular_file(folder / "eboot.bin", ec)) {
            continue;
        }
        ++checked;
        std::ifstream sfo_in(folder / "sce_sys" / "param.sfo", std::ios::binary);
        const std::string sfo((std::istreambuf_iterator<char>(sfo_in)), {});
        const auto source = OpenFolderSource(folder);
        const SourceClassification cls = ClassifySource(*source, sfo);
        CHECK(cls.kind == SourceClass::kSupported && cls.build == &build && cls.eboot_sha256 == build.eboot_sha256);
        CHECK(!DiagnoseUnsupportedBuild(*source, sfo).recognized);

        const AlteredSource altered(folder);
        const UnsupportedReport report = DiagnoseUnsupportedBuild(altered, sfo);
        const std::string text = Join(report.lines);
        CHECK(report.recognized && report.classification.kind == SourceClass::kUnsupported);
        CHECK(Has(text, "POSSIBLE REVISION: param.sfo is exactly " + std::string(build.label)));
        CHECK(Has(text, "Production validators: 13 accepted out of 13 (13 evaluated)"));
        CHECK(Has(text, "Features: 20 mapped out of 20 investigated"));
        CHECK(Has(text, "Executable mapping: complete"));
        CHECK(Has(text, "Modules: 2 mapped, 0 absent, 0 invalid"));
        CHECK(Has(text, "Ambiguous sites: 0") && Has(text, "Missing sites: 0"));
        CHECK(Has(text, "entry " + std::string("0x19105d0")));
        CHECK(text.size() <= diagnostics::kMaxReportBytes);
        if (!Has(text, "Production validators: 13 accepted out of 13")) {
            std::printf("%s", text.c_str());
        }
        std::printf("real build: %s recognized as supported; one-byte revision diagnosed, %zu lines, %zu bytes\n", build.folder,
                    report.lines.size(), text.size());
    }
    if (checked == 0) {
        std::printf("real build: SKIPPED (no supported build is prepared)\n");
    }
}

} // namespace

int main() {
    TestClassification();
    TestMetadata();
    TestBaseAndUpdate();
    TestPackageAndFolder();
    TestExecutable();
    TestModules();
    TestContent();
    TestNativeChecks();
    TestValidatorsInReport();
    TestKnownMetadataDifferentSha();
    TestInstallSafety();
    TestLog();
    TestParityFeatures();
    TestRealBuilds();
    std::printf("unsupported build diagnostics tests: %lld checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

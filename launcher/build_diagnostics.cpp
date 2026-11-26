#include "build_diagnostics.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <system_error>

extern "C" {
#include "guest_patches.h"
#include "isa_emulate.h"
}
#include "boot_image.h"
#include "build_identity.h"
#include "crypto.h"
#include "game_files.h"
#include "guest_display.h"
#include "guest_hud.h"
#include "guest_ultrawide.h"
#include "incident_report.h"
#include "lzcnt_compat.h"
#include "psn_notice.h"
#include "settings.h"
#include "skip_intro.h"
#include "sotb_exit_game.h"
#include "sotb_menu_buttons.h"
#include "sotb_prompt_assets.h"
#include "utf8.h"

namespace Sotb {
namespace {

namespace fs = std::filesystem;
using namespace diagnostics;
using Bytes = std::vector<uint8_t>;

static_assert(kMaxReportBytes < kMostLogBytes / 2, "the diagnostic block must survive Bug Report's log tail");

constexpr uint32_t kPakMagic = 0x5A6F12E1u;
constexpr size_t kPakFooterSize = 44;
constexpr size_t kPakIndexHead = 1024;
constexpr char kPakFolder[] = "beast/content/paks/";
constexpr char kGameTitle[] = "shadow of the beast";
constexpr char kContentName[] = "shadowofbeast";

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%" PRIx64, value);
    return text;
}

std::string Signed(int64_t value) {
    return value < 0 ? "-" + Hex(uint64_t(-value)) : "+" + Hex(uint64_t(value));
}

std::string HexBytes(const uint8_t* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (size_t i = 0; i < size; ++i) {
        hex += digits[data[i] >> 4];
        hex += digits[data[i] & 15];
    }
    return hex;
}

std::string Sha256Of(const Bytes& data) {
    uint8_t digest[32];
    return crypto::Sha256(data.data(), data.size(), digest) ? HexBytes(digest, sizeof(digest)) : std::string();
}

std::string Shown(const std::string& value) { return value.empty() ? "(missing)" : value; }

// Printable ASCII only, at most `most` characters.
std::string Printable(std::string_view text, size_t most) {
    std::string out;
    for (const char c : text) {
        if (c == '\0' || out.size() >= most) {
            break;
        }
        out += c >= 0x20 && c < 0x7f ? c : '?';
    }
    return out;
}

bool Contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

class FolderSource final : public GameSource {
public:
    explicit FolderSource(fs::path root) : root_(std::move(root)) {}

    std::string Kind() const override { return "game folder"; }

    std::vector<SourceEntry> Files(size_t limit) const override {
        std::vector<SourceEntry> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && files.size() < limit; it.increment(ec)) {
            std::error_code item_ec;
            if (!it->is_regular_file(item_ec) || item_ec) {
                continue;
            }
            std::string relative = Utf8(it->path().lexically_relative(root_));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            files.push_back({std::move(relative), it->file_size(item_ec)});
        }
        std::sort(files.begin(), files.end(), [](const SourceEntry& a, const SourceEntry& b) { return a.path < b.path; });
        return files;
    }

    bool Read(const std::string& path, uint64_t max_bytes, Bytes& out) const override {
        out.clear();
        std::error_code ec;
        const fs::path file = LocalPath(path);
        const uint64_t size = fs::file_size(file, ec);
        if (ec || size > max_bytes) {
            return false;
        }
        std::ifstream in(file, std::ios::binary);
        out.resize(static_cast<size_t>(size));
        if (!in || !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
            out.clear();
            return false;
        }
        return true;
    }

    bool ReadRange(const std::string& path, uint64_t offset, size_t length, Bytes& out) const override {
        out.clear();
        std::error_code ec;
        const uint64_t size = fs::file_size(LocalPath(path), ec);
        if (ec || offset > size || length > size - offset) {
            return false;
        }
        std::ifstream in(LocalPath(path), std::ios::binary);
        out.resize(length);
        if (!in || !in.seekg(static_cast<std::streamoff>(offset)) ||
            !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length))) {
            out.clear();
            return false;
        }
        return true;
    }

private:
    fs::path LocalPath(const std::string& path) const { return root_ / PathFromUtf8(path); }

    fs::path root_;
};

// The package's inner file system, plus its param.sfo, which the package keeps outside it.
class PackageSource final : public GameSource {
public:
    explicit PackageSource(Package& package) : package_(package) {}

    std::string Kind() const override { return "package"; }

    std::vector<SourceEntry> Files(size_t limit) const override {
        std::vector<SourceEntry> files;
        if (!package_.sfo().empty()) {
            files.push_back({kSfoPath, package_.sfo().size()});
        }
        for (const PackageFile& file : package_.Files()) {
            if (files.size() >= limit) {
                break;
            }
            if (file.path != kSfoPath) {
                files.push_back({file.path, file.size});
            }
        }
        std::sort(files.begin(), files.end(), [](const SourceEntry& a, const SourceEntry& b) { return a.path < b.path; });
        return files;
    }

    bool Read(const std::string& path, uint64_t max_bytes, Bytes& out) const override {
        if (path == kSfoPath) {
            out.assign(package_.sfo().begin(), package_.sfo().end());
            return !out.empty() && out.size() <= max_bytes;
        }
        return package_.ReadFile(path, max_bytes, out).ok();
    }

    bool ReadRange(const std::string& path, uint64_t offset, size_t length, Bytes& out) const override {
        if (path == kSfoPath) {
            const std::string& sfo = package_.sfo();
            if (offset > sfo.size() || length > sfo.size() - offset) {
                return false;
            }
            out.assign(sfo.begin() + std::ptrdiff_t(offset), sfo.begin() + std::ptrdiff_t(offset + length));
            return true;
        }
        return package_.ReadFileRange(path, offset, length, out).ok();
    }

private:
    static constexpr char kSfoPath[] = "sce_sys/param.sfo";
    Package& package_;
};

// The block, bounded in line length, line count and size; the end marker always fits.
class Lines {
public:
    __attribute__((format(printf, 2, 3))) void Add(const char* format, ...) {
        va_list args;
        va_start(args, format);
        char text[kMaxLineLength + 64];
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        Add(std::string(text));
    }

    void Add(std::string text) {
        if (full_) {
            return;
        }
        if (text.size() > kMaxLineLength) {
            text.resize(kMaxLineLength - 3);
            text += "...";
        }
        constexpr size_t kReserve = 2 * kMaxLineLength;
        if (lines_.size() + 2 >= kMaxReportLines || bytes_ + text.size() + 1 + kReserve > kMaxReportBytes) {
            full_ = true;
            lines_.push_back("(report truncated at its size limit)");
            return;
        }
        bytes_ += text.size() + 1;
        lines_.push_back(std::move(text));
    }

    std::vector<std::string> Finish() {
        lines_.push_back(kDiagnosticsEnd);
        return std::move(lines_);
    }

private:
    std::vector<std::string> lines_;
    size_t bytes_ = 0;
    bool full_ = false;
};

// The selection

struct PakInfo {
    std::string path;
    uint64_t size = 0;
    bool valid = false;
    uint32_t version = 0;
    uint64_t index_offset = 0, index_size = 0;
    std::string index_sha1, mount, error;
    int64_t entries = -1;
};

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32; }

// A UE4 pak's footer and the head of its index; the archive is never read beyond them.
PakInfo ProbePak(const GameSource& source, const SourceEntry& file) {
    PakInfo pak;
    pak.path = file.path;
    pak.size = file.size;
    Bytes footer;
    if (file.size < kPakFooterSize || !source.ReadRange(file.path, file.size - kPakFooterSize, kPakFooterSize, footer)) {
        pak.error = "footer unreadable";
        return pak;
    }
    if (Le32(footer.data()) != kPakMagic) {
        pak.error = "no UE4 pak footer";
        return pak;
    }
    pak.version = Le32(footer.data() + 4);
    pak.index_offset = Le64(footer.data() + 8);
    pak.index_size = Le64(footer.data() + 16);
    pak.index_sha1 = HexBytes(footer.data() + 24, 20);
    const uint64_t limit = file.size - kPakFooterSize;
    if (pak.index_offset > limit || pak.index_size > limit - pak.index_offset || pak.index_size < 8) {
        pak.error = "index outside the archive";
        return pak;
    }
    Bytes head;
    if (!source.ReadRange(file.path, pak.index_offset, size_t(std::min<uint64_t>(pak.index_size, kPakIndexHead)), head)) {
        pak.error = "index unreadable";
        return pak;
    }
    pak.valid = true;
    const int32_t length = int32_t(Le32(head.data()));
    if (length > 0 && size_t(length) + 8 <= head.size()) {
        pak.mount = Printable(std::string_view(reinterpret_cast<const char*>(head.data()) + 4, size_t(length)), 64);
        pak.entries = int32_t(Le32(head.data() + 4 + length));
    }
    return pak;
}

struct Context {
    const GameSource& source;
    std::string sfo;
    std::vector<SourceEntry> files;
    bool truncated = false;
    std::map<std::string, size_t> by_lower;
    std::vector<PakInfo> paks;
    Bytes eboot;
    bool eboot_read = false;
    std::string eboot_sha;
    SourceClassification cls;

    explicit Context(const GameSource& s, std::string sfo_text) : source(s), sfo(std::move(sfo_text)) {
        files = source.Files(kMaxSourceFiles + 1);
        if (files.size() > kMaxSourceFiles) {
            truncated = true;
            files.resize(kMaxSourceFiles);
        }
        for (size_t i = 0; i < files.size(); ++i) {
            by_lower.emplace(Lower(files[i].path), i);
        }
    }

    const SourceEntry* Find(std::string_view path) const {
        const auto it = by_lower.find(Lower(std::string(path)));
        return it == by_lower.end() ? nullptr : &files[it->second];
    }

    bool Category(const char* category) const { return SfoString(sfo, "CATEGORY") == category; }

    void LoadEboot() {
        if (eboot_read) {
            return;
        }
        const SourceEntry* file = Find("eboot.bin");
        eboot_read = file && file->size <= kMaxExecutableBytes && source.Read(file->path, kMaxExecutableBytes, eboot);
        eboot_sha = eboot_read ? Sha256Of(eboot) : std::string();
    }
};

bool SfoValid(const std::string& sfo) { return sfo.size() >= 20 && std::memcmp(sfo.data(), "\0PSF", 4) == 0; }

bool NamesGame(const std::string& sfo) {
    for (const SfoField& field : SfoFields(sfo)) {
        if (field.key.starts_with("TITLE") && field.key != "TITLE_ID" && Contains(Lower(field.value), kGameTitle)) {
            return true;
        }
    }
    return false;
}

bool KnownTitleId(const std::string& title_id) {
    const auto builds = SupportedBuilds();
    return std::any_of(builds.begin(), builds.end(), [&](const GameBuild& b) { return title_id == b.title_id; });
}

// What a complete base game holds besides param.sfo: the same for every supported build.
std::vector<std::string> BaseFiles() {
    std::set<std::string> names;
    for (const GameBuild& build : SupportedBuilds()) {
        for (const char* name : kSharedGameFiles) {
            names.insert(name);
        }
        if (build.regional_archive) {
            names.insert(build.regional_archive);
        }
        for (const char* name : build.extra_files) {
            names.insert(name);
        }
    }
    names.erase("sce_sys/param.sfo");
    return {names.begin(), names.end()};
}

std::vector<std::string> MissingBaseFiles(const Context& ctx) {
    std::vector<std::string> missing;
    for (const std::string& name : BaseFiles()) {
        const SourceEntry* file = ctx.Find(name);
        if (!file || file->size == 0) {
            missing.push_back(name);
        }
    }
    return missing;
}

void Classify(Context& ctx) {
    SourceClassification& cls = ctx.cls;
    const std::string& sfo = ctx.sfo;
    const std::string title_id = SfoString(sfo, "TITLE_ID");
    const bool sfo_valid = SfoValid(sfo);
    const bool known_title = KnownTitleId(title_id);
    const bool content_names = Contains(Lower(SfoString(sfo, "CONTENT_ID")), kContentName);
    const bool title_names = NamesGame(sfo);
    const bool metadata = known_title || content_names || title_names;
    if (!sfo_valid) {
        cls.evidence.push_back(sfo.empty() ? "no param.sfo" : "param.sfo is not a PSF file");
    }
    if (known_title) {
        cls.evidence.push_back("TITLE_ID " + title_id + " is a supported Shadow of the Beast title");
    } else if (sfo_valid) {
        cls.evidence.push_back("TITLE_ID " + Shown(title_id) + " is not a supported title");
    }
    if (content_names) {
        cls.evidence.push_back("CONTENT_ID names SHADOWOFBEAST");
    }
    if (title_names) {
        cls.evidence.push_back("TITLE names Shadow of the Beast");
    }

    size_t pak_files = 0;
    for (const SourceEntry& file : ctx.files) {
        const std::string lower = Lower(file.path);
        if (!lower.starts_with(kPakFolder) || !lower.ends_with(".pak")) {
            continue;
        }
        ++pak_files;
        if (ctx.paks.size() < kMaxArchiveProbes) {
            ctx.paks.push_back(ProbePak(ctx.source, file));
        }
    }
    const size_t valid_paks = size_t(std::count_if(ctx.paks.begin(), ctx.paks.end(), [](const PakInfo& p) { return p.valid; }));
    if (pak_files) {
        cls.evidence.push_back(std::to_string(pak_files) + " archives under " + kPakFolder + " (the game's UE4 project Beast), " +
                               std::to_string(valid_paks) + " with a valid UE4 pak footer");
    }
    const bool eboot = ctx.Find("eboot.bin") != nullptr;
    const bool update = ctx.Category("gp");
    const bool strong_archive = valid_paks > 0;
    if (!(strong_archive || (metadata && (eboot || pak_files)))) {
        cls.kind = sfo_valid || !ctx.files.empty() ? SourceClass::kOtherGame : SourceClass::kInvalid;
        if (!sfo_valid && !pak_files) {
            cls.kind = SourceClass::kInvalid;
        }
        cls.evidence.push_back("not identified as Shadow of the Beast: no game metadata with an executable or game archives");
        return;
    }
    if (update) {
        cls.evidence.push_back(eboot ? "an update package (CATEGORY gp) carrying eboot.bin" : "an update package (CATEGORY gp)");
    }

    const GameCheck named = CheckSfoIdentity(sfo);
    cls.build = named.state == GameFileState::kValid ? named.build : nullptr;
    const std::vector<std::string> missing = MissingBaseFiles(ctx);
    if (cls.build) {
        ctx.LoadEboot();
        cls.eboot_sha256 = ctx.eboot_sha;
        if (ctx.eboot_read && IsBuildExecutable(*cls.build, ctx.eboot_sha)) {
            cls.evidence.push_back(std::string("param.sfo and eboot.bin are ") + cls.build->label);
            cls.kind = missing.empty() ? SourceClass::kSupported : SourceClass::kIncomplete;
            return;
        }
        if (ctx.eboot_read) {
            cls.evidence.push_back(std::string("param.sfo names ") + cls.build->label +
                                   " exactly, but eboot.bin is another executable: possible revision");
        }
    }
    cls.kind = update ? SourceClass::kUpdate : missing.empty() ? SourceClass::kUnsupported : SourceClass::kIncomplete;
    if (!update && !missing.empty()) {
        cls.evidence.push_back(std::to_string(missing.size()) + " files of a complete base game are missing");
    }
}

// The report

const char* PackageKind(const Context& ctx) {
    return ctx.Category("gp") ? "update (CATEGORY gp)" : ctx.Category("gd") ? "base game (CATEGORY gd)" : "unknown (CATEGORY is not gd or gp)";
}

std::string RegionOf(const std::string& content_id) {
    static const std::pair<const char*, const char*> kPrefixes[] = {
        {"UP", "USA"}, {"EP", "Europe"}, {"JP", "Japan"}, {"HP", "Asia"}, {"KP", "Korea"}, {"IP", "internal"}};
    for (const auto& [prefix, region] : kPrefixes) {
        if (content_id.starts_with(prefix)) {
            return region;
        }
    }
    return "unknown";
}

void DescribeSource(Lines& out, const Context& ctx) {
    out.Add("Source:");
    out.Add("  kind: %s", ctx.source.Kind().c_str());
    out.Add("  files: %zu%s", ctx.files.size(), ctx.truncated ? " (listing stopped at its limit)" : "");
    out.Add("Classification: %s", SourceClassName(ctx.cls.kind));
    for (const std::string& line : ctx.cls.evidence) {
        out.Add("  evidence: " + line);
    }
}

void DescribeMetadata(Lines& out, const Context& ctx) {
    const std::string& sfo = ctx.sfo;
    out.Add("Package metadata (param.sfo, %zu bytes%s):", sfo.size(), SfoValid(sfo) ? "" : ", not a PSF file");
    for (const char* key : {"TITLE", "TITLE_ID", "CONTENT_ID", "APP_VER", "VERSION", "CATEGORY"}) {
        out.Add("  %s: %s", key, Printable(Shown(SfoString(sfo, key)), 96).c_str());
    }
    out.Add("  region (CONTENT_ID prefix): %s", RegionOf(SfoString(sfo, "CONTENT_ID")).c_str());
    out.Add("  package type: %s", PackageKind(ctx));
    std::string others;
    for (const SfoField& field : SfoFields(sfo)) {
        static const std::set<std::string> kShown = {"TITLE", "TITLE_ID", "CONTENT_ID", "APP_VER", "VERSION", "CATEGORY"};
        if (kShown.count(field.key) || field.key.starts_with("TITLE_")) {
            continue;
        }
        const std::string item = Printable(field.key, 32) + "=" + Printable(field.value, 40);
        if (others.size() + item.size() > 150) {
            out.Add("  other fields: " + others);
            others.clear();
        }
        others += (others.empty() ? "" : ", ") + item;
    }
    out.Add("  other fields: " + (others.empty() ? std::string("(none)") : others));
}

void DescribeComparison(Lines& out, const Context& ctx, const ExecutableHeader& header) {
    out.Add("Known-build comparison:");
    BuildEvidence evidence;
    evidence.source = ctx.source.Kind() + (ctx.eboot_read ? ": param.sfo and eboot.bin" : ": param.sfo only");
    const auto text = [&](const char* key) {
        const std::string value = SfoString(ctx.sfo, key);
        return value.empty() ? std::optional<std::string>{} : std::optional<std::string>{value};
    };
    evidence.title_id = text("TITLE_ID");
    evidence.content_id = text("CONTENT_ID");
    evidence.app_ver = text("APP_VER");
    evidence.sfo_version = text("VERSION");
    if (ctx.eboot_read) {
        AddExecutable(evidence, header, ctx.eboot.size(), ctx.eboot_sha);
    }
    for (const std::string& line : DescribeAssessment(evidence, AssessBuild(evidence))) {
        out.Add("  " + line);
    }
    const std::string title_id = SfoString(ctx.sfo, "TITLE_ID");
    for (const GameBuild& build : SupportedBuilds()) {
        if (title_id == build.title_id) {
            const PackageLayer& layer = FinishedLayer(build);
            out.Add("  supported build of this TITLE_ID: %s, APP_VER %s, VERSION %s, eboot SHA-256 %s", build.label, layer.app_ver,
                    layer.sfo_version, build.eboot_sha256);
        }
    }
    if (ctx.cls.build && ctx.eboot_read && !IsBuildExecutable(*ctx.cls.build, ctx.eboot_sha)) {
        out.Add("  POSSIBLE REVISION: param.sfo is exactly %s, but eboot.bin differs", ctx.cls.build->label);
        out.Add("    expected eboot SHA-256 %s", ctx.cls.build->eboot_sha256);
        out.Add("    actual eboot SHA-256   %s", ctx.eboot_sha.c_str());
    }
}

const char* SegmentType(uint32_t type) {
    switch (type) {
    case 1: return "LOAD";
    case 2: return "DYNAMIC";
    case 3: return "INTERP";
    case 7: return "TLS";
    case 0x61000000: return "SCE_DYNLIBDATA";
    case 0x61000001: return "SCE_PROCPARAM";
    case 0x61000002: return "SCE_MODULE_PARAM";
    case 0x61000010: return "SCE_RELRO";
    case 0x6fffff00: return "SCE_COMMENT";
    case 0x6fffff01: return "SCE_VERSION";
    case 0x6474e550: return "GNU_EH_FRAME";
    }
    return "other";
}

const char* Permissions(uint32_t flags) {
    static const char names[8][4] = {"---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx"};
    return names[flags & 7];
}

bool IsLoadable(const ProgramSegment& s) { return s.type == 1 || s.type == 0x61000010; }

// Structural faults that make the executable unfit to map, beyond what the header reader already refuses.
std::vector<std::string> LayoutProblems(const ExecutableHeader& header, uint64_t file_size) {
    std::vector<std::string> problems;
    std::vector<const ProgramSegment*> loads;
    for (size_t i = 0; i < header.segments.size(); ++i) {
        const ProgramSegment& s = header.segments[i];
        if (s.align && (s.align & (s.align - 1))) {
            problems.push_back("ph[" + std::to_string(i) + "] alignment " + Hex(s.align) + " is not a power of two");
        }
        if (!IsLoadable(s)) {
            continue;
        }
        if (s.filesz > s.memsz) {
            problems.push_back("ph[" + std::to_string(i) + "] file size exceeds its memory size");
        }
        if (s.memsz > kMaxImageBytes || s.vaddr > kMaxImageBytes - s.memsz) {
            problems.push_back("ph[" + std::to_string(i) + "] maps beyond " + Hex(kMaxImageBytes));
            continue;
        }
        for (const ProgramSegment* other : loads) {
            if (s.memsz && other->memsz && s.vaddr < other->vaddr + other->memsz && other->vaddr < s.vaddr + s.memsz) {
                problems.push_back("ph[" + std::to_string(i) + "] overlaps another loadable segment");
            }
        }
        loads.push_back(&s);
    }
    for (size_t i = 0; i < header.self_entries.size(); ++i) {
        const SelfEntry& e = header.self_entries[i];
        if ((e.flags & 0x800) && (e.offset > file_size || e.size > file_size - e.offset)) {
            problems.push_back("SELF segment " + std::to_string(i) + " lies beyond the end of the file");
        }
    }
    if (header.image_extent > kMaxImageBytes) {
        problems.push_back("declared image size " + Hex(header.image_extent) + " exceeds the " + Hex(kMaxImageBytes) + " limit");
    }
    return problems;
}

void DescribeExecutable(Lines& out, const Context& ctx, const ExecutableHeader& header, const std::vector<std::string>& problems) {
    out.Add("Executable (eboot.bin):");
    const SourceEntry* file = ctx.Find("eboot.bin");
    if (!file) {
        out.Add("  absent from this %s", ctx.source.Kind().c_str());
        return;
    }
    if (!ctx.eboot_read) {
        out.Add("  %" PRIu64 " bytes, not read: %s", file->size,
                file->size > kMaxExecutableBytes ? "larger than the diagnostic limit" : "unreadable");
        return;
    }
    out.Add("  size: %zu bytes", ctx.eboot.size());
    out.Add("  SHA-256: %s", ctx.eboot_sha.c_str());
    for (const GameBuild& build : SupportedBuilds()) {
        if (IsBuildExecutable(build, ctx.eboot_sha)) {
            out.Add("  this is the executable of %s", build.label);
        }
    }
    out.Add("  SELF: %s, version %u, mode %u, endian %u, attributes 0x%x, header 0x%x, meta 0x%x, declared size %" PRIu64
            "%s, %zu segments",
            header.error == "not a PS4 SELF image" ? "not a PS4 SELF" : "PS4 SELF magic", header.self_version, header.self_mode,
            header.self_endian, header.self_attributes, header.self_header_size, header.self_meta_size, header.self_file_size,
            header.self_file_size == ctx.eboot.size() ? " (the file size)" : " (differs from the file size)",
            header.self_entries.size());
    if (!header.valid) {
        out.Add("  structure: INVALID: %s", header.error.c_str());
        return;
    }
    out.Add("  ELF: type 0x%x, machine %u (x86-64), entry %s, %zu program headers, %zu loadable", header.elf_type, header.machine,
            Hex(header.entry).c_str(), header.segments.size(), header.load_segments);
    for (const std::string& problem : problems) {
        out.Add("  structure problem: " + problem);
    }
}

void DescribeLayout(Lines& out, const ExecutableHeader& header) {
    out.Add("SELF/ELF layout:");
    for (size_t i = 0; i < header.segments.size(); ++i) {
        const ProgramSegment& s = header.segments[i];
        out.Add("  ph[%zu]: %s (0x%x) %s vaddr %s memsz %s filesz %s offset %s align %s", i, SegmentType(s.type), s.type,
                Permissions(s.flags), Hex(s.vaddr).c_str(), Hex(s.memsz).c_str(), Hex(s.filesz).c_str(), Hex(s.offset).c_str(),
                Hex(s.align).c_str());
    }
    for (size_t i = 0; i < header.self_entries.size(); ++i) {
        const SelfEntry& e = header.self_entries[i];
        if (!(e.flags & 0x800)) {
            continue;
        }
        out.Add("  self[%zu]: flags %s offset %s size %s memory %s -> ph[%" PRIu64 "]%s", i, Hex(e.flags).c_str(), Hex(e.offset).c_str(),
                Hex(e.size).c_str(), Hex(e.memory).c_str(), (e.flags >> 20) & 4095, (e.flags & 10) ? " (encrypted or compressed)" : "");
    }
    for (size_t i = 0; i < header.segments.size(); ++i) {
        const ProgramSegment& s = header.segments[i];
        if (!IsLoadable(s) || !s.filesz) {
            continue;
        }
        for (const SelfEntry& e : header.self_entries) {
            if ((e.flags & 0x800) && ((e.flags >> 20) & 4095) == i) {
                out.Add("  rva %s..%s = file offset %s (rva %s)", Hex(s.vaddr).c_str(), Hex(s.vaddr + s.filesz).c_str(), Hex(e.offset).c_str(),
                        Signed(int64_t(e.offset) - int64_t(s.vaddr)).c_str());
            }
        }
    }
}

struct ModuleState {
    std::string path;
    bool present = false, read = false;
    uint64_t size = 0;
    std::string sha;
    ExecutableHeader header;
};

std::vector<ModuleState> ReadModules(const Context& ctx) {
    std::vector<ModuleState> modules;
    for (const char* name : kBundledModules) {
        ModuleState m;
        m.path = std::string("sce_module/") + name;
        const SourceEntry* file = ctx.Find(m.path);
        m.present = file != nullptr;
        if (file) {
            m.size = file->size;
            Bytes bytes;
            m.read = file->size <= kMaxModuleBytes && ctx.source.Read(file->path, kMaxModuleBytes, bytes);
            if (m.read) {
                m.sha = Sha256Of(bytes);
                m.header = ReadExecutableHeader(bytes);
            }
        }
        modules.push_back(std::move(m));
    }
    return modules;
}

void DescribeModules(Lines& out, const Context& ctx, const std::vector<ModuleState>& modules, const LinkedImage& image,
                     bool linked) {
    out.Add("Modules (linked after the executable, in this order, at 64 KiB boundaries):");
    for (const ModuleState& m : modules) {
        if (!m.present) {
            out.Add("  %s: absent%s", m.path.c_str(), ctx.Category("gp") ? " (base package only; legitimate in an update)" : "");
            continue;
        }
        if (!m.read) {
            out.Add("  %s: %" PRIu64 " bytes, not read (%s)", m.path.c_str(), m.size, m.size > kMaxModuleBytes ? "over the limit" : "unreadable");
            continue;
        }
        out.Add("  %s: %" PRIu64 " bytes, SHA-256 %s, %s", m.path.c_str(), m.size, m.sha.c_str(),
                m.header.valid ? "valid SELF and ELF" : ("INVALID: " + m.header.error).c_str());
        const auto it = std::find_if(image.modules.begin(), image.modules.end(),
                                     [&](const LinkedImage::Module& l) { return m.path.ends_with(l.file); });
        if (linked && it != image.modules.end()) {
            out.Add("    mapped at %s, size %s", Hex(it->base).c_str(), Hex(it->size).c_str());
        } else {
            out.Add("    not mapped");
        }
    }
    std::vector<std::string> others;
    for (const SourceEntry& file : ctx.files) {
        if (Lower(file.path).starts_with("sce_module/") &&
            std::none_of(modules.begin(), modules.end(), [&](const ModuleState& m) { return Lower(m.path) == Lower(file.path); })) {
            others.push_back(file.path.substr(11) + " " + std::to_string(file.size));
        }
    }
    std::string line;
    for (const std::string& other : others) {
        if (line.size() + other.size() > 150) {
            out.Add("  other system modules (not linked): " + line);
            line.clear();
        }
        line += (line.empty() ? "" : ", ") + other;
    }
    if (!line.empty()) {
        out.Add("  other system modules (not linked): " + line);
    }
}

void DescribeContent(Lines& out, const Context& ctx) {
    out.Add("Important content:");
    for (const char* name : {"sce_sys/param.sfo", "sce_discmap.plt", "ue4commandline.txt", "Manifest_NonUFSFiles.txt"}) {
        const SourceEntry* file = ctx.Find(name);
        if (!file) {
            out.Add("  %s: absent", name);
            continue;
        }
        Bytes data;
        std::string line = "  " + std::string(name) + ": " + std::to_string(file->size) + " bytes";
        if (file->size <= kMaxHashedBytes && ctx.source.Read(file->path, kMaxHashedBytes, data)) {
            line += ", SHA-256 " + Sha256Of(data);
        }
        out.Add(line);
        if (std::string_view(name) == "ue4commandline.txt" && !data.empty()) {
            out.Add("    text: " + Printable(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), 160));
        }
    }
    for (const std::string& name : BaseFiles()) {
        if (!Lower(name).starts_with(kPakFolder)) {
            continue;
        }
        const SourceEntry* file = ctx.Find(name);
        out.Add("  %s: %s", name.c_str(), file ? (std::to_string(file->size) + " bytes").c_str() : "absent");
    }
    std::map<std::string, size_t> folders;
    std::vector<std::string> root_files;
    for (const SourceEntry& file : ctx.files) {
        const size_t slash = file.path.find('/');
        if (slash != std::string::npos) {
            ++folders[file.path.substr(0, slash)];
        } else if (root_files.size() < kMaxListedFiles) {
            root_files.push_back(file.path + " " + std::to_string(file.size));
        }
    }
    std::string summary;
    for (const auto& [folder, count] : folders) {
        summary += (summary.empty() ? "" : ", ") + folder + "/ " + std::to_string(count);
    }
    out.Add("  folders (files): %s", summary.empty() ? "(none)" : summary.c_str());
    std::string line;
    for (const std::string& file : root_files) {
        if (line.size() + file.size() > 150) {
            out.Add("  top-level files: " + line);
            line.clear();
        }
        line += (line.empty() ? "" : ", ") + file;
    }
    if (!line.empty()) {
        out.Add("  top-level files: " + line);
    }
}

void DescribeArchives(Lines& out, const Context& ctx) {
    out.Add("Archive evidence (footer and index head only):");
    if (ctx.paks.empty()) {
        out.Add("  no archive under %s in this %s", kPakFolder, ctx.source.Kind().c_str());
    }
    for (const PakInfo& pak : ctx.paks) {
        if (!pak.valid) {
            out.Add("  %s: %" PRIu64 " bytes, %s", pak.path.c_str(), pak.size, pak.error.c_str());
            continue;
        }
        out.Add("  %s: %" PRIu64 " bytes, pak v%u, index %s+%s, index SHA-1 %s, mount '%s', %" PRId64 " entries", pak.path.c_str(), pak.size,
                pak.version, Hex(pak.index_offset).c_str(), Hex(pak.index_size).c_str(), pak.index_sha1.c_str(), pak.mount.c_str(),
                pak.entries);
    }
}

void DescribeImage(Lines& out, const std::string& error, const LinkedImage& image, bool linked) {
    out.Add("Mapped image (the launcher's boot image builder, in memory):");
    if (!error.empty()) {
        out.Add("  NOT MAPPED: %s", error.c_str());
        return;
    }
    out.Add("  executable: %s bytes at 0, entry %s", Hex(image.eboot_size).c_str(), Hex(image.entry).c_str());
    if (linked) {
        out.Add("  linked image: %s bytes with %zu modules", Hex(image.bytes.size()).c_str(), image.modules.size());
    } else {
        out.Add("  linked image: not built: %s; the checks below run on the executable alone", image.module_error.c_str());
    }
    out.Add("  %zu segments, %zu relocations (%zu to the image itself), %d FS loads rewritten", image.segments.size(),
            image.relocations, image.relative.size(), image.fs_loads);
    if (linked) {
        out.Add("  imports: %zu, %zu bound to the bundled modules, %zu left to the runtime", image.imports, image.bindings,
                image.unresolved);
    } else {
        out.Add("  imports of the executable: %zu", image.imports);
    }
}

// The loader's checks, run on a relocated copy that only this process holds

struct Verdict {
    std::string title;
    bool evaluated = true, accepted = false;
    std::string detail;
};

std::vector<std::string> BuildKeys() {
    std::vector<std::string> keys;
    for (const GameBuild& build : SupportedBuilds()) {
        const std::string key = CodeBuildKey(build);
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
        }
    }
    return keys;
}

std::string JoinKeys(const std::vector<std::string>& keys) {
    std::string text;
    for (const std::string& key : keys) {
        text += (text.empty() ? "" : ", ") + key;
    }
    return text;
}

using KeyedCheck = std::function<int(const char* key, char* error, size_t size)>;

// One verdict per distinct result over the build-keyed tables.
Verdict Keyed(const char* title, const KeyedCheck& check) {
    std::map<std::string, std::vector<std::string>> by_result;
    std::vector<std::string> order;
    bool any = false;
    for (const std::string& key : BuildKeys()) {
        char error[200] = "";
        const int ok = check(key.c_str(), error, sizeof(error));
        any = any || ok;
        const std::string result = ok ? "holds" : error;
        if (!by_result.count(result)) {
            order.push_back(result);
        }
        by_result[result].push_back(key);
    }
    Verdict v{title, true, any, ""};
    for (const std::string& result : order) {
        v.detail += (v.detail.empty() ? "" : "; ") + std::string("tables of ") + JoinKeys(by_result[result]) + ": " + result;
    }
    return v;
}

using Check = std::function<int(char* error, size_t size)>;

Verdict Single(const char* title, const Check& check, const std::string& site) {
    char error[200] = "";
    const bool ok = check(error, sizeof(error)) != 0;
    return {title, true, ok, ok ? site : error};
}

size_t SitesHolding(std::span<const uint8_t> image, const SotbPatchSite* sites, size_t count) {
    size_t held = 0;
    for (size_t i = 0; i < count; ++i) {
        held += uint64_t(sites[i].offset) + sites[i].length <= image.size() &&
                std::memcmp(image.data() + sites[i].offset, sites[i].bytes, sites[i].length) == 0;
    }
    return held;
}

std::string Holding(std::span<const uint8_t> image, const SotbPatchSite* sites, size_t count) {
    return std::to_string(SitesHolding(image, sites, count)) + " of " + std::to_string(count) + " checked sites hold their bytes";
}

int RefuseOverlay(const char*, uint64_t, const SotbPromptPatch*, size_t) { return -1; }

struct PakReader {
    const GameSource* source;
    std::string path;
};

int ReadPak(void* user, uint64_t offset, void* buffer, size_t length) {
    const PakReader* pak = static_cast<const PakReader*>(user);
    Bytes data;
    if (!pak->source->ReadRange(pak->path, offset, length, data) || data.size() != length) {
        return -1;
    }
    std::memcpy(buffer, data.data(), length);
    return 0;
}

// The production validator of the Xbox prompt overlay against the source's pak, without installing anything: it ends
// with an overlay that refuses, which it reaches only after every size, index and texture check held.
Verdict PromptsVerdict(const Context& ctx) {
    Verdict v{"Controller prompts (pak texture overlay)", false, false, ""};
    for (size_t i = 0; i < sotb_prompt_set_count; ++i) {
        const SotbPromptSet& set = sotb_prompt_sets[i];
        std::string guest = set.guest_path;
        if (guest.starts_with("/app0/")) {
            guest.erase(0, 6);
        }
        const SourceEntry* file = ctx.Find(guest);
        std::string result;
        if (!file) {
            result = guest + " not in this " + ctx.source.Kind();
        } else {
            v.evaluated = true;
            PakReader reader{&ctx.source, file->path};
            const SotbPromptSource source{ReadPak, file->size, &reader};
            const SotbPromptResult outcome = sotb_prompts_install(&set, &source, RefuseOverlay);
            const bool ok = outcome.status == SOTB_PROMPTS_OVERLAY_REFUSED;
            v.accepted = v.accepted || ok;
            result = ok ? "holds" : std::string(sotb_prompts_status_text(outcome.status)) + (outcome.asset ? std::string(" (") + outcome.asset + ")" : "");
        }
        v.detail += (v.detail.empty() ? "" : "; ") + std::string("set of ") + set.build + ": " + result;
    }
    return v;
}

Verdict LzcntCheck(std::span<const uint8_t> image, const LinkedImage& linked, bool modules_linked) {
    Verdict v{"CPU compatibility (LZCNT rewrite)", true, false, ""};
    for (size_t i = 0; i < LZCNT_BUILD_COUNT; ++i) {
        const LzcntBuild& known = lzcnt_builds[i];
        size_t matched = 0;
        const LzcntCompatStatus status = lzcnt_compat_check(image.data(), image.size(), &known, &matched);
        const bool identity = modules_linked && known.image.size == image.size() && known.image.entry == linked.entry;
        v.accepted = v.accepted || (identity && status == LZCNT_COMPAT_OK);
        v.detail += (v.detail.empty() ? "" : "; ") + std::string("table ") + known.build + ": " + std::to_string(matched) + " of " +
                    std::to_string(known.count) + " sites, image identity size " + Hex(known.image.size) + " entry " +
                    Hex(known.image.entry) + " " +
                    (!modules_linked ? std::string("needs the linked modules") : identity ? std::string("matches") : "differs (this image " + Hex(image.size()) + ")");
    }
    return v;
}

struct ValidatorRun {
    std::vector<Verdict> verdicts;
    std::map<std::string, size_t> by_feature; // parity feature key -> verdict index
};

ValidatorRun RunValidators(const Context& ctx, Bytes& image, const LinkedImage& linked, bool mapped, bool modules_linked) {
    ValidatorRun run;
    const std::span<const uint8_t> view(image.data(), image.size());
    const uint8_t* p = image.data();
    const size_t n = image.size();
    const auto add = [&](Verdict v, std::initializer_list<const char*> features) {
        for (const char* feature : features) {
            run.by_feature[feature] = run.verdicts.size();
        }
        if (!mapped && v.title != "Controller prompts (pak texture overlay)") {
            v = {v.title, false, false, "the executable did not map"};
        }
        run.verdicts.push_back(std::move(v));
    };
    add(LzcntCheck(view, linked, modules_linked), {"cpu_compatibility"});
    {
        const GuestPatchReport report = guest_patches_check(p, n);
        add({"Offline Social Features policy", true, report.skipped == 0 && report.applied > 0,
             std::to_string(report.applied) + " of " + std::to_string(report.applied + report.skipped) + " sites hold their bytes"},
            {"offline_policy"});
    }
    add(Keyed("Menu Button Layout", [&](const char* key, char* e, size_t s) { return sotb_menu_buttons_check(p, n, key, e, s); }),
        {"menu_button_layout"});
    add(Keyed("Exit Game", [&](const char* key, char* e, size_t s) { return sotb_exit_game_check(p, n, key, e, s); }), {"exit_game"});
    const SotbPatchSite* sites = nullptr;
    size_t count = psn_notice_sites(&sites);
    add(Single("PlayStation Network notice", [&](char* e, size_t s) { return psn_notice_check(p, n, e, s); },
               "hook " + Hex(PSN_NOTICE_HOOK_SITE) + ", " + Holding(view, sites, count)),
        {"psn_notice"});
    add(Single("True Ultrawide projection", [&](char* e, size_t s) { return guest_ultrawide_check(p, n, e, s); },
               "projection function " + Hex(SOTB_ULTRAWIDE_SITE)),
        {"true_ultrawide"});
    add(Single("Frontend safe area", [&](char* e, size_t s) { return guest_ultrawide_check_safe_area(p, n, e, s); },
               "SGameLayerManager vtable " + Hex(SOTB_GAME_LAYER_VTABLE) + " slot " + std::to_string(SOTB_GAME_LAYER_ARRANGE_SLOT)),
        {"frontend_safe_area"});
    add(Single("HUD anchoring and cutscene fade", [&](char* e, size_t s) { return guest_hud_check(p, n, e, s); },
               "post render " + Hex(SOTB_HUD_POST_RENDER) + ", fade call " + Hex(SOTB_HUD_FADE_CALL)),
        {"hud_anchoring", "cutscene_fade_fix"});
    add(Single("Frontend screen contexts", [&](char* e, size_t s) { return guest_ultrawide_check_frontend(p, n, e, s); },
               "frontend tick " + Hex(SOTB_FRONTEND_TICK) + ", vtable " + Hex(SOTB_FRONTEND_VTABLE)),
        {"title_context", "map_context", "mana_overlay_context", "options_context", "wisdom_of_shadows_context"});
    count = skip_intro_sites(&sites);
    add(Single("Skip Intro", [&](char* e, size_t s) { return skip_intro_check(p, n, e, s); },
               "boot sequence " + Hex(SKIP_INTRO_SITE) + ", " + Holding(view, sites, count)),
        {kSkipIntroKey});
    count = guest_display_sites(&sites);
    add(Single("Display size (Render resolution, Aspect Ratio, Resolution Guard)",
               [&](char* e, size_t s) { return guest_check_display_size(p, n, e, s); }, Holding(view, sites, count - 2)),
        {"render_resolution", "aspect_ratio", "resolution_guard"});
    add(Single("Loading hint layout", [&](char* e, size_t s) { return guest_check_loading_screen_layout(p, n, e, s); },
               Holding(view, sites + count - 2, 2) + ", vtable entry checked"),
        {"loading_hint"});
    add(PromptsVerdict(ctx), {"controller_prompts"});
    return run;
}

void Relocate(Bytes& image, const LinkedImage& linked) {
    const uint64_t base = uint64_t(reinterpret_cast<uintptr_t>(image.data()));
    for (const auto& [target, value] : linked.relative) {
        if (image.size() >= 8 && target <= image.size() - 8) {
            const uint64_t pointer = base + value;
            std::memcpy(image.data() + target, &pointer, sizeof(pointer));
        }
    }
}

// Exploratory search for sites that moved

struct Range {
    uint64_t begin, end;
};

struct Found {
    size_t count = 0;
    std::vector<uint64_t> first;
};

Found Search(std::span<const uint8_t> image, const std::vector<Range>& code, const uint8_t* pattern, size_t length) {
    Found found;
    const std::boyer_moore_horspool_searcher searcher(pattern, pattern + length);
    for (const Range& range : code) {
        const uint8_t* begin = image.data() + range.begin;
        const uint8_t* end = image.data() + range.end;
        for (auto it = std::search(begin, end, searcher); it != end; it = std::search(it + 1, end, searcher)) {
            if (found.first.size() < kMaxCandidates) {
                found.first.push_back(uint64_t(it - image.data()));
            }
            ++found.count;
        }
    }
    return found;
}

struct SiteSearch {
    size_t sites = 0, holding = 0, moved = 0, ambiguous = 0, missing = 0;
    std::vector<int64_t> deltas;
};

std::vector<SotbPatchSite> ExportedSites(std::vector<std::string>& features) {
    std::vector<SotbPatchSite> all;
    const auto add = [&](const char* feature, const SotbPatchSite* sites, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const bool seen = std::any_of(all.begin(), all.end(), [&](const SotbPatchSite& s) {
                return s.offset == sites[i].offset && s.length == sites[i].length && !std::memcmp(s.bytes, sites[i].bytes, s.length);
            });
            if (!seen) {
                all.push_back(sites[i]);
                features.push_back(feature);
            }
        }
    };
    const SotbPatchSite* sites = nullptr;
    size_t count = skip_intro_sites(&sites);
    add("Skip Intro", sites, count);
    count = psn_notice_sites(&sites);
    add("PSN notice", sites, count);
    count = guest_display_sites(&sites);
    add("Display", sites, count);
    for (const std::string& key : BuildKeys()) {
        SotbPatchSite buffer[8];
        count = sotb_exit_game_sites(key.c_str(), buffer, std::size(buffer));
        add("Exit Game", buffer, count);
        count = sotb_menu_buttons_sites(key.c_str(), buffer, std::size(buffer));
        add("Menu Button Layout", buffer, count);
    }
    return all;
}

SiteSearch DescribeSearch(Lines& out, std::span<const uint8_t> image, const LinkedImage& linked) {
    out.Add("Exploratory site search (exact production bytes searched in the executable code; NOT production acceptance):");
    std::vector<Range> code;
    for (const LinkedImage::Segment& s : linked.segments) {
        if ((s.flags & 1) && s.address < linked.eboot_size) {
            code.push_back({s.address, std::min<uint64_t>(s.address + s.size, std::min<uint64_t>(linked.eboot_size, image.size()))});
        }
    }
    SiteSearch result;
    std::vector<std::string> features;
    const std::vector<SotbPatchSite> sites = ExportedSites(features);
    for (size_t i = 0; i < sites.size(); ++i) {
        const SotbPatchSite& site = sites[i];
        ++result.sites;
        const std::string name = features[i] + ": " + site.name + " (" + std::to_string(site.length) + " bytes, production " + Hex(site.offset) + ")";
        if (uint64_t(site.offset) + site.length <= image.size() && !std::memcmp(image.data() + site.offset, site.bytes, site.length)) {
            ++result.holding;
            out.Add("  %s: holds", name.c_str());
            continue;
        }
        const Found exact = Search(image, code, site.bytes, site.length);
        std::string line = "  " + name + ": ";
        if (exact.count == 1) {
            ++result.moved;
            const int64_t delta = int64_t(exact.first[0]) - int64_t(site.offset);
            result.deltas.push_back(delta);
            out.Add(line + "MOVED, unique exact match at " + Hex(exact.first[0]) + " (" + Signed(delta) + ")");
            continue;
        }
        if (exact.count > 1) {
            ++result.ambiguous;
            line += "AMBIGUOUS, " + std::to_string(exact.count) + " exact matches, first:";
            for (const uint64_t at : exact.first) {
                line += " " + Hex(at);
            }
            out.Add(line);
            continue;
        }
        ++result.missing;
        if (site.length < 2 * kMinPartialRun) {
            out.Add(line + "MISSING, no exact match (too short for partial matching)");
            continue;
        }
        const size_t half = site.length / 2;
        const Found head = Search(image, code, site.bytes, half);
        const Found tail = Search(image, code, site.bytes + half, site.length - half);
        line += "MISSING; partial: first " + std::to_string(half) + " bytes x" + std::to_string(head.count) + ", last " +
                std::to_string(site.length - half) + " bytes x" + std::to_string(tail.count);
        for (const uint64_t at : head.first) {
            line += " " + Hex(at);
        }
        out.Add(line);
    }
    return result;
}


struct FeatureRow {
    const char* key;
    const char* title;
};

// The per-build features of the parity contract (tests/build_parity_tests.cpp) that depend on the executable or the pak.
constexpr FeatureRow kPatchFeatures[] = {
    {"cpu_compatibility", "MOVBE, BMI1 and LZCNT compatibility"},
    {"offline_policy", "offline Social Features policy"},
    {"controller_prompts", "in-game controller prompts"},
    {"menu_button_layout", "Menu Button Layout"},
    {"exit_game", "Exit Game"},
    {"psn_notice", "PlayStation Network notice removal"},
    {kSkipIntroKey, "Skip Intro"},
    {"render_resolution", "render resolution"},
    {"aspect_ratio", "aspect ratio"},
    {"resolution_guard", "Resolution Guard"},
    {"loading_hint", "loading hint layout"},
    {"true_ultrawide", "True Ultrawide Hor+ projection"},
    {"frontend_safe_area", "frontend safe area"},
    {"title_context", "title context"},
    {"map_context", "Map context"},
    {"mana_overlay_context", "Mana overlay context"},
    {"options_context", "Options context"},
    {"wisdom_of_shadows_context", "Wisdom of Shadows context"},
    {"hud_anchoring", "HUD component anchoring"},
    {"cutscene_fade_fix", "full-screen cutscene fade"},
};

void DescribeInventory(Lines& out, const ValidatorRun& run, size_t& mapped) {
    out.Add("Build-specific features (the parity contract's per-build features; mapped = its loader check accepts this image):");
    mapped = 0;
    std::string line;
    for (const FeatureRow& feature : kPatchFeatures) {
        const auto it = run.by_feature.find(feature.key);
        const Verdict* v = it == run.by_feature.end() ? nullptr : &run.verdicts[it->second];
        const char* state = !v || !v->evaluated ? "not evaluated" : v->accepted ? "mapped" : "NOT mapped";
        mapped += v && v->evaluated && v->accepted;
        const std::string item = std::string(feature.key) + " " + state;
        if (line.size() + item.size() > 150) {
            out.Add("  " + line);
            line.clear();
        }
        line += (line.empty() ? "" : ", ") + item;
    }
    out.Add("  " + line);
    out.Add("  build_profile, build_fingerprint: launcher table identity (see Known-build comparison)");
    out.Add("  localization: no build-specific table (every supported build uses the same text)");
}

void DescribeValidators(Lines& out, const ValidatorRun& run, size_t& accepted, size_t& evaluated) {
    out.Add("Production validators (the loader's own checks, read-only on a relocated in-memory copy):");
    accepted = evaluated = 0;
    for (const Verdict& v : run.verdicts) {
        evaluated += v.evaluated;
        accepted += v.evaluated && v.accepted;
        out.Add("  %s: %s: %s", v.title.c_str(), !v.evaluated ? "NOT EVALUATED" : v.accepted ? "accepted" : "REJECTED", v.detail.c_str());
    }
}

void DescribeRelations(Lines& out, const ValidatorRun& run, const SiteSearch& search) {
    out.Add("Relationships:");
    const auto state = [&](const char* feature) {
        const Verdict& v = run.verdicts[run.by_feature.at(feature)];
        return v.accepted ? "holds" : "unverified (its check rejected the image)";
    };
    out.Add("  HUD fade call targets UCanvas::DrawItem %s: %s", Hex(SOTB_HUD_DRAW_ITEM).c_str(), state("cutscene_fade_fix"));
    out.Add("  Menu Button Layout key sites reference each other's strings: %s", state("menu_button_layout"));
    out.Add("  safe area and loading hint share SCompoundWidget::OnArrangeChildren: %s / %s", state("frontend_safe_area"),
            state("loading_hint"));
    out.Add("  LZCNT sites do not overlap and each rewrites to a stub: %s", state("cpu_compatibility"));
    const bool caves = GUEST_CAVE_EXIT_GAME < GUEST_CAVE_PSN_NOTICE && GUEST_CAVE_PSN_NOTICE < GUEST_CAVE_ULTRAWIDE &&
                       GUEST_CAVE_ULTRAWIDE < GUEST_CAVE_HUD && GUEST_CAVE_HUD < GUEST_PATCH_CAVE_SIZE;
    out.Add("  shared code cave slices ordered and disjoint (Exit Game %u, PSN %u, Hor+ %u, HUD %u of %u): %s", GUEST_CAVE_EXIT_GAME,
            GUEST_CAVE_PSN_NOTICE, GUEST_CAVE_ULTRAWIDE, GUEST_CAVE_HUD, GUEST_PATCH_CAVE_SIZE, caves ? "holds" : "BROKEN");
    if (search.deltas.empty()) {
        out.Add("  moved sites: none found by exact search");
        return;
    }
    std::map<int64_t, size_t> deltas;
    for (const int64_t delta : search.deltas) {
        ++deltas[delta];
    }
    std::string text;
    for (const auto& [delta, count] : deltas) {
        text += (text.empty() ? "" : ", ") + Signed(delta) + " x" + std::to_string(count);
    }
    out.Add("  moved sites share %zu displacement%s: %s", deltas.size(), deltas.size() == 1 ? "" : "s", text.c_str());
}

} // namespace

const char* SourceClassName(SourceClass kind) {
    switch (kind) {
    case SourceClass::kSupported: return "supported Shadow of the Beast build";
    case SourceClass::kUnsupported: return "probable unsupported Shadow of the Beast build";
    case SourceClass::kUpdate: return "Shadow of the Beast update package";
    case SourceClass::kIncomplete: return "incomplete Shadow of the Beast source";
    case SourceClass::kOtherGame: return "different game";
    case SourceClass::kInvalid: return "invalid or malformed source";
    }
    return "unknown";
}

std::unique_ptr<GameSource> OpenFolderSource(const fs::path& folder) {
    return std::make_unique<FolderSource>(folder);
}

std::unique_ptr<GameSource> OpenPackageSource(Package& package) {
    return std::make_unique<PackageSource>(package);
}

SourceClassification ClassifySource(const GameSource& source, const std::string& sfo) {
    Context ctx(source, sfo);
    Classify(ctx);
    return ctx.cls;
}

bool ExecutableDiffers(const GameSource& source, const GameBuild& build) {
    Bytes eboot;
    return source.Read("eboot.bin", kMaxExecutableBytes, eboot) && !IsBuildExecutable(build, Sha256Of(eboot));
}

UnsupportedReport DiagnoseUnsupportedBuild(const GameSource& source, const std::string& sfo) {
    UnsupportedReport report;
    Context ctx(source, sfo);
    Classify(ctx);
    report.classification = ctx.cls;
    if (!ctx.cls.Shadow() || ctx.cls.kind == SourceClass::kSupported) {
        return report;
    }
    ctx.LoadEboot();
    const ExecutableHeader header = ctx.eboot_read ? ReadExecutableHeader(ctx.eboot) : ExecutableHeader{};
    const std::vector<std::string> problems = header.valid ? LayoutProblems(header, ctx.eboot.size()) : std::vector<std::string>{};

    Lines out;
    out.Add(kDiagnosticsBegin);
    out.Add("This Shadow of the Beast build is not supported: nothing was installed and no game file was changed.");
    DescribeSource(out, ctx);
    DescribeMetadata(out, ctx);
    DescribeComparison(out, ctx, header);
    DescribeExecutable(out, ctx, header, problems);
    if (header.valid) {
        DescribeLayout(out, header);
    }
    const std::vector<ModuleState> modules = ReadModules(ctx);

    LinkedImage linked;
    std::string map_error = !ctx.eboot_read ? "eboot.bin was not read"
                            : !header.valid  ? "eboot.bin is not a mappable PS4 executable: " + header.error
                            : !problems.empty() ? "the executable's layout is unsafe to map: " + problems.front()
                                                : std::string();
    if (map_error.empty()) {
        map_error = LinkGameImage(
            ctx.eboot,
            [&](const char* file, Bytes& bytes) {
                const SourceEntry* entry = ctx.Find(std::string("sce_module/") + file);
                return entry && entry->size <= kMaxModuleBytes && ctx.source.Read(entry->path, kMaxModuleBytes, bytes);
            },
            linked);
        if (map_error.empty() && linked.bytes.size() > kMaxImageBytes) {
            map_error = "the linked image exceeds the " + Hex(kMaxImageBytes) + " limit";
            linked = {};
        }
    }
    ctx.eboot = {};
    const bool mapped = map_error.empty();
    const bool modules_linked = mapped && linked.module_error.empty();
    DescribeModules(out, ctx, modules, linked, modules_linked);
    DescribeContent(out, ctx);
    DescribeArchives(out, ctx);
    DescribeImage(out, map_error, linked, modules_linked);

    Relocate(linked.bytes, linked);
    const ValidatorRun run = RunValidators(ctx, linked.bytes, linked, mapped, modules_linked);
    size_t mapped_features = 0, accepted = 0, evaluated = 0;
    DescribeInventory(out, run, mapped_features);
    DescribeValidators(out, run, accepted, evaluated);
    SiteSearch search;
    if (mapped) {
        search = DescribeSearch(out, std::span<const uint8_t>(linked.bytes.data(), linked.bytes.size()), linked);
    } else {
        out.Add("Exploratory site search: not run, the executable did not map");
    }
    DescribeRelations(out, run, search);

    const std::vector<std::string> missing = MissingBaseFiles(ctx);
    const bool update = ctx.Category("gp");
    const bool identity = SfoValid(sfo) && !SfoString(sfo, "TITLE_ID").empty() && !SfoString(sfo, "CONTENT_ID").empty() &&
                          !SfoString(sfo, "APP_VER").empty() && !SfoString(sfo, "VERSION").empty() && ctx.eboot_read;
    size_t modules_mapped = 0, modules_absent = 0, modules_invalid = 0;
    for (const ModuleState& m : modules) {
        modules_absent += !m.present;
        modules_invalid += m.present && (!m.read || !m.header.valid);
        modules_mapped += m.present && m.read && m.header.valid && modules_linked;
    }
    out.Add("Package limits:");
    out.Add("  this is a %s", PackageKind(ctx));
    for (const std::string& name : missing) {
        out.Add("  %s: absent%s", name.c_str(), update ? " (base package only; legitimate in an update)" : " (missing from a base game)");
    }
    if (!missing.empty()) {
        out.Add(update ? "  not evaluable from this update: LZCNT identity (needs the linked modules) and controller prompts (needs pakchunk0)"
                       : "  the checks that need the missing files are reported as not evaluated or rejected");
    }

    out.Add("Overall diagnostic result:");
    out.Add("  Identity: %s", identity ? "complete" : "incomplete");
    out.Add("  Executable mapping: %s", modules_linked ? "complete" : mapped ? "executable only (modules not linked)" : "incomplete");
    out.Add("  Modules: %zu mapped, %zu absent, %zu invalid", modules_mapped, modules_absent, modules_invalid);
    out.Add("  Features: %zu mapped out of %zu investigated", mapped_features, std::size(kPatchFeatures));
    out.Add("  Production validators: %zu accepted out of %zu (%zu evaluated)", accepted, run.verdicts.size(), evaluated);
    out.Add("  Ambiguous sites: %zu", search.ambiguous);
    out.Add("  Missing sites: %zu", search.missing);
    out.Add("  Package limits: %s, %zu base files absent", update ? "update" : missing.empty() ? "base" : "incomplete base", missing.size());

    out.Add("Still required for support:");
    if (ctx.eboot_read) {
        out.Add("  - add a build to launcher/game_profile.cpp and scripts/game_profiles.py: TITLE_ID %s, CONTENT_ID %s, APP_VER %s, "
                "VERSION %s",
                Shown(SfoString(sfo, "TITLE_ID")).c_str(), Shown(SfoString(sfo, "CONTENT_ID")).c_str(),
                Shown(SfoString(sfo, "APP_VER")).c_str(), Shown(SfoString(sfo, "VERSION")).c_str());
        out.Add("  - record eboot SHA-256 %s and its executable profile (size, entry, extent, code and data ranges)", ctx.eboot_sha.c_str());
    } else {
        out.Add("  - obtain this build's eboot.bin: nothing executable could be analysed");
    }
    if (!modules_linked && mapped) {
        out.Add("  - obtain the base package's sce_module files to link the image (%s)", linked.module_error.c_str());
    }
    for (const Verdict& v : run.verdicts) {
        if (!v.evaluated) {
            out.Add("  - evaluate %s with the files this source lacks", v.title.c_str());
        } else if (!v.accepted) {
            out.Add("  - locate or verify %s: %s", v.title.c_str(), v.detail.c_str());
        }
    }
    out.Add("  - add the build key to the Menu Button Layout, Exit Game and controller prompt tables once their sites are verified");
    out.Add("  - prepare the build and pass tools/test.ps1 -Filter build-parity, then validate the runtime on hardware");
    out.Add("This build stays unsupported; no executable patch or hook was applied and no game file was changed.");

    report.recognized = true;
    report.lines = out.Finish();
    return report;
}

} // namespace Sotb

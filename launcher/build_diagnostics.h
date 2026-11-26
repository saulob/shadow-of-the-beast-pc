// Diagnostics for a Shadow of the Beast build the port does not support: the selection is classified, refused, and what
// is needed to add its support is written to the log. Strictly read-only, and a result never makes a build supported.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pkg_extract.h"

namespace Sotb {

struct GameBuild;

struct SourceEntry {
    std::string path; // relative, '/' separated
    uint64_t size = 0;
};

// A read-only view of the game the player selected: an extracted folder or a package.
class GameSource {
public:
    virtual ~GameSource() = default;
    virtual std::string Kind() const = 0;
    // At most `limit` files, sorted by path.
    virtual std::vector<SourceEntry> Files(size_t limit) const = 0;
    // The file's bytes; false when it is absent, larger than `max_bytes` or unreadable.
    virtual bool Read(const std::string& path, uint64_t max_bytes, std::vector<uint8_t>& out) const = 0;
    virtual bool ReadRange(const std::string& path, uint64_t offset, size_t length, std::vector<uint8_t>& out) const = 0;
};

std::unique_ptr<GameSource> OpenFolderSource(const std::filesystem::path& folder);
// `package` must have loaded its files.
std::unique_ptr<GameSource> OpenPackageSource(Package& package);

namespace diagnostics {
inline constexpr uint64_t kMaxExecutableBytes = 128ull << 20;
inline constexpr uint64_t kMaxModuleBytes = 64ull << 20;
inline constexpr uint64_t kMaxSfoBytes = 1ull << 20;
inline constexpr uint64_t kMaxHashedBytes = 32ull << 20;
inline constexpr uint64_t kMaxImageBytes = 256ull << 20;
inline constexpr size_t kMaxSourceFiles = 5000;
inline constexpr size_t kMaxListedFiles = 40;
inline constexpr size_t kMaxArchiveProbes = 16;
inline constexpr size_t kMaxCandidates = 4;
inline constexpr size_t kMinPartialRun = 8;
inline constexpr size_t kMaxLineLength = 200;
inline constexpr size_t kMaxReportLines = 400;
// Bug Report sends the last 64 KiB of the log; the whole block has to fit well inside it.
inline constexpr size_t kMaxReportBytes = 30 * 1024;
} // namespace diagnostics

enum class SourceClass { kSupported, kUnsupported, kUpdate, kIncomplete, kOtherGame, kInvalid };

const char* SourceClassName(SourceClass kind);

struct SourceClassification {
    SourceClass kind = SourceClass::kOtherGame;
    const GameBuild* build = nullptr;  // kSupported: the build; otherwise the one param.sfo names exactly, if any
    std::string eboot_sha256;          // empty when eboot.bin was not read
    std::vector<std::string> evidence; // why, for the log
    bool Shadow() const { return kind != SourceClass::kOtherGame && kind != SourceClass::kInvalid; }
};

// What the selection is, from param.sfo (`sfo`, empty when it has none), its files and, when param.sfo names a supported
// build, the executable's hash. Reads only small files and archive footers otherwise.
SourceClassification ClassifySource(const GameSource& source, const std::string& sfo);

struct UnsupportedReport {
    bool recognized = false;           // Shadow of the Beast, not a supported build: `lines` holds the block
    SourceClassification classification;
    std::vector<std::string> lines;
};

inline constexpr char kDiagnosticsBegin[] = "===== Unsupported Shadow of the Beast build diagnostic =====";
inline constexpr char kDiagnosticsEnd[] = "===== End unsupported build diagnostic =====";

// Classifies the selection and, when it is a Shadow of the Beast build the port does not support, describes it with the
// loader's own patch checks run on its image. Nothing is written.
UnsupportedReport DiagnoseUnsupportedBuild(const GameSource& source, const std::string& sfo);

// True when the source carries an eboot.bin that is not `build`'s executable; false when it carries none.
bool ExecutableDiffers(const GameSource& source, const GameBuild& build);

} // namespace Sotb

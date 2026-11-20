#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace Sotb {

inline constexpr char kGameTitle[] = "Shadow of the Beast";
inline constexpr char kPortRepository[] = "github.com/saulob/shadow-of-the-beast-pc";
inline constexpr char kAuthorUrl[] = "https://github.com/saulob";
inline constexpr char kSponsorUrl[] = "https://github.com/sponsors/saulob";
inline constexpr char kIssuesUrl[] = "https://github.com/saulob/shadow-of-the-beast-pc/issues";
// The production bug report service; SOTB_BUG_REPORT_URL overrides it for development and tests.
inline constexpr char kBugReportServiceUrl[] = "https://shadow-bug-reports.saulobenigno.workers.dev";

// A package of a build, by its param.sfo APP_VER and VERSION.
struct PackageLayer {
    const char* app_ver;
    const char* sfo_version;
    bool update;
};

struct TextLanguage {
    std::string_view key;
    std::string_view name;
    int ps4_id;
    std::string_view archive;
};

struct GameBuild {
    const char* title_id;
    const char* content_id;
    const char* version;
    const char* folder;
    const char* eboot_sha256;
    const char* regional_archive;
    std::span<const char* const> extra_files;
    const char* label;
    const char* region;
    std::span<const PackageLayer> layers;
    std::span<const TextLanguage> text_languages;
    bool english_voices = false;
    const char* code_build = "";
    std::span<const char* const> alternate_eboots = {};
};

bool IsBuildExecutable(const GameBuild& build, std::string_view sha256);
std::span<const GameBuild> SupportedBuilds();
const PackageLayer& FinishedLayer(const GameBuild& build);
std::string BuildMetadataText(const GameBuild& build);
inline bool IsUpdateBuild(const GameBuild& build) { return build.layers.size() > 1; }
const GameBuild* BaseBuildOf(const GameBuild& build, std::span<const GameBuild> builds = SupportedBuilds());
bool RestrictedToNativeResolution(const GameBuild* build);
std::span<const TextLanguage> TextLanguagesOf(const GameBuild* build);
const TextLanguage& ResolveTextLanguage(std::span<const TextLanguage> languages, std::string_view key);
std::string SupportedBuildsText();
std::string CodeBuildKey(const GameBuild& build);

inline constexpr const char* kSharedGameFiles[] = {
    "eboot.bin",
    "sce_discmap.plt",
    "sce_module/libc.prx",
    "sce_module/libSceFios2.prx",
    "sce_sys/param.sfo",
};
// Linked after the eboot in this order: libc.prx needs libSceFios2.prx.
inline constexpr const char* kBundledModules[] = {"libc.prx", "libSceFios2.prx"};
inline constexpr char kPreparedMarker[] = ".sotb_view.json";

} // namespace Sotb

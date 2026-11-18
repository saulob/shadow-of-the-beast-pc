#include "game_profile.h"

#include <algorithm>
#include <string>

namespace Sotb {
namespace {

constexpr PackageLayer kBaseLayers[] = {{"01.00", "01.01", false}};
constexpr const char* kPakFiles[] = {
    "beast/content/paks/pakchunk1-ps4.pak", "beast/content/paks/pakchunk2-ps4.pak",
    "beast/content/paks/pakchunk3-ps4.pak", "beast/content/paks/pakchunk4-ps4.pak",
    "beast/content/paks/pakchunk5-ps4.pak", "beast/content/paks/pakchunk6-ps4.pak",
    "beast/content/paks/pakchunk7-ps4.pak", "beast/content/paks/pakchunk8-ps4.pak",
};

constexpr GameBuild kGameBuilds[] = {
    {.title_id = "CUSA03762",
     .content_id = "HP9000-CUSA03762_00-SHADOWOFBEAST0KR",
     .version = "1.00",
     .folder = "CUSA03762-1.00",
     .eboot_sha256 = "ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023",
     .regional_archive = "beast/content/paks/pakchunk0-ps4.pak",
     .extra_files = kPakFiles,
     .label = "CUSA03762 1.00 Korea",
     .region = "Korea",
     .layers = kBaseLayers,
     .text_languages = {},
     .code_build = "CUSA03762-1.00"},
};

constexpr TextLanguage kFallbackLanguage{"english", "English", 1, ""};

} // namespace

bool IsBuildExecutable(const GameBuild& build, std::string_view sha256) {
    return sha256 == build.eboot_sha256 ||
           std::any_of(std::begin(build.alternate_eboots), std::end(build.alternate_eboots),
                       [&](const char* alternate) { return sha256 == alternate; });
}

std::span<const GameBuild> SupportedBuilds() { return kGameBuilds; }

const PackageLayer& FinishedLayer(const GameBuild& build) { return build.layers.back(); }

std::string BuildMetadataText(const GameBuild& build) {
    const PackageLayer& layer = FinishedLayer(build);
    std::string app = layer.app_ver, version = layer.sfo_version;
    if (app.size() == 5 && app[0] == '0') app.erase(0, 1);
    if (version.size() == 5 && version[0] == '0') version.erase(0, 1);
    return "APP_VER " + app + ", VERSION " + version;
}

const GameBuild* BaseBuildOf(const GameBuild& build, std::span<const GameBuild> builds) {
    return IsUpdateBuild(build) && !builds.empty() ? &builds.front() : nullptr;
}

bool RestrictedToNativeResolution(const GameBuild*) { return true; }

std::span<const TextLanguage> TextLanguagesOf(const GameBuild* build) {
    if (build && !build->text_languages.empty()) {
        return build->text_languages;
    }
    return {&kFallbackLanguage, 1};
}

const TextLanguage& ResolveTextLanguage(std::span<const TextLanguage> languages, std::string_view key) {
    for (const TextLanguage& language : languages) {
        if (language.key == key) {
            return language;
        }
    }
    return languages.front();
}

std::string SupportedBuildsText() { return "CUSA03762 base 01.00"; }

std::string CodeBuildKey(const GameBuild& build) {
    return std::string(build.code_build && *build.code_build ? build.code_build : build.folder);
}

} // namespace Sotb

#include "update_config.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update_restart_arguments.h"
#include "updater/update_paths.h"
#include "updater/update_platform.h"

namespace Sotb {
namespace {

constexpr const char* kReleasesUrl =
    "https://api.github.com/repos/saulob/shadow-of-the-beast-pc/releases/latest";
constexpr const char* kDownloadUrlPrefix = "https://github.com/saulob/shadow-of-the-beast-pc/releases/download/";

std::optional<std::string> Environment(const char* name) {
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

bool TestMode() {
    const std::optional<std::string> value = Environment("SOTB_UPDATE_TEST");
    return value && *value == "1";
}

std::optional<std::string> TestVariable(const char* name) {
    if (!TestMode()) {
        return std::nullopt;
    }
    return Environment(name);
}

} // namespace

trg::update::UpdateConfig MakeUpdateConfig(const std::filesystem::path& install_dir) {
    using trg::update::SemanticVersion;
    using trg::update::UpdatePackageKind;

    trg::update::UpdateConfig config;
    config.current_version = SOTB_PORT_VERSION;
    config.releases_url = kReleasesUrl;
    config.download_url_prefix = kDownloadUrlPrefix;
    config.user_agent = std::string("ShadowOfTheBeastPC-Updater/") + SOTB_PORT_VERSION;
    config.install_dir = install_dir;
    config.restart_arguments = UpdateRestartArguments(trg::update::CurrentProcessArguments());

    config.package_kind = UpdatePackageKind::kZip;
    config.asset_name = [](const SemanticVersion& version) {
        return "ShadowOfTheBeastPC-v" + version.CoreString() + "-windows-x64.zip";
    };
    config.checksum_asset_name = [](const SemanticVersion& version) {
        return "ShadowOfTheBeastPC-v" + version.CoreString() + "-windows-x64.zip.sha256";
    };
    config.restart_executable = install_dir / "shadow_of_the_beast.exe";
    // Every later package must keep these, or the installed updater refuses it.
    config.required_staged_files = {"shadow_of_the_beast.exe", "trg-updater.exe", "SDL3.dll", "libc++.dll"};
    config.install_supported = std::filesystem::exists(install_dir / "trg-updater.exe");
    if (!config.install_supported) {
        config.install_unsupported_reason =
            "The update helper is missing from this installation. Reinstall this release to enable automatic updates.";
    }

    config.log = [](std::string_view text) { std::printf("Updater: %.*s\n", int(text.size()), text.data()); };

    if (const std::optional<std::string> version = TestVariable("SOTB_UPDATE_TEST_CURRENT_VERSION"); version) {
        config.current_version = *version;
        std::printf("Updater test mode: reporting version %s\n", config.current_version.c_str());
    }
    if (const std::optional<std::string> json = TestVariable("SOTB_UPDATE_TEST_RELEASE_JSON"); json) {
        config.release_json_override = trg::update::PathFromUtf8(*json);
    }
    if (const std::optional<std::string> hash = TestVariable("SOTB_UPDATE_TEST_ASSET_SHA256"); hash) {
        config.sha256_override = *hash;
    }
    return config;
}

} // namespace Sotb

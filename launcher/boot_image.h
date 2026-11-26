// The loader's boot image (boot.bin, content.bin and the image.json report) built from a prepared game tree: the
// eboot mapped, the game's bundled modules linked after it, every other import left by name for the host runtime.
// Same output as scripts/prepare_image.py.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Sotb {

// Must match the signatures src/loader.c checks.
inline constexpr std::string_view kBootImageMagic = "SOTBBOT1";
inline constexpr std::string_view kContentProfileMagic = "SOTBCONT";
inline constexpr std::size_t kContentProfileSize = 28;

// Writes the three files into `out` (created when missing). Empty on success, else the reason.
std::string BuildBootImage(const std::filesystem::path& game, const std::filesystem::path& out);

// The memory image boot.bin carries, built the same way but only in memory, before the loader relocates it.
struct LinkedImage {
    struct Segment {
        uint64_t address = 0, size = 0, flags = 0;
    };
    struct Module {
        std::string file;
        uint64_t base = 0, size = 0;
        std::string sha256;
    };
    std::vector<uint8_t> bytes;
    uint64_t entry = 0, eboot_size = 0;
    std::vector<Segment> segments;
    std::vector<Module> modules;
    // (target, image offset): the relocations the loader resolves to the image's own address plus the offset.
    std::vector<std::pair<uint64_t, uint64_t>> relative;
    size_t relocations = 0, imports = 0, bindings = 0, unresolved = 0;
    int fs_loads = 0;
    // Why the bundled modules are not linked; the image is then the executable's alone.
    std::string module_error;
};

// False when the bundled module (a kBundledModules name) is not available.
using ModuleReader = std::function<bool(const char* file, std::vector<uint8_t>& bytes)>;

// Empty when the executable maps (the modules may still be missing), else why it does not. Nothing is written.
std::string LinkGameImage(std::span<const uint8_t> eboot, const ModuleReader& read_module, LinkedImage& out);

} // namespace Sotb

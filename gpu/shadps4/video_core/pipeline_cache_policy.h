// SPDX-License-Identifier: GPL-2.0-or-later
// Vulkan-independent rules of the persistent pipeline cache: blob integrity, host identity,
// atomic writes and which files belong to the cache.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace PipelineCachePolicy {

constexpr std::size_t MaxBlobBytes = 256u << 20;

inline std::uint64_t Fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto byte : bytes) {
        hash = (hash ^ byte) * 0x100000001b3ull;
    }
    return hash;
}

namespace Detail {
template <typename T>
void Append(std::vector<std::uint8_t>& bytes, T value) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
}
template <typename T>
T At(std::span<const std::uint8_t> bytes, std::size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
} // namespace Detail

constexpr std::uint32_t SealMagic = 0x42534253; // "SBSB"
constexpr std::uint32_t SealVersion = 1;
constexpr std::size_t SealSize = 24;

// The seal trails the payload, so a truncated, zero-filled or foreign file never validates.
inline bool Seal(std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > MaxBlobBytes) {
        return false;
    }
    const auto size = std::uint64_t(bytes.size());
    const auto hash = Fnv1a(bytes);
    Detail::Append(bytes, SealMagic);
    Detail::Append(bytes, SealVersion);
    Detail::Append(bytes, size);
    Detail::Append(bytes, hash);
    return true;
}

// On failure `bytes` is left untouched.
inline bool Unseal(std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < SealSize || bytes.size() - SealSize > MaxBlobBytes) {
        return false;
    }
    const std::size_t payload = bytes.size() - SealSize;
    const std::span<const std::uint8_t> all{bytes};
    if (Detail::At<std::uint32_t>(all, payload) != SealMagic ||
        Detail::At<std::uint32_t>(all, payload + 4) != SealVersion ||
        Detail::At<std::uint64_t>(all, payload + 8) != payload ||
        Detail::At<std::uint64_t>(all, payload + 16) != Fnv1a(all.first(payload))) {
        return false;
    }
    bytes.resize(payload);
    return true;
}

constexpr std::array<std::string_view, 4> BlobExtensions{"meta", "spv", "key", "bin"};

inline bool IsBlobExtension(std::string_view extension) {
    for (const auto known : BlobExtensions) {
        if (extension == known) {
            return true;
        }
    }
    return false;
}

// "<name>.<blob extension>.tmp" only: any other .tmp file in the directory is not ours.
inline bool IsStaleTemp(const std::filesystem::path& path) {
    if (path.extension() != ".tmp") {
        return false;
    }
    auto inner = path.stem().extension().string();
    if (!inner.empty() && inner.front() == '.') {
        inner.erase(0, 1);
    }
    return IsBlobExtension(inner);
}

inline bool IsCacheBlob(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(0, 1);
    }
    return IsBlobExtension(extension);
}

inline bool IsCacheArtifact(const std::filesystem::path& path) {
    return IsCacheBlob(path) || IsStaleTemp(path);
}

// Writes beside the target and renames into place. A write that fails at any step leaves the
// previous file untouched and removes its own temporary.
inline bool WriteFileAtomically(const std::filesystem::path& file,
                                std::span<const std::uint8_t> bytes, int rename_attempts = 20) {
    auto temp = file;
    temp += ".tmp";
    std::error_code error;
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return false;
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        stream.flush();
        stream.close();
        if (stream.fail()) {
            std::filesystem::remove(temp, error);
            return false;
        }
    }
    // Windows refuses the rename while another thread reads the old file.
    for (int attempt = 0; attempt < rename_attempts; ++attempt) {
        std::filesystem::rename(temp, file, error);
        if (!error) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::filesystem::remove(temp, error);
    return false;
}

constexpr std::uint32_t BackendVulkan = 1;
constexpr std::uint32_t CacheFormatVersion = 1;

struct StoreIdentity {
    std::uint32_t vendor_id{};
    std::uint32_t device_id{};
    std::uint32_t driver_id{};
    std::uint32_t driver_version{};
    std::uint32_t api_version{};
    std::uint32_t backend{BackendVulkan};
    std::uint32_t format_version{CacheFormatVersion};
    std::uint32_t shader_binary_version{};
    std::uint32_t shader_meta_version{};
    std::uint32_t pipeline_key_version{};
    std::array<std::uint8_t, 16> pipeline_cache_uuid{};
    bool operator==(const StoreIdentity&) const = default;
};

constexpr std::uint32_t IdentityMagic = 0x44494253; // "SBID"
constexpr std::uint32_t IdentityRecordVersion = 1;
constexpr std::size_t IdentityPayloadSize = 10 * sizeof(std::uint32_t) + 16;

// Field by field, never the struct bytes: the encoding does not depend on padding or layout.
inline std::vector<std::uint8_t> EncodeStoreIdentity(const StoreIdentity& id) {
    std::vector<std::uint8_t> bytes;
    Detail::Append(bytes, IdentityMagic);
    Detail::Append(bytes, IdentityRecordVersion);
    Detail::Append(bytes, std::uint32_t(IdentityPayloadSize));
    for (const auto value :
         {id.vendor_id, id.device_id, id.driver_id, id.driver_version, id.api_version, id.backend,
          id.format_version, id.shader_binary_version, id.shader_meta_version,
          id.pipeline_key_version}) {
        Detail::Append(bytes, value);
    }
    bytes.insert(bytes.end(), id.pipeline_cache_uuid.begin(), id.pipeline_cache_uuid.end());
    return bytes;
}

inline std::optional<StoreIdentity> DecodeStoreIdentity(std::span<const std::uint8_t> bytes) {
    constexpr std::size_t header = 12;
    if (bytes.size() != header + IdentityPayloadSize ||
        Detail::At<std::uint32_t>(bytes, 0) != IdentityMagic ||
        Detail::At<std::uint32_t>(bytes, 4) != IdentityRecordVersion ||
        Detail::At<std::uint32_t>(bytes, 8) != IdentityPayloadSize) {
        return std::nullopt;
    }
    StoreIdentity id;
    std::size_t at = header;
    for (auto* field :
         {&id.vendor_id, &id.device_id, &id.driver_id, &id.driver_version, &id.api_version,
          &id.backend, &id.format_version, &id.shader_binary_version, &id.shader_meta_version,
          &id.pipeline_key_version}) {
        *field = Detail::At<std::uint32_t>(bytes, at);
        at += sizeof(std::uint32_t);
    }
    std::memcpy(id.pipeline_cache_uuid.data(), bytes.data() + at, id.pipeline_cache_uuid.size());
    return id;
}

enum class StoreVerdict { Reuse, Missing, Corrupt, Incompatible };

inline StoreVerdict CheckStore(std::span<const std::uint8_t> stored, const StoreIdentity& current) {
    if (stored.empty()) {
        return StoreVerdict::Missing;
    }
    const auto identity = DecodeStoreIdentity(stored);
    if (!identity) {
        return StoreVerdict::Corrupt;
    }
    return *identity == current ? StoreVerdict::Reuse : StoreVerdict::Incompatible;
}

// Names the fields that differ, in a fixed order.
inline std::string DescribeChange(const StoreIdentity& stored, const StoreIdentity& current) {
    std::string text;
    const auto note = [&](bool changed, const char* name) {
        if (changed) {
            text += text.empty() ? name : std::string{", "} + name;
        }
    };
    note(stored.backend != current.backend, "renderer backend");
    note(stored.vendor_id != current.vendor_id, "GPU vendor");
    note(stored.device_id != current.device_id, "GPU device");
    note(stored.driver_id != current.driver_id, "driver");
    note(stored.driver_version != current.driver_version, "driver version");
    note(stored.api_version != current.api_version, "Vulkan version");
    note(stored.pipeline_cache_uuid != current.pipeline_cache_uuid, "pipeline cache UUID");
    note(stored.format_version != current.format_version, "cache format");
    note(stored.shader_binary_version != current.shader_binary_version, "shader binary format");
    note(stored.shader_meta_version != current.shader_meta_version, "shader meta format");
    note(stored.pipeline_key_version != current.pipeline_key_version, "pipeline key format");
    return text;
}

inline std::string DescribeVerdict(StoreVerdict verdict, std::span<const std::uint8_t> stored,
                                   const StoreIdentity& current) {
    switch (verdict) {
    case StoreVerdict::Reuse:
        return "identity matches";
    case StoreVerdict::Missing:
        return "no identity stamp";
    case StoreVerdict::Corrupt:
        return "corrupted identity stamp";
    case StoreVerdict::Incompatible:
        if (const auto identity = DecodeStoreIdentity(stored)) {
            return "identity changed (" + DescribeChange(*identity, current) + ")";
        }
        break;
    }
    return "unknown";
}

} // namespace PipelineCachePolicy

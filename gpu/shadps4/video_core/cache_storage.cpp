// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/pipeline_cache_policy.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>

namespace {

std::mutex submit_mutex{};
// Raised by submitters and lowered by the IO thread, which also polls it outside submit_mutex.
std::atomic<u32> num_requests{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

std::atomic<u32> rejected_blobs{};

std::vector<u8> ReadFileBytes(const std::filesystem::path& path) {
    using namespace Common::FS;
    const auto file = IOFile{path, FileAccessMode::Read};
    if (!file.IsOpen()) {
        return {};
    }
    const u64 size = file.GetSize();
    if (size > PipelineCachePolicy::MaxBlobBytes + PipelineCachePolicy::SealSize) {
        return {};
    }
    std::vector<u8> bytes(size);
    if (file.Read(bytes) != bytes.size()) {
        return {};
    }
    return bytes;
}

void NoteRejectedBlob(const std::filesystem::path& path) {
    rejected_blobs.fetch_add(1, std::memory_order_relaxed);
    LOG_DEBUG(Render, "Pipeline cache: rejected {}", path.filename().string());
}

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return num_requests.load() != 0; });
        }

        if (stoken.stop_requested()) {
            break;
        }

        while (num_requests.load() != 0) {
            std::packaged_task<void()> request{};
            {
                std::scoped_lock lock{m_request};
                if (req_queue.empty()) {
                    continue;
                }
                request = std::move(req_queue.front());
                req_queue.pop();
            }

            if (request.valid()) {
                request();
                request.get_future().wait();
            }

            --num_requests;
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

static void Submit(std::packaged_task<void()>&& request) {
    {
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }
    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
}

/// Calls `func` for each entry of `dir` while it returns true; a listing error ends the walk.
template <typename Func>
void ForEachEntry(const std::filesystem::path& dir, Func&& func) {
    std::error_code error;
    for (std::filesystem::directory_iterator it{dir, error}, end; !error && it != end;
         it.increment(error)) {
        if (!func(*it)) {
            return;
        }
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        cache_path = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        u32 removed = 0;
        ForEachEntry(cache_path, [&](const std::filesystem::directory_entry& entry) {
            std::error_code error;
            if (PipelineCachePolicy::IsStaleTemp(entry.path()) &&
                std::filesystem::remove(entry.path(), error)) {
                ++removed;
            }
            return true;
        });
        if (removed) {
            LOG_INFO(Render, "Pipeline cache: removed {} unfinished writes", removed);
        }
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    io_worker.join();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    if (const u32 rejected = rejected_blobs.exchange(0)) {
        LOG_WARNING(Render, "Pipeline cache: {} invalid entries were ignored", rejected);
    }
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    std::vector<u8> bytes(v.size() * sizeof(T));
    std::memcpy(bytes.data(), v.data(), bytes.size());
    if (!PipelineCachePolicy::Seal(bytes)) {
        LOG_WARNING(Render, "Pipeline cache: {} is too large to store", path_.filename().string());
        return false;
    }
    Submit(std::packaged_task<void()>{[type, path_, bytes = std::move(bytes)]() {
        auto path{path_};
        path.replace_extension(GetBlobFileExtension(type));
        if (EmulatorSettings.IsPipelineCacheArchived()) {
            ASSERT_MSG(!ar_is_read_only,
                       "The archive is read-only. Did you forget to call `FinishPreload`?");
            if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), bytes.data(), bytes.size(),
                                       MZ_BEST_COMPRESSION)) {
                LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
            }
        } else if (!PipelineCachePolicy::WriteFileAtomically(path, bytes)) {
            LOG_WARNING(Render, "Pipeline cache: could not write {}", path.filename().string());
        }
    }});
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    v.clear();
    path.replace_extension(GetBlobFileExtension(type));
    std::vector<u8> bytes;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const int index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            return;
        }
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip_ar, index, &stat) ||
            stat.m_uncomp_size > PipelineCachePolicy::MaxBlobBytes + PipelineCachePolicy::SealSize) {
            NoteRejectedBlob(path);
            return;
        }
        bytes.resize(stat.m_uncomp_size);
        if (!mz_zip_reader_extract_to_mem(&zip_ar, index, bytes.data(), bytes.size(), 0)) {
            NoteRejectedBlob(path);
            return;
        }
    } else {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            return;
        }
        bytes = ReadFileBytes(path);
    }
    if (!PipelineCachePolicy::Unseal(bytes) || bytes.size() % sizeof(T) != 0) {
        NoteRejectedBlob(path);
        return;
    }
    v.resize(bytes.size() / sizeof(T));
    std::memcpy(v.data(), bytes.data(), bytes.size());
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

size_t DataBase::CountBlobs(BlobType type) {
    const auto& ext = GetBlobFileExtension(type);
    size_t count = 0;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            count += std::string{file_name.data()}.ends_with(ext);
        }
        return count;
    }
    ForEachEntry(cache_path, [&](const std::filesystem::directory_entry& entry) {
        count += entry.path().extension().string().ends_with(ext);
        return true;
    });
    return count;
}

bool DataBase::ConcurrentReads() const {
    return !EmulatorSettings.IsPipelineCacheArchived();
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func,
                           const std::function<bool()>& stop) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (!std::string{file_name.data()}.ends_with(ext)) {
                continue;
            }
            mz_zip_archive_file_stat stat{};
            std::vector<u8> data;
            if (mz_zip_reader_file_stat(&zip_ar, index, &stat) &&
                stat.m_uncomp_size <=
                    PipelineCachePolicy::MaxBlobBytes + PipelineCachePolicy::SealSize) {
                data.resize(stat.m_uncomp_size);
                if (!mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0)) {
                    data.clear();
                }
            }
            if (PipelineCachePolicy::Unseal(data)) {
                func(std::move(data));
            } else {
                NoteRejectedBlob(file_name.data());
            }
        }
        return;
    }
    ForEachEntry(cache_path, [&](const std::filesystem::directory_entry& entry) {
        if (stop && stop()) {
            return false;
        }
        const auto& path = entry.path();
        if (!path.extension().string().ends_with(ext)) {
            return true;
        }
        auto data = ReadFileBytes(path);
        if (PipelineCachePolicy::Unseal(data)) {
            func(std::move(data));
            return true;
        }
        NoteRejectedBlob(path);
        // Re-checked behind the pending saves: a save of this entry that is still queued wins.
        Submit(std::packaged_task<void()>{[path] {
            auto bytes = ReadFileBytes(path);
            if (!PipelineCachePolicy::Unseal(bytes)) {
                std::error_code error;
                std::filesystem::remove(path, error);
            }
        }});
        return true;
    });
}

void DataBase::Clear() {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    u64 removed = 0;
    ForEachEntry(cache_path, [&](const std::filesystem::directory_entry& entry) {
        std::error_code ec;
        removed += entry.is_regular_file(ec) && PipelineCachePolicy::IsCacheArtifact(entry.path()) &&
                   std::filesystem::remove(entry.path(), ec);
        return true;
    });
    if (removed) {
        LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt", removed);
    }
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage

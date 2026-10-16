#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace cpm {

// An exclusive, advisory lock on a file, shared by all CPM processes (docs/spec/store.md §4). The OS
// releases it if the process dies.
class FileLock {
public:
    // Blocks until the lock is held. If another process holds it for a moment, prints
    // "waiting for another cpm process (<what>)…".
    static FileLock acquire(const std::filesystem::path& file, std::string_view what);
    // Takes the lock only if it is free right now.
    static std::optional<FileLock> try_acquire(const std::filesystem::path& file);

    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock();

private:
    FileLock() = default;
    void release() noexcept;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace cpm

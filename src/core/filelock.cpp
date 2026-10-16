#include "core/filelock.hpp"

#include <format>
#include <utility>

#include "core/error.hpp"
#include "core/fsx.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace cpm {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
HANDLE open_lock_file(const fs::path& file) {
    fsx::create_dirs(file.parent_path());
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw Error(std::format("cannot open lock file '{}'", paths::to_utf8(file)));
    return h;
}

bool lock_handle(HANDLE h, bool wait) {
    OVERLAPPED ov{};
    DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    return LockFileEx(h, flags, 0, 1, 0, &ov) != 0;
}
#else
int open_lock_file(const fs::path& file) {
    fsx::create_dirs(file.parent_path());
    int fd = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) throw Error(std::format("cannot open lock file '{}': {}", paths::to_utf8(file), std::strerror(errno)));
    return fd;
}

bool lock_fd(int fd, bool wait) {
    while (flock(fd, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0) {
        if (errno != EINTR) return false;
    }
    return true;
}
#endif

}  // namespace

std::optional<FileLock> FileLock::try_acquire(const fs::path& file) {
    FileLock lock;
#ifdef _WIN32
    HANDLE h = open_lock_file(file);
    if (!lock_handle(h, false)) {
        CloseHandle(h);
        return std::nullopt;
    }
    lock.handle_ = h;
#else
    int fd = open_lock_file(file);
    if (!lock_fd(fd, false)) {
        close(fd);
        return std::nullopt;
    }
    lock.fd_ = fd;
#endif
    return lock;
}

FileLock FileLock::acquire(const fs::path& file, std::string_view what) {
    if (auto lock = try_acquire(file)) return std::move(*lock);
    log::info(std::format("waiting for another cpm process ({})...", what));
    FileLock lock;
#ifdef _WIN32
    HANDLE h = open_lock_file(file);
    if (!lock_handle(h, true)) {
        CloseHandle(h);
        throw Error(std::format("cannot lock '{}'", paths::to_utf8(file)));
    }
    lock.handle_ = h;
#else
    int fd = open_lock_file(file);
    if (!lock_fd(fd, true)) {
        close(fd);
        throw Error(std::format("cannot lock '{}': {}", paths::to_utf8(file), std::strerror(errno)));
    }
    lock.fd_ = fd;
#endif
    return lock;
}

FileLock::FileLock(FileLock&& other) noexcept {
#ifdef _WIN32
    handle_ = std::exchange(other.handle_, nullptr);
#else
    fd_ = std::exchange(other.fd_, -1);
#endif
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        release();
#ifdef _WIN32
        handle_ = std::exchange(other.handle_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
    }
    return *this;
}

FileLock::~FileLock() { release(); }

void FileLock::release() noexcept {
#ifdef _WIN32
    if (handle_ != nullptr) {
        OVERLAPPED ov{};
        UnlockFileEx(static_cast<HANDLE>(handle_), 0, 1, 0, &ov);
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
        flock(fd_, LOCK_UN);
        close(fd_);
        fd_ = -1;
    }
#endif
}

}  // namespace cpm

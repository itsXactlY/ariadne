#pragma once
// dd/context/fsutil.hpp — private files: directories 0700, files 0600, one writer at a time across processes.

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <string>

namespace dd {
namespace context {
namespace fsx {

// mkdir -p, every directory it has to create is 0700; existing ones are left as they are.
inline bool mkdirs(const std::filesystem::path& p, mode_t mode = 0700) {
    std::error_code ec;
    if (p.empty()) return false;
    if (std::filesystem::is_directory(p, ec)) return true;
    if (p.has_parent_path() && p.parent_path() != p) mkdirs(p.parent_path(), mode);
    if (::mkdir(p.c_str(), mode) != 0 && errno != EEXIST) return false;
    return std::filesystem::is_directory(p, ec);
}

inline bool write_all(int fd, const char* p, size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w, n -= static_cast<size_t>(w);
    }
    return true;
}

// A new 0600 file (truncated when it exists): the contents are the operator's work, nobody else's.
inline int open_private(const std::string& path, int extra = O_TRUNC) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | extra, 0600);
    if (fd >= 0) ::fchmod(fd, 0600);
    return fd;
}

// flock(LOCK_EX) on `path` for the life of the object: a second dd on the same home waits its turn.
class FileLock {
public:
    explicit FileLock(const std::string& path) : fd_(::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)) {
        if (fd_ >= 0)
            while (::flock(fd_, LOCK_EX) != 0 && errno == EINTR) {}
    }
    ~FileLock() {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    int fd_;
};

}  // namespace fsx
}  // namespace context
}  // namespace dd

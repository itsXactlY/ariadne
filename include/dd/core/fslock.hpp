#pragma once
// dd/core/fslock.hpp — the kernel lock: one writer at a time for a durable file, across threads
// AND across processes. It lives in core because the thing it guards is a file, not a data
// structure, and the layers that write private files (context, learn, tools, gateway) are all
// below the layer that owns them.
//
// W10's rule is that no hand-off in this program uses a userspace lock. A userspace mutex cannot
// make a read-modify-write of a file safe anyway: a second `dd` on the same home is outside it.
// So the disk paths take this, and everything that hands a value between threads hands it by
// atomic ownership (core::SlotRing, core::SnapCell) instead.
//
// flock(2) on a sidecar lock file, not on the guarded file itself: the guarded file is replaced by
// rename, which moves it to a new inode, and a lock taken on the old descriptor would then guard
// nothing. The sidecar's inode is stable, so the lock is the whole write.

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>   // fchmod
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <string>

namespace dd {
namespace core {

// The sidecar for a durable path: `<path>.lock`, same directory, 0600 like the file it guards.
inline std::filesystem::path lock_path_for(const std::filesystem::path& p) {
    return p.string() + ".lock";
}

// flock(LOCK_EX) for the life of the object. A lock that could not be opened is held nowhere:
// the caller writes anyway, because the write is atomic-by-rename and an unopenable lock means
// the directory is not there, which means there is nothing to collide with.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path)
        : fd_(::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)) {
        if (fd_ >= 0) {
            ::fchmod(fd_, 0600);
            while (::flock(fd_, LOCK_EX) != 0 && errno == EINTR) {}
        }
    }
    ~FileLock() {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    // False when the lock file could not be opened; the guarded write still happens.
    bool held() const { return fd_ >= 0; }

private:
    int fd_;
};

}  // namespace core
}  // namespace dd

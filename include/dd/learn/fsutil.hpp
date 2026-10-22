#pragma once
// dd/learn/fsutil.hpp — the few POSIX calls the ledger needs, with their errors folded to bool.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace dd {
namespace learn {
namespace fsx {

inline bool write_all(int fd, const void* p, size_t n) {
    const char* b = static_cast<const char*>(p);
    while (n > 0) {
        const ssize_t w = ::write(fd, b, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        b += w, n -= static_cast<size_t>(w);
    }
    return true;
}

inline bool read_file(const std::filesystem::path& p, std::string& out) {
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    out.clear();
    char buf[1 << 16];
    for (;;) {
        const ssize_t r = ::read(fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        out.append(buf, static_cast<size_t>(r));
    }
    ::close(fd);
    return true;
}

inline bool sync_dir(const std::filesystem::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

// temp file, fsync, rename over `path`: the file is whole or absent. The caller fsyncs the directory.
inline bool write_atomic(const std::filesystem::path& tmp, const std::filesystem::path& path, std::string_view bytes) {
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool ok = write_all(fd, bytes.data(), bytes.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) return ::unlink(tmp.c_str()), false;
    return ::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace fsx
}  // namespace learn
}  // namespace dd

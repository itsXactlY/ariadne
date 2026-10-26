// dd::tools fs_*: read_file / write_file / list_dir with bounded memory and honest errors.
#include "dd/tools/fs_tools.hpp"
#include "dd/tools/path_security.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace dd {
namespace tools {

using namespace dd::core;
namespace fs = std::filesystem;

namespace {

JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }
std::string jerr(const std::string& m) { return json_dump(json_obj({{"error", jsp(json_str(m))}})); }
std::string errno_text() { return std::strerror(errno); }

constexpr size_t kScanCap = 64u << 20;          // bytes scanned to count lines
constexpr size_t kBinaryProbe = 8192;
constexpr size_t kMaxListed = 100000;           // names held to sort one directory

const char* kind_of(mode_t m) {
    if (S_ISDIR(m)) return "a directory";
    if (S_ISCHR(m)) return "a character device";
    if (S_ISBLK(m)) return "a block device";
    if (S_ISFIFO(m)) return "a fifo";
    if (S_ISSOCK(m)) return "a socket";
    return "not a regular file";
}

bool write_all(int fd, const char* p, size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

mode_t current_umask() {   // /proc/self/status: umask() itself can only be read by setting it
    std::ifstream f("/proc/self/status");
    for (std::string l; std::getline(f, l);)
        if (l.rfind("Umask:", 0) == 0) return static_cast<mode_t>(std::strtoul(l.c_str() + 6, nullptr, 8));
    return 022;
}

}  // namespace

std::string fs_read(const Json& args, const FsLimits& limits) {
    const Json* p = json_get(args, "path");
    if (!p || p->type != Json::Str) return jerr("path required");
    const std::string& path = p->str;
    if (has_traversal_component(path)) return jerr("path contains '..': " + path);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) return jerr("cannot open " + path + ": " + errno_text());
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const std::string why = errno_text();
        ::close(fd);
        return jerr("cannot stat " + path + ": " + why);
    }
    if (S_ISDIR(st.st_mode)) {
        ::close(fd);
        return jerr("is a directory: use list_dir on " + path);
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        return jerr(std::string(kind_of(st.st_mode)) + ", not a file: refusing to read " + path);
    }
    long offset = 1, limit = static_cast<long>(limits.max_lines);
    if (const Json* o = json_get(args, "offset"); o && o->type == Json::Num) offset = std::max(1L, static_cast<long>(o->num));
    if (const Json* l = json_get(args, "limit"); l && l->type == Json::Num) limit = std::max(1L, static_cast<long>(l->num));

    std::string out, line;
    long n = 1, shown = 0, cuts = 0, next_offset = 0;
    size_t line_bytes = 0, scanned = 0;
    bool stopped = false, truncated = false, capped = false, have_data = false, first_chunk = true;
    char num[24];
    std::string buf(1 << 16, '\0');

    // one finished line n: kept when it is in the window and the output budget has room
    auto finish_line = [&]() {
        if (n >= offset && shown < limit && !stopped) {
            std::string text = line;
            if (line_bytes > limits.max_line_length) {
                text += " ...[line cut: " + std::to_string(line_bytes) + " bytes]";
                ++cuts;
            }
            const int w = std::snprintf(num, sizeof num, "%6ld|", n);
            if (out.size() + static_cast<size_t>(w) + text.size() + 1 > limits.max_read_bytes && shown > 0) {
                stopped = truncated = true;
                next_offset = n;
            } else {
                out.append(num, static_cast<size_t>(w));
                out += text;
                out += '\n';
                ++shown;
            }
        }
        line.clear();
        line_bytes = 0;
        ++n;
    };

    for (;;) {
        const ssize_t r = ::read(fd, buf.data(), buf.size());
        if (r < 0) {
            if (errno == EINTR) continue;
            const std::string why = errno_text();
            ::close(fd);
            return jerr("read failed on " + path + ": " + why);
        }
        if (r == 0) break;
        if (first_chunk) {
            first_chunk = false;
            if (std::memchr(buf.data(), 0, std::min(static_cast<size_t>(r), kBinaryProbe))) {
                ::close(fd);
                return jerr("binary file (" + std::to_string(st.st_size) + " bytes): not text, refusing to read " + path);
            }
        }
        scanned += static_cast<size_t>(r);
        size_t pos = 0;
        const size_t len = static_cast<size_t>(r);
        while (pos < len) {
            const char* nl = static_cast<const char*>(std::memchr(buf.data() + pos, '\n', len - pos));
            const size_t end = nl ? static_cast<size_t>(nl - buf.data()) : len;
            const bool in_window = n >= offset && shown < limit && !stopped;
            if (in_window) {
                line_bytes += end - pos;
                if (line.size() < limits.max_line_length)
                    line.append(buf.data() + pos, std::min(end - pos, limits.max_line_length - line.size()));
            }
            have_data = true;
            if (nl) {
                finish_line();
                have_data = false;
                pos = end + 1;
            } else {
                pos = len;
            }
        }
        if (scanned >= kScanCap) {
            capped = true;
            break;
        }
    }
    ::close(fd);
    if (have_data) finish_line();   // a last line without a newline
    const long total = n - 1;
    if (!truncated && shown >= limit && offset + shown <= total) next_offset = offset + shown;
    // What the call did not hand back. Lines before `offset` were asked for nothing, so they are not
    // a loss; only the lines past the window are. When the scan hit its cap the file's true length is
    // unknown, so this is a floor and total_lines_capped says so rather than pretending it is final.
    const long last_shown = offset + shown - 1;
    const long dropped = total > last_shown ? total - last_shown : 0;
    // Either kind of cut makes this a partial read: a byte budget or a line window. A caller that
    // checks `truncated` must see the line-window case too, or it reads a cut file as a whole one.
    if (dropped > 0) truncated = true;

    Json res = json_obj({{"path", jsp(json_str(path))},
                         {"content", jsp(json_str(out))},
                         {"total_lines", jsp(json_num(static_cast<double>(total)))},
                         {"offset", jsp(json_num(static_cast<double>(offset)))},
                         {"lines", jsp(json_num(static_cast<double>(shown)))}});
    if (truncated) obj_set(res, "truncated", json_bool(true));
    if (next_offset > 0) obj_set(res, "next_offset", json_num(static_cast<double>(next_offset)));
    if (cuts > 0) obj_set(res, "lines_cut", json_num(static_cast<double>(cuts)));
    if (dropped > 0) obj_set(res, "lines_dropped", json_num(static_cast<double>(dropped)));
    if (capped) obj_set(res, "total_lines_capped", json_bool(true));
    return json_dump(res);
}

std::string fs_write(const Json& args) {
    const Json* p = json_get(args, "path");
    const Json* c = json_get(args, "content");
    if (!p || p->type != Json::Str || !c || c->type != Json::Str) return jerr("path+content required");
    const std::string& path = p->str;
    if (path.empty()) return jerr("path required");
    if (has_traversal_component(path)) return jerr("path contains '..': " + path);
    const std::string& data = c->str;
    auto ok = [&]() { return json_dump(json_obj({{"path", jsp(json_str(path))}, {"bytes", jsp(json_num(static_cast<double>(data.size())))}})); };

    std::error_code ec;
    fs::path target(path);
    struct stat st {};
    const bool exists = ::stat(path.c_str(), &st) == 0;   // follows a symlink
    if (exists && S_ISDIR(st.st_mode)) return jerr("is a directory: " + path);
    if (exists) {
        const fs::path real = fs::canonical(target, ec);   // write through a symlink to the real file
        if (!ec) target = real;
    } else {
        struct stat ls {};
        if (::lstat(path.c_str(), &ls) == 0 && S_ISLNK(ls.st_mode)) {   // dangling link: create what it points to, in place
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
            if (fd < 0) return jerr("cannot write " + path + ": " + errno_text());
            const bool wrote = write_all(fd, data.data(), data.size());
            const std::string why = errno_text();
            if (::close(fd) != 0 || !wrote) return jerr("write failed on " + path + ": " + (wrote ? errno_text() : why));
            return ok();
        }
    }
    fs::path dir = target.parent_path();
    if (dir.empty()) dir = ".";
    if (!fs::is_directory(dir, ec)) return jerr("parent directory missing: " + dir.string());

    if (exists && !S_ISREG(st.st_mode)) {   // a device or fifo: no temp file, no rename
        const int fd = ::open(target.c_str(), O_WRONLY | O_CLOEXEC | O_NOCTTY);
        if (fd < 0) return jerr("cannot open " + path + ": " + errno_text());
        const bool wrote = write_all(fd, data.data(), data.size());
        const std::string why = errno_text();
        const bool closed = ::close(fd) == 0;
        if (!wrote) return jerr("write failed on " + path + ": " + why);
        if (!closed) return jerr("close failed on " + path + ": " + errno_text());
        return ok();
    }

    std::string tmpl = (dir / ("." + target.filename().string() + ".dd-tmp-XXXXXX")).string();
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) return jerr("cannot write in " + dir.string() + ": " + errno_text());
    const mode_t mode = exists ? (st.st_mode & 07777) : static_cast<mode_t>(0666 & ~current_umask());
    bool good = write_all(fd, data.data(), data.size());
    std::string why = good ? "" : errno_text();
    if (good && ::fchmod(fd, mode) != 0) good = false, why = errno_text();
    if (good && ::fsync(fd) != 0) good = false, why = errno_text();
    if (::close(fd) != 0 && good) good = false, why = errno_text();
    if (good && ::rename(tmpl.c_str(), target.c_str()) != 0) good = false, why = errno_text();
    if (!good) {
        ::unlink(tmpl.c_str());
        return jerr("write failed on " + path + ": " + why);
    }
    return ok();
}

std::string fs_list(const Json& args, const FsLimits& limits) {
    const Json* p = json_get(args, "path");
    const std::string dir = (p && p->type == Json::Str) ? p->str : std::string(".");
    if (has_traversal_component(dir)) return jerr("path contains '..': " + dir);
    std::error_code ec;
    const auto st = fs::status(dir, ec);
    if (ec || !fs::exists(st)) return jerr("not a dir: " + dir + " does not exist");
    if (!fs::is_directory(st)) return jerr("not a directory: " + dir);
    std::vector<std::string> names;
    bool capped = false;
    try {
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        if (ec) return jerr("cannot list " + dir + ": " + ec.message());
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            if (names.size() >= kMaxListed) {
                capped = true;
                break;
            }
            std::error_code ie;
            std::string name = it->path().string();
            if (it->symlink_status(ie).type() == fs::file_type::directory) name += '/';
            names.push_back(std::move(name));
        }
    } catch (const fs::filesystem_error& e) {
        if (names.empty()) return jerr(std::string("cannot list ") + dir + ": " + e.what());
    }
    std::sort(names.begin(), names.end());
    const size_t total = names.size();
    const bool cut = total > limits.max_entries;
    if (cut) names.resize(limits.max_entries);
    Json arr = json_arr();
    for (const auto& n : names) arr_push(arr, json_str(n));
    Json res = json_obj({{"path", jsp(json_str(dir))}, {"entries", jsp(std::move(arr))}});
    if (cut || capped) {
        obj_set(res, "truncated", json_bool(true));
        obj_set(res, "total", json_num(static_cast<double>(total)));
        if (capped) obj_set(res, "total_capped", json_bool(true));
    }
    return json_dump(res);
}

std::string fs_read_raw(const std::string& path, std::size_t cap, std::string& err) {
    err.clear();
    if (path.empty()) { err = "path required"; return {}; }
    if (has_traversal_component(path)) { err = "path contains '..': " + path; return {}; }
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) { err = "not found: " + path; return {}; }
    if (!S_ISREG(st.st_mode)) { err = "cannot edit " + path + ": " + kind_of(st.st_mode); return {}; }
    if (cap && static_cast<std::size_t>(st.st_size) > cap) {
        err = "too large to edit: " + path + " is " + std::to_string(st.st_size) +
              " bytes, the patch cap is " + std::to_string(cap);
        return {};
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { err = "cannot open " + path + ": " + errno_text(); return {}; }
    std::string data;
    std::vector<char> buf(64u << 10);
    bool first = true;
    for (;;) {
        const ssize_t r = ::read(fd, buf.data(), buf.size());
        if (r < 0) {
            if (errno == EINTR) continue;
            const std::string why = errno_text();
            ::close(fd);
            err = "read failed on " + path + ": " + why;
            return {};
        }
        if (r == 0) break;
        if (first) {
            first = false;
            if (std::memchr(buf.data(), 0, std::min(static_cast<size_t>(r), kBinaryProbe))) {
                ::close(fd);
                err = "binary file (" + std::to_string(st.st_size) + " bytes): not text, refusing to edit " + path;
                return {};
            }
        }
        data.append(buf.data(), static_cast<size_t>(r));
    }
    ::close(fd);
    return data;
}

Json fs_tool_schema(const std::vector<std::string>& props, const std::string& desc) {
    Json props_map = json_obj();
    for (const auto& k : props) {
        Json one = json_obj({{"type", jsp(json_str("string"))},
                             {"description", jsp(json_str(desc))}});
        obj_set(props_map, k, one);
    }
    obj_set(props_map, "intent", json_obj({{"type", jsp(json_str("string"))},
                                           {"description", jsp(json_str("what you are doing, in a few words (optional)"))}}));
    Json req = json_arr();
    for (const auto& k : props) arr_push(req, json_str(k));
    Json parameters = json_obj({{"type", jsp(json_str("object"))},
                                {"properties", jsp(std::move(props_map))},
                                {"required", jsp(std::move(req))}});
    return json_obj({{"description", jsp(json_str(desc))},
                     {"parameters", jsp(std::move(parameters))}});
}

}  // namespace tools
}  // namespace dd

// gateway/pairing.py -> dd::gateway::PairingStore
#include "dd/gateway/pairing.hpp"
#include "dd/core/fslock.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/random.h>
#include <unistd.h>
#include <cerrno>

namespace dd {
namespace gateway {

using namespace core;

// --- constants (pairing.py) ---
static const std::string ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
static const int CODE_LENGTH = 8;
static const double CODE_TTL_SECONDS = 3600.0;
static const double RATE_LIMIT_SECONDS = 600.0;
static const double LOCKOUT_SECONDS = 3600.0;
static const int MAX_PENDING_PER_PLATFORM = 3;
static const int MAX_FAILED_ATTEMPTS = 5;

// One kernel lock for the whole store: the entries under pairing_dir() are read and rewritten as
// whole files, and the lock's inode never moves (the guarded file is rename-replaced, so a lock
// taken on the file itself would guard nothing after the first write).
static std::filesystem::path store_lock_path(const std::filesystem::path& dir) {
    return dir / ".store.lock";
}

static double now_seconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// _legacy_path_has_content (daedalus_constants.py): populated dir / non-dir
// file counts; empty dir does not; dangling symlink does not; inspect errors
// (other than not-found) assume occupied.
static bool legacy_path_has_content(const std::filesystem::path& path) {
    std::error_code ec;
    auto lst = std::filesystem::symlink_status(path, ec);
    if (ec) return true;  // OSError short of not-found -> occupied
    if (lst.type() == std::filesystem::file_type::not_found) return false;
    if (std::filesystem::is_symlink(lst)) {
        std::error_code ec2;
        auto st = std::filesystem::status(path, ec2);  // follows symlink
        if (ec2) return true;
        if (st.type() == std::filesystem::file_type::not_found) return false;  // dangling
        if (!std::filesystem::is_directory(st)) return true;
    } else if (!std::filesystem::is_directory(lst)) {
        return true;  // non-dir file
    }
    // directory: any entry inside counts
    std::error_code ec3;
    auto it = std::filesystem::directory_iterator(path, ec3);
    if (ec3) return true;
    if (it == std::filesystem::directory_iterator{}) return false;  // empty
    return true;
}

// get_daedalus_dir(new_subpath, old_name): legacy if it has content, else new.
static std::filesystem::path get_daedalus_dir(const std::string& new_subpath,
                                              const std::string& old_name) {
    std::filesystem::path home = core::get_daedalus_home();
    std::filesystem::path old_path = home / old_name;
    if (legacy_path_has_content(old_path)) return old_path;
    return home / new_subpath;
}

static std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// _secure_write: temp file + atomic rename + chmod 0600.
static void secure_write(const std::filesystem::path& path, const std::string& data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::string dir = path.parent_path().string();
    std::string tmpl = dir + "/.pairing_XXXXXX.tmp";
    // mkstemps with suffix_len=4 (".tmp")
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemps(buf.data(), 4);
    if (fd < 0) throw std::runtime_error("mkstemps failed");
    std::string tmp_path(buf.data());
    try {
        {
            std::FILE* f = ::fdopen(fd, "w");
            if (!f) { ::close(fd); throw std::runtime_error("fdopen failed"); }
            std::fwrite(data.data(), 1, data.size(), f);
            std::fflush(f);
            ::fsync(::fileno(f));
            std::fclose(f);
        }
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) { ::unlink(tmp_path.c_str()); throw std::runtime_error("rename failed"); }
        ::chmod(path.c_str(), 0600);
    } catch (...) {
        ::unlink(tmp_path.c_str());
        throw;
    }
}

static std::string trim_upper(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (!std::isspace(c)) out.push_back((char)std::toupper(c));
    }
    return out;
}

// --- PairingStore ---
PairingStore::PairingStore() {
    std::error_code ec;
    std::filesystem::create_directories(pairing_dir(), ec);
}

std::filesystem::path PairingStore::pairing_dir() const {
    return get_daedalus_dir("platforms/pairing", "pairing");
}

std::filesystem::path PairingStore::pending_path(const std::string& platform) const {
    return pairing_dir() / (platform + "-pending.json");
}
std::filesystem::path PairingStore::approved_path(const std::string& platform) const {
    return pairing_dir() / (platform + "-approved.json");
}
std::filesystem::path PairingStore::rate_limit_path() const {
    return pairing_dir() / "_rate_limits.json";
}

Json PairingStore::load_json(const std::filesystem::path& p) const {
    Json empty = json_obj();
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return empty;
    std::string text = read_file(p);
    if (text.empty()) return empty;
    try {
        Json parsed = json_parse(text);
        if (parsed.type != Json::Obj) return empty;
        return parsed;
    } catch (...) {
        return empty;
    }
}

void PairingStore::save_json(const std::filesystem::path& p, const Json& data) {
    secure_write(p, json_dump(data));
}

bool PairingStore::is_approved(const std::string& platform,
                               const std::string& user_id) const {
    Json approved = load_json(approved_path(platform));
    return json_get(approved, user_id) != nullptr;
}

void PairingStore::approve_user(const std::string& platform,
                                const std::string& user_id,
                                const std::string& user_name) {
    std::filesystem::path p = approved_path(platform);
    Json approved = load_json(p);
    Json entry = json_obj();
    obj_set(entry, "user_name", json_str(user_name));
    obj_set(entry, "approved_at", json_num(now_seconds()));
    obj_set(approved, user_id, entry);
    save_json(p, approved);
}

bool PairingStore::revoke(const std::string& platform, const std::string& user_id) {
    const core::FileLock lock(store_lock_path(pairing_dir()));   // the whole read-modify-write, one step
    std::filesystem::path p = approved_path(platform);
    Json approved = load_json(p);
    const Json* hit = json_get(approved, user_id);
    if (!hit) return false;
    // rebuild without user_id
    Json rebuilt = json_obj();
    for (auto& kv : approved.obj) {
        if (kv.first != user_id) rebuilt.obj.push_back({kv.first, kv.second});
    }
    save_json(p, rebuilt);
    return true;
}

std::vector<Json> PairingStore::list_approved(const std::string& platform) const {
    std::vector<Json> results;
    std::vector<std::string> platforms;
    if (!platform.empty()) platforms.push_back(platform);
    else platforms = all_platforms("approved");
    for (const std::string& p : platforms) {
        Json approved = load_json(approved_path(p));
        for (auto& kv : approved.obj) {
            Json rec = json_obj();
            obj_set(rec, "platform", json_str(p));
            obj_set(rec, "user_id", json_str(kv.first));
            if (kv.second && kv.second->type == Json::Obj) {
                for (auto& ikv : kv.second->obj)
                    rec.obj.push_back({ikv.first, ikv.second});
            }
            results.push_back(std::move(rec));
        }
    }
    return results;
}

bool PairingStore::is_rate_limited(const std::string& platform,
                                   const std::string& user_id) const {
    Json limits = load_json(rate_limit_path());
    std::string key = platform + ":" + user_id;
    const Json* v = json_get(limits, key);
    double last = (v && v->type == Json::Num) ? v->num : 0.0;
    return (now_seconds() - last) < RATE_LIMIT_SECONDS;
}

void PairingStore::record_rate_limit(const std::string& platform,
                                     const std::string& user_id) {
    std::filesystem::path p = rate_limit_path();
    Json limits = load_json(p);
    obj_set(limits, platform + ":" + user_id, json_num(now_seconds()));
    save_json(p, limits);
}

bool PairingStore::is_locked_out(const std::string& platform) const {
    Json limits = load_json(rate_limit_path());
    const Json* v = json_get(limits, "_lockout:" + platform);
    double until = (v && v->type == Json::Num) ? v->num : 0.0;
    return now_seconds() < until;
}

void PairingStore::record_failed_attempt(const std::string& platform) {
    std::filesystem::path p = rate_limit_path();
    Json limits = load_json(p);
    std::string fail_key = "_failures:" + platform;
    const Json* v = json_get(limits, fail_key);
    double fails = ((v && v->type == Json::Num) ? v->num : 0.0) + 1.0;
    obj_set(limits, fail_key, json_num(fails));
    if (fails >= MAX_FAILED_ATTEMPTS) {
        obj_set(limits, "_lockout:" + platform, json_num(now_seconds() + LOCKOUT_SECONDS));
        obj_set(limits, fail_key, json_num(0.0));
        std::cout << "[pairing] Platform " << platform
                  << " locked out for " << (long long)LOCKOUT_SECONDS << "s after "
                  << MAX_FAILED_ATTEMPTS << " failed attempts" << std::endl;
    }
    save_json(p, limits);
}

void PairingStore::cleanup_expired(const std::string& platform) {
    std::filesystem::path p = pending_path(platform);
    Json pending = load_json(p);
    double now = now_seconds();
    Json rebuilt = json_obj();
    bool changed = false;
    for (auto& kv : pending.obj) {
        double created = 0.0;
        if (kv.second && kv.second->type == Json::Obj) {
            const Json* c = json_get(*kv.second, "created_at");
            if (c && c->type == Json::Num) created = c->num;
        }
        if ((now - created) > CODE_TTL_SECONDS) {
            changed = true;
            continue;
        }
        rebuilt.obj.push_back({kv.first, kv.second});
    }
    if (changed) save_json(p, rebuilt);
}

std::vector<std::string> PairingStore::all_platforms(const std::string& suffix) const {
    std::vector<std::string> platforms;
    std::error_code ec;
    std::filesystem::path dir = pairing_dir();
    if (!std::filesystem::exists(dir, ec)) return platforms;
    std::string tail = "-" + suffix + ".json";
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end; ++it) {
        std::string name = it->path().filename().string();
        if (name.size() >= tail.size() &&
            name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
            std::string platform = name.substr(0, name.size() - tail.size());
            if (!platform.empty() && platform[0] != '_') platforms.push_back(platform);
        }
    }
    return platforms;
}

// n bytes from the kernel CSPRNG; false when it cannot give them.
static bool os_random(unsigned char* p, size_t n) {
    while (n > 0) {
        const ssize_t r = ::getrandom(p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

std::string pairing_random_code(const std::string& alphabet, int length) {
    const size_t n = alphabet.size();
    if (n == 0 || n > 256 || length <= 0) return "";
    const unsigned limit = 256u - 256u % static_cast<unsigned>(n);   // bytes at or above it would favour the low symbols
    std::string code;
    code.reserve(static_cast<size_t>(length));
    unsigned char buf[64];
    while (code.size() < static_cast<size_t>(length)) {
        if (!os_random(buf, sizeof buf)) return "";
        for (unsigned char b : buf) {
            if (b >= limit) continue;
            code.push_back(alphabet[b % n]);
            if (code.size() == static_cast<size_t>(length)) break;
        }
    }
    return code;
}

std::optional<std::string> PairingStore::generate_code(const std::string& platform,
                                                       const std::string& user_id,
                                                       const std::string& user_name) {
    const core::FileLock lock(store_lock_path(pairing_dir()));   // the helpers below run under it
    cleanup_expired(platform);
    if (is_locked_out(platform)) return std::nullopt;
    if (is_rate_limited(platform, user_id)) return std::nullopt;

    std::filesystem::path pp = pending_path(platform);
    Json pending = load_json(pp);
    if ((int)pending.obj.size() >= MAX_PENDING_PER_PLATFORM) return std::nullopt;

    // secrets.choice(ALPHABET) x CODE_LENGTH
    const std::string code = pairing_random_code(ALPHABET, CODE_LENGTH);
    if (code.empty()) return std::nullopt;

    Json entry = json_obj();
    obj_set(entry, "user_id", json_str(user_id));
    obj_set(entry, "user_name", json_str(user_name));
    obj_set(entry, "created_at", json_num(now_seconds()));
    obj_set(pending, code, entry);
    save_json(pp, pending);

    record_rate_limit(platform, user_id);
    return code;
}

std::optional<Json> PairingStore::approve_code(const std::string& platform,
                                               const std::string& code) {
    const core::FileLock lock(store_lock_path(pairing_dir()));   // the helpers below run under it
    cleanup_expired(platform);
    std::string key = trim_upper(code);

    std::filesystem::path pp = pending_path(platform);
    Json pending = load_json(pp);
    const Json* entry = json_get(pending, key);
    if (!entry) {
        record_failed_attempt(platform);
        return std::nullopt;
    }
    Json entry_copy = json_deep_copy(*entry);
    // remove key
    Json rebuilt = json_obj();
    for (auto& kv : pending.obj) {
        if (kv.first != key) rebuilt.obj.push_back({kv.first, kv.second});
    }
    save_json(pp, rebuilt);

    const Json* uid = json_get(entry_copy, "user_id");
    const Json* name = json_get(entry_copy, "user_name");
    std::string user_id = (uid && uid->type == Json::Str) ? uid->str : "";
    std::string user_name = (name && name->type == Json::Str) ? name->str : "";
    approve_user(platform, user_id, user_name);

    Json result = json_obj();
    obj_set(result, "user_id", json_str(user_id));
    obj_set(result, "user_name", json_str(user_name));
    return result;
}

std::vector<Json> PairingStore::list_pending(const std::string& platform) {
    std::vector<Json> results;
    std::vector<std::string> platforms;
    if (!platform.empty()) platforms.push_back(platform);
    else platforms = all_platforms("pending");
    for (const std::string& p : platforms) {
        cleanup_expired(p);
        Json pending = load_json(pending_path(p));
        double now = now_seconds();
        for (auto& kv : pending.obj) {
            double created = 0.0;
            std::string uid, name;
            if (kv.second && kv.second->type == Json::Obj) {
                const Json* c = json_get(*kv.second, "created_at");
                if (c && c->type == Json::Num) created = c->num;
                const Json* u = json_get(*kv.second, "user_id");
                if (u && u->type == Json::Str) uid = u->str;
                const Json* n = json_get(*kv.second, "user_name");
                if (n && n->type == Json::Str) name = n->str;
            }
            long long age_min = (long long)((now - created) / 60.0);
            Json rec = json_obj();
            obj_set(rec, "platform", json_str(p));
            obj_set(rec, "code", json_str(kv.first));
            obj_set(rec, "user_id", json_str(uid));
            obj_set(rec, "user_name", json_str(name));
            obj_set(rec, "age_minutes", json_num((double)age_min));
            results.push_back(std::move(rec));
        }
    }
    return results;
}

int PairingStore::clear_pending(const std::string& platform) {
    const core::FileLock lock(store_lock_path(pairing_dir()));   // the whole read-modify-write, one step
    int count = 0;
    std::vector<std::string> platforms;
    if (!platform.empty()) platforms.push_back(platform);
    else platforms = all_platforms("pending");
    for (const std::string& p : platforms) {
        Json pending = load_json(pending_path(p));
        count += (int)pending.obj.size();
        save_json(pending_path(p), json_obj());
    }
    return count;
}

}  // gateway
}  // dd

// Ported from tools/mcp_schema_cache.py: config_fingerprint, the entry accessors,
// and the on-disk half (_cache_path, _load_all, _save_all, get_cached_entry,
// has_cached_entry, write_cache_entry, clear_cache_entry).
//
// The file is a whole cache read and rewritten as one object, so every write is
// read-modify-write: the lock is what makes two threads writing two servers not
// lose one of them. It is a boot-path and operator-command lock, not a hot one.
#include "dd/tools/mcp_schema_cache.hpp"
#include "dd/core/sha256.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <stdlib.h>    // mkstemp
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace dd {
namespace tools {

using namespace core;

namespace {
std::mutex g_cache_mu;   // Python's _cache_lock: the read-modify-write is one step

// Python truthiness for a JSON value (used by the `x or default` idiom).
bool json_truthy(const Json& v) {
    switch (v.type) {
        case Json::Null: return false;
        case Json::Bool: return v.b;
        case Json::Num: return v.num != 0.0;
        case Json::Str: return !v.str.empty();
        case Json::Arr: return !v.arr.empty();
        case Json::Obj: return !v.obj.empty();
    }
    return false;
}

// `config.get(key) or fallback` -> deep copy of value, else fallback.
Json or_default(const Json* v, const Json& fallback) {
    if (v && json_truthy(*v)) return json_deep_copy(*v);
    return fallback;
}

// `config.get(key)` -> value or null (no truthiness coercion).
Json get_or_null(const Json& o, const std::string& key) {
    const Json* v = json_get(o, key);
    if (v) return json_deep_copy(*v);
    return Json{};  // Null
}

// `sorted(value or [])` over a list of strings -> Json array.
Json sorted_str_arr(const Json* v) {
    std::vector<std::string> items;
    if (v && v->type == Json::Arr) {
        for (const auto& e : v->arr) {
            if (e && e->type == Json::Str) items.push_back(e->str);
        }
    }
    std::sort(items.begin(), items.end());
    std::vector<JsonSP> elems;
    elems.reserve(items.size());
    for (const auto& s : items) {
        Json j;
        j.type = Json::Str;
        j.str = s;
        elems.push_back(std::make_shared<Json>(std::move(j)));
    }
    return json_arr(std::move(elems));
}

bool write_all(int fd, const char* p, size_t left) {
    while (left > 0) {
        const ssize_t w = ::write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        p += static_cast<size_t>(w);
        left -= static_cast<size_t>(w);
    }
    return true;
}
}  // namespace

std::string config_fingerprint(const Json& config) {
    const Json* tools_filter_v = json_get(config, "tools");
    Json tools_filter = or_default(tools_filter_v, json_obj());

    // payload keys inserted in alphabetical order (json.dumps sort_keys=True).
    Json payload;
    payload.type = Json::Obj;
    obj_set(payload, "args", or_default(json_get(config, "args"), json_arr()));
    obj_set(payload, "command", get_or_null(config, "command"));
    obj_set(payload, "tools_exclude", sorted_str_arr(json_get(tools_filter, "exclude")));
    obj_set(payload, "tools_include", sorted_str_arr(json_get(tools_filter, "include")));
    obj_set(payload, "transport", get_or_null(config, "transport"));
    obj_set(payload, "url", get_or_null(config, "url"));

    std::string raw = json_dump(payload);
    std::string full = sha256_hex(raw);
    return full.substr(0, 16);
}

std::string server_fingerprint(const Json& server_config) {
    // dd gates a tool by name with `tools: {name: {enabled: false}}`; the disabled
    // set is what changes the manifest the boot registers, so it is the exclude list.
    std::vector<JsonSP> off;
    if (const Json* tmap = json_get(server_config, "tools"); tmap && tmap->type == Json::Obj) {
        for (const auto& [name, en] : tmap->obj) {
            if (!en || en->type != Json::Obj) continue;
            if (const Json* on = json_get(*en, "enabled"); on && on->type == Json::Bool && !on->b)
                off.push_back(std::make_shared<Json>(json_str(name)));
        }
    }
    Json keyed = json_obj();
    for (const char* k : {"command", "args", "url", "transport"})
        if (const Json* v = json_get(server_config, k)) obj_set(keyed, k, json_deep_copy(*v));
    Json tools = json_obj();
    obj_set(tools, "exclude", json_arr(std::move(off)));
    obj_set(keyed, "tools", std::move(tools));
    return config_fingerprint(keyed);
}

std::vector<JsonSP> tools_from_cache_entry(const Json& entry) {
    const Json* t = json_get(entry, "tools");
    if (t && t->type == Json::Arr) return t->arr;
    return {};
}

std::vector<JsonSP> utility_tools_from_cache_entry(const Json& entry) {
    const Json* u = json_get(entry, "utility_tools");
    if (u && u->type == Json::Arr) return u->arr;
    return {};
}

std::filesystem::path cache_path(const std::filesystem::path& cache_dir) {
    return cache_dir / "mcp_schema_cache.json";
}

Json load_all(const std::filesystem::path& file) {
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return json_obj();   // no cache yet: every server lists live
    std::string text;
    bool ok = true;
    char buf[16384];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (n == 0) break;
        text.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    if (!ok) return json_obj();
    try {
        Json data = json_parse(text);
        if (data.type != Json::Obj) return json_obj();   // a list or a scalar is not a cache
        return data;
    } catch (const std::exception&) {
        return json_obj();   // truncated or corrupt: fall back to listing, as Python does
    }
}

bool save_all(const std::filesystem::path& file, const Json& data) {
    std::error_code ec;
    std::filesystem::path dir = file.parent_path();
    if (dir.empty()) dir = ".";
    if (!std::filesystem::is_directory(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        if (ec || !std::filesystem::is_directory(dir, ec)) return false;
    }
    struct stat ls {};
    if (::lstat(file.c_str(), &ls) == 0) {
        if (S_ISLNK(ls.st_mode) || !S_ISREG(ls.st_mode)) return false;  // never rename over a link, a dir, or a device
    }

    // temp file in the same directory + rename: a reader sees the old cache or the
    // new one, never a half-written one. Same rule fs_tools.cpp writes files with.
    std::string tmpl = (dir / ("." + file.filename().string() + ".dd-tmp-XXXXXX")).string();
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) return false;
    const std::string text = json_dump(data);
    bool good = write_all(fd, text.data(), text.size());
    // The cache holds server manifests, which carry operator headers nowhere, but
    // 0600 is what Python writes it with and it costs nothing to keep the same mode.
    if (good && ::fchmod(fd, 0600) != 0) good = false;
    if (good && ::fsync(fd) != 0) good = false;
    if (::close(fd) != 0) good = false;
    if (good && ::rename(tmpl.c_str(), file.c_str()) != 0) good = false;
    if (good) {
        // The rename itself has to survive a crash, or the cache silently vanishes
        // and the next boot pays a live list it already paid for.
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    } else {
        ::unlink(tmpl.c_str());
    }
    return good;
}

Json get_cached_entry(const std::filesystem::path& file, const std::string& server_name,
                      const std::string& fingerprint) {
    std::lock_guard<std::mutex> lk(g_cache_mu);
    Json data = load_all(file);
    const Json* entry = json_get(data, server_name);
    if (!entry || entry->type != Json::Obj) return Json{};
    const Json* fp = json_get(*entry, "fingerprint");
    if (!fp || fp->type != Json::Str || fp->str != fingerprint) return Json{};
    return json_deep_copy(*entry);
}

bool has_cached_entry(const std::filesystem::path& file, const std::string& server_name,
                      const std::string& fingerprint) {
    return get_cached_entry(file, server_name, fingerprint).type == Json::Obj;
}

bool write_cache_entry(const std::filesystem::path& file, const std::string& server_name,
                       const std::string& fingerprint, const Json& tools,
                       const Json& utility_tools) {
    Json entry = json_obj();
    obj_set(entry, "fingerprint", json_str(fingerprint));
    obj_set(entry, "tools", json_deep_copy(tools));
    obj_set(entry, "utility_tools",
            utility_tools.type == Json::Arr ? json_deep_copy(utility_tools) : json_arr());

    std::lock_guard<std::mutex> lk(g_cache_mu);
    Json data = load_all(file);
    const Json* old = json_get(data, server_name);
    if (old && json_dump(*old) == json_dump(entry)) return true;   // unchanged: no rewrite, mtime stays
    obj_set(data, server_name, std::move(entry));
    return save_all(file, data);
}

bool clear_cache_entry(const std::filesystem::path& file, const std::string& server_name) {
    std::lock_guard<std::mutex> lk(g_cache_mu);
    Json data = load_all(file);
    if (!json_get(data, server_name)) return true;   // nothing to drop
    data.obj.erase(std::remove_if(data.obj.begin(), data.obj.end(),
                                  [&](const auto& kv) { return kv.first == server_name; }),
                   data.obj.end());
    return save_all(file, data);
}

}  // namespace tools
}  // namespace dd

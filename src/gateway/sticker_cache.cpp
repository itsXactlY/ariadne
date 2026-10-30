// gateway/sticker_cache.py -> dd::gateway::sticker_cache
#include "dd/gateway/sticker_cache.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

namespace dd {
namespace gateway {

using namespace core;

std::filesystem::path cache_path() {
    return core::get_ariadne_home() / "sticker_cache.json";
}

static double now_seconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Json load_cache() {
    std::filesystem::path p = cache_path();
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

void save_cache(const Json& cache) {
    std::filesystem::path p = cache_path();
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out << json_dump(cache);
}

std::optional<Json> get_cached_description(const std::string& file_unique_id) {
    Json cache = load_cache();
    const Json* entry = json_get(cache, file_unique_id);
    if (!entry) return std::nullopt;
    return json_deep_copy(*entry);
}

void cache_sticker_description(const std::string& file_unique_id,
                               const std::string& description,
                               const std::string& emoji,
                               const std::string& set_name) {
    Json cache = load_cache();
    Json entry = json_obj();
    obj_set(entry, "description", json_str(description));
    obj_set(entry, "emoji", json_str(emoji));
    obj_set(entry, "set_name", json_str(set_name));
    obj_set(entry, "cached_at", json_num(now_seconds()));
    obj_set(cache, file_unique_id, entry);
    save_cache(cache);
}

std::string build_sticker_injection(const std::string& description,
                                    const std::string& emoji,
                                    const std::string& set_name) {
    std::string context;
    if (!set_name.empty() && !emoji.empty()) {
        context = " " + emoji + " from \"" + set_name + "\"";
    } else if (!emoji.empty()) {
        context = " " + emoji;
    }
    return "[The user sent a sticker" + context + "~ It shows: \"" + description +
           "\" (=^.w.^=)]";
}

std::string build_animated_sticker_injection(const std::string& emoji) {
    (void)emoji;
    return "[The user sent an animated sticker~ I can't see animated ones yet]";
}

}  // gateway
}  // dd

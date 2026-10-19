// Pure port of agent/kv_watermarks.py (watermark math + slot helpers).
#include "dd/agent/kv_watermarks.hpp"
#include <optional>
#include <string>
#include <cctype>
#include <cmath>
#include "dd/core/json.hpp"

namespace dd {
namespace agent {

using namespace core;

static const char* kOK = "ok";
static const char* kLOW = "low";
static const char* kHIGH = "high";
static const char* kHARD = "hard";

// Python bool(x) over a parsed-JSON value.
static bool truthy_json(const Json& v) {
    switch (v.type) {
        case Json::Null: return false;
        case Json::Bool: return v.b;
        case Json::Num:  return v.num != 0.0;
        case Json::Str:  return !v.str.empty();
        case Json::Arr:  return !v.arr.empty();
        case Json::Obj:  return !v.obj.empty();
    }
    return false;
}

// s.get("id") == slot_id (ids are ints in the /slots payload).
static bool id_matches(const Json& s, int slot_id) {
    const Json* id = json_get(s, "id");
    if (!id || id->type != Json::Num) return false;
    return id->num == static_cast<double>(slot_id);
}

// int(x) for a numeric JSON value.
static long long to_int_json(const Json& v) {
    if (v.type == Json::Bool) return v.b ? 1 : 0;
    if (v.type == Json::Num) return static_cast<long long>(std::trunc(v.num));
    return 0;
}

std::string Watermarks::level(int pool_tokens) const {
    if (fast_layer_tokens <= 0) return kOK;
    double fill = static_cast<double>(pool_tokens) / static_cast<double>(fast_layer_tokens);
    if (fill >= hard) return kHARD;
    if (fill >= high) return kHIGH;
    if (fill >= low) return kLOW;
    return kOK;
}

int Watermarks::hard_tokens() const {
    return static_cast<int>(static_cast<double>(fast_layer_tokens) * hard);
}

double Watermarks::high_ratio_of_hard() const {
    return hard ? (high / hard) : 0.0;
}

std::optional<std::string> server_root(const std::string& base_url) {
    std::string base = base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0)
        base = base.substr(0, base.size() - 3);
    std::string low = base;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low.find("127.0.0.1") == std::string::npos && low.find("localhost") == std::string::npos)
        return std::nullopt;
    return base;
}

int slot_tokens(const core::Json& slots, int slot_id) {
    if (slots.type != Json::Arr) return 0;
    for (const JsonSP& sp : slots.arr) {
        if (!sp) continue;
        if (id_matches(*sp, slot_id)) {
            const Json* n = json_get(*sp, "n_prompt_tokens");
            if (!n || !truthy_json(*n)) return 0;
            return static_cast<int>(to_int_json(*n));
        }
    }
    return 0;
}

bool slot_busy(const core::Json& slots, int slot_id) {
    if (slots.type != Json::Arr) return false;
    for (const JsonSP& sp : slots.arr) {
        if (!sp) continue;
        if (id_matches(*sp, slot_id)) {
            const Json* p = json_get(*sp, "is_processing");
            return p && truthy_json(*p);
        }
    }
    return false;
}

}  // namespace agent
}  // namespace dd

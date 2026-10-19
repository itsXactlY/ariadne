// Pure subset of gateway/mirror.py (see header for the porting boundary).
#include "dd/gateway/mirror.hpp"
#include <string>
#include <optional>
#include <sstream>
#include <cctype>
#include "dd/core/json.hpp"

namespace dd {
namespace gateway {

using core::Json;

static std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string num_str(double v) {
    if (v == static_cast<long long>(v) && v >= -1e15 && v <= 1e15)
        return std::to_string(static_cast<long long>(v));
    std::ostringstream o;
    o << v;
    return o.str();
}

static std::string str_of(const Json& j) {
    switch (j.type) {
        case Json::Null: return "None";
        case Json::Bool: return j.b ? "True" : "False";
        case Json::Num: return num_str(j.num);
        case Json::Str: return j.str;
        case Json::Arr: {
            std::string r = "[";
            for (size_t i = 0; i < j.arr.size(); ++i) {
                if (i) r += ", ";
                r += str_of(*j.arr[i]);
            }
            return r + "]";
        }
        case Json::Obj: {
            std::string r = "{";
            for (size_t i = 0; i < j.obj.size(); ++i) {
                if (i) r += ", ";
                r += j.obj[i].first + ": " + str_of(*j.obj[i].second);
            }
            return r + "}";
        }
    }
    return "";
}

static bool truthy(const Json* p) {
    if (!p) return false;
    switch (p->type) {
        case Json::Null: return false;
        case Json::Bool: return p->b;
        case Json::Num: return p->num != 0.0;
        case Json::Str: return !p->str.empty();
        case Json::Arr: return !p->arr.empty();
        case Json::Obj: return !p->obj.empty();
    }
    return false;
}

std::optional<std::string> find_session_id(
    const std::string& platform,
    const std::string& chat_id,
    const std::optional<std::string>& thread_id,
    const core::Json& index) {
    std::string platform_lower = to_lower(platform);
    const Json* best_val = nullptr;
    std::string best_updated = "";
    Json origin_empty;
    origin_empty.type = Json::Obj;

    if (index.type == Json::Obj) {
        for (const auto& kv : index.obj) {
            const Json& entry = *kv.second;
            if (entry.type != Json::Obj) continue;

            const Json* origin_p = json_get(entry, "origin");
            const Json* origin = truthy(origin_p) ? origin_p : &origin_empty;

            const Json* op = json_get(*origin, "platform");
            const Json* ep = json_get(entry, "platform");
            std::string chosen;
            if (truthy(op)) chosen = str_of(*op);
            else chosen = ep ? str_of(*ep) : "";
            if (to_lower(chosen) != platform_lower) continue;

            const Json* oc = json_get(*origin, "chat_id");
            std::string origin_chat_id = oc ? str_of(*oc) : "";
            if (origin_chat_id != chat_id) continue;

            if (thread_id.has_value()) {
                const Json* ot = json_get(*origin, "thread_id");
                std::string ots = truthy(ot) ? str_of(*ot) : "";
                if (ots != *thread_id) continue;
            }

            const Json* up = json_get(entry, "updated_at");
            std::string updated = up ? str_of(*up) : "";
            if (updated > best_updated) {
                best_updated = updated;
                best_val = json_get(entry, "session_id");
            }
        }
    }

    if (best_val && best_val->type != Json::Null)
        return str_of(*best_val);
    return std::nullopt;
}

}  // namespace gateway
}  // namespace dd

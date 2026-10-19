// Pure subset of gateway/channel_directory.py (see header for the boundary).
#include "dd/gateway/channel_directory.hpp"
#include "dd/core/json.hpp"
#include <string>
#include <optional>
#include <cctype>
#include <sstream>

namespace dd {
namespace gateway {

using core::Json;

static std::string strip_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r' ||
                     s[a] == '\f' || s[a] == '\v'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' ||
                     s[b - 1] == '\r' || s[b - 1] == '\f' || s[b - 1] == '\v'))
        --b;
    return s.substr(a, b - a);
}

static std::string to_lower(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

// Python value truthiness over the Json model.
static bool truthy(const Json* p) {
    if (!p) return false;  // absent / None
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

// Python str() of a Json value (containers render via repr, like str()).
static std::string repr_of(const Json& j);

static std::string str_of(const Json* p) {
    if (!p) return "None";  // None
    if (p->type == Json::Str) return p->str;
    return repr_of(*p);
}

static std::string repr_of(const Json& j) {
    switch (j.type) {
        case Json::Null: return "None";
        case Json::Bool: return j.b ? "True" : "False";
        case Json::Num: {
            double v = j.num;
            if (v == (double)(long long)v && v >= -9.2e18 && v <= 9.2e18)
                return std::to_string((long long)v);
            std::ostringstream o;
            o << v;
            return o.str();
        }
        case Json::Str: {
            std::string out = "'";
            for (char c : j.str) {
                if (c == '\\' || c == '\'') { out.push_back('\\'); out.push_back(c); }
                else if (c == '\n') { out += "\\n"; }
                else out.push_back(c);
            }
            out.push_back('\'');
            return out;
        }
        case Json::Arr: {
            std::string out = "[";
            for (size_t i = 0; i < j.arr.size(); ++i) {
                if (i) out += ", ";
                out += repr_of(*j.arr[i]);
            }
            out.push_back(']');
            return out;
        }
        case Json::Obj: {
            std::string out = "{";
            for (size_t i = 0; i < j.obj.size(); ++i) {
                if (i) out += ", ";
                Json k;
                k.type = Json::Str;
                k.str = j.obj[i].first;
                out += repr_of(k);
                out += ": ";
                out += repr_of(*j.obj[i].second);
            }
            out.push_back('}');
            return out;
        }
    }
    return "";
}

// value.lstrip("#") : strip all leading '#'
static std::string lstrip_hash(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && s[a] == '#') ++a;
    return s.substr(a);
}

std::string normalize_channel_query(const std::string& value) {
    return to_lower(strip_ws(lstrip_hash(value)));
}

std::string channel_target_name(const std::string& platform_name, const Json& channel) {
    std::string name = str_of(json_get(channel, "name"));
    if (platform_name == "discord" && truthy(json_get(channel, "guild")))
        return "#" + name;
    if (platform_name != "discord" && truthy(json_get(channel, "type")))
        return name + " (" + str_of(json_get(channel, "type")) + ")";
    return name;
}

std::optional<std::string> session_entry_id(const Json& origin) {
    const Json* chat = json_get(origin, "chat_id");
    if (!truthy(chat)) return std::nullopt;
    const Json* thread = json_get(origin, "thread_id");
    if (truthy(thread)) return str_of(chat) + ":" + str_of(thread);
    return str_of(chat);
}

std::string session_entry_name(const Json& origin) {
    const Json* chat_name = json_get(origin, "chat_name");
    const Json* user_name = json_get(origin, "user_name");
    const Json* chat_id = json_get(origin, "chat_id");
    std::string base;
    if (truthy(chat_name)) base = str_of(chat_name);
    else if (truthy(user_name)) base = str_of(user_name);
    else base = str_of(chat_id);
    const Json* thread = json_get(origin, "thread_id");
    if (!truthy(thread)) return base;
    const Json* topic = json_get(origin, "chat_topic");
    std::string label = truthy(topic) ? str_of(topic) : "topic " + str_of(thread);
    return base + " / " + label;
}

}  // namespace gateway
}  // namespace dd

// Full pure port of agent/prompt_caching.py (Anthropic system_and_3 caching).
#include "dd/agent/prompt_caching.hpp"
#include <string>
#include <vector>
#include "dd/core/json.hpp"

namespace dd {
namespace agent {

using namespace core;

// Mirrors _apply_cache_marker; mutates one message object in place.
static void apply_cache_marker(Json& msg, const Json& marker, bool native_anthropic) {
    const Json* role = json_get(msg, "role");
    std::string role_s = (role && role->type == Json::Str) ? role->str : std::string();
    const Json* content = json_get(msg, "content");

    if (role_s == "tool") {
        if (native_anthropic) obj_set(msg, "cache_control", json_deep_copy(marker));
        return;
    }

    // content is None or content == ""
    if (!content || content->type == Json::Null ||
        (content->type == Json::Str && content->str.empty())) {
        obj_set(msg, "cache_control", json_deep_copy(marker));
        return;
    }

    if (content->type == Json::Str) {
        std::string cstr = content->str;
        Json item = json_obj();
        obj_set(item, "type", json_str("text"));
        obj_set(item, "text", json_str(cstr));
        obj_set(item, "cache_control", json_deep_copy(marker));
        Json arr = json_arr();
        arr_push(arr, item);
        obj_set(msg, "content", arr);
        return;
    }

    // isinstance(content, list) and content:
    if (content->type == Json::Arr && !content->arr.empty()) {
        JsonSP last = content->arr.back();
        if (last && last->type == Json::Obj) obj_set(*last, "cache_control", json_deep_copy(marker));
    }
}

std::vector<Json> apply_anthropic_cache_control(const std::vector<Json>& api_messages,
                                                const std::string& cache_ttl,
                                                bool native_anthropic) {
    std::vector<Json> messages;
    messages.reserve(api_messages.size());
    for (const Json& m : api_messages) messages.push_back(json_deep_copy(m));
    if (messages.empty()) return messages;

    Json marker = json_obj();
    obj_set(marker, "type", json_str("ephemeral"));
    if (cache_ttl == "1h") obj_set(marker, "ttl", json_str("1h"));

    int breakpoints_used = 0;
    {
        const Json* r = json_get(messages[0], "role");
        if (r && r->type == Json::Str && r->str == "system") {
            apply_cache_marker(messages[0], marker, native_anthropic);
            breakpoints_used = 1;
        }
    }

    int remaining = 4 - breakpoints_used;
    std::vector<size_t> non_sys;
    for (size_t i = 0; i < messages.size(); ++i) {
        const Json* r = json_get(messages[i], "role");
        std::string rs = (r && r->type == Json::Str) ? r->str : std::string();
        if (rs != "system") non_sys.push_back(i);
    }
    size_t start = non_sys.size() > static_cast<size_t>(remaining)
                       ? non_sys.size() - static_cast<size_t>(remaining)
                       : 0;
    for (size_t k = start; k < non_sys.size(); ++k)
        apply_cache_marker(messages[non_sys[k]], marker, native_anthropic);

    return messages;
}

}  // namespace agent
}  // namespace dd

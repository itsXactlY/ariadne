#pragma once
// Pure message-scrubbing logic from daedalus_state.py: stale tool-call-marker +
// background-review-harness classifiers and their in-session strip walkers, plus
// the _is_production_state_db path algebra (operates on already-tokenized Path.parts
// vectors; fs resolution stays at the caller). NOT ported (env/threading/fs-bound):
// _production_state_roots (_real_platform_state_root + expanduser/resolve), _ensure_test_isolation
// (pytest/env/threading), get_last_init_error (threading global _last_init_error),
// _set_last_init_error, WAL-fallback warn-path sets.
#include <cctype>
#include <algorithm>
#include <regex>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace core {

// Python truthiness for a JSON value (absent == None == falsy).
static inline bool json_truthy(const Json* v) {
    if (!v) return false;
    switch (v->type) {
        case Json::Null: return false;
        case Json::Bool: return v->b;
        case Json::Num: return v->num != 0.0;
        case Json::Str: return !v->str.empty();
        case Json::Arr: return !v->arr.empty();
        case Json::Obj: return !v->obj.empty();
    }
    return false;
}

inline std::string lstrip_ws(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    return s.substr(a, b - a);
}

inline std::string strip_ws(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// msg.get("role") as a string, only when it is a Str.
static inline std::string role_str(const Json& msg) {
    const Json* r = json_get(msg, "role");
    return (r && r->type == Json::Str) ? r->str : std::string();
}

inline bool _is_stale_tool_call_marker_message(const Json& msg) {
    if (msg.type != Json::Obj) return false;
    if (role_str(msg) != "assistant") return false;
    if (!json_truthy(json_get(msg, "tool_calls"))) return false;
    const Json* c = json_get(msg, "content");
    if (!c || c->type != Json::Str) return false;
    static const std::regex marker_re(R"(^\[[A-Za-z_][A-Za-z0-9_.-]*\]$)");
    return std::regex_match(strip_ws(c->str), marker_re);
}

inline bool _is_background_review_harness_message(const Json& msg) {
    if (msg.type != Json::Obj) return false;
    const std::string role = role_str(msg);
    if (role != "user" && role != "system") return false;
    const Json* c = json_get(msg, "content");
    if (!c || c->type != Json::Str) return false;
    const std::string head = lstrip_ws(c->str);
    const std::string p1 = "Review the conversation above and update the skill library";
    const std::string p2 = "Review the conversation above and consider saving to memory";
    return head.rfind(p1, 0) == 0 || head.rfind(p2, 0) == 0;
}

// Mutates a copy of the message list in place, blanks stale marker content.
inline std::vector<Json> _strip_stale_tool_call_markers(std::vector<Json> messages) {
    for (auto& msg : messages) {
        if (_is_stale_tool_call_marker_message(msg)) obj_set(msg, "content", json_str(""));
    }
    return messages;
}

// Drops review-harness turns and the assistant reply that immediately follows each.
inline std::vector<Json> _strip_background_review_harness(std::vector<Json> messages) {
    std::vector<Json> out;
    bool skip_next_assistant = false;
    for (auto& msg : messages) {
        if (_is_background_review_harness_message(msg)) {
            skip_next_assistant = true;
            continue;
        }
        if (skip_next_assistant) {
            skip_next_assistant = false;
            if (msg.type == Json::Obj && role_str(msg) == "assistant") continue;
        }
        out.push_back(std::move(msg));
    }
    return out;
}

// resolved/root Path.parts prefix algebra == Path.relative_to guard.
inline bool _is_production_state_db(const std::vector<std::string>& resolved,
                                    const std::vector<std::string>& root) {
    if (root.size() > resolved.size()) return false;
    if (!std::equal(root.begin(), root.end(), resolved.begin())) return false;
    if (resolved.size() == root.size() + 1) return true;
    const std::size_t rel_n = resolved.size() - root.size();
    return rel_n == 3 && resolved[root.size()] == "profiles";
}

}  // namespace core
}  // namespace dd

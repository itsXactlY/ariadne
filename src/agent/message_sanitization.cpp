// Pure subset of agent/message_sanitization.py.
#include "dd/agent/message_sanitization.hpp"
#include <cstdio>
#include <string>
#include <vector>
#include "dd/core/json.hpp"

namespace dd {
namespace agent {

using namespace core;

static inline char hex4_buf[16];

static void append_uescape(std::string& out, unsigned char ch) {
    std::snprintf(hex4_buf, sizeof(hex4_buf), "\\u%04x", ch);
    out.append(hex4_buf);
}

std::string escape_invalid_chars_in_json_strings(const std::string& raw) {
    std::string out;
    bool in_string = false;
    size_t i = 0, n = raw.size();
    while (i < n) {
        unsigned char ch = static_cast<unsigned char>(raw[i]);
        if (in_string) {
            if (ch == '\\' && i + 1 < n) {
                out.push_back(raw[i]);
                out.push_back(raw[i + 1]);
                i += 2;
                continue;
            }
            if (ch == '"') {
                in_string = false;
                out.push_back(raw[i]);
            } else if (ch < 0x20) {
                append_uescape(out, ch);
            } else {
                out.push_back(raw[i]);
            }
        } else {
            if (ch == '"') in_string = true;
            out.push_back(raw[i]);
        }
        ++i;
    }
    return out;
}

std::string strip_non_ascii(const std::string& text) {
    std::string out;
    for (unsigned char c : text)
        if (c < 0x80) out.push_back(static_cast<char>(c));
    return out;
}

// Python str.strip() whitespace set.
static inline bool is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static std::string trim_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_ws(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && is_ws(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool close_interrupted_tool_sequence(std::vector<core::Json>& messages,
                                     const std::string& final_response) {
    if (messages.empty()) return false;
    Json& last = messages.back();
    if (last.type != Json::Obj) return false;
    const Json* role = json_get(last, "role");
    if (!(role && role->type == Json::Str && role->str == "tool")) return false;
    std::string text = trim_ws(final_response);
    if (text.empty()) text = "Operation interrupted.";
    Json msg = json_obj();
    obj_set(msg, "role", json_str("assistant"));
    obj_set(msg, "content", json_str(text));
    messages.push_back(msg);
    return true;
}

}  // namespace agent
}  // namespace dd

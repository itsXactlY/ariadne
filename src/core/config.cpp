// src/core/config.cpp — general YAML loader -> core::Json tree + typed getters.
#include "dd/core/config.hpp"
#include "dd/core/json.hpp"
#include "dd/core/yaml.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dd {
namespace core {

namespace {

std::string ltrim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
std::string rtrim(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\r')) --i;
    return s.substr(0, i);
}
std::string trim(const std::string& s) { return rtrim(ltrim(s)); }

int leading_indent(const std::string& s) {
    int n = 0;
    for (char c : s) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 2;
        else break;
    }
    return n;
}

// Quote state at end of text: 0 = balanced, else the open quote char.
int quote_state(const std::string& s) {
    int q = 0;
    for (size_t k = 0; k < s.size(); ++k) {
        char c = s[k];
        if (q == 0) {
            if (c == '"' || c == '\'') q = c;
        } else if (q == '"') {
            if (c == '\\') { ++k; }  // skip escaped char
            else if (c == '"') q = 0;
        } else {  // single quote: '' escapes a quote, backslash is literal
            if (c == '\'') {
                if (k + 1 < s.size() && s[k + 1] == '\'') ++k;
                else q = 0;
            }
        }
    }
    return q;
}

// Count trailing backslashes (odd => last one is a line-continuation escape).
int trailing_backslashes(const std::string& s) {
    int n = 0;
    for (size_t k = s.size(); k-- > 0;) {
        if (s[k] == '\\') ++n;
        else break;
    }
    return n;
}

void utf8_append(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

unsigned hex4(const std::string& s, size_t& k) {
    unsigned v = 0;
    for (int i = 0; i < 4 && k < s.size(); ++i, ++k) {
        char c = s[k];
        unsigned d = 0;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        v = v * 16 + d;
    }
    return v;
}

std::string parse_dq(const std::string& s) {
    // s includes surrounding double quotes.
    std::string out;
    for (size_t k = 1; k + 1 < s.size(); ++k) {
        char c = s[k];
        if (c != '\\') { out += c; continue; }
        ++k;
        if (k >= s.size()) break;
        char e = s[k];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case '"': out += '"'; break;
            case '/': out += '/'; break;
            case '\\': out += '\\'; break;
            case 'u': {
                ++k;                 // move past 'u'
                unsigned cp = hex4(s, k);
                --k;                 // for-loop ++k lands on the char after the hex
                utf8_append(out, cp);
                break;
            }
            default: out += e; break;
        }
    }
    return out;
}

std::string parse_sq(const std::string& s) {
    // s includes surrounding single quotes; '' -> '.
    std::string out;
    for (size_t k = 1; k + 1 < s.size(); ++k) {
        char c = s[k];
        if (c == '\'' && k + 2 < s.size() && s[k + 1] == '\'') { out += '\''; ++k; }
        else out += c;
    }
    return out;
}

bool is_number(const std::string& s) {
    if (s.empty()) return false;
    size_t k = 0;
    if (s[k] == '+' || s[k] == '-') ++k;
    bool digits = false;
    while (k < s.size() && (s[k] >= '0' && s[k] <= '9')) { ++k; digits = true; }
    if (k < s.size() && s[k] == '.') {
        ++k;
        while (k < s.size() && (s[k] >= '0' && s[k] <= '9')) { ++k; digits = true; }
    }
    if (!digits) return false;
    if (k < s.size() && (s[k] == 'e' || s[k] == 'E')) {
        ++k;
        if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
        if (k >= s.size() || !(s[k] >= '0' && s[k] <= '9')) return false;
        while (k < s.size() && (s[k] >= '0' && s[k] <= '9')) ++k;
    }
    return k == s.size();
}

// ---- forward decls ----
Json parse_inline(const std::string& val);
Json parse_seq(int& idx, int indent, const std::vector<std::pair<int, std::string>>& lines);
Json parse_map(int& idx, int indent, const std::vector<std::pair<int, std::string>>& lines);
Json parse_node(int& idx, int indent, const std::vector<std::pair<int, std::string>>& lines);

bool is_list_item(const std::string& text) {
    return !text.empty() && text[0] == '-' && (text.size() == 1 || text[1] == ' ');
}

// Split "key: value" at the first ": " (or a trailing ':').
void split_kv(const std::string& text, std::string& key, std::string& val) {
    size_t pos = text.find(": ");
    if (pos != std::string::npos) {
        key = trim(text.substr(0, pos));
        val = trim(text.substr(pos + 2));
        return;
    }
    if (!text.empty() && text.back() == ':') {
        key = trim(text.substr(0, text.size() - 1));
        val = "";
        return;
    }
    key = trim(text);
    val = "";
}

// Split an inline collection body on top-level commas (respecting quotes/brackets).
std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int q = 0, depth = 0;
    for (size_t k = 0; k < s.size(); ++k) {
        char c = s[k];
        if (q == 0) {
            if (c == '[' || c == '{') ++depth;
            else if (c == ']' || c == '}') --depth;
            else if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        }
        if (q == 0 && (c == '"' || c == '\'')) q = c;
        else if (q == '"' && c == '\\') { cur += c; if (k + 1 < s.size()) cur += s[++k]; continue; }
        else if (q == '"' && c == '"') q = 0;
        else if (q == '\'' && c == '\'') {
            if (k + 1 < s.size() && s[k + 1] == '\'') { cur += c; cur += s[++k]; continue; }
            q = 0;
        }
        cur += c;
    }
    if (!trim(cur).empty()) out.push_back(cur);
    return out;
}

Json parse_inline(const std::string& raw) {
    std::string val = trim(raw);
    if (val.empty()) return json_null();
    if (val.size() >= 2 && val.front() == '[' && val.back() == ']') {
        Json a = json_arr();
        std::string inner = val.substr(1, val.size() - 2);
        for (const auto& item : split_commas(inner)) arr_push(a, parse_inline(item));
        return a;
    }
    if (val.size() >= 2 && val.front() == '{' && val.back() == '}') {
        Json o = json_obj();
        std::string inner = val.substr(1, val.size() - 2);
        for (const auto& kv : split_commas(inner)) {
            std::string k, v;
            split_kv(kv, k, v);
            obj_set(o, unquote(k), parse_inline(v));
        }
        return o;
    }
    if (val.front() == '"') return json_str(parse_dq(val));
    if (val.front() == '\'') return json_str(parse_sq(val));

    // bare scalar: strip trailing " #" comment
    size_t hash = val.find(" #");
    if (hash != std::string::npos) val = rtrim(val.substr(0, hash));
    if (val == "true" || val == "True") return json_bool(true);
    if (val == "false" || val == "False") return json_bool(false);
    if (val == "null" || val == "Null" || val == "~") return json_null();
    if (is_number(val)) return json_num(std::stod(val));
    return json_str(val);
}

Json parse_node(int& idx, int indent, const std::vector<std::pair<int, std::string>>& lines) {
    (void)indent;
    if (is_list_item(lines[idx].second)) return parse_seq(idx, lines[idx].first, lines);
    return parse_map(idx, lines[idx].first, lines);
}

Json parse_seq(int& idx, int indent, const std::vector<std::pair<int, std::string>>& lines) {
    Json s = json_arr();
    while (idx < (int)lines.size() && lines[idx].first == indent && is_list_item(lines[idx].second)) {
        std::string text = lines[idx].second;
        ++idx;
        std::string item = trim(text.size() > 1 ? text.substr(1) : "");
        Json v;
        if (item.empty()) {
            if (idx < (int)lines.size() && lines[idx].first > indent)
                v = parse_node(idx, lines[idx].first, lines);
            else
                v = json_null();
        } else {
            v = parse_inline(item);
        }
        arr_push(s, v);
    }
    return s;
}

Json parse_map(int& idx, int indent, const std::vector<std::pair<int, std::string>>& lines) {
    Json m = json_obj();
    while (idx < (int)lines.size() && lines[idx].first == indent && !is_list_item(lines[idx].second)) {
        std::string text = lines[idx].second;
        ++idx;
        std::string key, val;
        split_kv(text, key, val);
        Json value;
        if (val.empty()) {
            if (idx < (int)lines.size() && lines[idx].first > indent)
                value = parse_node(idx, lines[idx].first, lines);
            else if (idx < (int)lines.size() && lines[idx].first == indent && is_list_item(lines[idx].second))
                value = parse_seq(idx, indent, lines);
            else
                value = json_null();
        } else {
            value = parse_inline(val);
        }
        obj_set(m, unquote(key), value);
    }
    return m;
}

// ---- pre-pass: fold multi-line quoted scalars into logical lines ----
Json parse_yaml_impl(const std::string& text) {
    std::vector<std::string> raw;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) raw.push_back(line);

    std::vector<std::pair<int, std::string>> lines;  // {indent, content}
    for (size_t i = 0; i < raw.size(); ++i) {
        std::string content = ltrim(raw[i]);
        if (content.empty() || content[0] == '#') continue;
        int indent = leading_indent(raw[i]);
        std::string text = raw[i].substr(indent);
        text = rtrim(text);
        // Fold unterminated quoted scalars.
        while (quote_state(text) != 0 && i + 1 < raw.size()) {
            ++i;
            std::string cont = rtrim(ltrim(raw[i]));
            if (trailing_backslashes(text) % 2 == 1) {
                text.pop_back();  // drop the line-continuation backslash
                text += cont;     // join with no space
            } else {
                if (!text.empty()) text += ' ';
                text += cont;     // fold to a single space
            }
        }
        lines.emplace_back(indent, text);
    }

    int idx = 0;
    if (lines.empty()) return json_null();
    return parse_node(idx, lines[0].first, lines);
}

}  // anon

Json parse_yaml(const std::string& text) { return parse_yaml_impl(text); }

// ---- Config getters ----
const Json* Config::get(const std::string& path) const {
    const Json* cur = root.type == Json::Obj ? &root : nullptr;
    size_t pos = 0;
    while (true) {
        size_t dot = path.find('.', pos);
        std::string seg = path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (!cur || cur->type != Json::Obj) return nullptr;
        const Json* next = nullptr;
        for (const auto& kv : cur->obj) {
            if (kv.first == seg) { next = kv.second.get(); break; }
        }
        cur = next;
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return cur;
}

bool Config::has(const std::string& path) const {
    const Json* n = get(path);
    return n && n->type != Json::Null;
}

std::string Config::get_string(const std::string& path) const {
    const Json* n = get(path);
    if (!n) return "";
    if (n->type == Json::Str) return n->str;
    if (n->type == Json::Null) return "";
    if (n->type == Json::Bool) return n->b ? "true" : "false";
    if (n->type == Json::Num) {
        double d = n->num;
        if (d == (long long)d) return std::to_string((long long)d);
        return std::to_string(d);
    }
    return json_dump(*n);
}

long long Config::get_int(const std::string& path) const {
    const Json* n = get(path);
    if (!n) return 0;
    if (n->type == Json::Num) return (long long)n->num;
    if (n->type == Json::Bool) return n->b ? 1 : 0;
    if (n->type == Json::Str) {
        try { return std::stoll(n->str); } catch (...) { return 0; }
    }
    return 0;
}

double Config::get_double(const std::string& path) const {
    const Json* n = get(path);
    if (!n) return 0.0;
    if (n->type == Json::Num) return n->num;
    if (n->type == Json::Str) {
        try { return std::stod(n->str); } catch (...) { return 0.0; }
    }
    return 0.0;
}

bool Config::get_bool(const std::string& path) const {
    const Json* n = get(path);
    if (!n) return false;
    if (n->type == Json::Bool) return n->b;
    if (n->type == Json::Num) return n->num != 0;
    if (n->type == Json::Str) {
        const std::string& s = n->str;
        return s == "true" || s == "True" || s == "yes" || s == "on";
    }
    return false;
}

const Json* Config::get_map(const std::string& path) const {
    const Json* n = get(path);
    return (n && n->type == Json::Obj) ? n : nullptr;
}

const Json* Config::get_list(const std::string& path) const {
    const Json* n = get(path);
    return (n && n->type == Json::Arr) ? n : nullptr;
}

Config load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open config: " + path);
    std::stringstream buf;
    buf << f.rdbuf();
    return Config(parse_yaml(buf.str()));
}

}  // namespace core
}  // namespace dd

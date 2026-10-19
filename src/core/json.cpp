#include "dd/core/json.hpp"

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace dd {
namespace core {

static void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        ++i;
}

static Json parse_value(const std::string& s, size_t& i);

static std::string parse_string_raw(const std::string& s, size_t& i) {
    // i points at opening quote.
    if (i >= s.size() || s[i] != '"') throw std::runtime_error("json: expected string");
    ++i;
    std::string out;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return out;
        if (c != '\\') { out.push_back(c); continue; }
        if (i >= s.size()) break;
        char e = s[i++];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 > s.size()) throw std::runtime_error("json: bad \\u");
                unsigned cp = std::stoul(s.substr(i, 4), nullptr, 16);
                i += 4;
                // Encode code point as UTF-8 (BMP only; no surrogate pairing).
                if (cp < 0x80) out.push_back((char)cp);
                else if (cp < 0x800) {
                    out.push_back((char)(0xC0 | (cp >> 6)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back((char)(0xE0 | (cp >> 12)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
            } break;
            default: out.push_back('\\'); out.push_back(e); break;
        }
    }
    throw std::runtime_error("json: unterminated string");
}

static Json parse_value(const std::string& s, size_t& i) {
    skip_ws(s, i);
    if (i >= s.size()) throw std::runtime_error("json: eof");
    char c = s[i];
    if (c == '{') {
        Json o; o.type = Json::Obj;
        ++i;
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') { ++i; return o; }
        while (true) {
            skip_ws(s, i);
            auto key = parse_string_raw(s, i);
            skip_ws(s, i);
            if (i >= s.size() || s[i] != ':') throw std::runtime_error("json: expected ':'");
            ++i;
            auto val = parse_value(s, i);
            o.obj.emplace_back(key, std::make_shared<Json>(std::move(val)));
            skip_ws(s, i);
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; return o; }
            throw std::runtime_error("json: expected ',' or '}'");
        }
    }
    if (c == '[') {
        Json a; a.type = Json::Arr;
        ++i;
        skip_ws(s, i);
        if (i < s.size() && s[i] == ']') { ++i; return a; }
        while (true) {
            auto val = parse_value(s, i);
            a.arr.push_back(std::make_shared<Json>(std::move(val)));
            skip_ws(s, i);
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; return a; }
            throw std::runtime_error("json: expected ',' or ']'");
        }
    }
    if (c == '"') {
        Json v; v.type = Json::Str; v.str = parse_string_raw(s, i);
        return v;
    }
    if (s.compare(i, 4, "true") == 0) { i += 4; return json_bool(true); }
    if (s.compare(i, 5, "false") == 0) { i += 5; return json_bool(false); }
    if (s.compare(i, 4, "null") == 0) { i += 4; return json_null(); }
    // number
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    bool is_real = false;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' ||
                            s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
        if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') is_real = true;
        ++i;
    }
    if (i == start) throw std::runtime_error("json: bad number");
    Json v; v.type = Json::Num; v.num = std::stod(s.substr(start, i - start));
    (void)is_real;
    return v;
}

Json json_parse(const std::string& text) {
    size_t i = 0;
    auto v = parse_value(text, i);
    skip_ws(text, i);
    return v;
}

static void dump_str(const Json& v, std::string& out) {
    switch (v.type) {
        case Json::Null: out += "null"; break;
        case Json::Bool: out += v.b ? "true" : "false"; break;
        case Json::Num: {
            if (v.num == (long long)v.num && std::abs(v.num) < 1e15)
                out += std::to_string((long long)v.num);
            else {
                std::ostringstream os; os.precision(17); os << v.num; out += os.str();
            }
        } break;
        case Json::Str: {
            out.push_back('"');
            for (unsigned char c : v.str) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20) {
                            char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c);
                            out += buf;
                        } else out.push_back((char)c);  // UTF-8 pass-through
                }
            }
            out.push_back('"');
        } break;
        case Json::Arr: {
            out.push_back('[');
            for (size_t k = 0; k < v.arr.size(); ++k) {
                if (k) out.push_back(',');
                dump_str(*v.arr[k], out);
            }
            out.push_back(']');
        } break;
        case Json::Obj: {
            out.push_back('{');
            for (size_t k = 0; k < v.obj.size(); ++k) {
                if (k) out.push_back(',');
                Json key; key.type = Json::Str; key.str = v.obj[k].first;
                dump_str(key, out);
                out.push_back(':');
                dump_str(*v.obj[k].second, out);
            }
            out.push_back('}');
        } break;
    }
}

std::string json_dump(const Json& v) {
    std::string out;
    dump_str(v, out);
    return out;
}

Json json_null() { return Json{}; }
Json json_bool(bool x) { Json v; v.type = Json::Bool; v.b = x; return v; }
Json json_num(double x) { Json v; v.type = Json::Num; v.num = x; return v; }
Json json_str(std::string s) { Json v; v.type = Json::Str; v.str = std::move(s); return v; }
Json json_arr(std::vector<JsonSP> elems) {
    Json v; v.type = Json::Arr; v.arr = std::move(elems); return v;
}
Json json_obj(std::vector<std::pair<std::string, JsonSP>> members) {
    Json v; v.type = Json::Obj; v.obj = std::move(members); return v;
}

Json json_arr() { Json v; v.type = Json::Arr; return v; }
Json json_obj() { Json v; v.type = Json::Obj; return v; }
void arr_push(Json& a, const Json& v) { a.arr.push_back(std::make_shared<Json>(v)); }
void obj_set(Json& o, const std::string& key, const Json& v) {
    for (auto& kv : o.obj)
        if (kv.first == key) { kv.second = std::make_shared<Json>(v); return; }
    o.obj.emplace_back(key, std::make_shared<Json>(v));
}

const Json* json_get(const Json& o, const std::string& key) {
    if (o.type != Json::Obj) return nullptr;
    for (const auto& [k, sp] : o.obj)
        if (k == key) return sp.get();
    return nullptr;
}

Json json_deep_copy(const Json& v) {
    Json out;
    out.type = v.type;
    out.b = v.b;
    out.num = v.num;
    out.str = v.str;
    out.arr.reserve(v.arr.size());
    for (const auto& e : v.arr) out.arr.push_back(std::make_shared<Json>(json_deep_copy(*e)));
    out.obj.reserve(v.obj.size());
    for (const auto& kv : v.obj) out.obj.push_back({kv.first, std::make_shared<Json>(json_deep_copy(*kv.second))});
    return out;
}

}  // namespace core
}  // namespace dd

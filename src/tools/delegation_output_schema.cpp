// Pure helpers ported from tools/delegation_output_schema.py.
#include "dd/tools/delegation_output_schema.hpp"
#include "dd/core/json.hpp"
#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

namespace dd {
namespace tools {

using namespace core;
using Json::Obj;
using Json::Arr;
using Json::Str;
using Json::Bool;
using Json::Num;
using Json::Null;

const std::string CONTRACT_HEADER = "OUTPUT CONTRACT (machine-validated)";
const int MAX_SCHEMA_RETRIES = 1;

// ---- internal helpers ----
static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static std::string lstrip(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && is_ws(s[i])) ++i;
    return s.substr(i);
}
static std::string rstrip(const std::string& s) {
    size_t n = s.size();
    while (n > 0 && is_ws(s[n - 1])) --n;
    return s.substr(0, n);
}
static std::string strip(const std::string& s) { return rstrip(lstrip(s)); }
static std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
static bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
static bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

// json.dumps(schema, indent=2, ensure_ascii=False) — 2-space pretty printer.
static std::string pad(int level) { return std::string(2 * level, ' '); }
static std::string pretty_dump(const Json& v, int level) {
    switch (v.type) {
        case Null: return "null";
        case Bool: return v.b ? "true" : "false";
        case Num:
        case Str: return json_dump(v);
        case Arr: {
            if (v.arr.empty()) return "[]";
            std::string out = "[";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ",";
                out += "\n" + pad(level + 1) + pretty_dump(*v.arr[i], level + 1);
            }
            out += "\n" + pad(level) + "]";
            return out;
        }
        case Obj: {
            if (v.obj.empty()) return "{}";
            std::string out = "{";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out += ",";
                out += "\n" + pad(level + 1) + json_dump(json_str(v.obj[i].first)) + ": " +
                       pretty_dump(*v.obj[i].second, level + 1);
            }
            out += "\n" + pad(level) + "}";
            return out;
        }
    }
    return "null";
}

// ---- public ----
std::pair<std::optional<Json>, std::optional<std::string>> coerce_output_schema(const Json& raw) {
    if (raw.type == Null) return {std::nullopt, std::nullopt};
    Json r = raw;
    if (r.type == Str) {
        Json parsed;
        try {
            parsed = json_parse(r.str);
        } catch (...) {
            return {std::nullopt, std::string("output_schema must be a JSON Schema object, got a non-JSON string.")};
        }
        if (parsed.type != Obj) return {std::nullopt, std::string("output_schema must be a JSON Schema object.")};
        r = std::move(parsed);
    }
    if (r.type != Obj) {
        std::string tn;
        switch (r.type) {
            case Bool: tn = "bool"; break;
            case Num: tn = (r.num == static_cast<long long>(r.num)) ? "int" : "float"; break;
            case Arr: tn = "list"; break;
            default: tn = "object"; break;
        }
        return {std::nullopt, "output_schema must be a JSON Schema object, got " + tn + "."};
    }
    // jsonschema meta-validation intentionally skipped (jsonschema-unavailable path).
    return {r, std::nullopt};
}

std::string append_output_contract(const std::string& context, const Json& schema) {
    std::string schema_text = pretty_dump(schema, 0);
    std::string block = CONTRACT_HEADER + ":\n"
                              "Your FINAL response must be a single JSON object that validates "
                              "against this JSON Schema. No prose before or after the JSON; a "
                              "```json code fence is acceptable but not required.\n" +
                              schema_text;
    std::string base = rstrip(context);
    return base.empty() ? block : (base + "\n\n" + block);
}

std::string extract_json_candidate(const std::string& text) {
    std::string raw = strip(text);
    if (starts_with(raw, "```")) {
        auto nl = raw.find('\n');
        if (nl != std::string::npos) raw = raw.substr(nl + 1);
        if (ends_with(rstrip(raw), "```")) raw = rstrip(raw).substr(0, rstrip(raw).size() - 3);
        raw = strip(raw);
        if (starts_with(to_lower(raw), "json\n")) {
            auto n2 = raw.find('\n');
            raw = raw.substr(n2 + 1);
        }
    }
    const char* pairs[][2] = {{"{", "}"}, {"[", "]"}};
    for (auto& pr : pairs) {
        const std::string opener = pr[0], closer = pr[1];
        if (starts_with(raw, opener)) return raw;
        auto start = raw.find(opener);
        auto end = raw.rfind(closer);
        if (start != std::string::npos && end != std::string::npos && end > start)
            return raw.substr(start, end - start + 1);
    }
    return raw;
}

std::string build_retry_message(const std::vector<std::string>& errors) {
    std::string error_block;
    for (size_t i = 0; i < errors.size(); ++i) {
        if (i) error_block += "\n";
        error_block += "- " + errors[i];
    }
    return "Your previous final response was rejected by the output contract validator. Validation errors:\n" +
           error_block + "\n\n"
           "Reply with ONLY the corrected JSON object matching the OUTPUT CONTRACT schema from your task context. No prose, no explanations.";
}

}  // namespace tools
}  // namespace dd

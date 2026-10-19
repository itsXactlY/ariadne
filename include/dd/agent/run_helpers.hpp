#pragma once
// Pure heuristic helpers from run_agent.py: _is_destructive_command (terminal-
// command safety heuristic), _paths_overlap (path-subtree overlap over
// Path.parts), and the config-coercion pair hot_swap_hard_ratio_from /
// thinking_budget_from. NOT ported: _extract_parallel_scope_path (expanduser/
// abspath/cwd -> filesystem), _should_parallelize_tool_batch (calls _extract_parallel_scope_path,
// cwd-bound), _sanitize_messages_surrogates (UTF-16 surrogate range, not
// representable in a UTF-8 std::string), _qwen_portal_headers (platform.system/
// machine -> environment), _strip_budget_warnings_from_history (json round-trip
// + in-place mutation), IterationBudget (threading state). _paths_overlap models
// Path.parts as a vector<string>.
#include <algorithm>
#include <cctype>
#include <limits>
#include <regex>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace agent {

using namespace core;

inline bool _is_destructive_command(const std::string& cmd) {
    if (cmd.empty()) return false;
    static const std::regex destr(
        R"((?:^|\s|&&|\|\||;|`)(?:rm\s|rmdir\s|mv\s|sed\s+-i|truncate\s|dd\s|shred\s|git\s+(?:reset|clean|checkout)\s))");
    static const std::regex redir(R"([^>]>[^>]|^>[^>])");
    if (std::regex_search(cmd, destr)) return true;
    if (std::regex_search(cmd, redir)) return true;
    return false;
}

inline bool _paths_overlap(const std::vector<std::string>& left,
                           const std::vector<std::string>& right) {
    if (left.empty() || right.empty()) return false;
    const std::size_t common = std::min(left.size(), right.size());
    return std::equal(left.begin(), left.begin() + common, right.begin());
}

// ---- config coercion helpers (faithful to Python float()/int()) ----
static inline std::string trim_ws(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Python int(str): optional sign, decimal digits, underscores ONLY between
// digits. Full-token match required.
static inline bool py_int_from_str(const std::string& raw, long long& out) {
    const std::string t = trim_ws(raw);
    if (t.empty()) return false;
    std::size_t i = 0;
    int sign = 1;
    if (t[i] == '+' || t[i] == '-') {
        if (t[i] == '-') sign = -1;
        ++i;
    }
    if (i >= t.size()) return false;
    long long acc = 0;
    bool any = false, prev = false;
    for (; i < t.size(); ++i) {
        const char c = t[i];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            acc = acc * 10 + (c - '0');
            any = true;
            prev = true;
        } else if (c == '_') {
            if (!prev) return false;
            prev = false;
        } else {
            return false;
        }
    }
    if (!any || !prev) return false;  // no digits, or trailing underscore
    out = sign * acc;
    return true;
}

// Python float(str): whitespace-stripped, optional sign, then inf/nan (no
// underscores) or a decimal mantissa [exp] with underscores only between digits.
static inline bool py_float_from_str(const std::string& raw, double& out) {
    const std::string t = trim_ws(raw);
    if (t.empty()) return false;
    std::size_t i = 0;
    bool neg = false;
    if (t[i] == '+' || t[i] == '-') {
        neg = (t[i] == '-');
        ++i;
    }
    std::string rest = t.substr(i);
    std::string low;
    low.reserve(rest.size());
    for (char c : rest) low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (low == "inf" || low == "infinity") {
        out = neg ? -std::numeric_limits<double>::infinity()
                  : std::numeric_limits<double>::infinity();
        return true;
    }
    if (low == "nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    // Decimal mantissa. Scan digit runs allowing underscores between digits only.
    // Returns the count of digit chars written into `clean` and advances j; false
    // if the run is malformed (leading/double/trailing underscore, no digits).
    auto scan_digits = [&](std::size_t& j, std::string& clean) -> bool {
        bool any = false, prev = false;
        while (j < rest.size()) {
            const char c = rest[j];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                clean.push_back(c);
                any = true;
                prev = true;
                ++j;
            } else if (c == '_') {
                if (!prev) return false;
                prev = false;
                ++j;
            } else {
                break;
            }
        }
        if (!any || !prev) return false;
        return true;
    };
    std::size_t j = 0;
    std::string clean;
    bool int_ok = scan_digits(j, clean);  // consumes digits before '.', if any
    bool frac_ok = false;
    if (j < rest.size() && rest[j] == '.') {
        ++j;
        frac_ok = scan_digits(j, clean);
        if (!int_ok && !frac_ok) return false;  // "." alone
        // mantissa dot placed correctly: clean currently holds int digits then frac
        // digits appended; insert a dot between them only if int had digits.
    }
    if (!int_ok) {
        if (!frac_ok) return false;  // no int, no frac, no '.': not a number
    }
    // Handle exponent if present.
    if (j < rest.size() && (rest[j] == 'e' || rest[j] == 'E')) {
        ++j;
        std::string e;
        // exponent: optional sign then at least one digit
        if (j < rest.size() && (rest[j] == '+' || rest[j] == '-')) {
            e.push_back(rest[j]);
            ++j;
        }
        std::string mantissa_tail;
        if (!scan_digits(j, mantissa_tail)) return false;
        clean.push_back('e');
        clean += e;
        clean += mantissa_tail;
    }
    if (j != rest.size()) return false;  // trailing garbage (e.g. "1.2.3")
    // Reassemble with the mantissa dot. `clean` currently is <int-digits><frac-digits><exp>
    // without the dot. Redo: rebuild from rest is complex; instead reconstruct by
    // walking `rest` copying digits/dot/e/sign and dropping underscores.
    std::string num;
    if (neg) num.push_back('-');
    bool sawDot = false;
    for (char c : rest) {
        if (c == '_') continue;
        if (c == '.') {
            if (sawDot) return false;
            sawDot = true;
        }
        num.push_back(c);
    }
    try {
        out = std::stod(num);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

static inline bool py_to_int(const Json& v, long long& out) {
    switch (v.type) {
        case Json::Num:
            out = static_cast<long long>(v.num);  // trunc toward zero == int()
            return true;
        case Json::Bool:
            out = v.b ? 1 : 0;
            return true;
        case Json::Str:
            return py_int_from_str(v.str, out);
        default:
            return false;  // Null, Arr, Obj -> TypeError
    }
}

// cli.py: _coerce_output_history_limit — max(10, int(value)), default 200 on coercion failure.
inline long long _coerce_output_history_limit(const Json& value) {
    long long x;
    if (!py_to_int(value, x)) return 200LL;
    return x < 10 ? 10 : x;
}

static inline bool py_to_float(const Json& v, double& out) {
    switch (v.type) {
        case Json::Num:
            out = v.num;
            return true;
        case Json::Bool:
            out = v.b ? 1.0 : 0.0;
            return true;
        case Json::Str:
            return py_float_from_str(v.str, out);
        default:
            return false;  // Null, Arr, Obj -> TypeError
    }
}

static inline bool json_is_truthy(const Json& v) {
    switch (v.type) {
        case Json::Null: return false;
        case Json::Bool: return v.b;
        case Json::Num: return v.num != 0.0;
        case Json::Str: return !v.str.empty();
        case Json::Arr: return !v.arr.empty();
        case Json::Obj: return !v.obj.empty();
    }
    return false;
}

// run_agent.py: hot_swap_hard_ratio_from
inline double hot_swap_hard_ratio_from(const Json& comp_cfg) {
    const Json* v = json_get(comp_cfg, "hot_swap_hard_ratio");
    double x = 0.0;
    if (v && json_is_truthy(*v)) {
        if (!py_to_float(*v, x)) return 0.85;
    }
    return (0.5 < x && x <= 0.99) ? x : 0.85;
}

// run_agent.py: thinking_budget_from
inline long long thinking_budget_from(const Json& agent_cfg) {
    const Json* v = json_get(agent_cfg, "thinking_budget");
    if (!v) return 1024LL;  // dict.get default when key absent
    long long x;
    if (!py_to_int(*v, x)) return 1024LL;
    return x < 0 ? 0 : x;
}

}  // namespace agent
}  // namespace dd

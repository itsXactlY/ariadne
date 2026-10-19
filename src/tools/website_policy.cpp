// Pure host/URL policy helpers — tools/website_policy.py.
// Only _normalize_host, _normalize_rule, _match_host_against_rule,
// _extract_host_from_urlish are ported; YAML load + threading cache +
// check_website_access remain fs/threading coupled (NOT claimed).
#include "dd/tools/website_policy.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace dd {
namespace tools {

// Python str.strip() (ASCII whitespace).
static std::string strip_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// ASCII lowercase (hosts are ASCII; str.lower's unicode set is moot here).
static std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static void rstrip_dot(std::string& s) {
    while (!s.empty() && s.back() == '.') s.pop_back();
}

// Minimal urlparse: only netloc + path matter for these helpers.
// netloc is non-empty only when the URL carries "://" or a leading "//".
struct UrlBits {
    std::string netloc, path;
};
static UrlBits uparse(const std::string& url) {
    std::string rest;
    auto pos = url.find("://");
    if (pos != std::string::npos) {
        rest = url.substr(pos + 3);
    } else if (url.rfind("//", 0) == 0) {
        rest = url.substr(2);
    } else {
        return {};  // no netloc; path unused unless "://" present
    }
    size_t stop = rest.size();
    for (size_t i = 0; i < rest.size(); ++i) {
        char c = rest[i];
        if (c == '/' || c == '?' || c == '#') { stop = i; break; }
    }
    return {rest.substr(0, stop), rest.substr(stop)};
}

static std::string hostname_of(const std::string& netloc) {
    std::string h = netloc;
    auto at = h.find('@');
    if (at != std::string::npos) h = h.substr(at + 1);
    auto c = h.find(':');
    h = (c == std::string::npos) ? h : h.substr(0, c);
    return to_lower(h);
}

// fnmatch.fnmatch for non-Windows (case-sensitive): * ? [sets].
static bool glob_at(const std::string& p, size_t pi, const std::string& s, size_t si) {
    if (pi == p.size()) return si == s.size();
    char pc = p[pi];
    if (pc == '*') {
        for (size_t k = si;; ++k) {
            if (glob_at(p, pi + 1, s, k)) return true;
            if (k == s.size()) break;
        }
        return false;
    }
    if (pc == '?') {
        if (si >= s.size()) return false;
        return glob_at(p, pi + 1, s, si + 1);
    }
    if (pc == '[') {
        size_t j = pi + 1;
        bool neg = false;
        if (j < p.size() && p[j] == '!') { neg = true; ++j; }
        bool matched = false;
        if (j < p.size() && p[j] == ']') {
            if (si < s.size() && s[si] == ']') matched = true;
            ++j;
        }
        while (j < p.size() && p[j] != ']') {
            if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']') {
                char lo = p[j], hi = p[j + 2];
                if (si < s.size() && s[si] >= lo && s[si] <= hi) matched = true;
                j += 3;
            } else {
                if (si < s.size() && s[si] == p[j]) matched = true;
                ++j;
            }
        }
        // (j points at the closing ']')
        bool char_ok = neg ? !matched : matched;
        if (!char_ok) return false;
        if (si >= s.size()) return false;  // the [ consumed one char
        return glob_at(p, j + 1, s, si + 1);
    }
    // literal
    if (si >= s.size() || s[si] != pc) return false;
    return glob_at(p, pi + 1, s, si + 1);
}
static bool glob_match(const std::string& p, const std::string& s) {
    return glob_at(p, 0, s, 0);
}

// ---- public helpers ----

std::string normalize_host(const std::string& host) {
    std::string s = strip_ws(host);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    rstrip_dot(s);
    return s;
}

std::optional<std::string> normalize_rule(const core::Json& rule) {
    if (rule.type != core::Json::Str) return std::nullopt;
    std::string s = to_lower(strip_ws(rule.str));
    if (s.empty() || s[0] == '#') return std::nullopt;
    if (s.find("://") != std::string::npos) {
        UrlBits u = uparse(s);
        s = u.netloc.empty() ? u.path : u.netloc;
    }
    auto sl = s.find('/');
    if (sl != std::string::npos) s = s.substr(0, sl);
    s = strip_ws(s);
    rstrip_dot(s);
    if (s.rfind("www.", 0) == 0) s = s.substr(4);
    if (s.empty()) return std::nullopt;
    return s;
}

bool match_host_against_rule(const std::string& host, const std::string& pattern) {
    if (host.empty() || pattern.empty()) return false;
    if (pattern.rfind("*.", 0) == 0) return glob_match(pattern, host);
    if (host == pattern) return true;
    return host.size() > pattern.size() + 1 &&
           host.compare(host.size() - pattern.size() - 1, pattern.size() + 1,
                        "." + pattern) == 0;
}

std::string extract_host_from_urlish(const std::string& url) {
    UrlBits u = uparse(url);
    std::string hn = hostname_of(u.netloc);
    std::string base = hn.empty() ? u.netloc : hn;
    std::string host = normalize_host(base);
    if (!host.empty()) return host;

    if (url.find("://") == std::string::npos) {
        UrlBits s2 = uparse("//" + url);
        std::string hn2 = hostname_of(s2.netloc);
        std::string base2 = hn2.empty() ? s2.netloc : hn2;
        host = normalize_host(base2);
        if (!host.empty()) return host;
    }
    return {};
}

}  // namespace tools
}  // namespace dd

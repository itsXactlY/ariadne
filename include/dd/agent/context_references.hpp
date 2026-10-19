#pragma once
// Pure subset of agent/context_references.py: the reference-token scanner and
// the string helpers around it. Everything that touches the filesystem,
// subprocess (rg/git), network (url fetch), or credential/sensitivity checks
// (_resolve_path, _ensure_reference_path_allowed, _expand_*, _build_folder_*,
// _is_binary_file, _file_metadata) is NOT ported. Offsets (start/end) are BYTE
// indices, matching Python's codepoint indices for the ASCII corpora under test.
#include <cctype>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace dd {
namespace agent {
namespace detail {

inline bool isword(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
inline bool isspace_b(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
// ^\d+(-\d+)?$ on `rest`; returns {start_digits, end_digits?} or nullopt.
inline std::optional<std::pair<long long, long long>> line_range_tail(const std::string& rest) {
    size_t n = rest.size(), i = 0;
    if (n == 0 || !std::isdigit(static_cast<unsigned char>(rest[0]))) return std::nullopt;
    size_t s0 = i;
    while (i < n && std::isdigit(static_cast<unsigned char>(rest[i]))) ++i;
    long long start = std::stoll(rest.substr(s0, i - s0));
    long long end = start;
    if (i < n) {
        if (rest[i] != '-') return std::nullopt;
        ++i;
        if (i >= n || !std::isdigit(static_cast<unsigned char>(rest[i]))) return std::nullopt;
        size_t s1 = i;
        while (i < n && std::isdigit(static_cast<unsigned char>(rest[i]))) ++i;
        end = std::stoll(rest.substr(s1, i - s1));
    }
    if (i != n) return std::nullopt;  // anchored to end
    return std::make_pair(start, end);
}

}  // namespace detail

struct ContextReference {
    std::string raw;
    std::string kind;
    std::string target;
    long long start = -1;
    long long end = -1;
    std::optional<long long> line_start;
    std::optional<long long> line_end;
};

// _strip_trailing_punctuation
inline std::string strip_trailing_punctuation(const std::string& value) {
    static const std::string punct = ",.;!?";
    std::string s = value;
    while (!s.empty() && punct.find(s.back()) != std::string::npos) s.pop_back();
    while (!s.empty()) {
        char c = s.back();
        char opener;
        if (c == ')') opener = '(';
        else if (c == ']') opener = '[';
        else if (c == '}') opener = '{';
        else break;
        long cnt_c = 0, cnt_o = 0;
        for (char ch : s) {
            if (ch == c) ++cnt_c;
            if (ch == opener) ++cnt_o;
        }
        if (cnt_c > cnt_o) { s.pop_back(); continue; }
        break;
    }
    return s;
}

// parse_context_references (REFERENCE_PATTERN scanned by hand: no lookbehind in
// std::regex, so the (?<![\w/]) guard is enforced manually).
inline std::vector<ContextReference> parse_context_references(const std::string& msg) {
    using detail::isword;
    using detail::isspace_b;
    std::vector<ContextReference> refs;
    if (msg.empty()) return refs;
    const size_t n = msg.size();
    static const std::string simples[] = {"diff", "staged"};
    static const std::string kinds[] = {"file", "folder", "git", "url"};
    size_t i = 0;
    while (i < n) {
        size_t at = i;
        while (at < n && msg[at] != '@') ++at;
        if (at >= n) break;
        if (at > 0) {
            char prev = msg[at - 1];
            if (isword(prev) || prev == '/') { i = at + 1; continue; }
        }
        const size_t j = at + 1;
        bool matched = false;
        std::string kind, value;
        size_t endpos = 0;
        for (const std::string& kw : simples) {
            if (msg.compare(j, kw.size(), kw) == 0) {
                const size_t after = j + kw.size();
                if (after >= n || !isword(msg[after])) {
                    matched = true; kind = kw; endpos = after; break;
                }
            }
        }
        if (!matched) {
            for (const std::string& kd : kinds) {
                if (msg.compare(j, kd.size(), kd) == 0) {
                    const size_t colon = j + kd.size();
                    const size_t vs = colon + 1;
                    if (colon < n && msg[colon] == ':' && vs < n && !isspace_b(msg[vs])) {
                        size_t ve = vs;
                        while (ve < n && !isspace_b(msg[ve])) ++ve;
                        matched = true; kind = kd; value = msg.substr(vs, ve - vs); endpos = ve;
                        break;
                    }
                }
            }
        }
        if (!matched) { i = at + 1; continue; }
        ContextReference r;
        r.raw = msg.substr(at, endpos - at);
        r.kind = kind;
        r.start = static_cast<long long>(at);
        r.end = static_cast<long long>(endpos);
        if (kind == "diff" || kind == "staged") {
            // simple branch: no target/line numbers
        } else {
            const std::string stripped = strip_trailing_punctuation(value);
            std::string target = stripped;
            if (kind == "file") {
                for (size_t p = 1; p < stripped.size(); ++p) {
                    if (stripped[p] != ':') continue;
                    const std::string rest = stripped.substr(p + 1);
                    if (auto lr = detail::line_range_tail(rest)) {
                        target = stripped.substr(0, p);
                        r.line_start = lr->first;
                        r.line_end = lr->second;
                        break;
                    }
                }
            }
            r.target = target;
        }
        refs.push_back(std::move(r));
        i = endpos;  // finditer resumes at the match end (non-overlapping)
    }
    return refs;
}

// _remove_reference_tokens
inline std::string remove_reference_tokens(
    const std::string& msg, const std::vector<ContextReference>& refs) {
    using detail::isspace_b;
    std::string text;
    size_t cursor = 0;
    for (const auto& ref : refs) {
        text += msg.substr(cursor, static_cast<size_t>(ref.start) - cursor);
        cursor = static_cast<size_t>(ref.end);
    }
    text += msg.substr(cursor);
    static const std::regex ws2(R"(\s{2,})");
    text = std::regex_replace(text, ws2, " ");
    static const std::regex spaced(R"(\s+([,.;:!?]))");
    text = std::regex_replace(text, spaced, "$1");
    // .strip()
    size_t a = 0, b = text.size();
    while (a < b && isspace_b(text[a])) ++a;
    while (b > a && isspace_b(text[b - 1])) --b;
    return text.substr(a, b - a);
}

// _code_fence_language (path.suffix.lower() -> language; "" when unmapped).
inline std::string code_fence_language(const std::string& suffix) {
    std::string s = suffix;
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<std::string, std::string> map[] = {
        {".py", "python"}, {".js", "javascript"}, {".ts", "typescript"},
        {".tsx", "tsx"}, {".jsx", "jsx"}, {".json", "json"}, {".md", "markdown"},
        {".sh", "bash"}, {".yml", "yaml"}, {".yaml", "yaml"}, {".toml", "toml"},
    };
    for (const auto& kv : map)
        if (kv.first == s) return kv.second;
    return "";
}

}  // namespace agent
}  // namespace dd

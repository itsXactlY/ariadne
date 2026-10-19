#pragma once
// Pure CLI-text heuristics from cli.py: _looks_like_slash_command, _split_path_input,
// _path_is_within_root (Path.relative_to == root-parts prefix; modeled on already-tokenized
// Path.parts vectors, fs resolution stays at the caller), and _collapse_blank_line_floods
// (blank-line run collapse with fenced-code-block awareness). NOT ported (fs/TTY/env/globals
// coupled): _termux_example_image_path (os.path.isdir), _detect_file_drop (Path.exists/is_file),
// _b/_d (sys.stdout.isatty), _resolve_attachment_path (fs), _accent_hex (skin_engine registry),
// _rich_text_from_ansi (rich Text), _assistant_copy_text/_strip_reasoning_tags (Python regex
// lookbehind (?<=)/(?!) std::regex cannot compile), _sync_process_session_id (module import
// + global setter), and the output-history record/config/replay trio (mutate module globals +
// deque). _path_is_within_root models Path.parts as a vector<string>; _luminance_from_hex ported;
// NOT ported: _hex_to_ansi / _maybe_remap_for_light_mode / _detect_light_mode / _query_osc11_background
// (all consult the terminal via OSC escapes or isatty — env/TTY coupled).
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>
#include <optional>

namespace dd {
namespace cli {

inline std::string trim_ws(const std::string& s) {
    std::size_t a = 0, b = s.size();
    const auto sp = [](char c) { return std::isspace(static_cast<unsigned char>(c)); };
    while (a < b && sp(s[a])) ++a;
    while (b > a && sp(s[b - 1])) --b;
    return s.substr(a, b - a);
}

inline bool _looks_like_slash_command(const std::string& text) {
    if (text.empty() || text[0] != '/') return false;
    const std::string::size_type end = text.find_first_of(" \t\n\r\f\v");
    const std::string first_word =
        end == std::string::npos ? text : text.substr(0, end);
    return first_word.find('/', 1) == std::string::npos;
}

inline std::pair<std::string, std::string> split_path_input(const std::string& raw_in) {
    const std::string raw = trim_ws(raw_in);
    if (raw.empty()) return {"", ""};
    const char q0 = raw[0];
    if (q0 == '"' || q0 == '\'') {
        const char quote = q0;
        std::size_t pos = 1;
        const std::size_t n = raw.size();
        while (pos < n) {
            const char ch = raw[pos];
            if (ch == '\\' && pos + 1 < n) {
                pos += 2;
                continue;
            }
            if (ch == quote) return {raw.substr(1, pos - 1), trim_ws(raw.substr(pos + 1))};
            pos += 1;
        }
        return {raw.substr(1), ""};
    }
    std::size_t pos = 0;
    const std::size_t n = raw.size();
    while (pos < n) {
        const char ch = raw[pos];
        if (ch == '\\' && pos + 1 < n && raw[pos + 1] == ' ')
            pos += 2;
        else if (ch == ' ')
            break;
        else
            pos += 1;
    }
    const std::string token = raw.substr(0, pos);
    std::string tok2;
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '\\' && i + 1 < token.size() && token[i + 1] == ' ') {
            tok2 += ' ';
            ++i;
        } else {
            tok2 += token[i];
        }
    }
    return {tok2, trim_ws(raw.substr(pos))};
}

inline bool path_is_within_root(const std::vector<std::string>& path_parts,
                                const std::vector<std::string>& root_parts) {
    if (root_parts.size() > path_parts.size()) return false;
    return std::equal(root_parts.begin(), root_parts.end(), path_parts.begin());
}

inline std::string _collapse_blank_line_floods(const std::string& text) {
    if (text.empty()) return text;
    // Python text.split("\n"): n+1 pieces, empty segments preserved.
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    const auto sp = [](char c) { return std::isspace(static_cast<unsigned char>(c)); };
    const auto starts_fence = [&](const std::string& s) {
        std::size_t a = 0;
        while (a < s.size() && sp(s[a])) ++a;
        const std::string f = s.substr(a);
        return f.rfind("```", 0) == 0 || f.rfind("~~~", 0) == 0;
    };
    std::vector<std::string> out;
    bool in_fence = false;
    int blanks = 0;
    for (const std::string& line : lines) {
        if (starts_fence(line)) {
            in_fence = !in_fence;
            blanks = 0;
            out.push_back(line);
            continue;
        }
        if (in_fence) {
            out.push_back(line);
            continue;
        }
        if (trim_ws(line).empty()) {
            ++blanks;
            if (blanks <= 1) out.push_back("");
            continue;
        }
        blanks = 0;
        out.push_back(line);
    }
    while (!out.empty() && trim_ws(out.back()).empty()) out.pop_back();
    std::string joined;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i) joined += '\n';
        joined += out[i];
    }
    return joined;
}


// cli.py: _luminance_from_hex — hex (3- or 6-digit, optional #) to relative luminance,
// None on malformed. .strip().lstrip("#"), 3-digit expand, hexdigit guard, weighted sum.
inline std::optional<double> _luminance_from_hex(const std::string& hex_str) {
    std::string s = trim_ws(hex_str);
    std::size_t a = 0;
    while (a < s.size() && s[a] == '#') ++a;
    s = s.substr(a);
    if (s.size() == 3) {
        std::string e;
        for (char c : s) { e += c; e += c; }
        s = e;
    }
    if (s.size() != 6) return std::nullopt;
    const auto hx = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = hx(s[i]);
        if (v[i] < 0) return std::nullopt;
    }
    const int r = v[0] * 16 + v[1];
    const int g = v[2] * 16 + v[3];
    const int b = v[4] * 16 + v[5];
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
}
}  // namespace cli
}  // namespace dd

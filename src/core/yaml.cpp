#include "dd/core/yaml.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace dd {
namespace core {

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string unquote(const std::string& v) {
    auto s = trim(v);
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

std::vector<std::string> parse_list(const std::string& value) {
    auto s = trim(value);
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']')
        s = s.substr(1, s.size() - 2);
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        auto t = trim(tok);
        if (!t.empty()) out.push_back(unquote(t));
    }
    return out;
}

// Locate the leading `---` ... `---` block.
static size_t frontmatter_end(const std::string& content, size_t& body_start) {
    // Skip a leading BOM.
    size_t i = 0;
    if (content.size() >= 3 && (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
        i = 3;
    // First non-empty line must be `---`.
    while (i < content.size() && (content[i] == ' ' || content[i] == '\t')) ++i;
    if (content.compare(i, 3, "---") != 0) return std::string::npos;
    // Advance past that line.
    auto nl = content.find('\n', i);
    if (nl == std::string::npos) return std::string::npos;
    size_t block_start = nl + 1;
    // Find closing `---` at start of a line.
    size_t j = block_start;
    while (j < content.size()) {
        size_t line_end = content.find('\n', j);
        if (line_end == std::string::npos) line_end = content.size();
        std::string line = content.substr(j, line_end - j);
        auto t = trim(line);
        if (t == "---") {
            body_start = (line_end < content.size()) ? line_end + 1 : line_end;
            return j;  // end of frontmatter block (position of closing ---)
        }
        j = line_end + 1;
    }
    return std::string::npos;
}

static int indent_of(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 2;
        else break;
    }
    return n;
}

// `|` or `>` after a key, with an optional chomping/indent mark and a comment: "|", ">-", "|+2 # why".
// chomp: '-' strips the trailing newlines, '+' keeps all of them, nothing (clip) keeps exactly one.
static bool block_scalar_head(const std::string& v, bool& folded, char& chomp, int& expl) {
    if (v.empty() || (v[0] != '|' && v[0] != '>')) return false;
    folded = v[0] == '>';
    chomp = 0;
    expl = 0;
    size_t i = 1;
    for (; i < v.size() && (v[i] == '+' || v[i] == '-' || std::isdigit(static_cast<unsigned char>(v[i]))); ++i) {
        if (v[i] == '+' || v[i] == '-') chomp = v[i];
        else expl = expl * 10 + (v[i] - '0');
    }
    while (i < v.size() && v[i] == ' ') ++i;
    if (i < v.size() && v[i] != '#') return false;
    return true;
}

// The lines of a block scalar: everything more indented than its key (blank lines belong). Literal keeps the lines,
// folded joins them with a space (a blank line is a newline); the head line's chomping mark decides the tail.
static std::string block_scalar(const std::vector<std::string>& lines, size_t from, int key_indent, bool folded, char chomp, int expl, size_t& next) {
    std::vector<std::string> body;
    size_t m = from;
    for (; m < lines.size(); ++m) {
        if (trim(lines[m]).empty()) {
            body.push_back("");
            continue;
        }
        if (indent_of(lines[m]) <= key_indent) break;
        body.push_back(lines[m]);
    }
    next = m;
    int tail = 0;   // blank lines behind the content: chomping decides how many survive
    while (!body.empty() && body.back().empty()) { body.pop_back(); ++tail; }
    // The block's indent is its first content line's; an explicit digit on the head line shallows it.
    int common = 1 << 20;
    for (const auto& l : body)
        if (!l.empty()) { common = indent_of(l); break; }
    if (expl > 0) common = std::min(common, key_indent + expl);
    std::string out;
    bool joined = false;   // folded: the previous line was text the next one continues
    bool first = true;
    for (const auto& l : body) {
        const std::string stripped = l.empty() ? std::string() : l.substr(std::min(l.size(), static_cast<size_t>(common)));
        if (!folded) {
            if (!first) out += '\n';
            out += stripped;
            first = false;
            continue;
        }
        const std::string text = trim(stripped);
        if (text.empty()) {
            out += '\n';
            joined = false;
        } else {
            if (joined) out += ' ';
            out += text;
            joined = true;
        }
    }
    if (!out.empty()) {   // chomping only applies when there is content; an empty block stays empty
        if (chomp == '+') out += std::string(tail + 1, '\n');
        else if (chomp != '-') out += '\n';
    }
    return out;
}

// Top-level (indent 0) keys win; a nested key only fills a name that is absent (metadata.hermes.tags -> tags).
// Block scalars (`key: >`, `|`, with - + marks) and block sequences (`key:` then `- a` lines, read back as `[a, b]`).
std::map<std::string, std::string> parse_frontmatter(const std::string& content) {
    std::map<std::string, std::string> top, nested;
    size_t body_start = 0;
    size_t end = frontmatter_end(content, body_start);
    if (end == std::string::npos) return top;

    size_t i = 0;
    if (content.size() >= 3 && (unsigned char)content[0] == 0xEF) i = 3;
    auto nl = content.find('\n', i);
    size_t j = (nl == std::string::npos) ? content.size() : nl + 1;
    std::vector<std::string> lines;
    while (j < end) {
        size_t line_end = content.find('\n', j);
        if (line_end == std::string::npos) line_end = content.size();
        std::string line = content.substr(j, line_end - j);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        j = line_end + 1;
    }

    for (size_t k = 0; k < lines.size(); ++k) {
        const std::string& line = lines[k];
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == '-') continue;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(line.substr(0, colon));
        const std::string val = trim(line.substr(colon + 1));
        if (key.empty()) continue;
        auto& dst = indent_of(line) == 0 ? top : nested;
        bool folded = false;
        char chomp = 0;
        int expl = 0;
        if (block_scalar_head(val, folded, chomp, expl)) {
            size_t next = k + 1;
            dst[key] = block_scalar(lines, k + 1, indent_of(line), folded, chomp, expl, next);
            k = next - 1;
            continue;
        }
        if (val.empty()) {
            std::string items;
            size_t m = k + 1;
            for (; m < lines.size(); ++m) {
                const std::string tt = trim(lines[m]);
                if (tt.empty()) continue;
                if (tt[0] != '-' || (tt.size() > 1 && tt[1] != ' ')) break;
                items += (items.empty() ? "" : ", ") + unquote(trim(tt.substr(1)));
            }
            if (!items.empty()) {
                dst[key] = "[" + items + "]";
                k = m - 1;
                continue;
            }
        }
        dst[key] = val;
    }
    for (const auto& kv : nested) top.emplace(kv.first, kv.second);
    return top;
}

std::string body(const std::string& content) {
    size_t body_start = 0;
    size_t end = frontmatter_end(content, body_start);
    if (end == std::string::npos) return content;
    return content.substr(body_start);
}

}  // namespace core
}  // namespace dd

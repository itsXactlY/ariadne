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

std::map<std::string, std::string> parse_frontmatter(const std::string& content) {
    std::map<std::string, std::string> out;
    size_t body_start = 0;
    size_t end = frontmatter_end(content, body_start);
    if (end == std::string::npos) return out;

    // Re-scan the block between the fences to collect key: value pairs.
    size_t i = 0;
    if (content.size() >= 3 && (unsigned char)content[0] == 0xEF) i = 3;
    auto nl = content.find('\n', i);
    size_t j = (nl == std::string::npos) ? content.size() : nl + 1;
    while (j < end) {
        size_t line_end = content.find('\n', j);
        if (line_end == std::string::npos) line_end = content.size();
        std::string line = content.substr(j, line_end - j);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto key = trim(line.substr(0, colon));
            auto val = trim(line.substr(colon + 1));
            if (!key.empty()) out[key] = val;
        }
        j = line_end + 1;
    }
    return out;
}

std::string body(const std::string& content) {
    size_t body_start = 0;
    size_t end = frontmatter_end(content, body_start);
    if (end == std::string::npos) return content;
    return content.substr(body_start);
}

}  // namespace core
}  // namespace dd

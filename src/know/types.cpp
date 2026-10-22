// src/know/types.cpp — impl of dd/know/types.hpp.
#include "dd/know/types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace dd {
namespace know {

namespace {

struct Scheme {
    Kind kind;
    const char* name;
};
constexpr Scheme kSchemes[] = {{Kind::Skill, "skill"}, {Kind::Memory, "mem"},   {Kind::Tool, "tool"},
                               {Kind::Session, "sess"}, {Kind::Rule, "rule"}};

bool id_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-' || c == '@' || c == ':';
}

}  // namespace

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Skill: return "skill";
        case Kind::Memory: return "memory";
        case Kind::Tool: return "tool";
        case Kind::Session: return "session";
        case Kind::Rule: return "rule";
    }
    return "skill";
}

std::optional<Kind> kind_from(const std::string& s) {
    for (const auto& sc : kSchemes)
        if (s == sc.name) return sc.kind;
    for (Kind k : {Kind::Skill, Kind::Memory, Kind::Tool, Kind::Session, Kind::Rule})
        if (s == kind_name(k)) return k;
    return std::nullopt;
}

std::string Uri::str() const {
    const char* scheme = "skill";
    for (const auto& sc : kSchemes)
        if (sc.kind == kind) scheme = sc.name;
    std::string out = std::string(scheme) + "://" + id;
    if (!path.empty()) out += "/" + path;
    if (!page.empty()) out += "#" + page;
    return out;
}

std::optional<Uri> Uri::parse(const std::string& text) {
    const size_t b = text.find_first_not_of(" \t\r\n"), e = text.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return std::nullopt;
    const std::string t = text.substr(b, e - b + 1);
    const size_t sep = t.find("://");
    if (sep == std::string::npos) return std::nullopt;
    Uri u;
    bool known = false;
    for (const auto& sc : kSchemes)
        if (t.compare(0, sep, sc.name) == 0 && std::string(sc.name).size() == sep) {
            u.kind = sc.kind;
            known = true;
        }
    if (!known) return std::nullopt;
    std::string rest = t.substr(sep + 3);
    if (const size_t h = rest.rfind('#'); h != std::string::npos) {
        u.page = rest.substr(h + 1);
        rest.resize(h);
        if (u.page != "all" && (u.page.empty() || !std::all_of(u.page.begin(), u.page.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })))
            return std::nullopt;
    }
    const size_t slash = rest.find('/');
    u.id = rest.substr(0, slash);
    if (slash != std::string::npos) u.path = rest.substr(slash + 1);
    if (u.id.empty() || !std::all_of(u.id.begin(), u.id.end(), id_char)) return std::nullopt;
    if (u.kind == Kind::Memory && !std::all_of(u.id.begin(), u.id.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        return std::nullopt;
    if (!u.path.empty()) {   // inside the item, never out of it
        if (u.path.front() == '/' || u.path.find("..") != std::string::npos) return std::nullopt;
        while (!u.path.empty() && u.path.back() == '/') u.path.pop_back();
    }
    return u;
}

std::optional<Body> paginate(const Uri& uri, const std::string& text, size_t page_bytes, size_t all_cap) {
    std::vector<std::pair<size_t, size_t>> pages;   // [begin, end)
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.size();
        if (page_bytes > 0 && text.size() - pos > page_bytes) {
            end = pos + page_bytes;
            const size_t lo = pos + page_bytes / 2;   // a cut in the back half of the page: a blank line, else a line end
            if (const size_t p = text.rfind("\n\n", end); p != std::string::npos && p >= lo) end = p + 2;
            else if (const size_t n = text.rfind('\n', end); n != std::string::npos && n >= lo) end = n + 1;
            while (end > pos && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
        }
        pages.emplace_back(pos, end);
        pos = end;
    }
    if (pages.empty()) pages.emplace_back(0, 0);
    const int n = static_cast<int>(pages.size());

    Body b;
    b.uri = uri;
    b.uri.page.clear();
    b.total = text.size();
    b.pages = n;
    auto next = [&](int p) {
        Uri m = uri;
        m.page = std::to_string(p);
        return m.str();
    };
    if (uri.page == "all") {
        int last = 1;
        size_t bytes = pages[0].second - pages[0].first;
        while (last < n && bytes + (pages[static_cast<size_t>(last)].second - pages[static_cast<size_t>(last)].first) <= all_cap) {
            bytes += pages[static_cast<size_t>(last)].second - pages[static_cast<size_t>(last)].first;
            ++last;
        }
        b.text = text.substr(0, pages[static_cast<size_t>(last) - 1].second);
        b.page = 1;
        if (last < n) b.more = next(last + 1);
        return b;
    }
    const int p = uri.page.empty() ? 1 : std::atoi(uri.page.c_str());
    if (p < 1 || p > n) return std::nullopt;
    b.page = p;
    b.text = text.substr(pages[static_cast<size_t>(p) - 1].first, pages[static_cast<size_t>(p) - 1].second - pages[static_cast<size_t>(p) - 1].first);
    if (p < n) b.more = next(p + 1);
    return b;
}

}  // namespace know
}  // namespace dd

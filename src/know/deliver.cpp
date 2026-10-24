// src/know/deliver.cpp — impl of dd/know/deliver.hpp.
#include "dd/know/deliver.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "dd/know/text.hpp"

namespace dd {
namespace know {

namespace {

std::string cut(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return utf8_head(s, n) + "…";
}

// An attribute value built from text that may come from a tool's output: cut to max bytes (never inside a UTF-8
// sequence), no quote, angle bracket, ampersand or line break gets through.
std::string attr(const std::string& s, size_t max = 120) {
    std::string o;
    for (const unsigned char c : utf8_head(s, max)) {
        switch (c) {
            case '"': o += "&quot;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '&': o += "&amp;"; break;
            default: o.push_back(c < 0x20 || c == 0x7F ? ' ' : static_cast<char>(c));
        }
    }
    return o;
}

// Card text on a pointer line: no angle bracket, so a title or a gist cannot close the note around it.
std::string line_text(const std::string& s) {
    std::string o;
    for (const char c : s) {
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o.push_back(c);
    }
    return o;
}

bool tag_name_at(const std::string& s, size_t i) {   // s[i] is '<': the harness's own tags, closing or opening
    static const char* const names[] = {"skill", "retrieval", "system-notice", "memory-context"};
    size_t k = i + 1;
    if (k < s.size() && s[k] == '/') ++k;
    for (const char* n : names) {
        const size_t len = std::char_traits<char>::length(n);
        if (s.size() - k < len) continue;
        bool same = true;
        for (size_t j = 0; j < len && same; ++j) same = std::tolower(static_cast<unsigned char>(s[k + j])) == n[j];
        if (!same) continue;
        const char next = k + len < s.size() ? s[k + len] : '>';
        if (next == '>' || next == ' ' || next == '\t' || next == '\n' || next == '/') return true;
    }
    return false;
}

// A skill's own text goes into <skill>: its code may keep every angle bracket, but not one that spells a tag of ours.
std::string neutralize_tags(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<' && tag_name_at(s, i)) o += "&lt;";
        else o.push_back(s[i]);
    }
    return o;
}

std::string pad(std::string s, size_t n) {
    if (s.size() < n) s.append(n - s.size(), ' ');
    return s;
}

}  // namespace

size_t Deliverer::tokens(const std::string& text) const {
    const double ratio = ratio_ ? std::max(1.0, ratio_()) : 1.0;
    return static_cast<size_t>(std::ceil(static_cast<double>(text.size()) / 4.0 * ratio));
}

std::string Deliverer::read_hint(const Uri& u) {
    Uri bare = u;
    bare.page.clear();
    if (u.kind == Kind::Tool) return "tool_describe(\"" + u.id + "\")";
    return "read_file(\"" + bare.str() + "\")";
}

std::string Deliverer::pointer(const Hit& h) {
    char score[16];
    std::snprintf(score, sizeof score, "%.2f", h.score);
    std::string line = pad(h.card.uri.kind == Kind::Memory ? "memory" : kind_name(h.card.uri.kind), 6) + " ";
    if (h.card.uri.kind == Kind::Memory) line += "#" + h.card.uri.id + " [" + line_text(h.card.title) + "]";
    else line += line_text(h.card.title);
    line += std::string(" ") + score;
    if (!h.card.gist.empty()) line += " — " + line_text(cut(h.card.gist, 110));
    if (!h.card.has_body && h.card.uri.kind == Kind::Skill) return line + "   (no body to open)";
    return line + "   → " + read_hint(h.card.uri);
}

std::string Deliverer::note(const Shown& s, const std::string& why, size_t budget, size_t total_budget, std::vector<Uri>* offered,
                            bool* head_in) const {
    std::string body;
    bool full = false;
    if (head_in) *head_in = false;
    auto add = [&](const Hit& h) {
        if (full) return;
        const std::string line = pointer(h) + "\n";
        if (!body.empty() && tokens(body) + tokens(line) > budget) {
            full = true;
            return;
        }
        body += line;
        if (offered) offered->push_back(h.card.uri);
    };
    for (const auto& h : s.skills) add(h);
    for (const auto& h : s.memory) add(h);
    for (const auto& h : s.tools) add(h);

    if (s.head && total_budget > tokens(body)) {
        const Body& b = *s.head;
        const std::string page = std::to_string(b.page) + "/" + std::to_string(b.pages);
        const std::string text = neutralize_tags(b.text);
        std::string opened = "opened for you — " + line_text(s.head_title) + ", page " + page + ":\n<skill name=\"" + attr(s.head_title, 80) +
                             "\" page=\"" + page + "\">\n" + text + (text.empty() || text.back() != '\n' ? "\n" : "") + "</skill>\n";
        if (!b.more.empty()) opened += "more: read_file(\"" + b.more + "\")\n";
        if (tokens(body) + tokens(opened) <= total_budget) {
            body += opened;
            if (head_in) *head_in = true;
        }
    }
    if (body.empty()) return "";
    return "<retrieval why=\"" + attr(why) + "\">\n" + body + "</retrieval>";
}

std::string Deliverer::notice(const std::string& reason, const std::string& text) {
    return "<system-notice reason=\"" + attr(reason) + "\">\n" + neutralize_tags(text) + "\n</system-notice>";
}

std::string Deliverer::reattach(const std::vector<WorkingSet::Entry>& in_use) {
    if (in_use.empty()) return "";
    std::string body;
    for (const auto& e : in_use) {
        Hit h;
        h.card.uri = e.uri;
        h.card.title = e.title;
        body += pad(kind_name(e.uri.kind), 6) + " " + line_text(e.title) + " — you were working from it; its text left the window   → " + read_hint(e.uri) + "\n";
    }
    return "<retrieval why=\"rebase\">\n" + body + "</retrieval>";
}

}  // namespace know
}  // namespace dd

// src/know/deliver.cpp — impl of dd/know/deliver.hpp.
#include "dd/know/deliver.hpp"

#include <algorithm>
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
    if (h.card.uri.kind == Kind::Memory) line += "#" + h.card.uri.id + " [" + h.card.title + "]";
    else line += h.card.title;
    line += std::string(" ") + score;
    if (!h.card.gist.empty()) line += " — " + cut(h.card.gist, 110);
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
        std::string opened = "opened for you — " + s.head_title + ", page " + page + ":\n<skill name=\"" + s.head_title + "\" page=\"" + page +
                             "\">\n" + b.text + (b.text.empty() || b.text.back() != '\n' ? "\n" : "") + "</skill>\n";
        if (!b.more.empty()) opened += "more: read_file(\"" + b.more + "\")\n";
        if (tokens(body) + tokens(opened) <= total_budget) {
            body += opened;
            if (head_in) *head_in = true;
        }
    }
    if (body.empty()) return "";
    return "<retrieval why=\"" + why + "\">\n" + body + "</retrieval>";
}

std::string Deliverer::notice(const std::string& reason, const std::string& text) {
    return "<system-notice reason=\"" + reason + "\">\n" + text + "\n</system-notice>";
}

std::string Deliverer::reattach(const std::vector<WorkingSet::Entry>& in_use) {
    if (in_use.empty()) return "";
    std::string body;
    for (const auto& e : in_use) {
        Hit h;
        h.card.uri = e.uri;
        h.card.title = e.title;
        body += pad(kind_name(e.uri.kind), 6) + " " + e.title + " — you were working from it; its text left the window   → " + read_hint(e.uri) + "\n";
    }
    return "<retrieval why=\"rebase\">\n" + body + "</retrieval>";
}

}  // namespace know
}  // namespace dd

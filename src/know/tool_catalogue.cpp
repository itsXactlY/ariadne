// src/know/tool_catalogue.cpp — impl of dd/know/tool_catalogue.hpp.
#include "dd/know/tool_catalogue.hpp"

#include <algorithm>

#include "dd/know/text.hpp"

namespace dd {
namespace know {

void ToolCatalogue::refresh() {
    if (!snap_.empty()) return;
    auto snap = std::make_unique<Snapshot>();
    std::vector<Card> cards;
    for (const auto& e : deck_.catalog()) {
        Card c;
        c.uri.kind = Kind::Tool;
        c.uri.id = e.name;
        c.title = e.name;
        c.gist = utf8_head(e.description, 300);
        c.group = e.source_name.empty() ? "other" : e.source_name;
        c.source = "deck";
        ++snap->by_source[c.group];
        cards.push_back(std::move(c));
    }
    snap->index = std::make_unique<Bm25f>(std::move(cards));
    snap_.publish(std::move(snap));
}

size_t ToolCatalogue::size() const {
    const auto s = snap_.read();
    return s ? s->index->size() : 0;
}

std::vector<Hit> ToolCatalogue::search(const Query& q, int k) const {
    const auto s = snap_.read();
    return s ? s->index->search(q, k) : std::vector<Hit>{};
}

float ToolCatalogue::idf(const std::string& term) const {
    const auto s = snap_.read();
    return s ? s->index->idf(term) : 0.0f;
}

std::optional<Body> ToolCatalogue::open(const Uri& u) {
    if (u.kind != Kind::Tool) return std::nullopt;
    const core::Json args = core::json_obj({{"name", std::make_shared<core::Json>(core::json_str(u.id))}});
    Body b;
    b.uri = u;
    b.text = deck_.describe(args);
    b.total = b.text.size();
    return b;
}

std::string ToolCatalogue::map() const {
    const auto s = snap_.read();
    if (!s || s->index->size() == 0) return "";
    // every source gets its line: a truncated list is read as the catalogue being
    //     smaller than it is, and 4 slots cannot hold one line per connected pod
    std::vector<std::pair<size_t, std::string>> by_size;
    for (const auto& [src, k] : s->by_source) by_size.push_back({k, src});
    std::sort(by_size.begin(), by_size.end(),
              [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    std::string out = "tools " + std::to_string(s->index->size()) + " deferred (behind tool_search):";
    for (std::size_t i = 0; i < by_size.size(); ++i) {
        if (i) out += (by_size.size() > 2 && i + 1 == by_size.size() ? " + " : " · ");
        out += by_size[i].second + " " + std::to_string(by_size[i].first);
    }
    return out;
}

}  // namespace know
}  // namespace dd

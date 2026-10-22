// src/know/tool_catalogue.cpp — impl of dd/know/tool_catalogue.hpp.
#include "dd/know/tool_catalogue.hpp"

#include <algorithm>

#include "dd/know/text.hpp"

namespace dd {
namespace know {

void ToolCatalogue::refresh() {
    if (snap_.load()) return;
    auto snap = std::make_shared<Snapshot>();
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
    snap_.store(snap);
}

size_t ToolCatalogue::size() const {
    const auto s = snap_.load();
    return s ? s->index->size() : 0;
}

std::vector<Hit> ToolCatalogue::search(const Query& q, int k) const {
    const auto s = snap_.load();
    return s ? s->index->search(q, k) : std::vector<Hit>{};
}

float ToolCatalogue::idf(const std::string& term) const {
    const auto s = snap_.load();
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
    const auto s = snap_.load();
    if (!s || s->index->size() == 0) return "";
    std::string out = "tools " + std::to_string(s->index->size()) + " deferred (behind tool_search):";
    size_t n = 0;
    for (const auto& [src, k] : s->by_source) {
        if (n++ == 4) break;
        out += (n == 1 ? " " : " · ") + src + " " + std::to_string(k);
    }
    return out;
}

}  // namespace know
}  // namespace dd

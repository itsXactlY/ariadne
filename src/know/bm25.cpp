// src/know/bm25.cpp — impl of dd/know/bm25.hpp.
#include "dd/know/bm25.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "dd/know/text.hpp"

namespace dd {
namespace know {

namespace {

constexpr int kFields = 4;   // name, tags, group, gist

struct DocFields {
    std::unordered_map<std::string, int> tf[kFields];
    int len[kFields] = {0, 0, 0, 0};
};

void count(DocFields& d, int field, const std::vector<std::string>& terms) {
    for (const auto& t : terms) ++d.tf[field][t];
    d.len[field] += static_cast<int>(terms.size());
}

}  // namespace

Bm25f::Bm25f(std::vector<Card> cards, Bm25fParams p) : p_(p), cards_(std::move(cards)) {
    const float w[kFields] = {p_.w_name, p_.w_tags, p_.w_group, p_.w_gist};
    const float b[kFields] = {p_.b_name, p_.b_tags, p_.b_group, p_.b_gist};

    std::vector<DocFields> docs(cards_.size());
    double total_len[kFields] = {0, 0, 0, 0};
    for (size_t i = 0; i < cards_.size(); ++i) {
        const Card& c = cards_[i];
        count(docs[i], 0, terms_of(c.title, true));
        std::string tags;
        for (const auto& t : c.tags) tags += t + " ";
        count(docs[i], 1, terms_of(tags));
        count(docs[i], 2, terms_of(c.group, true));
        count(docs[i], 3, terms_of(c.gist));
        for (int f = 0; f < kFields; ++f) total_len[f] += docs[i].len[f];
    }
    float avg[kFields];
    for (int f = 0; f < kFields; ++f) avg[f] = cards_.empty() ? 1.0f : static_cast<float>(std::max(1.0, total_len[f] / static_cast<double>(cards_.size())));

    for (size_t i = 0; i < docs.size(); ++i) {
        std::unordered_map<std::string, float> weighted;
        for (int f = 0; f < kFields; ++f) {
            const float norm = 1.0f - b[f] + b[f] * static_cast<float>(docs[i].len[f]) / avg[f];
            for (const auto& [term, tf] : docs[i].tf[f]) weighted[term] += w[f] * static_cast<float>(tf) / std::max(norm, 0.2f);
        }
        for (const auto& [term, tf] : weighted) postings_[term].push_back({static_cast<uint32_t>(i), tf});
    }
    const double n = static_cast<double>(cards_.size());
    for (const auto& [term, list] : postings_) {
        const double df = static_cast<double>(list.size());
        idf_[term] = static_cast<float>(std::log(1.0 + (n - df + 0.5) / (df + 0.5)));
    }
}

float Bm25f::idf(const std::string& term) const {
    auto it = idf_.find(term);
    return it == idf_.end() ? 0.0f : it->second;
}

std::vector<Hit> Bm25f::search(const Query& q, int k) const {
    std::vector<Hit> out;
    if (cards_.empty() || q.terms.empty() || k <= 0) return out;

    thread_local std::vector<float> acc;   // zero between calls: only what a call touched is reset
    thread_local std::vector<uint32_t> touched;
    if (acc.size() < cards_.size()) acc.assign(cards_.size(), 0.0f);
    touched.clear();

    double ideal = 0, ideal_mass = 0;
    for (const auto& term : q.terms) {
        auto it = postings_.find(term.text);
        if (it == postings_.end()) continue;
        const float idf = idf_.at(term.text);
        ideal += static_cast<double>(term.weight) * idf * (p_.k1 + 1.0f);
        ideal_mass += static_cast<double>(term.weight) * idf;
        for (const Posting& post : it->second) {
            if (acc[post.doc] == 0.0f) touched.push_back(post.doc);
            acc[post.doc] += term.weight * idf * (post.tf * (p_.k1 + 1.0f)) / (post.tf + p_.k1);
        }
    }
    if (ideal <= 0 || touched.empty()) return out;

    struct Cand {
        uint32_t doc;
        float raw;
        double norm;
    };
    std::vector<Cand> cands;
    cands.reserve(touched.size());
    for (uint32_t d : touched) {
        const double norm = std::min(1.0, static_cast<double>(acc[d]) / ideal * cards_[d].prior);
        cands.push_back({d, acc[d], norm});
        acc[d] = 0.0f;
    }
    const size_t keep = std::min<size_t>(static_cast<size_t>(k), cands.size());
    const size_t sorted = std::min(keep + 1, cands.size());   // one more: the margin of the last hit
    std::partial_sort(cands.begin(), cands.begin() + static_cast<long>(sorted), cands.end(), [&](const Cand& a, const Cand& b) {
        if (a.norm != b.norm) return a.norm > b.norm;
        return cards_[a.doc].title < cards_[b.doc].title;
    });

    for (size_t i = 0; i < keep; ++i) {
        Hit h;
        h.card = cards_[cands[i].doc];
        h.raw = cands[i].raw;
        h.score = cands[i].norm;
        h.margin = h.score - (i + 1 < cands.size() ? cands[i + 1].norm : 0.0);
        h.ideal = ideal_mass;
        for (const auto& term : q.terms) {
            auto it = postings_.find(term.text);
            if (it == postings_.end()) continue;
            const auto& list = it->second;
            const auto at = std::lower_bound(list.begin(), list.end(), cands[i].doc, [](const Posting& p, uint32_t d) { return p.doc < d; });
            if (at != list.end() && at->doc == cands[i].doc) {
                h.why += (h.why.empty() ? "" : " ") + term.text;
                ++h.matched;
                h.mass += static_cast<double>(term.weight) * idf_.at(term.text);
            }
        }
        out.push_back(std::move(h));
    }
    return out;
}

}  // namespace know
}  // namespace dd

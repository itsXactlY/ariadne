// src/know/bm25.cpp — impl of dd/know/bm25.hpp.
#include "dd/know/bm25.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_map>

#include "dd/core/par.hpp"
#include "dd/know/text.hpp"

namespace dd {
namespace know {

namespace {

constexpr int kFields = 5;   // name, tags, group, gist, learned
constexpr size_t kShards = 16;   // postings built in parallel: one map per shard, a term in hash % kShards

size_t shard_of(const std::string& term, size_t shards) { return std::hash<std::string>{}(term) % shards; }

struct Weighted {
    std::string term;
    float tf;
    uint8_t shard;   // which postings shard the term lives in, worked out once
};

struct DocFields {
    std::unordered_map<std::string, int> tf[kFields];
    int len[kFields] = {0, 0, 0, 0, 0};
};

void count(DocFields& d, int field, const std::vector<std::string>& terms) {
    for (const auto& t : terms) ++d.tf[field][t];
    d.len[field] += static_cast<int>(terms.size());
}

}  // namespace

Bm25f::Bm25f(std::vector<Card> cards, Bm25fParams p) : p_(p), cards_(std::move(cards)) {
    const float w[kFields] = {p_.w_name, p_.w_tags, p_.w_group, p_.w_gist, p_.w_learned};
    const float b[kFields] = {p_.b_name, p_.b_tags, p_.b_group, p_.b_gist, p_.b_learned};
    const long n = static_cast<long>(cards_.size());
    const bool par = cards_.size() >= p_.par_docs;
    [[maybe_unused]] const int threads = par ? core::par_threads() : 1;   // only the OpenMP clauses read it
    std::atomic<bool> failed{false};   // an exception must not leave an OpenMP region

    // each card on its own: its terms per field. The loops are independent, so they run on all cores.
    std::vector<DocFields> docs(cards_.size());
    double total_len[kFields] = {0, 0, 0, 0, 0};
#pragma omp parallel for schedule(dynamic, 16) num_threads(threads) if (par) reduction(+ : total_len[:kFields])
    for (long i = 0; i < n; ++i) {
        try {
            const Card& c = cards_[static_cast<size_t>(i)];
            DocFields& d = docs[static_cast<size_t>(i)];
            count(d, 0, terms_of(c.title, true));
            std::string tags;
            for (const auto& t : c.tags) tags += t + " ";
            count(d, 1, terms_of(tags));
            count(d, 2, terms_of(c.group, true));
            count(d, 3, terms_of(c.gist));
            if (!c.learned.empty()) {
                std::string learned;
                for (const auto& t : c.learned) learned += t.text + " ";
                count(d, 4, terms_of(learned));
            }
            for (int f = 0; f < kFields; ++f) total_len[f] += d.len[f];
        } catch (...) {
            failed.store(true);
        }
    }
    if (failed.load()) throw std::runtime_error("bm25: the cards could not be indexed");
    float avg[kFields];
    for (int f = 0; f < kFields; ++f) avg[f] = cards_.empty() ? 1.0f : static_cast<float>(std::max(1.0, total_len[f] / static_cast<double>(cards_.size())));

    // each card's weight per term, fields in order (the sum's order is part of the score)
    const size_t shards = par ? kShards : 1;
    std::vector<std::vector<Weighted>> weighted(cards_.size());
#pragma omp parallel for schedule(dynamic, 16) num_threads(threads) if (par)
    for (long i = 0; i < n; ++i) {
        try {
            const DocFields& d = docs[static_cast<size_t>(i)];
            std::unordered_map<std::string, float> sum;
            for (int f = 0; f < kFields; ++f) {
                const float norm = 1.0f - b[f] + b[f] * static_cast<float>(d.len[f]) / avg[f];
                for (const auto& [term, tf] : d.tf[f]) sum[term] += w[f] * static_cast<float>(tf) / std::max(norm, 0.2f);
            }
            auto& list = weighted[static_cast<size_t>(i)];
            list.reserve(sum.size());
            for (const auto& [term, tf] : sum) list.push_back({term, tf, static_cast<uint8_t>(shards == 1 ? 0 : shard_of(term, shards))});
        } catch (...) {
            failed.store(true);
        }
    }
    if (failed.load()) throw std::runtime_error("bm25: the cards could not be indexed");

    // the postings: one shard, or kShards each built by a thread from the same read-only weights, every list in doc order
    shards_.assign(shards, Shard{});
    const double total = static_cast<double>(cards_.size());
#pragma omp parallel for schedule(static) num_threads(threads) if (par)
    for (long s = 0; s < static_cast<long>(shards); ++s) {
        try {
            Shard& shard = shards_[static_cast<size_t>(s)];
            for (long i = 0; i < n; ++i)
                for (const auto& w : weighted[static_cast<size_t>(i)])
                    if (w.shard == s) shard[w.term].list.push_back({static_cast<uint32_t>(i), w.tf});
            for (auto& [term, e] : shard) {
                const double df = static_cast<double>(e.list.size());
                e.idf = static_cast<float>(std::log(1.0 + (total - df + 0.5) / (df + 0.5)));
            }
        } catch (...) {
            failed.store(true);
        }
    }
    if (failed.load()) throw std::runtime_error("bm25: the cards could not be indexed");
}

const Bm25f::Entry* Bm25f::entry(const std::string& term) const {
    if (shards_.empty()) return nullptr;
    const Shard& shard = shards_[shards_.size() == 1 ? 0 : shard_of(term, shards_.size())];
    auto it = shard.find(term);
    return it == shard.end() ? nullptr : &it->second;
}

float Bm25f::idf(const std::string& term) const {
    const Entry* e = entry(term);
    return e ? e->idf : 0.0f;
}

std::vector<Hit> Bm25f::search(const Query& q, int k) const {
    std::vector<Hit> out;
    if (cards_.empty() || q.terms.empty() || k <= 0) return out;

    thread_local std::vector<float> acc;   // zero between calls: only what a call touched is reset
    thread_local std::vector<uint32_t> touched;
    if (acc.size() < cards_.size()) acc.assign(cards_.size(), 0.0f);
    touched.clear();

    std::vector<const Entry*> ent;   // each query term's entry, looked up once
    ent.reserve(q.terms.size());
    for (const auto& term : q.terms) ent.push_back(entry(term.text));

    double ideal = 0, ideal_mass = 0;
    for (size_t t = 0; t < q.terms.size(); ++t) {
        if (!ent[t]) continue;
        const auto& term = q.terms[t];
        const float idf = ent[t]->idf;
        ideal += static_cast<double>(term.weight) * idf * (p_.k1 + 1.0f);
        ideal_mass += static_cast<double>(term.weight) * idf;
        for (const Posting& post : ent[t]->list) {
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
        for (size_t t = 0; t < q.terms.size(); ++t) {
            if (!ent[t]) continue;
            const auto& list = ent[t]->list;
            const auto at = std::lower_bound(list.begin(), list.end(), cands[i].doc, [](const Posting& p, uint32_t d) { return p.doc < d; });
            if (at != list.end() && at->doc == cands[i].doc) {
                h.why += (h.why.empty() ? "" : " ") + q.terms[t].text;
                ++h.matched;
                h.mass += static_cast<double>(q.terms[t].weight) * ent[t]->idf;
            }
        }
        out.push_back(std::move(h));
    }
    return out;
}

}  // namespace know
}  // namespace dd

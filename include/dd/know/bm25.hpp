#pragma once
// dd/know/bm25.hpp — BM25F over cards: name, tags, group, gist and learned are fields
// with their own weight and length normalisation. Built once per catalogue
// generation (an inverted index: a search touches only the postings of the
// query's terms), read lock-free from any thread.
//
// A score is normalised by what the query could have reached at best (every
// term it shares with the index, saturated in the best field), so it reads as
// "how much of what was asked is there": a long message cannot buy a high
// score with common words, a one-word question for a skill's name can reach 1.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "dd/know/types.hpp"

namespace dd {
namespace know {

struct Bm25fParams {
    float w_name = 8.0f, w_tags = 3.0f, w_group = 1.5f, w_gist = 1.0f, w_learned = 2.0f;
    float b_name = 0.2f, b_tags = 0.2f, b_group = 0.0f, b_gist = 0.75f, b_learned = 0.0f;
    float k1 = 2.0f;
    size_t par_docs = 512;   // a catalogue of at least this many cards is indexed in parallel (OpenMP); smaller ones never wake a thread
};

class Bm25f {
public:
    explicit Bm25f(std::vector<Card> cards, Bm25fParams p = {});

    size_t size() const { return cards_.size(); }
    const std::vector<Card>& cards() const { return cards_; }
    float idf(const std::string& term) const;   // 0 when no card has the term
    std::vector<Hit> search(const Query& q, int k) const;

private:
    struct Posting {
        uint32_t doc;
        float tf;   // weighted over the fields, length-normalised
    };
    struct Entry {
        std::vector<Posting> list;   // by doc, ascending
        float idf = 0.0f;
    };
    using Shard = std::unordered_map<std::string, Entry>;
    const Entry* entry(const std::string& term) const;
    Bm25fParams p_;
    std::vector<Card> cards_;
    std::vector<Shard> shards_;   // one, or kShards when built in parallel: a term lives in hash(term) % size
};

}  // namespace know
}  // namespace dd

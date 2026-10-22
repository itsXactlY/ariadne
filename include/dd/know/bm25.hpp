#pragma once
// dd/know/bm25.hpp — BM25F over cards: name, tags, group and gist are fields
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
    float w_name = 8.0f, w_tags = 3.0f, w_group = 1.5f, w_gist = 1.0f;
    float b_name = 0.2f, b_tags = 0.2f, b_group = 0.0f, b_gist = 0.75f;
    float k1 = 2.0f;
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
    Bm25fParams p_;
    std::vector<Card> cards_;
    std::unordered_map<std::string, std::vector<Posting>> postings_;
    std::unordered_map<std::string, float> idf_;
};

}  // namespace know
}  // namespace dd

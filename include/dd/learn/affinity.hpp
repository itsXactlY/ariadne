#pragma once
// dd/learn/affinity.hpp — which query terms have earned a place in a card's `learned` field.
//
// A (term, card) pair is promoted only when it was confirmed in several distinct episodes on
// several distinct days; a card keeps the heaviest few and the rest are evicted. Pure: Stats in,
// terms out, so the same Stats give the same terms on any thread count.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dd/know/types.hpp"
#include "dd/learn/stats.hpp"

namespace dd {
namespace learn {

struct AffinityParams {
    uint32_t min_episodes = 3;
    uint16_t min_days = 2;
    size_t max_terms = 24;      // per card; the lowest decayed weight goes first
    double min_weight = 0.0;    // a decayed weight at or under this is forgotten
};

// Assoc key: card uri '\x1f' term.
constexpr char kAssocSep = '\x1f';
std::string assoc_key(const std::string& card, const std::string& term);

// card uri string -> promoted terms, heaviest first (ties by text); Term::weight is the decayed weight.
std::map<std::string, std::vector<know::Term>> promote(const Stats& s, int64_t now, const AffinityParams& p = {});

}  // namespace learn
}  // namespace dd

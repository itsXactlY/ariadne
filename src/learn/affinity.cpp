// src/learn/affinity.cpp — impl of dd/learn/affinity.hpp.
#include "dd/learn/affinity.hpp"

#include <algorithm>

namespace dd {
namespace learn {

std::string assoc_key(const std::string& card, const std::string& term) { return card + kAssocSep + term; }

std::map<std::string, std::vector<know::Term>> promote(const Stats& s, int64_t now, const AffinityParams& p) {
    std::map<std::string, std::vector<know::Term>> out;
    for (const auto& [key, a] : s.assoc) {   // ordered by key: a card's terms are contiguous
        if (a.episodes < p.min_episodes || a.days < p.min_days) continue;
        const size_t sep = key.find(kAssocSep);
        if (sep == std::string::npos || sep + 1 >= key.size()) continue;
        const double w = a.weight.at(now, kHlAssoc);
        if (w <= p.min_weight) continue;
        out[key.substr(0, sep)].push_back({key.substr(sep + 1), static_cast<float>(w)});
    }
    for (auto& [card, terms] : out) {
        std::sort(terms.begin(), terms.end(), [](const know::Term& a, const know::Term& b) { return a.weight != b.weight ? a.weight > b.weight : a.text < b.text; });
        if (terms.size() > p.max_terms) terms.resize(p.max_terms);
    }
    return out;
}

}  // namespace learn
}  // namespace dd

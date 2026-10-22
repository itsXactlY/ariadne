#pragma once
// dd/learn/stats.hpp — the aggregates. A pure fold of Records: replay(journal) == live state, bit for bit.

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

#include "dd/learn/decayed.hpp"
#include "dd/learn/record.hpp"

namespace dd {
namespace learn {

// half-lives, seconds
constexpr double kHlConversion = 30 * kDay, kHlEfficacy = 90 * kDay, kHlAssoc = 45 * kDay;

struct CardStats {
    Decayed offered, opened;                    // conversion: opened / offered
    Decayed helped, failed;                     // efficacy: helped / (helped + failed)
    Decayed shadow;                             // near misses that were not offered
    State state = State::Active;                // learned skills move; disk and maze skills stay Active
    int64_t first_seen = 0, last_used = 0;

    // Wilson lower bounds in [0,1]; 0.5 when there is no evidence.
    double conversion_lcb(int64_t now) const;
    double efficacy_lcb(int64_t now) const;
};

// (term -> card): confirmed by distinct episodes on distinct days before it may enter the `learned` field.
struct Assoc {
    Decayed weight;
    uint32_t episodes = 0;       // distinct episodes (capped at 255)
    uint32_t last_episode = 0;
    uint16_t days = 0;           // number of distinct days (capped)
    int64_t last_day = 0;
};

struct Stats {
    std::unordered_map<std::string, CardStats> cards;      // key: Uri::str()
    std::map<std::string, Assoc> assoc;                    // key: card '\x1f' term (ordered: deterministic serialisation)
    uint64_t records = 0;                                  // folded so far
    int64_t now_hint = 0;                                  // the newest ts seen: the fold never reads the wall clock

    void apply(const Record& r);                           // the fold; deterministic; ignores events it does not use
};

}  // namespace learn
}  // namespace dd

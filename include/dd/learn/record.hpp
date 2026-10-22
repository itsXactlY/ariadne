#pragma once
// dd/learn/record.hpp — what the journal holds. The journal is the truth: every aggregate is a fold over these.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "dd/know/event.hpp"

namespace dd {
namespace learn {

enum class Outcome : uint8_t { Ignored, Opened, Helped, Failed, Vetoed };
const char* outcome_name(Outcome o);

enum class State : uint8_t { Draft, Probation, Active, Dormant, Retired, Rejected };
const char* state_name(State s);

// One raw router event, as emitted (the Observer re-reads these to build exposures).
struct EventRec { know::Event e; };

// What an exposure turned out to be worth: written by the CreditAssigner, folded into CardStats.
struct OutcomeRec {
    know::Uri uri;
    Outcome outcome = Outcome::Ignored;
    std::string trigger;                 // turn | shift | failure | stall
    float weight = 1.0f;                 // 1 for the top offer, 0.5 below (position bias)
    std::vector<std::string> terms;      // the query terms of the exposure (Affinity candidates)
    uint32_t episode = 0;                // distinct-episode counter for the affinity promotion rule
    int64_t ts = 0;                      // unix seconds
};

// A lifecycle step for a learned skill.
struct StateRec { know::Uri uri; State from = State::Draft, to = State::Draft; std::string why; int64_t ts = 0; };

// A drafted or rejected skill (the content lives on disk; the journal keeps the digest and the verdict).
struct DraftRec { std::string slug, category, digest, verdict; bool accepted = false; int64_t ts = 0; };

// `forget`: the fold drops everything about this uri and ignores earlier records for it.
struct TombstoneRec { know::Uri uri; int64_t ts = 0; };

using Record = std::variant<EventRec, OutcomeRec, StateRec, DraftRec, TombstoneRec>;

}  // namespace learn
}  // namespace dd

#pragma once
// dd/know/policy.hpp — when the router speaks, how much, and how sure it has to be.
//
// Confidence is calibrated on real prompts against the real catalogue (`dd replay`): a hit
// is offered when it matched at least two terms of the question, those terms carry real
// evidence (idf mass: two rare terms, or one rare and more), and they are a fair share of
// what was asked. A single generic word ("python") scores high on share alone and carries
// no evidence, which is why share alone cannot be the rule.
//
//   offer  matched >= 2, mass >= 6.5, score >= 0.15   a pointer line      (81% right in the top 3, 3 false in 44)
//   open   ... and score >= 0.25                      the head of the skill opened for the model (92%, 1 false)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "dd/core/config.hpp"
#include "dd/know/signal.hpp"
#include "dd/know/types.hpp"

namespace dd {
namespace know {

// where a note lands in the tape
enum class Placement {
    Prefix,         // the system prompt: once per launch
    QuestionNote,   // appended to the user's question, rendered once
    AfterTools,     // a hidden item appended after a batch of tool results
    Anchor,         // inside the rebase anchor
};

struct Confidence {
    int min_matched = 2;
    double offer_mass = 6.5, offer_score = 0.15, open_score = 0.25;
    size_t calibrated_at = 568;   // the catalogue size offer_mass was fitted at; 0: offer_mass is absolute
    // Evidence is idf, and idf grows with ln(catalogue size): the same two terms are worth less in a
    // catalogue of twenty than of five hundred, so the bar scales with it (never under 1).
    double needed(size_t catalogue) const {
        if (calibrated_at == 0 || catalogue == 0) return offer_mass;
        const double scale = std::log(static_cast<double>(catalogue) + 1.0) / std::log(static_cast<double>(calibrated_at) + 1.0);
        return std::max(1.0, offer_mass * scale);
    }
    bool offers(const Hit& h, size_t catalogue = 0) const { return h.matched >= min_matched && h.mass >= needed(catalogue) && h.score >= offer_score; }
    bool opens(const Hit& h, size_t catalogue = 0) const { return offers(h, catalogue) && h.score >= open_score; }
};

// Demand refusal. A card the offer bar keeps surfacing and the model keeps not opening holds a slot nothing
// reads. The fold's decayed conversion is the evidence; the probe floor is the exit condition — without it the
// guard becomes the deadlock it replaces, since a card never offered can never earn the evidence that would let
// it back. Shares the thresholds with learn::LifecycleParams, which demotes on the same evidence.
struct Refusal {
    bool on = true;
    double refuse_offers = 8;    // decayed offers at the fold's now ...
    double refuse_floor = 0.15;  // ... with conversion lower bound under this: refused
    int probe_turns = 6;         // ... and it is offered once every this many turns, never silenced
};

struct Policy {
    Confidence skills;
    Confidence memory{3, 6.5, 0.20, 2.0};   // curated rows are many and long: three terms, and never opened for the model
    int deep_ms = 1500;                      // a turn start waits this long for the pod's recall
    int deep_recall_ms = 30000;              // ... and this long when the question is about the past
    size_t turn_tokens = 600, hint_tokens = 250, failure_tokens = 300, stall_tokens = 350, max_turn_tokens = 1500;
    size_t open_head_bytes = 4200;           // the head opened for the model: ~1.2k tokens
    size_t open_max_bytes = 16000;           // a skill past this is never opened for the model, only pointed to
    int top_k = 3;
    int shift_gap_rounds = 2;                // no shift note within this many rounds of the last note
    int shift_new_terms = 2;                 // a shift needs this many content terms the epoch has not covered
    int stall_identical = 2;                 // the same call this many times in a row
    int stall_blind = 3;                     // this many looks around in a row that found nothing
    bool auto_route = true;                  // skills.auto_route: offers on its own at all
    bool auto_open = true;                   // the head of a confident skill is opened for the model
    bool shift = true, failure = true, stall = true;
    Refusal refusal;

    static Policy from(const core::Config& cfg);
};

// A retrieval the router will run for a signal.
struct Intent {
    std::vector<Kind> kinds;
    Query query;
    bool deep = false;           // also the pod's recall (6-17 s), within the deadline
    int wait_ms = 0;             // how long this intent waits for it
    Placement where = Placement::QuestionNote;
    size_t tokens = 600;         // the most the note may cost
    std::string reason;          // "turn", "shift: podman quadlet", "failure: ...", "stall: blind"
    std::string key;             // what to remember as spoken to (an error signature)
    bool notice = false;         // a redirect notice leads the note (stall)
};
using Plan = std::vector<Intent>;

}  // namespace know
}  // namespace dd

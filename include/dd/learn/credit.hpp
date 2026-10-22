#pragma once
// dd/learn/credit.hpp — what an offer turned out to be worth.
//
// An Exposure is one pointer (or one open the model made itself) and what happened in its window:
// until the turn ended or `window` rounds passed. A CreditAssigner judges a closed Exposure; the
// rules are pure so each row of the table is one test.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dd/learn/record.hpp"

namespace dd {
namespace learn {

struct Exposure {
    know::Uri uri;                    // kind + id only
    std::string trigger;              // turn | shift | failure | stall | ask | model | tripwire
    std::string key;                  // the error signature that triggered it ("" for a turn start)
    std::vector<std::string> terms;   // the query terms that produced the hit
    uint32_t turn = 0, round_start = 0, episode = 0;
    int64_t ts = 0;                   // unix seconds when it closed
    int rank = 0;                     // 0: the top offer of its batch
    bool opened = false;              // the router opened it or the model read it
    bool by_model = false;            // ... and it was the model
    bool error_recurred = false;      // `key` showed up again after the offer
    bool stalled = false;             // a stall fired while it was open
    bool vetoed = false;              // a tripwire held a call (uri is a rule)
    bool turn_ended = false;          // closed by the turn's end, not by the round cap
    int trailing_failures = 0;        // failed calls in a row at the close

    float weight() const { return rank == 0 ? 1.0f : 0.5f; }   // position bias
};

class CreditAssigner {
public:
    virtual ~CreditAssigner() = default;
    virtual Outcome judge(const Exposure& x) const = 0;
};

// The table of LEARN.md 4.3.
class RuleCredit final : public CreditAssigner {
public:
    int failed_run = 2;   // this many failed calls in a row at the close is "a failed tail"
    Outcome judge(const Exposure& x) const override;
};

}  // namespace learn
}  // namespace dd

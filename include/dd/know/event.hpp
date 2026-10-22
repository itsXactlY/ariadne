#pragma once
// dd/know/event.hpp — what the router tells whoever learns from it.
//
// The router emits; it never waits and never learns. An EventSink is one push onto a queue
// (NullSink by default, so learning off is the router of P4, byte for byte). An Overlay is the read
// side: priors and learned terms a catalogue folds in while it builds its next snapshot.

#include <cstdint>
#include <string>
#include <vector>

#include "dd/know/policy.hpp"
#include "dd/know/signal.hpp"
#include "dd/know/types.hpp"

namespace dd {
namespace know {

struct Event {
    enum class Type : uint8_t {
        Offered,          // a pointer line was delivered for `uri`
        OpenedByRouter,   // the head was opened for the model (auto_open)
        OpenedByModel,    // the model read `uri` itself
        Calls,            // a batch of tool calls finished (`call` holds one; one Event per call)
        TurnStart,        // a user turn began (`key` = distilled query digest)
        TurnEnd,
        Rebase,
        Tripwire,         // a rule held a call (`uri` = rule://name)
        Stall,            // the stall trigger fired (`reason` = identical|blind)
        Shadow,           // a hit with matched >= 2 that missed the offer bar: the counterfactual
    };
    Type type = Type::Offered;
    uint64_t ts_ns = 0;                 // wall clock; ordering is by the sink's sequence, not this
    uint32_t turn = 0, round = 0, epoch = 0;
    Uri uri;
    Placement where = Placement::QuestionNote;
    std::string reason;                 // the trigger: turn | shift | failure | stall | ask
    std::string key;                    // error signature / intent / query digest
    std::vector<std::string> terms;     // distilled query terms (<= 12) that produced the hit
    float score = 0, mass = 0;
    int matched = 0;
    CallRecord call;                    // Type::Calls
};

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void emit(Event&& e) noexcept = 0;   // wait-free for the caller; drops (and counts) rather than blocks
};

class NullSink final : public EventSink {
public:
    void emit(Event&&) noexcept override {}
};

class Overlay {
public:
    virtual ~Overlay() = default;
    virtual double prior(const Uri&) const { return 1.0; }                 // multiplies Card::prior; in [0.5, 1.5]
    virtual void learned_terms(const Uri&, std::vector<Term>&) const {}    // the Bm25f `learned` field
    virtual uint64_t version() const { return 0; }                         // a catalogue rebuilds when this moves
};

}  // namespace know
}  // namespace dd

#pragma once
// dd/context/recall.hpp — what the maze hands a turn, and the skills it holds.
//
// Recaller: one embedding recall per turn (the pod serialises them, ~7 s),
// then the graph around it in parallel, all cheap: spreading activation from
// the best hit read back by id, the facts extracted from the top hits, and —
// for a question about earlier — this session's own turns by id. Nothing the
// window already carries comes back (the ledger knows what is on the tape),
// and the block fits a budget. The intent classifier routes it (40 ms).
//
// The skill catalogue is not here: dd/know owns it (and every other decision about
// when and what to look up); this is the pod's deep leg.

#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dd/context/memory.hpp"

namespace dd {
namespace context {

struct Prefetch {
    std::string block;        // <memory-context>… or ""
    std::string intent, skill;   // skill: set by the router, for the card
    int hits = 0, linked = 0, facts = 0, earlier = 0, dropped = 0;   // dropped: already in the window
    std::vector<long> ids;    // what was injected
    // the same rows for a recall card: 'r' recalled, 'l' linked, 'f' fact, 'e' earlier; content is a snippet
    std::vector<std::pair<char, Hit>> shown;
    double ms = 0;
};

class Recaller {
public:
    explicit Recaller(std::shared_ptr<Memory> mem) : mem_(std::move(mem)) {}
    // in_window: ids already on the tape; earlier: this session's turn ids that are not.
    Prefetch run(const std::string& query, const std::set<long>& in_window, const std::vector<long>& earlier,
                 size_t budget_chars = 6000) const;

private:
    std::shared_ptr<Memory> mem_;
};

}  // namespace context
}  // namespace dd

#pragma once
// dd/learn/episode.hpp — the recent past, boiled down: calls and their error signatures, no result bodies.
//
// The recorder is the learn thread's alone. It keeps the last 3 turns and at most 200 calls; a
// detector reads the window, and an accepted episode is cut away with drop_through so it is not found twice.

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "dd/know/event.hpp"

namespace dd {
namespace learn {

struct Step {
    know::CallRecord call;
    uint32_t turn = 0, round = 0;
};

struct Episode {
    std::vector<Step> steps;

    bool empty() const { return steps.empty(); }
    // distinct (turn, round) pairs in order: the clock of "rounds"
    std::vector<std::pair<uint32_t, uint32_t>> ticks() const;
    // sha256 over the steps' digests and error signatures: the same work gives the same digest
    std::string digest() const;
};

class EpisodeRecorder {
public:
    struct Options {
        size_t max_steps = 200;
        uint32_t max_turns = 3;
        size_t max_args = 240, max_error = 240;
    };
    EpisodeRecorder() = default;
    explicit EpisodeRecorder(Options o) : opt_(o) {}

    void feed(const know::Event& e);   // Calls, TurnStart, TurnEnd; the rest is ignored
    Episode window() const;
    void drop_through(uint32_t turn, uint32_t round);   // forget everything up to and including that round
    void clear() { steps_.clear(); }
    size_t size() const { return steps_.size(); }
    uint32_t last_turn() const { return last_turn_; }
    uint64_t evicted() const { return evicted_; }

private:
    Options opt_;
    std::deque<Step> steps_;   // the learn worker thread's alone: EpisodeRecorder is Service's own,
                               // fed by drain_once, so the fold reads the same deque the sink wrote
    uint32_t last_turn_ = 0;
    uint64_t evicted_ = 0;
    void trim();
};

}  // namespace learn
}  // namespace dd

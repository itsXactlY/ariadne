#pragma once
// dd/know/working_set.hpp — what the model has been told and has opened, in which epoch and turn.
//
// The router asks it before it speaks: was this card offered already (once per turn at a turn's
// start, once per epoch mid-turn), is its body still in the window, which topics has this turn
// already covered (a shift is a topic the turn has not covered), what has the model been doing
// lately (the same call twice, a run of looks around that found nothing). After a rebase it names
// the skills the model was working from, so they are pointed to again instead of forgotten.
//
// One thread owns it (the agent loop's); tool threads report what the model opened through the
// router's queue.

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dd/know/signal.hpp"
#include "dd/know/types.hpp"

namespace dd {
namespace know {

class WorkingSet {
public:
    struct Entry {
        Uri uri;
        std::string title;
        bool opened = false;
        int epoch = 0, turn = 0;
        size_t tokens = 0;
    };

    // ---- the clock ----
    void new_turn(const std::string& question);   // a user message arrived
    void new_round();                             // a batch of tool calls finished
    void new_epoch(int epoch);                    // the window was rebuilt: bodies left it
    int turn() const { return turn_; }
    int round() const { return round_; }
    int epoch() const { return epoch_; }
    const std::string& question() const { return question_; }

    // ---- what was said ----
    void offered(const Uri& u, const std::string& title, size_t tokens);
    void opened(const Uri& u, const std::string& title, size_t tokens);
    bool offered_this_turn(const Uri& u) const;
    bool offered_this_epoch(const Uri& u) const;
    bool open_in_window(const Uri& u) const;                       // opened in the current epoch
    std::vector<Entry> in_use(Kind kind, int turns = 2) const;     // opened in the last `turns` turns, newest first
    size_t turn_tokens() const { return turn_tokens_; }            // spent on notes this turn
    void spend(size_t tokens) { turn_tokens_ += tokens; }

    // ---- topics ----
    void cover(const std::vector<Term>& terms);
    bool covered(const std::string& term) const { return covered_.count(term) > 0; }
    int uncovered(const std::vector<Term>& terms) const;

    // ---- notes ----
    int rounds_since_note() const { return round_ - last_note_round_; }
    void noted() { last_note_round_ = round_; }

    // ---- what the model has been doing ----
    void record(const std::vector<CallRecord>& calls);
    int identical_streak() const;                  // the newest call, repeated this many times in a row
    int blind_streak() const;                      // looks around in a row, newest first, that found nothing
    std::string last_blind_summary() const;        // "list_dir x2, read_file miss"
    int failures(const std::string& signature) const;
    int family_failures(const std::string& tool) const;
    bool stalled() const { return stalled_; }
    void set_stalled() { stalled_ = true; }
    bool was_warned(const std::string& signature) const { return warned_.count(signature) > 0; }   // spoken to this turn
    void warn(const std::string& signature) { warned_.insert(signature); }

private:
    int turn_ = 0, round_ = 0, epoch_ = 0, last_note_round_ = -1000;
    std::string question_;
    std::vector<Entry> entries_;
    std::set<std::string> covered_, warned_;
    std::deque<CallRecord> calls_;
    std::map<std::string, int> fail_sig_, fail_tool_;
    size_t turn_tokens_ = 0;
    bool stalled_ = false;
};

}  // namespace know
}  // namespace dd

#pragma once
// dd/know/router.hpp — one place that decides when, what and how to look something up.
//
// The engine tells the router what happened (a turn started, a batch of calls finished, the window
// was rebuilt); triggers plan retrievals; the router runs them against the catalogues (cards: local,
// microseconds) and the pod's recall (deep: seconds, one at a time, with a deadline), keeps what it
// said in the working set so it says nothing twice, and hands back one note per placement. Nothing
// else in the harness looks anything up on its own.
//
// Threads: on(), late() and set_ratio() belong to the agent loop. note_opened() is for tool threads
// (a lock-free queue the agent thread drains). reattach(), recall_now() and find() are safe anywhere.
// A deep recall that outlives its deadline finishes on its own thread into a slot the router reads.

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "dd/context/memory.hpp"
#include "dd/context/recall.hpp"
#include "dd/core/mpsc.hpp"
#include "dd/core/snap_cell.hpp"
#include "dd/know/deliver.hpp"
#include "dd/know/event.hpp"
#include "dd/know/memory_catalogue.hpp"
#include "dd/know/policy.hpp"
#include "dd/know/skills.hpp"
#include "dd/know/tool_catalogue.hpp"
#include "dd/know/trigger.hpp"
#include "dd/know/working_set.hpp"

namespace dd {
namespace know {

struct Delivery {
    std::string note;                // "" = nothing to say; one note for the placement the signal implies
    std::vector<Uri> offered, opened;
    context::Prefetch deep;          // the pod's answer, when it came in time (the recall card, the ids)
    bool deep_late = false;          // it did not: late() has it when it lands
    std::string summary;             // one line for the recall card
    std::string skill;               // the best skill offered
    double ms = 0;
};

class Router {
public:
    struct Catalogues {
        std::shared_ptr<SkillCatalogue> skills;
        std::shared_ptr<MemoryCatalogue> memory;   // may be null
        std::shared_ptr<ToolCatalogue> tools;      // may be null
    };
    // pod: the deep leg's memory (null: no deep leg)
    Router(Catalogues cats, Policy policy, std::shared_ptr<context::Memory> pod);

    // ---- the agent loop ----
    Delivery on(const Signal& s);
    std::optional<Delivery> late();                 // a deep answer that missed its deadline and has landed since
    void set_ratio(Deliverer::Ratio r) { deliver_ = Deliverer(std::move(r)); }
    void end_turn();                                // the turn is over: learning closes its exposures
    void note_tripwire(const std::string& rule);   // a rule held a call
    void set_sink(std::shared_ptr<EventSink> s) { sink_ = s ? std::move(s) : std::make_shared<NullSink>(); }   // before the first signal
    // Demand, from the same overlay the catalogues read for priors. It never moves version(), so asking it is
    // free; without it every card's demand is unknown and the guard leaves everything alone.
    void set_overlay(std::shared_ptr<const Overlay> o) { overlay_ = std::move(o); }   // before the first signal

    // ---- any thread ----
    std::string reattach() const;                   // the skills the model was working from, as pointers; "" if none
    context::Prefetch recall_now(const std::string& query, const std::set<long>& in_window, const std::vector<long>& earlier) const;
    std::vector<Hit> find(const std::string& text, const std::vector<Kind>& kinds, int k) const;
    bool offers(const Hit& h) const;                // would a pointer be offered for this hit (the `find` tool's strong/weak)
    std::optional<Body> open(const Uri& u);         // the model opens something: any catalogue; recorded
    void note_opened(const Uri& u, const std::string& title, size_t bytes);   // tool threads

    // ---- facts for the prompt and the UI ----
    std::vector<std::string> map() const;           // one line per catalogue
    size_t skills() const { return cats_.skills ? cats_.skills->size() : 0; }
    std::shared_ptr<SkillCatalogue> skill_catalogue() const { return cats_.skills; } // the enumeration route needs the cards, not just the census count
    int turn() const { return ws_.turn(); }          // agent thread
    const Policy& policy() const { return policy_; }
    Catalogue* catalogue(Kind k) const;

    struct Stats {
        std::atomic<uint64_t> signals{0}, notes{0}, offered{0}, opened_for{0}, opened_by_model{0}, deep_in{0}, deep_late{0}, tokens{0};
        std::atomic<uint64_t> refused{0};   // candidate slots the demand guard declined, over the process
    };
    const Stats& stats() const { return stats_; }
    std::function<void(const std::string&)> on_log;   // one JSON line per signal that said something
    std::function<bool()> cancelled;                  // the turn was interrupted: stop waiting for the pod

private:
    struct DeepJob {
        std::atomic<uint32_t> done{0};   // 1 when `result` is in; the waiter parks on it
        context::Prefetch result;
        int turn = 0;
        bool delivered = false;   // the agent thread's
    };
    struct Opened {
        Uri uri;
        std::string title;
        size_t bytes = 0;
    };

    Catalogues cats_;
    Policy policy_;
    std::shared_ptr<context::Memory> pod_;
    context::Recaller recaller_;
    Distiller distill_;
    Deliverer deliver_;
    WorkingSet ws_;
    std::vector<std::unique_ptr<Trigger>> triggers_;
    core::MpscQueue<Opened> opened_q_;
    std::shared_ptr<DeepJob> inflight_;
    std::set<long> last_in_window_;
    std::vector<long> last_earlier_;
    core::SnapCell<std::string> reattach_;
    std::shared_ptr<const Overlay> overlay_;
    int declined_ = 0;   // slots the guard declined during the signal being run: the log line's own count
    // One entry per card offered since its last open. The agent thread only: opens arrive through opened_q_.
    struct Refusal { int offers = 0, opens = 0; int closed_until = 0; };
    std::unordered_map<std::string, Refusal> refusal_;
    Stats stats_;
    std::shared_ptr<EventSink> sink_ = std::make_shared<NullSink>();

    Event ev(Event::Type t, const Uri& u = Uri(), const std::string& reason = std::string()) const;
    void drain_opened();
    void publish_reattach();
    IdfFn idf_fn() const;
    void run(const Intent& in, Delivery& d, std::vector<std::string>& notes);
    void submit_deep(const std::string& query);
    void log(const char* signal, const Delivery& d, const std::string& reason, const char* deep) const;
    bool seen(const Uri& u, Placement where) const;
    bool refused(const Uri& u);      // demand refused: it may not take a slot this turn (the probe window moves it)
    void note_offer(const Uri& u);   // an offer was made: it is the demand channel's numerator
    void note_demand_open(const Uri& u);
};

}  // namespace know
}  // namespace dd

#pragma once
// dd/learn/lifecycle.hpp — a learned skill's life: probation, active, dormant, retired.
//
// A table of transitions, evaluated against the Stats on the learn thread. Only skills whose SKILL.md says
// `origin: learned` take part; disk and maze skills are never looked at. The file is the truth for a skill's state
// (state, state_since, demoted in its frontmatter); the journal mirrors it with a StateRec per change. Nothing is
// deleted: a retired skill moves to <skills>/.retired/<category>/<name>/.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "dd/learn/record.hpp"
#include "dd/learn/stats.hpp"

namespace dd {
namespace learn {

struct LifecycleParams {
    double promote_wins = 3;             // Helped to leave probation ...
    double decay_allowance = 0.9;        // ... counted decayed, so wins a few days apart still add up: 0.9 x 3 = 2.7
    int64_t promote_quiet_s = 30 * 86400;   // ... with no failure this recently
    double retire_failures = 2;          // Failed (decayed) after entering probation
    int64_t probation_max_s = 60 * 86400;   // probation without a win
    int64_t dormant_after_s = 120 * 86400;  // Active, unused
    int64_t retire_after_s = 365 * 86400;   // Dormant, unused
    double demote_floor = 0.25;          // efficacy lower bound under this ...
    double demote_evidence = 5;          // ... with this much evidence (Helped + Failed) demotes once
    // Demand: offered often, opened almost never. Its own channel, its own thresholds — it costs a card its
    // cap, never its place. Retirement still needs efficacy draws, so a refused card goes quiet, not gone.
    double refuse_offers = 8;            // decayed offers ...
    double refuse_floor = 0.15;          // ... with a conversion lower bound under this demotes once
};

// A learned skill as the lifecycle sees it: its file and its card (null: no evidence yet).
struct Subject {
    std::string name, category;
    State state = State::Probation;
    int64_t since = 0;                   // when it entered `state`
    bool demoted = false;
    const CardStats* card = nullptr;
    int64_t now = 0;
    int64_t last_used() const;           // newest use, never before `since`
};

struct Transition {
    State from, to;
    const char* why;
    std::function<bool(const Subject&, const LifecycleParams&)> guard;
};

class Lifecycle {
public:
    struct Change {
        std::string name, category;
        State from, to;
        std::string why;
    };
    using Notify = std::function<void(const StateRec&)>;

    explicit Lifecycle(std::filesystem::path skills_root, LifecycleParams p = {});

    // One pass: at most one transition per skill. Each change rewrites the file, moves it if retired, and calls `record`.
    std::vector<Change> step(const Stats& stats, int64_t now, const Notify& record) const;

    const std::vector<Transition>& table() const { return table_; }
    const LifecycleParams& params() const { return p_; }

    // `state:` (and `state_since:`, `demoted:`) in a learned skill's frontmatter, atomically. False: not learned, or no file.
    static bool rewrite_state(const std::filesystem::path& md, State to, int64_t since, bool demoted);
    static std::filesystem::path retired_dir(const std::filesystem::path& root, const std::string& category, const std::string& name);

private:
    std::filesystem::path root_;
    LifecycleParams p_;
    std::vector<Transition> table_;
};

}  // namespace learn
}  // namespace dd

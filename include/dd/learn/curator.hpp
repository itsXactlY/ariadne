#pragma once
// dd/learn/curator.hpp — folds Stats into the overlay the catalogues read.
//
// A pass turns each card's evidence into a prior (the efficacy lower bound, mapped to 0.5..1.5,
// moving at most 5% per pass) and its confirmed associations into learned terms. It publishes only
// when something moved past a threshold, so the catalogue does not rebuild on noise. The fold is
// deterministic: any thread count gives the same snapshot.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "dd/learn/affinity.hpp"
#include "dd/learn/overlay.hpp"
#include "dd/learn/stats.hpp"

namespace dd {
namespace learn {

struct CuratorParams {
    double lo = 0.5, hi = 1.5;
    double step = 0.05;              // the most a prior may move, relative, per pass
    double move = 0.01;              // a pass publishes when a prior moved this much from the published one
    double shadow_boost = 0.1;       // the most a card with near misses and no efficacy evidence gains
    double probation_cap = 0.7, dormant_cap = 0.8;
    size_t par_cards = 256;          // fewer cards than this never open a parallel region
    AffinityParams affinity;
};

class Curator {
public:
    Curator(std::shared_ptr<LearnOverlay> overlay, CuratorParams p = {});

    // Folds `s` at `now` (unix seconds). True when a new snapshot was published (version bumped).
    bool pass(const Stats& s, int64_t now);

    // report.md: top movers, learned terms, dormant and retired, hubs. Atomic (temp + rename).
    bool write_report(const Stats& s, int64_t now, const std::filesystem::path& dir) const;

    const std::shared_ptr<LearnOverlay>& overlay() const { return overlay_; }
    const CuratorParams& params() const { return p_; }

private:
    std::shared_ptr<LearnOverlay> overlay_;
    CuratorParams p_;
};

}  // namespace learn
}  // namespace dd

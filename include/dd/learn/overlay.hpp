#pragma once
// dd/learn/overlay.hpp — the read side of learning: priors and learned terms, served lock-free.

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dd/core/snap_cell.hpp"
#include "dd/know/event.hpp"

namespace dd {
namespace learn {

struct OverlaySnapshot {
    std::unordered_map<std::string, double> prior;                    // Uri::str() -> multiplier in [0.5, 1.5]; absent = 1
    std::unordered_map<std::string, std::vector<know::Term>> terms;   // Uri::str() -> learned terms
};

// Demand rows, refreshed every pass. Kept out of OverlaySnapshot on purpose: decayed counts change constantly,
// and publishing them through publish() would bump version() and rebuild the catalogues on every pass.
struct DemandSnapshot {
    std::unordered_map<std::string, know::Demand> rows;               // Uri::str() -> offered/opened/lcb; absent = no evidence
};

class LearnOverlay final : public know::Overlay {
public:
    double prior(const know::Uri& u) const override;
    void learned_terms(const know::Uri& u, std::vector<know::Term>& out) const override;
    know::Demand demand(const know::Uri& u) const override;
    uint64_t version() const override { return version_.load(std::memory_order_acquire); }

    // The Curator's alone: one publisher at a time.
    void publish(std::unique_ptr<const OverlaySnapshot> s);
    // ... and demand does NOT bump version_: the router reads it each turn, the catalogues never rebuild for it.
    void publish_demand(std::unique_ptr<const DemandSnapshot> s);
    std::shared_ptr<const OverlaySnapshot> copy() const;   // for the next pass to diff against

private:
    core::SnapCell<OverlaySnapshot> cell_;
    core::SnapCell<DemandSnapshot> demand_;
    std::atomic<uint64_t> version_{0};
};

}  // namespace learn
}  // namespace dd

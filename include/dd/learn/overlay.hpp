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

class LearnOverlay final : public know::Overlay {
public:
    double prior(const know::Uri& u) const override;
    void learned_terms(const know::Uri& u, std::vector<know::Term>& out) const override;
    uint64_t version() const override { return version_.load(std::memory_order_acquire); }

    // The Curator's alone: one publisher at a time.
    void publish(std::unique_ptr<const OverlaySnapshot> s);
    std::shared_ptr<const OverlaySnapshot> copy() const;   // for the next pass to diff against

private:
    core::SnapCell<OverlaySnapshot> cell_;
    std::atomic<uint64_t> version_{0};
};

}  // namespace learn
}  // namespace dd

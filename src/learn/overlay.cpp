// src/learn/overlay.cpp — impl of dd/learn/overlay.hpp.
#include "dd/learn/overlay.hpp"

namespace dd {
namespace learn {

double LearnOverlay::prior(const know::Uri& u) const {
    const auto r = cell_.read();
    if (!r) return 1.0;
    const auto it = r->prior.find(u.str());
    return it == r->prior.end() ? 1.0 : it->second;
}

void LearnOverlay::learned_terms(const know::Uri& u, std::vector<know::Term>& out) const {
    const auto r = cell_.read();
    if (!r) return;
    const auto it = r->terms.find(u.str());
    if (it != r->terms.end()) out.insert(out.end(), it->second.begin(), it->second.end());
}

know::Demand LearnOverlay::demand(const know::Uri& u) const {
    const auto r = demand_.read();
    if (!r) return {};
    const auto it = r->rows.find(u.str());
    return it == r->rows.end() ? know::Demand{} : it->second;
}

void LearnOverlay::publish_demand(std::unique_ptr<const DemandSnapshot> s) {
    demand_.publish(std::move(s));   // no version_: demand is the router's input, not a card's content
}

void LearnOverlay::publish(std::unique_ptr<const OverlaySnapshot> s) {
    cell_.publish(std::move(s));
    version_.fetch_add(1, std::memory_order_release);
}

std::shared_ptr<const OverlaySnapshot> LearnOverlay::copy() const {
    const auto r = cell_.read();
    return r ? std::make_shared<const OverlaySnapshot>(*r) : std::make_shared<const OverlaySnapshot>();
}

}  // namespace learn
}  // namespace dd

#pragma once
// dd/learn/observer.hpp — turns the router's event stream into exposures, and exposures into outcomes.
//
// Single-threaded: the learn thread feeds it in arrival order. Events carry turn and round, so the
// window is counted in rounds of the agent loop, not in wall time.

#include <functional>
#include <memory>
#include <vector>

#include "dd/learn/credit.hpp"

namespace dd {
namespace learn {

class Observer {
public:
    using Emit = std::function<void(OutcomeRec&&)>;
    explicit Observer(Emit emit, std::shared_ptr<const CreditAssigner> credit = std::make_shared<RuleCredit>(), uint32_t window_rounds = 6)
        : emit_(std::move(emit)), credit_(std::move(credit)), window_(window_rounds) {}

    void feed(const know::Event& e);
    void flush();                                      // close everything still open (shutdown)
    size_t open() const { return open_.size(); }

private:
    void close(size_t i);
    void close_where(const std::function<bool(const Exposure&)>& pred);
    Exposure* find(const know::Uri& u);

    Emit emit_;
    std::shared_ptr<const CreditAssigner> credit_;
    uint32_t window_;
    std::vector<Exposure> open_;
    uint32_t episode_ = 0, round_ = 0, batch_turn_ = ~0u, batch_round_ = ~0u;
    int batch_n_ = 0;       // offers made in the current (turn, round)
    int64_t now_ = 0;
};

}  // namespace learn
}  // namespace dd

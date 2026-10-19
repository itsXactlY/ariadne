#pragma once
// dd/agent/iteration_budget.hpp — thread-safe iteration counter.
// Port of run_agent.py:202 IterationBudget.
//
// Each agent (parent or subagent) gets its own budget. Parent capped at
// max_iterations (default 90); subagents at delegation.max_iterations (50).
// execute_code turns refund() so they don't eat the budget.

#include <mutex>

namespace dd {
namespace agent {

class IterationBudget {
public:
    int max_total;
    int used = 0;
    mutable std::mutex m;

    explicit IterationBudget(int max_total_ = 90) : max_total(max_total_) {}

    // Try to consume one iteration. Returns true if allowed.
    bool consume() {
        std::lock_guard<std::mutex> lk(m);
        if (used >= max_total) return false;
        ++used;
        return true;
    }

    // Give back one iteration (e.g. for execute_code turns).
    void refund() {
        std::lock_guard<std::mutex> lk(m);
        if (used > 0) --used;
    }

    int remaining() const {
        std::lock_guard<std::mutex> lk(m);
        return max_total - used;
    }

    // Copy is disabled (mutex); budgets are shared by pointer/reference.
    IterationBudget(const IterationBudget&) = delete;
    IterationBudget& operator=(const IterationBudget&) = delete;
};

}  // namespace agent
}  // namespace dd

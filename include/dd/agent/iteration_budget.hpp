#pragma once
// dd/agent/iteration_budget.hpp — thread-safe iteration counter.
// Port of run_agent.py:202 IterationBudget.
//
// Each agent (parent or subagent) gets its own budget. Parent capped at
// max_iterations (default 90); subagents at delegation.max_iterations (50).
// execute_code turns refund() so they don't eat the budget.

#include <atomic>

namespace dd {
namespace agent {

class IterationBudget {
public:
    int max_total;
    std::atomic<int> used{0};

    explicit IterationBudget(int max_total_ = 90) : max_total(max_total_) {}

    // Try to consume one iteration. Returns true if allowed.
    bool consume() {
        int u = used.load(std::memory_order_relaxed);
        do {
            if (u >= max_total) return false;
        } while (!used.compare_exchange_weak(u, u + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
        return true;
    }

    // Give back one iteration (e.g. for execute_code turns).
    void refund() {
        int u = used.load(std::memory_order_relaxed);
        while (u > 0 && !used.compare_exchange_weak(u, u - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        }
    }

    int remaining() const { return max_total - used.load(std::memory_order_acquire); }

    // Budgets are shared by pointer/reference, never copied.
    IterationBudget(const IterationBudget&) = delete;
    IterationBudget& operator=(const IterationBudget&) = delete;
};

static_assert(std::atomic<int>::is_always_lock_free, "the budget is a plain atomic counter");

}  // namespace agent
}  // namespace dd

#include "dd/learn/sink.hpp"

#include "dd/core/park.hpp"

namespace dd {
namespace learn {

void QueueSink::emit(know::Event&& e) noexcept {
    const int64_t before = depth_.fetch_add(1, std::memory_order_acq_rel);
    if (before >= cap_) {
        depth_.fetch_sub(1, std::memory_order_acq_rel);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try {
        q_.push(std::move(e));
    } catch (...) {   // out of memory: drop, never throw into the agent loop
        depth_.fetch_sub(1, std::memory_order_acq_rel);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    pushed_.fetch_add(1, std::memory_order_relaxed);
    if (before == 0) {
        seq_.fetch_add(1, std::memory_order_release);
        core::unpark(seq_);
    }
}

bool QueueSink::pop(know::Event& out) {
    if (!q_.pop(out)) return false;
    depth_.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

bool QueueSink::wait(std::chrono::nanoseconds timeout) {
    const uint32_t s = seq_.load(std::memory_order_acquire);
    if (depth_.load(std::memory_order_acquire) > 0) return true;
    core::park(seq_, s, timeout);
    return depth_.load(std::memory_order_acquire) > 0;
}

void QueueSink::wake() {
    seq_.fetch_add(1, std::memory_order_release);
    core::unpark(seq_, 1 << 20);
}

}  // namespace learn
}  // namespace dd

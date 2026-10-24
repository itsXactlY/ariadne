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
    if (wake_pending_.exchange(false, std::memory_order_acquire))
        return depth_.load(std::memory_order_acquire) > 0;   // the wake fired before we got here: end now, report the queue
    core::park(seq_, s, timeout);
    wake_pending_.exchange(false, std::memory_order_acquire);   // consume our own wake so it cannot leak to the next wait
    return depth_.load(std::memory_order_acquire) > 0;
}

void QueueSink::wake() {
    wake_pending_.store(true, std::memory_order_release);   // a consumer that parks after this finds the flag, not a lost wake
    seq_.fetch_add(1, std::memory_order_release);
    core::unpark(seq_, 1 << 20);
}

void QueueSink::note_drain() {
    drained_.fetch_add(1, std::memory_order_release);
    core::unpark(drained_, 1 << 20);
}

bool QueueSink::wait_drain(std::chrono::nanoseconds timeout) {
    const uint32_t seen = drained_.load(std::memory_order_acquire);
    core::park(drained_, seen, timeout);
    return drained_.load(std::memory_order_acquire) != seen;
}

}  // namespace learn
}  // namespace dd

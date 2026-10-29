#pragma once
// dd/learn/sink.hpp — the router's side of the hand-off: one publish, never a wait.
//
// emit() is the agent thread's whole cost: a slot taken, the payload moved into it, one store to
// publish it. `cap` is the ring's own capacity, so the bound is the storage itself — past it the
// publish is refused and counted in dropped(), which is the number the learn status already
// names. The queue this replaced allocated a node per event and grew past any cap that a counter
// enforced; here the slots are allocated once at construction and reused forever. Event is 480
// bytes, so the default 8192-slot ring is 4 MiB, fixed, never grown, and never a leak.
//
// The learn thread claims with pop() (oldest first) and sleeps in wait() — a futex on an atomic
// word, not a lock and not a poll.

#include <atomic>
#include <chrono>
#include <cstdint>

#include "dd/core/slot_ring.hpp"
#include "dd/know/event.hpp"

namespace dd {
namespace learn {

class QueueSink final : public know::EventSink {
public:
    // The cap is the capacity: the ring refuses at it, so no counter has to police the queue.
    explicit QueueSink(size_t cap = 8192) : q_(cap) {}

    void emit(know::Event&& e) noexcept override;   // any thread

    // ---- the learn thread ----
    bool pop(know::Event& out);                     // false: nothing published (a slot being filled is not readable yet)
    bool wait(std::chrono::nanoseconds timeout);    // sleeps until something is queued, wake(), or the timeout; true when queued
    void wake();                                    // for shutdown
    void note_drain();                              // consumer: after a drain pass — wakes anything parked in wait_drain()
    bool wait_drain(std::chrono::nanoseconds timeout);  // park until the next note_drain(), or the timeout; true when noted

    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t pushed() const { return pushed_.load(std::memory_order_relaxed); }
    int64_t depth() const { return depth_.load(std::memory_order_relaxed); }

private:
    core::SlotRing<8192, know::Event> q_;   // ceiling 8192, the runtime cap selects how many slots it admits
    std::atomic<int64_t> depth_{0};
    std::atomic<uint64_t> dropped_{0}, pushed_{0};
    std::atomic<uint32_t> seq_{0};   // bumped on empty -> non-empty and by wake(); the consumer parks on it
    std::atomic<uint32_t> drained_{0};   // bumped by note_drain(); wait_drain parks on it
    std::atomic<bool> wake_pending_{false};   // wake() before the consumer reached wait(): the park would miss it
};

}  // namespace learn
}  // namespace dd

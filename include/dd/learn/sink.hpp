#pragma once
// dd/learn/sink.hpp — the router's side of the queue: one push, never a wait.
//
// emit() is the agent thread's whole cost. Past `cap` events in flight it drops the event and
// counts it. The learn thread drains with pop() and sleeps in wait() (a futex, no polling).

#include <atomic>
#include <chrono>
#include <cstdint>

#include "dd/core/mpsc.hpp"
#include "dd/know/event.hpp"

namespace dd {
namespace learn {

class QueueSink final : public know::EventSink {
public:
    explicit QueueSink(size_t cap = 8192) : cap_(static_cast<int64_t>(cap)) {}

    void emit(know::Event&& e) noexcept override;   // any thread

    // ---- the learn thread ----
    bool pop(know::Event& out);                     // false: empty, or a producer is mid-link (wait() returns at once, try again)
    bool wait(std::chrono::nanoseconds timeout);    // sleeps until something is queued, wake(), or the timeout; true when queued
    void wake();                                    // for shutdown
    void note_drain();                              // consumer: after a drain pass — wakes anything parked in wait_drain()
    bool wait_drain(std::chrono::nanoseconds timeout);  // park until the next note_drain(), or the timeout; true when noted

    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t pushed() const { return pushed_.load(std::memory_order_relaxed); }
    int64_t depth() const { return depth_.load(std::memory_order_relaxed); }

private:
    core::MpscQueue<know::Event> q_;
    const int64_t cap_;
    std::atomic<int64_t> depth_{0};
    std::atomic<uint64_t> dropped_{0}, pushed_{0};
    std::atomic<uint32_t> seq_{0};   // bumped on empty -> non-empty and by wake(); the consumer parks on it
    std::atomic<uint32_t> drained_{0};   // bumped by note_drain(); wait_drain parks on it
    std::atomic<bool> wake_pending_{false};   // wake() before the consumer reached wait(): the park would miss it
};

}  // namespace learn
}  // namespace dd

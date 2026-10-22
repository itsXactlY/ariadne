#pragma once
// dd/core/park.hpp — wait on an atomic word without polling and without a lock.
//
// A thread parks in the kernel (a futex) until another thread changes the word and wakes
// it, or the timeout runs out: no mutex, no condition variable, no wakeups while idle. The
// kernel compares the word with `expected` itself, so a change made just before the call is
// never missed. Wakeups may be early or spurious: callers loop on their own condition.
// Off Linux it degrades to short sleeps.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <ctime>
#else
#include <thread>
#endif

namespace dd {
namespace core {

static_assert(std::atomic<uint32_t>::is_always_lock_free && sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "a futex word is a plain 32-bit integer");

// Returns when `word` no longer holds `expected`, when unparked, or after `timeout`.
inline void park(std::atomic<uint32_t>& word, uint32_t expected, std::chrono::nanoseconds timeout) {
    if (timeout.count() <= 0 || word.load(std::memory_order_acquire) != expected) return;
#if defined(__linux__)
    timespec ts;   // relative, on CLOCK_MONOTONIC: what steady_clock reads
    ts.tv_sec = static_cast<time_t>(timeout.count() / 1000000000);
    ts.tv_nsec = static_cast<long>(timeout.count() % 1000000000);
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAIT_PRIVATE, expected, &ts, nullptr, 0);
#else
    std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(timeout, std::chrono::milliseconds(2)));
#endif
}

// Wakes up to `waiters` threads parked on `word` (call it after changing the word).
inline void unpark(std::atomic<uint32_t>& word, int waiters = 1) {
#if defined(__linux__)
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE_PRIVATE, waiters, nullptr, nullptr, 0);
#else
    (void)word;
    (void)waiters;
#endif
}

}  // namespace core
}  // namespace dd

// dd::core::park — a thread asleep on an atomic word wakes when the word changes and is unparked, never misses a change
// made just before it parks, and wakes up on its own at the timeout.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "dd/core/park.hpp"

using namespace dd::core;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", label.c_str());
    }
}
static double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

static void test_timeout_and_immediate() {
    std::atomic<uint32_t> w{0};
    auto t0 = Clock::now();
    park(w, 0, milliseconds(60));
    double ms = ms_since(t0);
    chk(ms >= 55 && ms < 400, "timeout: parks for about the timeout (" + std::to_string(ms) + " ms)");

    w = 1;
    t0 = Clock::now();
    park(w, 0, std::chrono::seconds(10));   // the word already changed: no sleep at all
    ms = ms_since(t0);
    chk(ms < 50, "immediate: a change made before the call is not slept through (" + std::to_string(ms) + " ms)");

    t0 = Clock::now();
    park(w, 1, std::chrono::nanoseconds(0));
    chk(ms_since(t0) < 50, "a zero timeout returns at once");
}

static void test_wake_latency() {
    std::atomic<uint32_t> w{0};
    std::atomic<double> woke_after{-1};
    std::atomic<bool> parked{false};
    Clock::time_point signalled;
    std::thread waiter([&] {
        parked = true;
        while (w.load(std::memory_order_acquire) == 0) park(w, 0, std::chrono::seconds(10));
        woke_after = ms_since(signalled);
    });
    while (!parked) std::this_thread::yield();
    std::this_thread::sleep_for(milliseconds(40));   // let it fall asleep
    signalled = Clock::now();
    w.store(1, std::memory_order_release);
    unpark(w);
    waiter.join();
    chk(woke_after.load() >= 0 && woke_after.load() < 25, "wake: an unpark wakes the sleeper at once (" + std::to_string(woke_after.load()) + " ms)");
}

static void test_ping_pong() {
    // two threads hand a token back and forth through one word: a missed wakeup would cost a whole timeout
    std::atomic<uint32_t> turn{0};
    const int rounds = 30000;
    auto side = [&](uint32_t me) {
        for (int i = 0; i < rounds; ++i) {
            while (turn.load(std::memory_order_acquire) != me) park(turn, 1 - me, std::chrono::seconds(2));
            turn.store(1 - me, std::memory_order_release);
            unpark(turn);
        }
    };
    const auto t0 = Clock::now();
    std::thread a(side, 0), b(side, 1);
    a.join();
    b.join();
    const double ms = ms_since(t0);
    chk(ms < 5000, "ping-pong: " + std::to_string(2 * rounds) + " hand-offs without a lost wakeup (" + std::to_string(ms) + " ms)");
}

int main() {
    test_timeout_and_immediate();
    test_wake_latency();
    test_ping_pong();
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all park tests passed\n");
    return g_fail ? 1 : 0;
}

// tests/relay_policy_test.cpp — the relay's two wait policies, which no live socket can
// show: a peer that stops mid-frame, and a session that ends while a worker is answering.
//
// Both used to be wrong in the same direction — wait forever. A relay that delivered half
// a request parked the socket thread past its deadline and never answered the next request;
// a worker whose session had gone spun on a SlotRing nobody drained any more, so every
// relayed request leaked a thread until the process died.

#include "dd/gateway/relay.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace dd;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
}

int main() {
    const auto now = Clock::now();

    // A frame that stopped arriving ends the request at the deadline, partial or not.
    const auto past = now - std::chrono::milliseconds(1);
    chk(gateway::relay_detail::again_policy(past, now, 0).give_up, "an empty frame past the deadline is dropped");
    chk(gateway::relay_detail::again_policy(past, now, 4096).give_up,
        "a half-delivered frame past the deadline is dropped, not held open");

    // Inside the deadline it waits, bounded to a poll slice, so a stalled socket is checked
    // again instead of sleeping the whole remaining time.
    const auto ahead = now + std::chrono::seconds(30);
    const auto inside = gateway::relay_detail::again_policy(ahead, now, 8);
    chk(!inside.give_up, "a frame inside the deadline keeps going");
    chk(inside.wait_ms > 0 && inside.wait_ms <= 250, "the wait is one bounded slice");
    chk(gateway::relay_detail::again_policy(now + std::chrono::milliseconds(50), now, 0).wait_ms <= 50,
        "the wait never outlives the deadline");

    // A peer that streams past the cap is cut off, however fast it sends.
    chk(gateway::relay_detail::again_policy(ahead, now, gateway::relay_detail::kMaxRelayMessage).give_up,
        "a message over the cap is dropped");

    // A reply for a session that already ended is dropped, promptly: the ring has no drainer.
    gateway::relay_detail::Outbox out;
    out.dead.store(1);
    std::atomic<int> stop{0};
    const auto t0 = Clock::now();
    chk(!gateway::relay_detail::publish_reply(out, "{\"corr_id\":\"a\"}", stop, 5000),
        "a reply to a dead session is dropped");
    chk(Clock::now() - t0 < std::chrono::milliseconds(500), "dropping does not wait out the timeout");

    // Stopping drops the same way.
    gateway::relay_detail::Outbox alive;
    std::atomic<int> halting{1};
    chk(!gateway::relay_detail::publish_reply(alive, "{}", halting, 5000), "a reply is dropped when the client is stopping");

    // With a live session a reply lands, and a full ring drains into the slot the socket
    // thread is reading -- waiting is allowed only while someone can take it.
    gateway::relay_detail::Outbox live;
    std::atomic<int> running{0};
    chk(gateway::relay_detail::publish_reply(live, "{\"corr_id\":\"one\"}", running, 1000), "a reply is published");
    std::string frame;
    chk(live.ring.claim_one(frame) && frame == "{\"corr_id\":\"one\"}", "the socket thread reads the reply it published");

    // Fill the ring, then end the session mid-publish: the worker must not spin forever.
    gateway::relay_detail::Outbox full;
    for (int i = 0; i < 64; ++i)
        chk(full.ring.publish_any("x" + std::to_string(i)), "the ring accepts its slots");
    std::atomic<int> still{0};
    std::thread waiter([&] { chk(!gateway::relay_detail::publish_reply(full, "late", still, 4000),
                                "a reply to a full ring waits while the session lives"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    full.dead.store(1);   // the session ends: the waiter gives up instead of leaking
    waiter.join();

    std::printf("%s: relay wait policies (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}

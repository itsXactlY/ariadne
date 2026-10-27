// dd::core::SlotRing — a payload published by one thread is claimed by exactly one of several
// racing claimers, none is lost, none is read twice, and a drain comes back in publish order.
#include <atomic>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dd/core/slot_ring.hpp"

using namespace dd::core;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", label.c_str());
    }
}

// The payload carries its publish index ("p<i>" / "m<i>"), and the ring stamps from 1 in the
// same order, so comparing indices is comparing stamps.
static uint64_t idx(const std::string& t) { return std::stoull(t.substr(1)); }

// Slots wrap: slot 0 can hold a payload older than one in slot 3. A drain must sort by the
// stamp, not by the slot index — otherwise a mid-turn redirect would enter the turn behind a
// prompt typed after it.
static void test_order_across_wrap() {
    SlotRing<4> ring;
    std::vector<uint64_t> seen;
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < 3; ++i) chk(ring.publish("p" + std::to_string(round * 3 + i)), "publish a free slot");
        for (auto& t : ring.claim_all()) seen.push_back(idx(t));
    }
    bool ascending = true;
    for (size_t i = 1; i < seen.size(); ++i)
        if (seen[i] <= seen[i - 1]) ascending = false;
    chk(seen.size() == 18, "every publish is claimed (18, saw " + std::to_string(seen.size()) + ")");
    chk(ascending, "a drain comes back in publish order across the wrap");
    chk(ring.published() == 0, "an emptied ring publishes nothing");
}

// The oldest payload is the one a loop that sends one prompt per turn must take, and the stamp
// it pinned is the one it reports.
static void test_claim_one_is_oldest() {
    SlotRing<4> ring;
    ring.publish("first");
    ring.publish("second");
    ring.publish("third");
    chk(ring.published() == 3, "three slots hold");
    uint64_t st = 0;
    chk(ring.claim_one(&st) == "first" && st == 1, "claim_one takes the oldest stamp and names it");
    chk(ring.claim_one(&st) == "second" && st == 2, "then the next oldest");
    chk(ring.claim_one(&st) == "third" && st == 3, "then the newest");
    chk(ring.claim_one(&st).empty(), "nothing published gives nothing, not a stale payload");
}

// A full ring is a fact, not a drop: publish says so and leaves what it holds alone.
static void test_full_ring_refuses() {
    SlotRing<3> ring;
    chk(ring.publish("a") && ring.publish("b") && ring.publish("c"), "fill the ring");
    chk(!ring.publish("d"), "a full ring refuses rather than overwriting an unread payload");
    chk(ring.published() == 3, "and says how full it is");
    chk(ring.claim_all()[0] == "a", "the payloads it kept are intact and in order");
    chk(ring.publish("d"), "one claim frees a slot for the next publish");
}

// The claim, not the queue: every payload lands in exactly one consumer. One thread claims
// everything at a boundary, one claims one at a time — the two shapes dd_tui uses — so both
// race against the writer and against each other. Ownership is what this proves; order is
// asserted only where the primitive can promise it (see the drain below and the quiescent
// drain after the writer stops).
static void test_racing_claimers() {
    constexpr int kTotal = 20000;
    SlotRing<8> ring;
    std::atomic<bool> done{false};
    std::vector<std::string> bulk, singles;
    std::vector<uint64_t> single_stamps;
    std::vector<std::vector<uint64_t>> drains;   // one entry per claim_all call
    bool drain_sorted = true;

    std::thread writer([&] {
        for (int i = 0; i < kTotal; ++i) {
            // The writer's side of the rule: a full ring is retried, never overwritten.
            while (!ring.publish("m" + std::to_string(i))) std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });
    // Both claimers run until the writer is done, then drain what it left. A claimer that
    // stops as soon as the ring looks empty would leave the writer spinning against a full
    // ring with nobody to free a slot.
    std::thread bulk_claimer([&] {
        while (!done.load(std::memory_order_acquire)) {
            auto got = ring.claim_all();
            std::vector<uint64_t> stamps;
            for (auto& t : got) stamps.push_back(idx(t));
            drains.push_back(std::move(stamps));
            for (auto& t : got) bulk.push_back(std::move(t));
        }
        auto got = ring.claim_all();
        std::vector<uint64_t> stamps;
        for (auto& t : got) stamps.push_back(idx(t));
        drains.push_back(std::move(stamps));
        for (auto& t : got) bulk.push_back(std::move(t));
    });
    std::thread single_claimer([&] {
        while (!done.load(std::memory_order_acquire)) {
            uint64_t st = 0;
            auto t = ring.claim_one(&st);
            if (t.empty()) { std::this_thread::yield(); continue; }
            single_stamps.push_back(st);
            singles.push_back(std::move(t));
        }
        for (;;) {
            uint64_t st = 0;
            auto t = ring.claim_one(&st);
            if (t.empty()) break;
            single_stamps.push_back(st);
            singles.push_back(std::move(t));
        }
    });
    writer.join();
    bulk_claimer.join();
    single_claimer.join();

    // What a drain promises: the payloads of one claim_all call come back in the order they
    // were published, because the sort uses the stamps the call pinned. What it cannot promise
    // is order *across* calls while the writer races it — a payload published into a slot the
    // scan already walked belongs to the next call, at a stamp older than that call's newest.
    for (const auto& d : drains)
        for (size_t i = 1; i < d.size(); ++i)
            if (d[i] <= d[i - 1]) drain_sorted = false;
    chk(drain_sorted, "each drain of everything is in publish order while the writer races it");

    // The limit, measured rather than asserted: how far out of publish order the two claimers
    // interleaved. dd_tui's boundary takes one drain, so this counts nothing there.
    size_t inversions = 0;
    for (size_t i = 1; i < single_stamps.size(); ++i)
        if (single_stamps[i] <= single_stamps[i - 1]) ++inversions;
    std::printf("  cross-claim inversions while racing: %zu over %zu claims of one, %zu drains\n",
                inversions, single_stamps.size(), drains.size());

    std::set<std::string> all;
    all.insert(bulk.begin(), bulk.end());
    all.insert(singles.begin(), singles.end());
    chk(bulk.size() + singles.size() == static_cast<size_t>(kTotal),
        "nothing was lost (" + std::to_string(bulk.size() + singles.size()) + " of " + std::to_string(kTotal) + ")");
    chk(all.size() == static_cast<size_t>(kTotal), "nothing was claimed twice");
    chk(ring.published() == 0, "the ring is empty when both claimers finish");
}

// With the writer quiescent, a drain is publish order even when the slots wrapped mid-race:
// the payloads two claimers left behind are still ordered by the stamps they carry.
static void test_quiescent_drain_is_ordered_after_a_race() {
    constexpr int kTotal = 5000;
    SlotRing<8> ring;
    std::atomic<bool> done{false};
    std::vector<std::string> raced;
    std::thread writer([&] {
        for (int i = 0; i < kTotal; ++i) { while (!ring.publish("m" + std::to_string(i))) std::this_thread::yield(); }
        done.store(true, std::memory_order_release);
    });
    std::thread claimer([&] {
        while (!done.load(std::memory_order_acquire)) { auto t = ring.claim_one(); if (!t.empty()) raced.push_back(std::move(t)); else std::this_thread::yield(); }
    });
    writer.join(); claimer.join();
    auto left = ring.claim_all();
    bool ordered = true;
    for (size_t i = 1; i < left.size(); ++i)
        if (idx(left[i]) <= idx(left[i - 1])) ordered = false;
    chk(ordered, "what the race left behind drains in publish order (" + std::to_string(left.size()) + " payloads)");
    chk(raced.size() + left.size() == static_cast<size_t>(kTotal), "and it is exactly what was not claimed yet");
}

// The shape the primitive cannot promise, stated as a test so the limitation is measured
// rather than assumed: with two claimers draining while the writer publishes, a payload can
// enter the slower claimer's *next* claim rather than its current one, because the scan reads
// one slot at a time. Ownership still holds, which is what the turn boundary needs.
static void test_two_claimers_hand_each_payload_once() {
    constexpr int kTotal = 8000;
    SlotRing<4> ring;
    std::atomic<bool> done{false};
    std::vector<std::string> a, b;
    std::thread writer([&] {
        for (int i = 0; i < kTotal; ++i) { while (!ring.publish("m" + std::to_string(i))) std::this_thread::yield(); }
        done.store(true, std::memory_order_release);
    });
    std::thread claimer_a([&] {
        while (!done.load(std::memory_order_acquire)) { auto t = ring.claim_one(); if (!t.empty()) a.push_back(std::move(t)); else std::this_thread::yield(); }
        for (;;) { auto t = ring.claim_one(); if (t.empty()) break; a.push_back(std::move(t)); }
    });
    std::thread claimer_b([&] {
        while (!done.load(std::memory_order_acquire)) { auto t = ring.claim_one(); if (!t.empty()) b.push_back(std::move(t)); else std::this_thread::yield(); }
        for (;;) { auto t = ring.claim_one(); if (t.empty()) break; b.push_back(std::move(t)); }
    });
    writer.join(); claimer_a.join(); claimer_b.join();
    std::set<std::string> all;
    all.insert(a.begin(), a.end());
    all.insert(b.begin(), b.end());
    chk(a.size() + b.size() == static_cast<size_t>(kTotal), "each of the two claimers took what it pinned, none lost");
    chk(all.size() == static_cast<size_t>(kTotal), "and no payload crossed to both");
}

int main() {
    test_order_across_wrap();
    test_claim_one_is_oldest();
    test_full_ring_refuses();
    test_racing_claimers();
    test_two_claimers_hand_each_payload_once();
    test_quiescent_drain_is_ordered_after_a_race();
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all slot ring tests passed\n");
    return g_fail ? 1 : 0;
}

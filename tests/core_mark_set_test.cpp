// tests/core_mark_set_test.cpp — falsifies MarkSet: the two answers the gateway reads on every
// encrypted request ("this nonce was already answered", "this session was killed"), under a bound.
//
// The bug this exists to kill: nonce protection was an unordered_set counted FIFO, so 1024 later
// handshakes pushed out a session kill that still mattered — the killed session went on decrypting
// while `kill` had answered "destroyed", and the word in the code said "bounded" while memory grew
// without limit. The fix is one atomic word per slot holding digest *and* expiry, so the slot that
// stops mattering soonest is the one taken, and displacement never happens outside the chain the
// insert just walked — a lookup in flight must never stop early and miss the mark it was hunting.
//
// No clock is faked by the object: every call is handed `now`, so time is a parameter here.
#include "dd/core/mark_set.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using dd::core::MarkSet;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (cond) { std::printf("  ok   %s\n", what); }
    else { std::printf("  FAIL %s\n", what); ++failures; }
}

static const long long kBase = 1'700'000'000LL;

static uint64_t digest(int i) { return MarkSet::digest_of(0x1000000000000ULL + static_cast<uint64_t>(i) * 7919ULL); }

// ---- what a mark means ----
static void test_a_mark_means_what_it_says() {
    MarkSet m(16, kBase);
    check(m.insert(digest(1), kBase + 120, kBase) == MarkSet::Outcome::kNew, "a handshake is recorded");
    check(m.contains(digest(1)), "a recorded handshake is refused, expired or not");
    check(m.live(digest(1), kBase + 119), "a mark still matters one second before its expiry");
    check(!m.live(digest(1), kBase + 120), "a mark stops mattering at its expiry, not after");
    check(!m.contains(digest(2)), "an unrecorded handshake is not a replay");

    check(m.insert(digest(1), kBase + 400, kBase) == MarkSet::Outcome::kAlready,
          "a second mark of the same digest says it was already there");
    check(m.size() == 1, "and it does not become a second mark");

    // A digest of zero is the empty marker, so a mark of zero must still be recorded — a nonce that
    // hashed to nothing may not be answered twice.
    check(m.insert(0, kBase + 60, kBase) == MarkSet::Outcome::kNew, "a zero digest is still a mark");
    check(m.contains(0) && m.live(0, kBase + 59), "and it is refused while it matters");
    check(m.size() == 2, "and it occupies a slot");

    // An expiry at or behind the base means "stops mattering now": offset 0, the first thing
    // displaced — but the word is nonzero, so it is still recorded.
    check(m.insert(digest(3), kBase, kBase) == MarkSet::Outcome::kNew, "an expiry at the base is recorded");
    check(m.contains(digest(3)) && !m.live(digest(3), kBase), "and it already matters nothing");
}

// ---- the bound is the table, and it never grows ----
static void test_the_bound_is_the_table() {
    const size_t cap = 8;
    MarkSet m(cap, kBase);
    check(m.slots() == 2 * cap, "the table is twice the target occupancy, allocated once");

    // Past the target occupancy a mark is still recorded: filling the table is strictly better than
    // dropping something that matters, and the table is the hard bound.
    for (int i = 0; i < 2 * static_cast<int>(cap); ++i)
        check(m.insert(digest(i), kBase + 120, kBase) == MarkSet::Outcome::kNew,
              "a mark past the target occupancy is recorded, not dropped");
    check(m.size() == 2 * cap, "occupancy reached the table and stopped there");
    for (int i = 0; i < 2 * static_cast<int>(cap); ++i) check(m.contains(digest(i)), "every mark is still findable");

    // A full table displaces: the mark that already matters nothing goes first.
    check(m.insert(digest(99), kBase + 120, kBase) == MarkSet::Outcome::kDisplacedLive,
          "a full table displaces a mark that still mattered, and says so");
    check(m.size() == 2 * cap, "and occupancy never exceeded the table");
    check(m.contains(digest(99)), "the new mark is held");
}

// ---- the oldest mark that still matters is never the one lost ----
static void test_the_longest_lived_mark_survives() {
    MarkSet m(4, kBase);   // table of 8
    check(m.insert(digest(0), kBase + 3600, kBase) == MarkSet::Outcome::kNew, "a session kill is recorded");
    for (int i = 1; i <= 7; ++i) m.insert(digest(i), kBase + 300, kBase);   // fills the table
    check(m.size() == 8, "the table is full");

    // The newest handshake displaces the shortest-lived mark in its own chain — never the kill.
    const MarkSet::Outcome o = m.insert(digest(8), kBase + 200, kBase);
    check(o == MarkSet::Outcome::kDisplacedLive, "displacement happened and was reported");
    check(m.live(digest(0), kBase + 100), "the session the operator killed survived, so it stays refused");
    check(m.contains(digest(8)), "the newest mark is held");
    check(m.size() == 8, "and no slot leaked");

    // Displacement only ever takes the mark that expires soonest among those this chain can see, so
    // pushing the kill out would need 7 marks that matter even less than an hour.
    for (int i = 9; i < 60; ++i) m.insert(digest(i), kBase + 200, kBase);
    check(m.live(digest(0), kBase + 100), "an hour of handshakes never resurrected the killed session");

    // A mark whose expiry has already passed matters nothing, so it is the first thing pushed out.
    check(m.live(digest(0), kBase + 3599), "the kill mattered up to its expiry");
    m.insert(digest(60), kBase, kBase);   // offset 0: recorded, matters nothing
    const MarkSet::Outcome gone = m.insert(digest(61), kBase + 300, kBase);
    check(gone == MarkSet::Outcome::kNew, "the mark that already matters nothing is displaced, not a live one");
    check(!m.contains(digest(60)), "and it is gone");
    check(m.live(digest(0), kBase + 3599), "the kill survived a displacement that cost nothing");
}

// ---- displacement stays inside the chain, so a lookup keeps its bound ----
static void test_displacement_never_opens_a_gap_in_a_chain() {
    MarkSet m(4, kBase);   // table of 8
    for (int i = 0; i < 8; ++i) m.insert(digest(i), kBase + 120 + i, kBase);
    // Fill, then push: every displacement is inside the digest's own chain, so the other marks stay
    // findable and no reader's walk can stop at a gap that a write opened mid-read.
    for (int i = 8; i < 200; ++i) m.insert(digest(i), kBase + 300 + (i % 40), kBase);
    check(m.size() == 8, "occupancy stayed at the table");

    int findable = 0;
    for (int i = 0; i < 200; ++i) if (m.contains(digest(i))) ++findable;
    check(findable >= 8, "the marks the table still holds are all findable");
    check(findable <= 8, "and nothing beyond the table is claimed");
}

// ---- the shape of the answer, not just its content ----
static void test_outcomes_are_distinguished() {
    MarkSet m(4, kBase);
    check(m.insert(digest(1), kBase + 120, kBase) == MarkSet::Outcome::kNew, "first mark: kNew");
    check(m.insert(digest(1), kBase + 120, kBase) == MarkSet::Outcome::kAlready,
          "same digest again: kAlready, so a replay is not counted as a fresh refusal");
    check(m.insert(digest(1), kBase + 120, kBase) == MarkSet::Outcome::kAlready, "and again");
    for (int i = 2; i <= 8; ++i) m.insert(digest(i), kBase + 120, kBase);   // fills the table of 8
    check(m.insert(digest(9), kBase + 120, kBase) == MarkSet::Outcome::kDisplacedLive,
          "a full table: kDisplacedLive, the caller's cue to say what the record cost");
}

// ---- concurrent readers and writers ----
static void test_concurrent_readers_never_miss_a_live_mark() {
    MarkSet m(64, kBase);   // table of 128
    std::atomic<bool> stop{false};
    std::atomic<int> held{0}, already{0}, displaced{0}, held_out{0}, missed{0};

    // Writers: threads insert handshakes and kills, readers: threads refuse and decrypt.
    std::vector<std::thread> writers;
    for (int t = 0; t < 8; ++t) {
        writers.emplace_back([&] {
           for (int i = 0; i < 500; ++i) {
                // Every writer works the same 500 digests, so two threads reach the same mark:
                // one records it, the other must be told it was already there.
                const uint64_t d = digest(i);
                const MarkSet::Outcome o = m.insert(d, kBase + 60 + (i % 200), kBase);
                switch (o) {
                    case MarkSet::Outcome::kNew: ++held; break;
                    case MarkSet::Outcome::kAlready: ++already; break;
                    case MarkSet::Outcome::kDisplacedLive: ++displaced; break;
                    case MarkSet::Outcome::kHeld: ++held_out; break;
                }
            }
        });
    }
   std::vector<std::thread> readers;
   for (int t = 0; t < 4; ++t) {
       readers.emplace_back([&, t] {
            // A reader records its own digest and immediately looks it up. A miss is legitimate
            // only if somebody displaced that mark — a displacement is the only thing that can
            // empty a slot, and displacement never happens outside the chain being written, so a
            // reader can never stop early at a gap a write opened mid-read.
            for (long long n = 0; n < 200'000; ++n) {
                const uint64_t d = digest(40000 + t * 200'000 + static_cast<uint64_t>(n));
                const MarkSet::Outcome o = m.insert(d, kBase + 400 + static_cast<long long>(n % 5), kBase);
                if (o == MarkSet::Outcome::kNew && !m.contains(d)) ++missed;
                m.live(d, kBase);
            }
           stop.store(true);
       });
   }
    for (auto& w : writers) w.join();
    for (auto& r : readers) r.join();

    // A saturated table can lose the displacement race and answer kHeld. That is the honest cost of
    // a bound with no lock: the caller fails closed on it (a handshake refused, a kill logged). It
    // must stay a handful, not a stream — otherwise the bound is a refusal generator.
    check(m.size() == m.slots() && held_out < 8,
          "only a saturated table refuses, and it refuses a handful, never a stream");
    check(already > 0, "overlapping digests were recognized as already marked");
    check(displaced > 0, "the table filled and displaced, as a bound must");
    check(m.size() <= m.slots(), "occupancy never exceeded the table under contention");
    // Every miss must be paid for by a displacement: that is the only way a mark can vanish.
    check(missed <= displaced, "a reader missed only marks that were displaced, never a live mark");
    std::printf("  info held=%d already=%d displaced=%d held_out=%d missed=%d size=%zu/%zu\n",
                held.load(), already.load(), displaced.load(), held_out.load(), missed.load(), m.size(), m.slots());
}

// ---- a gateway that stays up ----
static void test_a_gateway_up_for_years_still_refuses_the_right_things() {
    MarkSet m(16, kBase);
    // An offset measured from a base fixed at construction runs out once uptime passes its range,
    // and running out clamps to the largest offset, which reads as "live forever": every nonce
    // would look replayed and every handshake would be refused. The word names the base it was
    // written against, so the clock moving forward changes no answer.
    const long long years = 3LL * 365 * 86400;
    check(MarkSet::kMaxLifeS < years, "the offset range is shorter than three years of uptime");

    check(m.insert(digest(1), kBase + 86400, kBase) == MarkSet::Outcome::kNew, "a mark from the first day");
    check(!m.live(digest(1), kBase + 86400), "and it stopped mattering after a day");

    const long long far = kBase + years;
    check(m.insert(digest(2), far + 300, far) == MarkSet::Outcome::kNew, "a mark from year three is recorded");
    check(m.live(digest(2), far + 299), "and it matters inside its skew window");
    check(!m.live(digest(2), far + 300), "and it stops at its expiry, years from the base");
    check(!m.live(digest(1), far - 1), "the first mark is not revived by the new base");
    check(m.contains(digest(1)), "and it is still recorded, expired or not");

    // A session kill written after the move still lives its full TTL, and an hour of later
    // handshakes still cannot push it out.
    check(m.insert(digest(3), far + 86400, far) == MarkSet::Outcome::kNew, "a kill at year three is recorded");
    for (int i = 4; i < 20; ++i) m.insert(digest(i), far + 300, far);
    check(m.live(digest(3), far + 86399), "the kill still matters a second before its TTL ends");
    check(!m.live(digest(3), far + 86400), "and stops at the TTL, not past it");
}

int main() {
    std::printf("MarkSet — bounded, lock-free, expiry decides what is displaced\n");
    test_a_gateway_up_for_years_still_refuses_the_right_things();
    test_a_mark_means_what_it_says();
    test_the_bound_is_the_table();
    test_the_longest_lived_mark_survives();
    test_displacement_never_opens_a_gap_in_a_chain();
    test_outcomes_are_distinguished();
    test_concurrent_readers_never_miss_a_live_mark();
    if (failures) { std::printf("%d FAILED\n", failures); return 1; }
    std::printf("all passed\n");
    return 0;
}

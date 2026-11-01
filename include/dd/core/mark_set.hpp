#pragma once
// dd/core/mark_set.hpp — a bounded membership set that any thread may read and add to without a lock.
//
// It holds a *mark*: a digest of a thing, never the thing itself. The gateway needs two answers that
// are race-proof, and both are read on every encrypted request while being written by whichever
// thread happens to be serving a handshake or a kill: "this handshake nonce was already answered",
// and "this session was killed". A mutex there puts a userspace lock on the request path — a reader
// preempted inside the lock stalls every thread behind it — and the operator's rule (§3.6 of the
// gateway plan) is no mutex unless 100% needed. It is not needed: one atomic word per slot carries
// both the digest and the expiry that bounds it, and the CAS is the ownership, exactly as SlotRing's
// claim is.
//
// Slot word: 0 = empty. Otherwise digest in bits 0..43 (never zero), an expiry offset in bits 44..61,
// and a generation in bits 62..63. The offset is whole *minutes* past that generation's base, rounded
// UP, so a stored expiry never claims to matter for less than the caller promised: a minute of
// rounding refuses a replay a little longer than the skew window, and keeps a killed session refused
// a little longer than its TTL. At 18 bits that is 4472 days of headroom — a gateway's session TTL
// (86400 s by default) and its skew window (300 s) both fit with room to spare.
//
// The generation is why a long-running gateway cannot rot. An offset measured from a base fixed at
// construction saturates once uptime passes the offset field, and a saturated offset clamps to the
// largest value, which reads as "live forever": every nonce would look replayed and every handshake
// would be refused. So `insert` hands the clock forward: past half the offset range it publishes a
// new base under the next generation and every write from then on is measured from it. Old words
// stay readable, because a word names the base it was written against — a reader never has to guess,
// and a rebase changes no answer. Generations are reused only after four hops, far past any word's
// life, and a stale word can only ever refuse, never admit.
//
// A lookup probes from digest % slots and stops at an empty word or a match; the walk wraps at the
// table size, so it always terminates. Insert never empties a slot, and never writes outside the
// chain it just walked: it takes an empty slot inside the chain, or CASes the new word over the
// soonest-expiring occupied one inside the same chain. Both keep the chain contiguous, which is what
// lets a lookup in flight keep its bound — an eviction can only lengthen a chain, never shorten it,
// so a reader can never stop early and miss the mark it was hunting.
//
// Displacing the soonest-expiring mark in the chain is what makes the bound honest: a mark that
// matters longer than the one being recorded is never the one displaced, unless the chain has no
// empty slot left and bounded memory forces the choice. That is what a FIFO count over an
// unordered_set had backwards — 1024 later marks pushed the first out whatever it still mattered
// for, so a session the operator had killed went on decrypting while `kill` had answered
// "destroyed". When displacement does cost a mark that still matters, insert says so.
//
// `cap` is the steady-state occupancy a caller asked for; the table of 2*cap words is the hard
// bound, allocated once, and nothing in the steady state allocates at all. Past `cap` an empty slot
// is still used, so the table drifts toward full rather than dropping a mark that matters. The set
// owns no clock — every operation is handed `now`, so a test drives time itself, including years of
// uptime. `insert` answers with an Outcome: "already there" and "could not be held" are different
// facts, and a caller must not treat them alike — refusing a handshake on a full table is
// fail-closed, while dropping a kill mark that still matters is a lie the operator hears.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dd::core {

class MarkSet {
public:
    static constexpr uint64_t kDigestBits = 44;
    static constexpr uint64_t kDigestMask = (1ULL << kDigestBits) - 1;
    static constexpr uint64_t kOffsetBits = 18;
    static constexpr uint64_t kMaxOffset = (1ULL << kOffsetBits) - 1;   // whole minutes
    static constexpr long long kUnitS = 60;                             // the offset's unit, rounded up
    static constexpr uint64_t kGenerations = 4;                         // bits 62..63
    // The longest life a mark can promise, in seconds: the offset field times its unit. Past half
    // of it a write moves the base forward, so saturation can never read as "live forever".
    static constexpr long long kMaxLifeS = static_cast<long long>(kMaxOffset) * kUnitS;

    enum class Outcome {
        kNew,           // recorded, until expires_at
        kAlready,       // it was there already: a replay, or a kill of an already-killed session
        kDisplacedLive, // recorded, but a mark that still mattered at `now` was pushed out
        kHeld,          // no word could be taken this pass: nothing was recorded
    };

    // `base` is the unix second offsets are measured from. Marks past the offset range clamp at the
    // largest offset, which errs toward refusing: a mark that would overflow reads as live.
    MarkSet(size_t cap, long long base) : cap_(cap ? cap : 1), slots_(pow2_at_least(2 * cap_)) {
        cells_.reset(new std::atomic<uint64_t>[slots_]);
        for (size_t i = 0; i < slots_; ++i) cells_[i].store(0, std::memory_order_relaxed);
        bases_[0].store(base, std::memory_order_release);
        for (uint64_t g = 1; g < kGenerations; ++g) bases_[g].store(base, std::memory_order_release);
    }
    MarkSet(const MarkSet&) = delete;
    MarkSet& operator=(const MarkSet&) = delete;

    // A digest for marking: masked to the digest field and never zero, since zero means empty.
    static uint64_t digest_of(uint64_t h) {
        const uint64_t d = h & kDigestMask;
        return d ? d : (0x9e3779b97f4a7c15ULL & kDigestMask);   // a fixed nonzero mark
    }

    // Records `digest` until `expires_at`. kDisplacedLive is the caller's cue to say what the
    // record cost; kHeld means nothing was recorded, so a caller must fail closed on it.
    Outcome insert(uint64_t digest, long long expires_at, long long now) {
        const uint64_t d = digest_of(digest);
        // The base this write is measured from is published before any word carries its generation,
        // so a reader that finds that word always has the base that produced it.
        if (now - bases_[gen_.load(std::memory_order_acquire)].load(std::memory_order_acquire) >
            kMaxLifeS / 2) {
            const uint64_t next = (gen_.load(std::memory_order_relaxed) + 1) & (kGenerations - 1);
            bases_[next].store(now, std::memory_order_release);
            gen_.store(next, std::memory_order_release);
        }
        const uint64_t g = gen_.load(std::memory_order_acquire);
        const long long base = bases_[g].load(std::memory_order_acquire);
        const uint64_t word = word_of(d, offset_for(expires_at, base), g);
        for (int attempt = 0; attempt < 256; ++attempt) {
            const size_t start = d & (slots_ - 1);
            size_t empty = slots_;
            size_t soonest = slots_;
            uint64_t soonest_word = 0;
            uint64_t soonest_offset = kMaxOffset + 1;
            for (size_t i = 0; i < slots_; ++i) {
                const size_t s = (start + i) & (slots_ - 1);
                const uint64_t w = cells_[s].load(std::memory_order_acquire);
                if (w == 0) { empty = s; break; }
                if (digest_of_w(w) == d) return Outcome::kAlready;
                // The soonest-expiring mark this chain can see, kept while walking: displacement
                // may pick only from here, so the chain stays contiguous for every lookup.
                const uint64_t off = offset_of_w(w);
                if (off < soonest_offset) { soonest_offset = off; soonest = s; soonest_word = w; }
            }
            const size_t occupied = count_.load(std::memory_order_relaxed);
            // An empty slot inside the chain is taken while the table has room, even past `cap`:
            // the hard bound is the table (allocated once), so filling it is strictly better than
            // dropping a mark that matters. Displacement is the full-table path.
            if (occupied < slots_ && empty != slots_) {
                uint64_t expect = 0;   // compare_exchange takes the expected value by reference
                if (cells_[empty].compare_exchange_strong(expect, word, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed)) {
                    count_.fetch_add(1, std::memory_order_relaxed);
                    return Outcome::kNew;
                }
                continue;   // somebody took that slot first: walk again; this digest may now be there
            }
            if (soonest == slots_) continue;   // nothing occupied in the chain: retry, then report
            uint64_t expect = soonest_word;
            if (cells_[soonest].compare_exchange_strong(expect, word, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
                // Did the displaced mark still matter at the caller's clock? Then it is the
                // caller's to announce: a lost kill is a lie, not a detail. Its own generation's
                // base is what its expiry is measured from, not this write's.
                const long long then = bases_[gen_of_w(soonest_word)].load(std::memory_order_acquire);
                if (then + static_cast<long long>(offset_of_w(soonest_word)) * kUnitS > now)
                    return Outcome::kDisplacedLive;
                return Outcome::kNew;
            }
        }
        return Outcome::kHeld;
    }

    // Recorded at all, expired or not. Never waits, never allocates.
    bool contains(uint64_t digest) const {
        const uint64_t d = digest_of(digest);
        const size_t start = d & (slots_ - 1);
        for (size_t i = 0; i < slots_; ++i) {
            const uint64_t w = cells_[(start + i) & (slots_ - 1)].load(std::memory_order_acquire);
            if (w == 0) return false;
            if (digest_of_w(w) == d) return true;
        }
        return false;
    }

    // Recorded and still mattering at `now`. The word names the base it was written against, so a
    // rebase in another thread cannot change this answer.
    bool live(uint64_t digest, long long now) const {
        const uint64_t d = digest_of(digest);
        const size_t start = d & (slots_ - 1);
        for (size_t i = 0; i < slots_; ++i) {
            const uint64_t w = cells_[(start + i) & (slots_ - 1)].load(std::memory_order_acquire);
            if (w == 0) return false;
            if (digest_of_w(w) == d) {
                const long long base = bases_[gen_of_w(w)].load(std::memory_order_acquire);
                return base + static_cast<long long>(offset_of_w(w)) * kUnitS > now;
            }
        }
        return false;
    }

    // Slots occupied — a high-water count, since marks age out by expiry and are displaced, never
    // erased. It stops at the table size, which is the point of the whole structure.
    size_t size() const { return count_.load(std::memory_order_relaxed); }
    size_t capacity() const { return cap_; }   // the target occupancy
    size_t slots() const { return slots_; }    // the hard bound

private:
    static size_t pow2_at_least(size_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }
    static uint64_t word_of(uint64_t d, uint64_t off, uint64_t gen) {
        return d | (off << kDigestBits) | (gen << (kDigestBits + kOffsetBits));
    }
    static uint64_t digest_of_w(uint64_t w) { return w & kDigestMask; }
    static uint64_t offset_of_w(uint64_t w) { return (w >> kDigestBits) & ((1ULL << kOffsetBits) - 1); }
    static uint64_t gen_of_w(uint64_t w) { return w >> (kDigestBits + kOffsetBits); }

    // Whole minutes past `base`, rounded up: rounding down would promise less than the caller asked
    // for, and a kill that stops mattering early is the unsafe direction.
    static uint64_t offset_for(long long expires_at, long long base) {
        const long long off = expires_at - base;
        if (off <= 0) return 0;   // "matters nothing already": the first thing displaced
        const long long minutes = (off + kUnitS - 1) / kUnitS;   // ceil
        return static_cast<uint64_t>(minutes) > kMaxOffset ? kMaxOffset : static_cast<uint64_t>(minutes);
    }

    const size_t cap_;
    const size_t slots_;
    std::unique_ptr<std::atomic<uint64_t>[]> cells_;
    std::atomic<size_t> count_{0};
    std::atomic<long long> bases_[kGenerations];   // one base per generation, published before use
    std::atomic<uint64_t> gen_{0};
};

}   // namespace dd::core

#pragma once
// dd/core/slot_ring.hpp — hand a payload across threads by moving ownership, with no lock,
// no queue, and no allocation in the steady state.
//
// One atomic word per slot carries BOTH the payload's stamp and its state, so no thread can
// reach a state it did not take. The word is the stamp in its low 62 bits and the state in its
// top two:
//
//   state 00  free       the slot is held by nobody; a producer may take it
//   state 01  writing    a producer holds it and is filling the payload — not readable yet
//   state 10  published  payload `seq` is in it; whoever CASes this exact word owns it
//   state 11  taken      claimed; the claimer alone may move the payload out and reset the word
//
// Deriving the state from the word is what makes the handoff atomic: there is no window where a
// slot says "published" while its payload belongs to somebody else who already took it, and a
// producer's half-written payload is never readable, because reading requires the published
// state. Ordering is not queue order — a caller cannot enqueue and expect FIFO — it is
// oldest-first: each claim takes the lowest stamp still published.
//
// Capacity is fixed at construction and never grows: N is the ceiling, and a ring built with a
// runtime `slots` admits a subset of it. The slots are allocated once, when the ring is built,
// and reused forever — the steady state allocates nothing, which is the point of replacing the
// queue.
//
// Two producer shapes, and a ring has exactly one of them:
//   - publish(): one writer thread. A free slot means the sole writer owns it, so it may write
//     the payload before the state becomes visible (release on that store carries the payload
//     with it). No CAS — this is the cheaper path, and it is only sound for one writer.
//   - publish_any(): any thread. The producer takes a stamp with one fetch_add on a shared
//     counter — publish order is global, not per-thread — then CASes free -> writing(seq) on the
//     slot it picked and writes the payload only once it holds it. The CAS is the ownership.
//   A consumer, either way, CASes published(seq) -> taken on the exact word it read, moves the
//   payload out, and resets the slot (release), so the next fill of that slot is ordered after
//   that move. Any number of consumers may race: the CAS is the claim.
//
// A full ring refuses the payload rather than evicting one, and counts the refusal: the caller's
// own backpressure is the right place to decide what an over-queued steer means, and a refusal is
// a number the reply can name rather than a payload that vanished.
//
// The house used to carry a MPSC queue (Vyukov, one atomic exchange) for multi-producer intake.
// It is gone: it handed payloads through nodes it allocated per push, so a queue's steady-state
// cost was an allocation per event — one per streamed token, which is what made a streamed turn
// the most allocation-heavy thing the process did. This is the other shape: fixed slots reused,
// one word per slot, no node to allocate and none to reclaim.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <algorithm>
#include <vector>

namespace dd::core {

namespace detail {
// The stamp lives in the low 62 bits; the state is the top two. A counter would have to publish
// 2^62 payloads before a stamp collided with a state, which no session reaches.
constexpr uint64_t kSeqMask = (1ULL << 62) - 1;
constexpr uint64_t kFreeWord = 0;
constexpr uint64_t kWritingWord = 1ULL << 62;
constexpr uint64_t kPublishedWord = 2ULL << 62;
constexpr uint64_t kTakenWord = 3ULL << 62;

constexpr uint64_t state_of(uint64_t w) { return w >> 62; }
constexpr uint64_t seq_of(uint64_t w) { return w & kSeqMask; }
constexpr uint64_t published_word(uint64_t seq) { return kPublishedWord | seq; }
constexpr uint64_t writing_word(uint64_t seq) { return kWritingWord | seq; }
constexpr bool is_published(uint64_t w) { return state_of(w) == 2; }
}

// Payload needs nothing but a default value and a move: the slot holds it, and the claimer moves
// it out. std::string, an Event, a Job — the same primitive hands all of them.
template <size_t N, class Payload = std::string>
class SlotRing {
public:
  // N slots: the compile-time ceiling is the whole capacity.
  SlotRing() : slot_(std::make_unique<Slot[]>(N)), slots_(N) { reset_all(); }

  // `slots` of the ceiling, clamped to [1, N]: a ring whose capacity is a runtime knob — a sink's
    // cap, a spool's depth — still has fixed slots once it is built.
  explicit SlotRing(size_t slots)
      : slot_(std::make_unique<Slot[]>(slots > N ? N : (slots ? slots : 1))),
        slots_(slots > N ? N : (slots ? slots : 1)) { reset_all(); }

  SlotRing(const SlotRing&) = delete;
  SlotRing& operator=(const SlotRing&) = delete;

  // The sole writer's side: fill a free slot, publish it with one store. False = the ring is
  // full and nothing was written — the payload was not moved from, `refused()` counted it, and
  // the caller decides what an over-queued hand-off means (the Soaker spools it to disk).
  template <class P>
  bool publish(P&& payload) {
    const uint64_t seq = stamp_.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t i = static_cast<size_t>(seq % slots_);
    for (size_t k = 0; k < slots_; ++k) {
      // A free word means nobody holds this slot, and this thread is the only one that ever
      // moves it out of that state — so the payload may be written before the state appears.
      if (detail::state_of(slot_[i].word.load(std::memory_order_acquire)) == 0) {
        slot_[i].payload = std::forward<P>(payload);   // the free word is the ownership: ours to fill
        slot_[i].word.store(detail::published_word(seq), std::memory_order_release);
        return true;
      }
      if (++i == slots_) i = 0;
    }
    refused_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  // Any thread's side, against an lvalue: stamp globally, take a slot by CAS, then write the
  // payload. False = the ring is full, and the caller still holds exactly what it handed over —
  // `refused()` counted it, and the caller decides what an over-queued hand-off means. A site
  // whose overflow path is real (spool it, count it, name it) needs the payload back, which is
  // why the move happens only after the slot is held.
  template <class P>
  bool publish_any(P&& payload) {
    const uint64_t seq = stamp_.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t i = static_cast<size_t>(seq % slots_);
    for (size_t k = 0; k < slots_; ++k) {
      uint64_t free_word = detail::kFreeWord;
      // The CAS is the ownership: from here the slot is ours and no other thread may touch the
      // payload, so writing it before publishing is safe.
      if (slot_[i].word.compare_exchange_strong(free_word, detail::writing_word(seq),
                                                std::memory_order_acq_rel, std::memory_order_relaxed)) {
        slot_[i].payload = std::forward<P>(payload);   // ours: the CAS took the slot before this move
        slot_[i].word.store(detail::published_word(seq), std::memory_order_release);
        return true;
      }
      if (++i == slots_) i = 0;
    }
    refused_.fetch_add(1, std::memory_order_relaxed);   // the payload never left the caller
    return false;
  }

  // Everything published, oldest first. Each payload is handed to exactly one thread: the CAS
  // on the word it read is the claim.
  std::vector<Payload> claim_all() {
    // The word first, the payload second: the stamp is ours the moment the CAS succeeds, and a
    // slot scan walks slots, not publish order — slot 0 can hold a payload newer than slot 3.
    std::vector<std::pair<uint64_t, Payload>> got;
    for (size_t i = 0; i < slots_; ++i) {
      uint64_t w = slot_[i].word.load(std::memory_order_acquire);
      if (!detail::is_published(w)) continue;
      if (!slot_[i].word.compare_exchange_strong(w, detail::kTakenWord,
                                                 std::memory_order_acquire, std::memory_order_relaxed))
        continue;   // someone else took this payload, or a newer one landed in the slot
      got.emplace_back(detail::seq_of(w), std::move(slot_[i].payload));   // ours: the CAS pinned the word we read
      slot_[i].word.store(detail::kFreeWord, std::memory_order_release);
    }
    std::sort(got.begin(), got.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Payload> out;
    out.reserve(got.size());
    for (auto& item : got) out.push_back(std::move(item.second));
    return out;
  }

  // The oldest published payload, or nothing. Claiming one leaves the rest for whoever drains
  // next — which is what a loop that sends one prompt per turn wants.
  // stamp_out reports the stamp the claim pinned, so a caller can verify oldest-first without
  // guessing it from the payload.
  //
  // The pick converges rather than trusting one pass: a slot read is a snapshot of that slot
  // alone, so a payload published into a slot the scan already walked is invisible to this
  // claim and surfaces on the next one. Re-scanning until the pick stops changing makes the
  // claim oldest-first whenever the producers are quiet; against a producer publishing mid-scan
  // the honest guarantee is "the oldest payload published before this claim's scan finished".
  bool claim_one(Payload& out, uint64_t* stamp_out = nullptr) {
    int best = -1;
    uint64_t best_seq = 0;
    for (int pass = 0; pass < 2; ++pass) {
      int next_best = -1;
      uint64_t next_seq = 0;
      for (size_t i = 0; i < slots_; ++i) {
        const uint64_t w = slot_[i].word.load(std::memory_order_acquire);
        if (!detail::is_published(w)) continue;
        const uint64_t seq = detail::seq_of(w);
        if (next_best < 0 || seq < next_seq) { next_best = static_cast<int>(i); next_seq = seq; }
      }
      if (next_best < 0) return false;
      if (next_best == best && next_seq == best_seq) break;   // the pick is stable; claim it
      best = next_best;
      best_seq = next_seq;
    }
    uint64_t want = detail::published_word(best_seq);
    if (!slot_[static_cast<size_t>(best)].word.compare_exchange_strong(
            want, detail::kTakenWord, std::memory_order_acquire, std::memory_order_relaxed))
      return false;   // the slot moved underneath the scan; the next scan sees the newer payload
    out = std::move(slot_[static_cast<size_t>(best)].payload);
    slot_[static_cast<size_t>(best)].word.store(detail::kFreeWord, std::memory_order_release);
    if (stamp_out) *stamp_out = best_seq;
    return true;
  }

  // Payloads a consumer can read right now. A slot in the writing state holds a payload that is
  // being filled and is not counted — it is claimed by a producer, not published.
  size_t published() const {
    size_t n = 0;
    for (size_t i = 0; i < slots_; ++i)
      if (detail::is_published(slot_[i].word.load(std::memory_order_acquire))) ++n;
    return n;
  }

  // Publishes this ring turned away. Every site names this number; none of them lose a payload
  // silently, because the caller keeps what the ring refused.
  uint64_t refused() const { return refused_.load(std::memory_order_relaxed); }

  // Slots this ring actually admits — N built with none, `slots` built with a runtime cap.
  size_t capacity() const { return slots_; }

  // The stamp counter — how many payloads have been stamped, refused ones included, which is
  // what a caller compares against to see whether the ring kept up.
  uint64_t stamped() const { return stamp_.load(std::memory_order_relaxed); }

private:
  // One cache line per slot: claimers scan words, and a scan must not drag a payload's header
  // across a line a producer is filling.
  struct alignas(64) Slot {
    std::atomic<uint64_t> word;
    Payload payload{};
  };

  void reset_all() {
    for (size_t i = 0; i < slots_; ++i) slot_[i].word.store(detail::kFreeWord, std::memory_order_relaxed);
  }

  // Built once, at construction; the slots are then fixed for the ring's whole life.
  std::unique_ptr<Slot[]> slot_;
  size_t slots_;
  std::atomic<uint64_t> stamp_{0};   // shared: publish order is global, not per-thread
  std::atomic<uint64_t> refused_{0};
};

}  // namespace dd::core

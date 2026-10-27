#pragma once
// dd/core/slot_ring.hpp — hand a payload across threads by moving ownership, with no lock,
// no queue, and no allocation in the steady state.
//
// One atomic word per slot carries BOTH the payload's stamp and its ownership, so there is
// no window where a slot says "published" while its stamp belongs to a payload somebody else
// already took. Deriving the state from the stamp is what makes the handoff atomic:
//
//   stamp == 0          slot EMPTY, held by nobody — the writer may fill it
//   stamp == s (s >= 1) payload s is published; whoever CASes s -> kTaken owns it
//   stamp == kTaken     claimed; the claimer alone may move the payload out and reset to 0
//
// The payload's text is readable only by whoever holds the slot, and a claim pins the stamp
// it read: CAS(s -> kTaken) fails if the slot was reclaimed and republished underneath, so a
// claimer can never hand back a payload it did not select. Ordering is not queue order — a
// caller cannot enqueue and expect FIFO — it is oldest-first: each claim takes the lowest
// stamp still published.
//
// Ownership boundaries, which is the whole safety argument:
//   - the writer owns slots whose stamp is 0, and is the only thread that ever moves a slot
//     out of that state, so it may write the text before the stamp becomes visible (release
//     on that store makes the text arrive with the stamp);
//   - a claimer owns a slot only after a CAS on its exact stamp succeeds, and only then
//     touches the text (acquire on that CAS);
//   - resetting a slot to 0 is done by the thread that claimed it, after moving the payload
//     out (release), so the writer's next fill of that slot is ordered after that move.
//
// A full ring refuses the payload rather than evicting one: the caller's own backpressure is
// the right place to decide what an over-queued steer means.
//
// The house already has a MPSC queue (Vyukov, one atomic exchange) for multi-producer intake.
// This is the other shape — one producer handing a payload to one consumer per claim, fixed
// slots, no node to reclaim.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <algorithm>
#include <vector>

namespace dd::core {

namespace detail {
constexpr uint64_t kEmptyStamp = 0;
constexpr uint64_t kTakenStamp = UINT64_MAX;
}

template <size_t N>
class SlotRing {
public:
  SlotRing() {
    for (auto& s : slot_) s.stamp.store(detail::kEmptyStamp, std::memory_order_relaxed);
  }

  // The writer's side: fill a slot nobody holds, publish it with one store.
  // False = the ring is full and nothing was written.
  bool publish(std::string text) {
    const uint64_t seq = ++seq_;   // 0 and kTakenStamp are stamps this counter never reaches
    size_t i = static_cast<size_t>(seq % N);
    for (size_t k = 0; k < N; ++k) {
      // Stamp 0 means nobody holds this slot, so the writer — the only thread that moves a
      // slot out of that state — may write the payload before the stamp appears.
      if (slot_[i].stamp.load(std::memory_order_acquire) == detail::kEmptyStamp) {
        slot_[i].text = std::move(text);
        slot_[i].stamp.store(seq, std::memory_order_release);
        return true;
      }
      if (++i == N) i = 0;
    }
    return false;
  }

  // Everything published, oldest first. Each payload is handed to exactly one thread: the
  // CAS on the stamp it read is the claim.
  std::vector<std::string> claim_all() {
    // Stamp first, payload second: the stamp is ours the moment the CAS succeeds, and a slot
    // scan walks slots, not publish order — slot 0 can hold a payload newer than slot 3.
    std::vector<std::pair<uint64_t, std::string>> got;
    for (size_t i = 0; i < N; ++i) {
      const uint64_t seq = slot_[i].stamp.load(std::memory_order_acquire);
      if (seq == detail::kEmptyStamp || seq == detail::kTakenStamp) continue;
      uint64_t want = seq;
      if (!slot_[i].stamp.compare_exchange_strong(want, detail::kTakenStamp,
                                                  std::memory_order_acquire, std::memory_order_relaxed))
        continue;   // someone else took this payload, or a newer one landed in the slot
      got.emplace_back(seq, std::move(slot_[i].text));   // ours: the CAS pinned the stamp we read
      slot_[i].stamp.store(detail::kEmptyStamp, std::memory_order_release);
    }
    std::sort(got.begin(), got.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> out;
    out.reserve(got.size());
    for (auto& item : got) out.push_back(std::move(item.second));
    return out;
  }

  // The oldest published payload, or empty. Claiming one leaves the rest for whoever drains
  // next — which is what a loop that sends one prompt per turn wants.
  // stamp_out reports the stamp the claim pinned, so a caller can verify oldest-first
  // without guessing it from the payload.
  //
  // The pick converges rather than trusting one pass: a slot read is a snapshot of that slot
  // alone, so a payload published into a slot the scan already walked is invisible to this
  // claim and surfaces on the next one. Re-scanning until the pick stops changing makes the
  // claim oldest-first whenever the writer is quiet; against a writer publishing mid-scan the
  // honest guarantee is "the oldest payload published before this claim's scan finished".
  std::string claim_one(uint64_t* stamp_out = nullptr) {
    int best = -1;
    uint64_t best_seq = 0;
    for (int pass = 0; pass < 2; ++pass) {
      int next_best = -1;
      uint64_t next_seq = 0;
      for (size_t i = 0; i < N; ++i) {
        const uint64_t seq = slot_[i].stamp.load(std::memory_order_acquire);
        if (seq == detail::kEmptyStamp || seq == detail::kTakenStamp) continue;
        if (next_best < 0 || seq < next_seq) { next_best = static_cast<int>(i); next_seq = seq; }
      }
      if (next_best < 0) return {};
      if (next_best == best && next_seq == best_seq) break;   // the pick is stable; claim it
      best = next_best;
      best_seq = next_seq;
    }
    uint64_t want = best_seq;
    if (!slot_[static_cast<size_t>(best)].stamp.compare_exchange_strong(
            want, detail::kTakenStamp, std::memory_order_acquire, std::memory_order_relaxed))
      return {};   // the slot moved underneath the scan; the next scan sees the newer payload
    std::string out = std::move(slot_[static_cast<size_t>(best)].text);
    slot_[static_cast<size_t>(best)].stamp.store(detail::kEmptyStamp, std::memory_order_release);
    if (stamp_out) *stamp_out = best_seq;
    return out;
  }

  size_t published() const {
    size_t n = 0;
    for (const auto& s : slot_) {
      const uint64_t seq = s.stamp.load(std::memory_order_acquire);
      if (seq != detail::kEmptyStamp && seq != detail::kTakenStamp) ++n;
    }
    return n;
  }

  size_t capacity() const { return N; }

  // The writer's cursor after its last publish attempt — readable by the writer thread only,
  // which is the thread publish() is called from.
  uint64_t last_stamp() const { return seq_; }

private:
  // One cache line per slot: claimers scan stamps, and a scan must not drag a payload's
  // string header across a line the writer is filling.
  struct alignas(64) Slot {
    std::atomic<uint64_t> stamp;
    std::string text;
  };

  Slot slot_[N];
  uint64_t seq_ = 0;   // the writer's own cursor; no other thread touches it
};

}  // namespace dd::core

#pragma once
// dd/core/snap_cell.hpp — one immutable snapshot that any number of threads read
// without a lock and without waiting, and a publisher replaces.
//
// std::atomic<std::shared_ptr<T>> is not this: libstdc++ guards it with a spin lock
// (is_lock_free() is false), so a reader preempted inside load() stalls everyone.
// Here a reader pins the current epoch (one store), loads the pointer (one load) and
// unpins (one store): wait-free. The publisher swaps the pointer with one exchange and
// frees a retired snapshot once no reader is pinned at or before the epoch it was
// retired in. It never waits for a reader; it frees later.
//
// Rules: a Read stays on the thread that made it and ends before its cell does; one
// publisher at a time (the refresh thread; passing the role to another thread needs a
// happens-before edge, a start or a join); a Read held for long only delays freeing,
// it blocks nobody. Not from static destructors: a thread's record is a thread_local.
// Wait-free from a thread's second read on: its first allocates the thread's record.
// wait() sleeps until the first snapshot is published, woken by publish(): no polling.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "dd/core/park.hpp"

namespace dd {
namespace core {
namespace snap {

constexpr uint64_t kIdle = ~uint64_t{0};

// One per thread that has ever read; leased by CAS, handed back when the thread ends,
// never freed (so the list a publisher walks is stable).
struct Record {
    std::atomic<uint64_t> pinned{kIdle};   // the epoch this thread's outermost Read announced
    std::atomic<bool> leased{false};
    Record* next = nullptr;                // fixed before the record is linked
    unsigned depth = 0;                    // the owner thread's alone: nested Reads pin once
};

inline std::atomic<uint64_t> g_epoch{1};
inline std::atomic<Record*> g_records{nullptr};

inline Record* lease() {
    for (Record* r = g_records.load(std::memory_order_acquire); r; r = r->next) {
        bool free = false;
        if (r->leased.compare_exchange_strong(free, true, std::memory_order_acq_rel)) return r;
    }
    Record* r = new Record;
    r->leased.store(true, std::memory_order_relaxed);
    Record* head = g_records.load(std::memory_order_relaxed);
    do {
        r->next = head;
    } while (!g_records.compare_exchange_weak(head, r, std::memory_order_seq_cst, std::memory_order_relaxed));   // seq_cst: see floor_epoch
    return r;
}

struct Local {
    Record* rec = lease();
    ~Local() {
        rec->depth = 0;
        rec->pinned.store(kIdle, std::memory_order_release);
        rec->leased.store(false, std::memory_order_release);
    }
};

inline Record& mine() {
    thread_local Local local;
    return *local.rec;
}

inline void pin(Record& r) {
    if (r.depth++ == 0) r.pinned.store(g_epoch.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
}

inline void unpin(Record& r) {
    if (--r.depth == 0) r.pinned.store(kIdle, std::memory_order_release);
}

// The oldest epoch any reader is pinned to; kIdle when none is. The head load is seq_cst like the link that
// published a record: a record linked just before its thread's first pin must be found by a scan that follows
// the publisher's exchange, or that reader's pin would be missed.
inline uint64_t floor_epoch() {
    uint64_t m = kIdle;
    for (Record* r = g_records.load(std::memory_order_seq_cst); r; r = r->next) m = std::min(m, r->pinned.load(std::memory_order_seq_cst));
    return m;
}

inline size_t record_count() {
    size_t n = 0;
    for (Record* r = g_records.load(std::memory_order_acquire); r; r = r->next) ++n;
    return n;
}

}  // namespace snap

template <class T>
class SnapCell {
public:
    SnapCell() = default;
    SnapCell(const SnapCell&) = delete;
    SnapCell& operator=(const SnapCell&) = delete;
    ~SnapCell() {
        delete cell_.load(std::memory_order_relaxed);
        for (const auto& r : retired_) delete r.p;
    }

    // A pinned view: the snapshot it shows is not freed while it lives.
    class Read {
    public:
        Read() = default;
        Read(const Read&) = delete;
        Read& operator=(const Read&) = delete;
        Read(Read&& o) noexcept : p_(o.p_), rec_(o.rec_) { o.p_ = nullptr, o.rec_ = nullptr; }
        Read& operator=(Read&& o) noexcept {
            if (this != &o) {
                release();
                p_ = o.p_, rec_ = o.rec_;
                o.p_ = nullptr, o.rec_ = nullptr;
            }
            return *this;
        }
        ~Read() { release(); }
        const T* get() const { return p_; }
        const T* operator->() const { return p_; }
        const T& operator*() const { return *p_; }
        explicit operator bool() const { return p_ != nullptr; }

    private:
        friend class SnapCell;
        void release() {
            if (rec_) snap::unpin(*rec_);
            rec_ = nullptr, p_ = nullptr;
        }
        const T* p_ = nullptr;
        snap::Record* rec_ = nullptr;
    };

    Read read() const {
        Read r;
        r.rec_ = &snap::mine();
        snap::pin(*r.rec_);
        r.p_ = cell_.load(std::memory_order_seq_cst);
        return r;
    }

    bool empty() const { return cell_.load(std::memory_order_acquire) == nullptr; }

    // The publisher's: `next` becomes visible, the old snapshot is retired. Never waits for a reader.
    void publish(std::unique_ptr<const T> next) {
        retired_.reserve(retired_.size() + 1);   // nothing after the exchange may throw: the old snapshot must not leak
        const T* old = cell_.exchange(next.release(), std::memory_order_seq_cst);
        if (old) retired_.push_back({old, snap::g_epoch.load(std::memory_order_seq_cst)});
        snap::g_epoch.fetch_add(1, std::memory_order_seq_cst);   // readers that start now pin past the retirement
        gen_.fetch_add(1, std::memory_order_release);
        unpark(gen_, INT_MAX);   // whoever sleeps in wait()
        reclaim();
    }

    // Sleeps until the cell holds a snapshot or the timeout runs out; true when it does. Woken by publish().
    bool wait(std::chrono::nanoseconds timeout) const {
        const auto end = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const uint32_t seen = gen_.load(std::memory_order_acquire);   // read before looking: a publish after it is never missed
            if (!empty()) return true;
            const auto left = end - std::chrono::steady_clock::now();
            if (left <= std::chrono::nanoseconds(0)) return false;
            park(gen_, seen, std::chrono::duration_cast<std::chrono::nanoseconds>(left));
        }
    }

    // Retired snapshots not yet freed (a reader still pinned to them): for tests and diagnostics.
    size_t retired() const { return retired_.size(); }

private:
    struct Retired {
        const T* p;
        uint64_t epoch;
    };
    void reclaim() {
        const uint64_t floor = snap::floor_epoch();
        size_t keep = 0;
        for (const auto& r : retired_) {
            if (r.epoch < floor) delete r.p;
            else retired_[keep++] = r;
        }
        retired_.resize(keep);
    }

    std::atomic<const T*> cell_{nullptr};
    mutable std::atomic<uint32_t> gen_{0};   // bumped by publish(): what wait() sleeps on
    std::vector<Retired> retired_;   // the publisher's alone
};

}  // namespace core
}  // namespace dd

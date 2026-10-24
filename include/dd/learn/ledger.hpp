#pragma once
// dd/learn/ledger.hpp — the durable half of learning: journal + snapshots + the live Stats.
//
// record() appends a Record to the journal and folds it into the live Stats; open() rebuilds the
// same Stats from the newest snapshot plus the frames after it. What is folded is the codec's own
// form of the record (cut to the bounds), so replaying the journal gives the live state bit for
// bit. One thread (the learn thread) calls everything except snapshot() and health(), which any
// thread may: snapshot() is a lock-free read of an immutable copy published every `publish_every`
// records, on tick() and on publish().
//
// One process at a time writes a ledger: open() holds an exclusive flock on <dir>/LOCK (the holder's pid
// inside) until the Ledger is gone; a second writer is refused with the pid. load_readonly() needs no
// lock. A run that saw failed syncs or writes leaves <dir>/HEALTH behind for `dd learn status`.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "dd/core/snap_cell.hpp"
#include "dd/learn/compactor.hpp"
#include "dd/learn/journal.hpp"
#include "dd/learn/stats.hpp"

namespace dd {
namespace learn {

struct LedgerOptions {
    JournalOptions journal;
    size_t compact_after = 2;       // sealed segments not yet in a snapshot before one is made
    size_t publish_every = 1024;    // records between published copies of the Stats
    Compactor::Hook compact_hook;   // tests: stop a compaction at a step
};

class Ledger {
public:
    struct Health {
        uint64_t appended = 0, write_errors = 0, recovered = 0, replay_newer = 0, replay_undecodable = 0;
        uint64_t torn_bytes = 0, bad_segments = 0, snapshots_corrupt = 0, compactions = 0, compaction_failures = 0;
        uint64_t last_seq = 0;
        uint64_t sync_errors = 0;   // failed fsync/fdatasync: the newest records may not be durable
    };

    // nullptr (and *err) when the directory cannot be used, or holds a snapshot from a newer version
    // (left untouched: learning stays off rather than damage it).
    static std::unique_ptr<Ledger> open(const std::filesystem::path& dir, LedgerOptions opt = {}, std::string* err = nullptr);
    // Read-only: the newest snapshot and the journal tail folded into `out`; creates, truncates and deletes nothing.
    static bool load_readonly(const std::filesystem::path& dir, Stats& out, Health* health = nullptr, std::string* err = nullptr);
    ~Ledger();
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;

    // ---- the learn thread ----
    bool record(const Record& r);       // false: the journal write failed and nothing was folded
    void tick();                        // sync by interval, compact when due, publish if the copy is stale
    void sync();
    void publish();
    void compact_now();                 // fold every sealed segment
    const Stats& live() const { return live_; }

    // ---- any thread ----
    core::SnapCell<Stats>::Read snapshot() const { return cell_.read(); }
    Health health() const;
    const std::filesystem::path& dir() const { return dir_; }

private:
    Ledger() = default;
    void maybe_compact();
    void note_health();   // writes <dir>/HEALTH when the error counters moved

    // the first member, so it is released last: nothing of this ledger is open when another process may take over
    struct FileLock {
        int fd = -1;
        FileLock() = default;
        FileLock(const FileLock&) = delete;
        FileLock& operator=(const FileLock&) = delete;
        ~FileLock();
    } lock_;
    std::filesystem::path dir_;
    LedgerOptions opt_;
    std::unique_ptr<Journal> journal_;
    Stats live_;
    core::SnapCell<Stats> cell_;
    uint64_t covered_ = 0, rotations_seen_ = 0;
    size_t unpublished_ = 0;
    std::atomic<uint64_t> recovered_{0}, replay_newer_{0}, replay_undecodable_{0}, snapshots_corrupt_{0}, compactions_{0}, compaction_failures_{0},
        compaction_sync_errors_{0};
    uint64_t health_sync_ = 0, health_write_ = 0;   // what HEALTH last said
};

}  // namespace learn
}  // namespace dd

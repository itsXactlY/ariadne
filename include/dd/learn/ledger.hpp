#pragma once
// dd/learn/ledger.hpp — the durable half of learning: journal + snapshots + the live Stats.
//
// record() appends a Record to the journal and folds it into the live Stats; open() rebuilds the
// same Stats from the newest snapshot plus the frames after it. What is folded is the codec's own
// form of the record (cut to the bounds), so replaying the journal gives the live state bit for
// bit. One thread (the learn thread) calls everything except snapshot() and health(), which any
// thread may: snapshot() is a lock-free read of an immutable copy published every `publish_every`
// records, on tick() and on publish().

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
    };

    // nullptr (and *err) when the directory cannot be used, or holds a snapshot from a newer version
    // (left untouched: learning stays off rather than damage it).
    static std::unique_ptr<Ledger> open(const std::filesystem::path& dir, LedgerOptions opt = {}, std::string* err = nullptr);
    ~Ledger();
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;

    // ---- the learn thread ----
    bool record(const Record& r);       // false: the journal write failed and nothing was folded
    void tick();                        // sync by interval, compact when due, publish if the copy is stale
    void sync() { journal_->sync(); }
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

    std::filesystem::path dir_;
    LedgerOptions opt_;
    std::unique_ptr<Journal> journal_;
    Stats live_;
    core::SnapCell<Stats> cell_;
    uint64_t covered_ = 0, rotations_seen_ = 0;
    size_t unpublished_ = 0;
    std::atomic<uint64_t> recovered_{0}, replay_newer_{0}, replay_undecodable_{0}, snapshots_corrupt_{0}, compactions_{0}, compaction_failures_{0};
};

}  // namespace learn
}  // namespace dd

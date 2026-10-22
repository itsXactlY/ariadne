#pragma once
// dd/learn/compactor.hpp — folds the sealed segments into one snapshot and retires them.
//
// snap-<n>.dat holds the Stats after every frame of segments <= n (header: magic, version, last seq,
// covered segment, body length, crc32c of the body). The order is what makes a kill at any step safe:
// write a temp file, fsync it, rename it into place, fsync the directory, and only then delete the
// segments it covers and the older snapshots. A frame the snapshot holds is skipped on replay by its
// seq, so a segment that outlived a crash is harmless and the next compaction removes it.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "dd/learn/stats.hpp"

namespace dd {
namespace learn {

struct Snapshot {
    Stats stats;
    uint64_t last_seq = 0;          // every frame with seq <= this is inside `stats`
    uint64_t covered_segment = 0;   // segments <= this are folded (and may be gone)
};

struct SnapshotLoad {
    std::optional<Snapshot> snap;   // the newest one that reads whole
    bool newer = false;             // a snapshot from a newer version exists: do not touch this directory
    uint64_t corrupt = 0;           // snapshots skipped as damaged
};

SnapshotLoad load_snapshot(const std::filesystem::path& dir);
std::filesystem::path snapshot_path(const std::filesystem::path& dir, uint64_t covered_segment);

class Compactor {
public:
    // Called before each step with its name; false stops there, as a kill would (tests).
    using Hook = std::function<bool(const char* step)>;
    struct Result {
        bool ok = false, aborted = false, newer = false;
        uint64_t covered_segment = 0, last_seq = 0;
        size_t segments_removed = 0, snapshots_removed = 0;
        uint64_t bad_segments = 0;   // folded segments that ended on a bad frame: what followed it is gone
    };

    explicit Compactor(std::filesystem::path dir, Hook hook = {}) : dir_(std::move(dir)), hook_(std::move(hook)) {}
    // Folds the newest snapshot and every segment <= through_segment (all sealed) into a new snapshot.
    Result compact(uint64_t through_segment);

private:
    bool step(const char* name) const { return !hook_ || hook_(name); }
    std::filesystem::path dir_;
    Hook hook_;
};

}  // namespace learn
}  // namespace dd

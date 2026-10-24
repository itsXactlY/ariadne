#pragma once
// dd/learn/journal.hpp — the append-only record of what was learned from.
//
// A directory of segments seg-<n>.jnl, each a run of frames:
//   u32 len | u32 crc32c | u8 codec version | u64 seq | payload        (len counts version+seq+payload;
//                                                                       the crc covers the same bytes)
// seq grows by one per frame across segments and across restarts; it, not the clock, orders the
// journal and tells a snapshot which frames it already holds. Opening truncates a torn tail off the
// last segment; a bad frame anywhere ends that segment's readable part and is counted, never read
// past. One writer thread (the learn thread) owns a Journal; nothing here locks.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dd/learn/record.hpp"

namespace dd {
namespace learn {

constexpr size_t kFrameHeader = 4 + 4 + 1 + 8;

struct JournalOptions {
    size_t segment_bytes = 8u << 20;                        // rotate before a frame would pass this
    size_t sync_records = 256;                              // fdatasync after this many appends ...
    std::chrono::milliseconds sync_interval{2000};          // ... or when the oldest unsynced one is this old
};

struct ReplayInfo {
    uint64_t records = 0;       // frames handed to the callback
    uint64_t newer = 0;         // frames from a newer codec: skipped
    uint64_t undecodable = 0;   // crc fine, payload not: skipped
    uint64_t bad_segments = 0;  // segments that ended on a bad frame before their end
    uint64_t last_seq = 0;      // highest seq seen in a good frame
};

using ReplayFn = std::function<void(uint64_t seq, const Record&)>;

struct JournalCounters {
    std::atomic<uint64_t> appended{0}, syncs{0}, rotations{0}, torn_bytes{0}, bad_segments{0}, write_errors{0}, sync_errors{0};
};

class Journal {
public:
    // floor_seq / floor_segment: the highest seq and segment a snapshot already holds, so numbering
    // never goes back when the segments themselves are gone.
    static std::unique_ptr<Journal> open(const std::filesystem::path& dir, JournalOptions opt, uint64_t floor_seq, uint64_t floor_segment, std::string* err = nullptr);
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    uint64_t append(std::string_view payload);        // seq of the frame; 0 when the write failed (counted)
    void maybe_sync();                                // the interval rule, for an idle tick
    void sync();
    uint64_t last_seq() const { return next_seq_.load(std::memory_order_relaxed) - 1; }
    uint64_t active_segment() const { return active_; }
    size_t active_bytes() const { return active_bytes_; }
    const JournalCounters& counters() const { return c_; }
    const std::filesystem::path& dir() const { return dir_; }

    // ---- files, for the compactor and for recovery ----
    static std::vector<uint64_t> segments(const std::filesystem::path& dir);            // ascending
    static std::filesystem::path segment_path(const std::filesystem::path& dir, uint64_t index);
    // Frames with seq > after_seq from every segment up to last_segment, in order.
    static ReplayInfo replay(const std::filesystem::path& dir, uint64_t after_seq, uint64_t last_segment, const ReplayFn& fn);

private:
    Journal() = default;
    bool rotate();
    void open_active(uint64_t index);

    std::filesystem::path dir_;
    JournalOptions opt_;
    JournalCounters c_;
    int fd_ = -1;
    uint64_t active_ = 0;
    std::atomic<uint64_t> next_seq_{1};
    size_t active_bytes_ = 0, unsynced_ = 0;
    std::chrono::steady_clock::time_point first_unsynced_;
};

// Frame helpers, shared with tests.
void frame_encode(std::string& out, uint8_t version, uint64_t seq, std::string_view payload);

}  // namespace learn
}  // namespace dd

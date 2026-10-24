#include "dd/learn/journal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dd/core/crc32c.hpp"
#include "dd/learn/codec.hpp"
#include "dd/learn/fsutil.hpp"

namespace fs = std::filesystem;

namespace dd {
namespace learn {
namespace {

constexpr size_t kMaxFrame = kMaxPayload + 1 + 8;

uint32_t rd32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t rd64(const char* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

struct Frame {
    uint64_t seq;
    uint8_t version;
    std::string_view payload;
};

// Walks the good frames of one segment's bytes. Returns the offset after the last good one;
// `clean` is false when it stopped on a bad frame or a torn tail rather than at the end.
template <class F>
size_t scan(const std::string& bytes, F&& fn, bool* clean) {
    size_t off = 0;
    while (off < bytes.size()) {
        if (bytes.size() - off < kFrameHeader) break;
        const uint32_t len = rd32(bytes.data() + off);
        if (len < 1 + 8 || len > kMaxFrame || bytes.size() - off - 8 < len) break;
        if (core::crc32c(bytes.data() + off + 8, len) != rd32(bytes.data() + off + 4)) break;
        const char* body = bytes.data() + off + 8;
        fn(Frame{rd64(body + 1), static_cast<uint8_t>(body[0]), std::string_view(body + 9, len - 9)});
        off += 8 + len;
    }
    *clean = off == bytes.size();
    return off;
}

}  // namespace

void frame_encode(std::string& out, uint8_t version, uint64_t seq, std::string_view payload) {
    const uint32_t len = static_cast<uint32_t>(1 + 8 + payload.size());
    const size_t at = out.size();
    out.resize(at + 8);
    out.push_back(static_cast<char>(version));
    out.append(reinterpret_cast<const char*>(&seq), 8);
    out.append(payload);
    const uint32_t crc = core::crc32c(out.data() + at + 8, len);
    std::memcpy(&out[at], &len, 4);
    std::memcpy(&out[at + 4], &crc, 4);
}

fs::path Journal::segment_path(const fs::path& dir, uint64_t index) {
    char name[40];
    std::snprintf(name, sizeof name, "seg-%08llu.jnl", static_cast<unsigned long long>(index));
    return dir / name;
}

std::vector<uint64_t> Journal::segments(const fs::path& dir) {
    std::vector<uint64_t> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 8 && n.compare(0, 4, "seg-") == 0 && n.compare(n.size() - 4, 4, ".jnl") == 0) {
            char* end = nullptr;
            const unsigned long long i = std::strtoull(n.c_str() + 4, &end, 10);
            if (end && std::strcmp(end, ".jnl") == 0) v.push_back(i);
        }
    }
    std::sort(v.begin(), v.end());
    return v;
}

ReplayInfo Journal::replay(const fs::path& dir, uint64_t after_seq, uint64_t last_segment, const ReplayFn& fn) {
    ReplayInfo info;
    std::string bytes;
    for (const uint64_t idx : segments(dir)) {
        if (idx > last_segment) break;
        if (!fsx::read_file(segment_path(dir, idx), bytes)) continue;
        bool clean = true;
        scan(bytes, [&](const Frame& f) {
            info.last_seq = std::max(info.last_seq, f.seq);
            if (f.seq <= after_seq) return;
            Record r;
            switch (decode(f.version, f.payload, r)) {
                case Decode::Ok: ++info.records, fn(f.seq, r); break;
                case Decode::Newer: ++info.newer; break;
                case Decode::Corrupt: ++info.undecodable; break;
            }
        }, &clean);
        if (!clean) ++info.bad_segments;
    }
    return info;
}

std::unique_ptr<Journal> Journal::open(const fs::path& dir, JournalOptions opt, uint64_t floor_seq, uint64_t floor_segment, std::string* err) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (err) *err = "cannot create " + dir.string() + ": " + ec.message();
        return nullptr;
    }
    std::unique_ptr<Journal> j(new Journal);
    j->dir_ = dir, j->opt_ = opt;
    uint64_t last_seq = floor_seq;
    const auto segs = segments(dir);
    std::string bytes;
    for (size_t i = 0; i < segs.size(); ++i) {
        const fs::path p = segment_path(dir, segs[i]);
        if (!fsx::read_file(p, bytes)) continue;
        bool clean = true;
        const size_t good = scan(bytes, [&](const Frame& f) { last_seq = std::max(last_seq, f.seq); }, &clean);
        if (clean) continue;
        j->c_.bad_segments++;
        if (i + 1 == segs.size()) {   // the segment being written when the process died: cut it to its last good frame
            if (::truncate(p.c_str(), static_cast<off_t>(good)) != 0) {
                if (err) *err = "cannot truncate " + p.string();
                return nullptr;
            }
            j->c_.torn_bytes += bytes.size() - good;
        }
    }
    j->next_seq_.store(last_seq + 1, std::memory_order_relaxed);
    uint64_t index = segs.empty() || segs.back() < floor_segment ? floor_segment + 1 : segs.back();
    if (!segs.empty() && index == segs.back()) {
        std::error_code sec;
        const auto sz = fs::file_size(segment_path(dir, index), sec);
        if (!sec && sz >= opt.segment_bytes) ++index;   // a full one is sealed: start the next
    }
    j->open_active(index);
    if (j->fd_ < 0) {
        if (err) *err = "cannot open " + segment_path(dir, index).string();
        return nullptr;
    }
    return j;
}

void Journal::open_active(uint64_t index) {
    const fs::path p = segment_path(dir_, index);
    fd_ = ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    active_ = index;
    struct stat st;
    active_bytes_ = fd_ >= 0 && ::fstat(fd_, &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
    if (fd_ >= 0 && !fsx::sync_dir(dir_)) c_.sync_errors++;
}

Journal::~Journal() {
    if (fd_ >= 0) {
        sync();
        ::close(fd_);
    }
}

bool Journal::rotate() {
    sync();
    ::close(fd_);
    open_active(active_ + 1);
    if (fd_ < 0) return false;
    c_.rotations++;
    return true;
}

uint64_t Journal::append(std::string_view payload) {
    if (fd_ < 0 || payload.size() > kMaxPayload) return c_.write_errors++, 0;
    std::string frame;
    frame.reserve(kFrameHeader + payload.size());
    const uint64_t seq = next_seq_.load(std::memory_order_relaxed);
    frame_encode(frame, kCodecVersion, seq, payload);
    if (active_bytes_ > 0 && active_bytes_ + frame.size() > opt_.segment_bytes && !rotate()) return c_.write_errors++, 0;
    if (!fsx::write_all(fd_, frame.data(), frame.size())) {
        // A partial write leaves a torn frame that the next open cuts; stop appending behind it.
        ::close(fd_);
        fd_ = -1;
        return c_.write_errors++, 0;
    }
    active_bytes_ += frame.size();
    if (unsynced_++ == 0) first_unsynced_ = std::chrono::steady_clock::now();
    c_.appended++;
    next_seq_.store(seq + 1, std::memory_order_relaxed);
    if (unsynced_ >= opt_.sync_records) sync();
    else maybe_sync();
    return seq;
}

void Journal::maybe_sync() {
    if (unsynced_ > 0 && std::chrono::steady_clock::now() - first_unsynced_ >= opt_.sync_interval) sync();
}

void Journal::sync() {
    if (unsynced_ == 0 || fd_ < 0) return;
    // the data and the size; the name was synced when the segment was created. A failed sync is not retried (the kernel may
    // have dropped the pages): it is counted, and Ledger::health() says so
    if (::fdatasync(fd_) != 0) c_.sync_errors++;
    unsynced_ = 0;
    c_.syncs++;
}

}  // namespace learn
}  // namespace dd

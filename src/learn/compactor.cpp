#include "dd/learn/compactor.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dd/core/crc32c.hpp"
#include "dd/learn/codec.hpp"
#include "dd/learn/fsutil.hpp"
#include "dd/learn/journal.hpp"

namespace fs = std::filesystem;

namespace dd {
namespace learn {
namespace {

constexpr char kMagic[4] = {'D', 'D', 'L', 'S'};
constexpr size_t kSnapHeader = 4 + 4 + 8 + 8 + 4 + 4;

std::vector<uint64_t> snapshots(const fs::path& dir) {
    std::vector<uint64_t> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 9 && n.compare(0, 5, "snap-") == 0 && n.compare(n.size() - 4, 4, ".dat") == 0) {
            char* end = nullptr;
            const unsigned long long i = std::strtoull(n.c_str() + 5, &end, 10);
            if (end && std::strcmp(end, ".dat") == 0) v.push_back(i);
        }
    }
    std::sort(v.begin(), v.end());
    return v;
}

enum class Read { Ok, Newer, Corrupt };

Read read_snapshot(const fs::path& p, Snapshot& out) {
    std::string b;
    if (!fsx::read_file(p, b) || b.size() < kSnapHeader || std::memcmp(b.data(), kMagic, 4) != 0) return Read::Corrupt;
    uint32_t version, len, crc;
    uint64_t last_seq, covered;
    std::memcpy(&version, b.data() + 4, 4);
    std::memcpy(&last_seq, b.data() + 8, 8);
    std::memcpy(&covered, b.data() + 16, 8);
    std::memcpy(&len, b.data() + 24, 4);
    std::memcpy(&crc, b.data() + 28, 4);
    if (b.size() - kSnapHeader != len || core::crc32c(b.data() + kSnapHeader, len) != crc) return Read::Corrupt;
    switch (decode_stats(version, std::string_view(b).substr(kSnapHeader), out.stats)) {
        case Decode::Ok: out.last_seq = last_seq, out.covered_segment = covered; return Read::Ok;
        case Decode::Newer: return Read::Newer;
        case Decode::Corrupt: break;
    }
    return Read::Corrupt;
}

std::string snapshot_bytes(const Snapshot& s) {
    const std::string body = encode_stats(s.stats);
    const uint32_t version = kStatsVersion, len = static_cast<uint32_t>(body.size()), crc = core::crc32c(body.data(), body.size());
    std::string b(kMagic, 4);
    b.append(reinterpret_cast<const char*>(&version), 4);
    b.append(reinterpret_cast<const char*>(&s.last_seq), 8);
    b.append(reinterpret_cast<const char*>(&s.covered_segment), 8);
    b.append(reinterpret_cast<const char*>(&len), 4);
    b.append(reinterpret_cast<const char*>(&crc), 4);
    b.append(body);
    return b;
}

}  // namespace

fs::path snapshot_path(const fs::path& dir, uint64_t covered_segment) {
    char name[40];
    std::snprintf(name, sizeof name, "snap-%08llu.dat", static_cast<unsigned long long>(covered_segment));
    return dir / name;
}

SnapshotLoad load_snapshot(const fs::path& dir) {
    SnapshotLoad r;
    const auto all = snapshots(dir);
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        Snapshot s;
        switch (read_snapshot(snapshot_path(dir, *it), s)) {
            case Read::Ok: r.snap = std::move(s); return r;
            case Read::Newer: r.newer = true; return r;
            case Read::Corrupt: ++r.corrupt; break;
        }
    }
    return r;
}

Compactor::Result Compactor::compact(uint64_t through) {
    Result res;
    if (!step("begin")) return res.aborted = true, res;
    SnapshotLoad base = load_snapshot(dir_);
    if (base.newer) return res.newer = true, res;
    Snapshot s;
    if (base.snap) s = std::move(*base.snap);
    const uint64_t had = s.covered_segment;
    const ReplayInfo info = Journal::replay(dir_, s.last_seq, through, [&](uint64_t, const Record& r) { s.stats.apply(r); });
    // a frame the callback never saw (newer codec, undecodable) still counts as folded: it is in the segment we delete
    s.last_seq = std::max(s.last_seq, info.last_seq);
    res.bad_segments = info.bad_segments;
    s.covered_segment = std::max(had, through);
    res.covered_segment = s.covered_segment, res.last_seq = s.last_seq;

    if (s.covered_segment != had || !base.snap) {
        if (!step("write_tmp")) return res.aborted = true, res;
        const fs::path tmp = dir_ / "snap.tmp", dst = snapshot_path(dir_, s.covered_segment);
        const std::string bytes = snapshot_bytes(s);
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 || !fsx::write_all(fd, bytes.data(), bytes.size())) {
            if (fd >= 0) ::close(fd);
            return res;
        }
        if (!step("fsync_tmp")) return ::close(fd), res.aborted = true, res;
        if (::fsync(fd) != 0) return ::close(fd), ++res.sync_errors, res;
        ::close(fd);
        if (!step("rename")) return res.aborted = true, res;
        if (::rename(tmp.c_str(), dst.c_str()) != 0) return res;
        if (!step("fsync_dir")) return res.aborted = true, res;
        if (!fsx::sync_dir(dir_)) ++res.sync_errors;
    }
    for (const uint64_t idx : Journal::segments(dir_)) {
        if (idx > through) break;
        if (!step("unlink_segment")) return res.aborted = true, res;
        if (::unlink(Journal::segment_path(dir_, idx).c_str()) == 0) ++res.segments_removed;
    }
    for (const uint64_t idx : snapshots(dir_)) {
        if (idx >= s.covered_segment) continue;
        if (!step("unlink_snapshot")) return res.aborted = true, res;
        if (::unlink(snapshot_path(dir_, idx).c_str()) == 0) ++res.snapshots_removed;
    }
    if (!fsx::sync_dir(dir_)) ++res.sync_errors;
    res.ok = true;
    return res;
}

}  // namespace learn
}  // namespace dd

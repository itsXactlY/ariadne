#include "dd/learn/ledger.hpp"

#include <sys/file.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "dd/learn/codec.hpp"
#include "dd/learn/fsutil.hpp"

namespace fs = std::filesystem;

namespace dd {
namespace learn {

Ledger::FileLock::~FileLock() {
    if (fd >= 0) ::close(fd);   // closing the descriptor releases the flock
}

namespace {

// Reads "<name> <n>" lines of the HEALTH file; missing or unreadable is all zeros.
void read_health(const fs::path& dir, Ledger::Health& h) {
    std::ifstream in(dir / "HEALTH");
    std::string key;
    uint64_t v = 0;
    while (in >> key >> v) {
        if (key == "sync_errors") h.sync_errors = v;
        else if (key == "write_errors") h.write_errors = v;
    }
}

}  // namespace

std::unique_ptr<Ledger> Ledger::open(const fs::path& dir, LedgerOptions opt, std::string* err) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (err) *err = "cannot create " + dir.string() + ": " + ec.message();
        return nullptr;
    }
    std::unique_ptr<Ledger> l(new Ledger);   // every return below releases the lock through it
    const fs::path lock_path = dir / "LOCK";
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (err) *err = "cannot open " + lock_path.string() + ": " + std::strerror(errno);
        return nullptr;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int why = errno;
        long pid = 0;
        char buf[32];
        if (const ssize_t n = ::pread(fd, buf, sizeof buf - 1, 0); n > 0) {
            buf[n] = '\0';
            pid = std::atol(buf);
        }
        ::close(fd);
        if (err)
            *err = why == EWOULDBLOCK ? "the learn ledger at " + dir.string() + " is in use by another ariadne process" +
                                            (pid > 0 ? " (pid " + std::to_string(pid) + ")" : std::string())
                                      : "cannot lock " + lock_path.string() + ": " + std::strerror(why);
        return nullptr;
    }
    l->lock_.fd = fd;
    if (::ftruncate(fd, 0) == 0) {   // who holds it, for the next one that asks
        const std::string me = std::to_string(::getpid()) + "\n";
        if (::pwrite(fd, me.data(), me.size(), 0) < 0) {}
    }
    // only the holder may clear what another run left half-done
    for (const auto& e : fs::directory_iterator(dir, ec))   // a temp file is a snapshot that never got renamed
        if (e.path().extension() == ".tmp") fs::remove(e.path(), ec);
    fs::remove(dir / "HEALTH", ec);   // a fresh run starts with a clean bill

    l->dir_ = dir, l->opt_ = std::move(opt);
    SnapshotLoad snap = load_snapshot(dir);
    if (snap.newer) {
        if (err) *err = "snapshot from a newer version in " + dir.string();
        return nullptr;
    }
    l->snapshots_corrupt_ = snap.corrupt;
    uint64_t floor = 0;
    if (snap.snap) {
        floor = snap.snap->last_seq;
        l->covered_ = snap.snap->covered_segment;
        l->live_ = std::move(snap.snap->stats);
    }
    l->journal_ = Journal::open(dir, l->opt_.journal, floor, l->covered_, err);
    if (!l->journal_) return nullptr;
    const ReplayInfo info = Journal::replay(dir, floor, ~uint64_t{0}, [&](uint64_t, const Record& r) { l->live_.apply(r); });
    l->recovered_ = info.records, l->replay_newer_ = info.newer, l->replay_undecodable_ = info.undecodable;
    l->publish();
    l->maybe_compact();
    return l;
}

bool Ledger::load_readonly(const fs::path& dir, Stats& out, Health* health, std::string* err) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        if (err) *err = "no ledger at " + dir.string();
        return false;
    }
    SnapshotLoad snap = load_snapshot(dir);
    if (snap.newer) {
        if (err) *err = "snapshot from a newer version in " + dir.string();
        return false;
    }
    uint64_t floor = 0;
    out = Stats();
    if (snap.snap) {
        floor = snap.snap->last_seq;
        out = std::move(snap.snap->stats);
    }
    const ReplayInfo info = Journal::replay(dir, floor, ~uint64_t{0}, [&](uint64_t, const Record& r) { out.apply(r); });
    if (health) {
        *health = Health();
        health->recovered = info.records, health->replay_newer = info.newer, health->replay_undecodable = info.undecodable;
        health->bad_segments = info.bad_segments, health->snapshots_corrupt = snap.corrupt;
        health->last_seq = std::max(info.last_seq, floor);
        read_health(dir, *health);
    }
    return true;
}

Ledger::~Ledger() {
    if (journal_) sync();
}

void Ledger::sync() {
    journal_->sync();
    note_health();
}

void Ledger::note_health() {
    const uint64_t sync_errors = journal_->counters().sync_errors.load(std::memory_order_relaxed) + compaction_sync_errors_.load(std::memory_order_relaxed);
    const uint64_t write_errors = journal_->counters().write_errors.load(std::memory_order_relaxed);
    if (sync_errors == health_sync_ && write_errors == health_write_) return;
    health_sync_ = sync_errors, health_write_ = write_errors;
    std::ostringstream o;
    o << "sync_errors " << sync_errors << "\nwrite_errors " << write_errors << "\n";
    fsx::write_atomic(dir_ / "HEALTH.tmp", dir_ / "HEALTH", o.str());
}

bool Ledger::record(const Record& r) {
    std::string payload;
    encode(r, payload);
    Record canon;
    if (decode(kCodecVersion, payload, canon) != Decode::Ok) return false;
    if (journal_->append(payload) == 0) {
        note_health();
        return false;
    }
    live_.apply(canon);
    if (++unpublished_ >= opt_.publish_every) publish();
    if (journal_->counters().rotations.load(std::memory_order_relaxed) != rotations_seen_) maybe_compact();
    return true;
}

void Ledger::publish() {
    cell_.publish(std::make_unique<const Stats>(live_));
    unpublished_ = 0;
}

void Ledger::tick() {
    journal_->maybe_sync();
    note_health();
    if (unpublished_ > 0) publish();
    maybe_compact();
}

void Ledger::maybe_compact() {
    rotations_seen_ = journal_->counters().rotations.load(std::memory_order_relaxed);
    const uint64_t active = journal_->active_segment();
    size_t due = 0;
    for (const uint64_t idx : Journal::segments(dir_))
        if (idx < active && idx > covered_) ++due;
    if (due >= opt_.compact_after && active > 1) compact_now();
}

void Ledger::compact_now() {
    const uint64_t active = journal_->active_segment();
    if (active <= 1) return;
    journal_->sync();
    const Compactor::Result r = Compactor(dir_, opt_.compact_hook).compact(active - 1);
    compaction_sync_errors_ += r.sync_errors;
    if (r.ok) covered_ = r.covered_segment, ++compactions_;
    else ++compaction_failures_;
}

Ledger::Health Ledger::health() const {
    Health h;
    const JournalCounters& c = journal_->counters();
    h.appended = c.appended, h.write_errors = c.write_errors, h.torn_bytes = c.torn_bytes, h.bad_segments = c.bad_segments;
    h.recovered = recovered_, h.replay_newer = replay_newer_, h.replay_undecodable = replay_undecodable_;
    h.snapshots_corrupt = snapshots_corrupt_, h.compactions = compactions_, h.compaction_failures = compaction_failures_;
    h.last_seq = journal_->last_seq();
    h.sync_errors = c.sync_errors + compaction_sync_errors_.load();
    return h;
}

}  // namespace learn
}  // namespace dd

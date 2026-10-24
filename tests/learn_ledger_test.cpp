// dd::learn durability: crc32c, the codec, the journal, the compactor and the ledger, including
// truncation at every byte, a kill at every compaction step, and a million-event run.
// Stats::apply (fork B's) is stubbed here with a small deterministic fold.
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "dd/core/crc32c.hpp"
#include "learn_tmp.hpp"
#include "dd/learn/codec.hpp"
#include "dd/learn/compactor.hpp"
#include "dd/learn/fsutil.hpp"
#include "dd/learn/journal.hpp"
#include "dd/learn/ledger.hpp"

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::learn;

namespace dd {
namespace learn {
double CardStats::conversion_lcb(int64_t) const { return 0.5; }
double CardStats::efficacy_lcb(int64_t) const { return 0.5; }
// The stub fold: counts per card, a tombstone drops the card. Reads no clock.
void Stats::apply(const Record& r) {
    ++records;
    if (auto* e = std::get_if<EventRec>(&r)) {
        const int64_t t = static_cast<int64_t>(e->e.ts_ns / 1000000000ull);
        CardStats& c = cards[e->e.uri.str()];
        c.offered.add(1, t, kHlConversion);
        c.last_used = std::max(c.last_used, t), now_hint = std::max(now_hint, t);
    } else if (auto* o = std::get_if<OutcomeRec>(&r)) {
        CardStats& c = cards[o->uri.str()];
        (o->outcome == Outcome::Helped ? c.helped : c.failed).add(o->weight, o->ts, kHlEfficacy);
        for (const auto& t : o->terms) assoc[o->uri.str() + '\x1f' + t].weight.add(1, o->ts, kHlAssoc);
        now_hint = std::max(now_hint, o->ts);
    } else if (auto* s = std::get_if<StateRec>(&r)) {
        cards[s->uri.str()].state = s->to;
        now_hint = std::max(now_hint, s->ts);
    } else if (auto* d = std::get_if<DraftRec>(&r)) {
        cards["skill:" + d->slug].first_seen = d->ts;
    } else if (auto* t = std::get_if<TombstoneRec>(&r)) {
        cards.erase(t->uri.str());
    }
}
}  // namespace learn
}  // namespace dd

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", label.c_str());
    }
}

static fs::path base_dir() {
    static const fs::path p = std::getenv("DD_TEST_DIR") ? fs::path(std::getenv("DD_TEST_DIR")) : learn_test_dir("learn_ledger_tmp");
    return p;
}
static fs::path fresh(const std::string& name) {
    const fs::path p = base_dir() / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

static uint64_t g_rng = 88172645463325252ull;
static uint64_t rnd() {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return g_rng;
}

static know::Uri uri_of(uint64_t i) {
    know::Uri u;
    u.kind = static_cast<know::Kind>(i % 5);
    u.id = "card-" + std::to_string(i % 1000);
    if (i % 7 == 0) u.path = "references/x" + std::to_string(i % 13) + ".md";
    if (i % 11 == 0) u.page = "2";
    return u;
}

static Record make_record(uint64_t i) {
    switch (i % 5) {
        case 0: {
            know::Event e;
            e.type = static_cast<know::Event::Type>(i % 10);
            e.ts_ns = 1700000000ull * 1000000000ull + i * 1000000ull, e.turn = static_cast<uint32_t>(i / 3), e.round = i % 7, e.epoch = i % 5;
            e.uri = uri_of(i), e.where = static_cast<know::Placement>(i % 4);
            e.reason = "failure", e.key = "ENOENT <path>", e.terms = {"podman", "quadlet", "unit"};
            e.score = 0.5f + (i % 10) / 20.0f, e.mass = 7.25f, e.matched = static_cast<int>(i % 4);
            if (e.type == know::Event::Type::Calls) {
                e.call.tool = "run_terminal", e.call.intent = "start the unit", e.call.args = "systemctl --user start x";
                e.call.digest = "run_terminal|systemctl", e.call.paths = {"/a/b", "/c"}, e.call.binaries = {"systemctl"};
                e.call.failed = i % 2, e.call.refused = i % 3 == 0, e.call.exploring = i % 4 == 0, e.call.empty = i % 5 == 0;
                e.call.exit = static_cast<int>(i % 3) - 1, e.call.error = "unit not found", e.call.bytes = i * 11;
            }
            return EventRec{e};
        }
        case 1: {
            OutcomeRec o;
            o.uri = uri_of(i), o.outcome = static_cast<Outcome>(i % 5), o.trigger = "shift", o.weight = (i % 2) ? 1.0f : 0.5f;
            o.terms = {"alpha", "beta"}, o.episode = static_cast<uint32_t>(i), o.ts = 1700000000 + static_cast<int64_t>(i);
            return o;
        }
        case 2: return StateRec{uri_of(i), State::Draft, static_cast<State>(i % 6), "evidence", 1700000000 + static_cast<int64_t>(i)};
        case 3: return DraftRec{"slug-" + std::to_string(i), "learned", "abcdef0123", "ok", i % 2 == 0, 1700000000 + static_cast<int64_t>(i)};
        default: return TombstoneRec{uri_of(i / 3), 1700000000 + static_cast<int64_t>(i)};
    }
}

static std::string enc(const Record& r) {
    std::string b;
    encode(r, b);
    return b;
}

static Stats fold(const std::vector<Record>& rs, size_t n) {
    Stats s;
    for (size_t i = 0; i < n; ++i) s.apply(canonical(rs[i]));
    return s;
}

// ---------- crc ----------
static void test_crc() {
    const char* v = "123456789";
    chk(core::crc32c_table(v, 9) == 0xE3069283u, "crc table check value");
    chk(core::crc32c(v, 9) == 0xE3069283u, "crc dispatched check value");
    std::vector<uint8_t> buf(4096);
    for (auto& b : buf) b = static_cast<uint8_t>(rnd());
    bool eq = true, chain = true;
    for (size_t off = 0; off < 9; ++off)
        for (size_t n : {0, 1, 2, 7, 8, 9, 15, 16, 17, 63, 64, 65, 1000, 4000}) {
            if (off + n > buf.size()) continue;
            eq &= core::crc32c(buf.data() + off, n) == core::crc32c_table(buf.data() + off, n);
            const size_t h = n / 3;
            chain &= core::crc32c(buf.data() + off + h, n - h, core::crc32c(buf.data() + off, h)) == core::crc32c(buf.data() + off, n);
        }
    chk(eq, "crc hw == table on random data");
    chk(chain, "crc chains");
    const std::string level = core::crc32c_level();
    chk(core::crc32c_force("table") && core::crc32c(v, 9) == 0xE3069283u, "crc forced table");
    if (level == "sse4.2") chk(core::crc32c_force("sse4.2") && core::crc32c(v, 9) == 0xE3069283u, "crc forced hw");
    chk(!core::crc32c_force("bogus"), "crc unknown level refused");
    std::printf("crc level %s\n", level.c_str());
}

// ---------- codec ----------
static void test_codec() {
    for (uint64_t i = 0; i < 400; ++i) {
        const Record r = make_record(i);
        const std::string a = enc(r);
        Record back;
        chk(decode(kCodecVersion, a, back) == Decode::Ok, "decode ok " + std::to_string(i));
        chk(enc(back) == a, "round trip bytes " + std::to_string(i));
        chk(back.index() == r.index(), "round trip alternative " + std::to_string(i));
        chk(decode(kCodecVersion + 1, a, back) == Decode::Newer, "newer version");
        chk(decode(0, a, back) == Decode::Corrupt, "version 0");
        bool any_ok = false;
        for (size_t n = 0; n < a.size(); ++n) any_ok |= decode(kCodecVersion, std::string_view(a).substr(0, n), back) == Decode::Ok;
        chk(!any_ok, "no prefix of a payload decodes " + std::to_string(i));
        chk(decode(kCodecVersion, a + "x", back) == Decode::Corrupt, "trailing byte refused");
    }
    // field spot checks
    {
        know::Event ev;
        ev.type = know::Event::Type::Calls, ev.call.tool = "t", ev.call.failed = true, ev.call.exit = -3, ev.call.bytes = 123456789, ev.score = 0.25f;
        ev.uri = {know::Kind::Memory, "1290233", "", ""}, ev.terms = {"é", "b"};
        Record back;
        decode(kCodecVersion, enc(EventRec{ev}), back);
        const know::Event& g = std::get<EventRec>(back).e;
        chk(g.call.tool == "t" && g.call.failed && g.call.exit == -3 && g.call.bytes == 123456789 && g.score == 0.25f && g.uri.id == "1290233" && g.terms[0] == "é", "event fields");
    }
    // bounds: a long string is cut on a character boundary, a long list is cut
    {
        OutcomeRec o;
        o.trigger = std::string(kMaxStr - 1, 'a') + "é" + std::string(100, 'b');   // 'é' straddles the cap
        for (int i = 0; i < 100; ++i) o.terms.push_back("t" + std::to_string(i));
        Record back;
        chk(decode(kCodecVersion, enc(o), back) == Decode::Ok, "bounded decodes");
        const OutcomeRec& g = std::get<OutcomeRec>(back);
        chk(g.trigger.size() == kMaxStr - 1 && g.trigger.find("é") == std::string::npos, "string cut before a split character");
        chk(g.terms.size() == kMaxList, "list cut");
        // a length field beyond the bounds is corrupt, not an allocation
        std::string bad;
        bad.push_back(1);   // OutcomeRec
        bad.push_back(0), bad.push_back(0), bad.push_back(0), bad.push_back(0);   // uri: kind, id, path, page
        bad.push_back(0);   // outcome
        bad.append("\xff\xff\xff\xff\x0f", 5);   // trigger length ~4 GiB
        chk(decode(kCodecVersion, bad, back) == Decode::Corrupt, "huge length is corrupt");
    }
    // garbage never crashes and never decodes into something that does not re-encode the same
    for (int i = 0; i < 30000; ++i) {
        std::string g(1 + rnd() % 80, 0);
        for (auto& c : g) c = static_cast<char>(rnd());
        g[0] = static_cast<char>(rnd() % 6);
        Record r;
        if (decode(kCodecVersion, g, r) == Decode::Ok) {
            Record r2;
            chk(decode(kCodecVersion, enc(r), r2) == Decode::Ok && enc(r2) == enc(r), "garbage that decodes is stable");
        }
    }
    // stats
    {
        std::vector<Record> rs;
        for (uint64_t i = 0; i < 500; ++i) rs.push_back(make_record(i));
        const Stats a = fold(rs, rs.size());
        const std::string bytes = encode_stats(a);
        Stats b;
        chk(decode_stats(kStatsVersion, bytes, b) == Decode::Ok, "stats decode");
        chk(encode_stats(b) == bytes, "stats round trip");
        chk(b.cards.size() == a.cards.size() && b.assoc.size() == a.assoc.size() && b.records == a.records, "stats sizes");
        Stats d;
        std::vector<std::pair<std::string, CardStats>> items(a.cards.begin(), a.cards.end());
        for (auto it = items.rbegin(); it != items.rend(); ++it) d.cards.emplace(it->first, it->second);
        d.assoc = a.assoc, d.records = a.records, d.now_hint = a.now_hint;
        chk(encode_stats(d) == bytes, "stats bytes do not depend on insertion order");
        chk(decode_stats(kStatsVersion + 1, bytes, b) == Decode::Newer, "stats newer");
        bool any = false;
        for (size_t n = 0; n < bytes.size(); n += 7) any |= decode_stats(kStatsVersion, std::string_view(bytes).substr(0, n), b) == Decode::Ok;
        chk(!any, "no prefix of stats decodes");
    }
}

// ---------- journal / ledger ----------
static LedgerOptions opts_fast(size_t seg = 8u << 20) {
    LedgerOptions o;
    o.journal.segment_bytes = seg;
    o.journal.sync_records = 1u << 30;
    o.journal.sync_interval = std::chrono::hours(1);
    o.compact_after = 1u << 30;
    return o;
}

static std::string read_all(const fs::path& p) {
    std::string s;
    fsx::read_file(p, s);
    return s;
}
static void write_file(const fs::path& p, const std::string& s) {
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    fsx::write_all(fd, s.data(), s.size());
    ::close(fd);
}

static void test_truncation_and_flips() {
    const fs::path src = fresh("trunc_src");
    std::vector<Record> rs;
    std::vector<size_t> ends;   // byte offset after each frame
    {
        auto l = Ledger::open(src, opts_fast());
        for (uint64_t i = 0; i < 24; ++i) {
            rs.push_back(make_record(i));
            chk(l->record(rs.back()), "record");
            ends.push_back(read_all(Journal::segment_path(src, 1)).size());
        }
    }
    const std::string bytes = read_all(Journal::segment_path(src, 1));
    chk(bytes.size() == ends.back(), "frame offsets add up");
    const fs::path dst = fresh("trunc_dst");
    for (size_t cut = 0; cut <= bytes.size(); ++cut) {
        size_t k = 0;
        while (k < ends.size() && ends[k] <= cut) ++k;
        std::error_code ec;
        fs::remove_all(dst, ec), fs::create_directories(dst, ec);
        write_file(Journal::segment_path(dst, 1), bytes.substr(0, cut));
        auto l = Ledger::open(dst, opts_fast());
        if (!l) {
            chk(false, "open after cut " + std::to_string(cut));
            continue;
        }
        const size_t good = k ? ends[k - 1] : 0;
        const bool state_ok = encode_stats(l->live()) == encode_stats(fold(rs, k));
        const bool size_ok = fs::file_size(Journal::segment_path(dst, 1)) == good;
        if (!state_ok || !size_ok || l->health().recovered != k) chk(false, "cut at " + std::to_string(cut) + " recovered " + std::to_string(l->health().recovered) + " want " + std::to_string(k));
        if (cut % 61 == 0) {   // and the journal goes on from there
            chk(l->record(make_record(100)), "append after recovery");
            l.reset();
            auto m = Ledger::open(dst, opts_fast());
            auto want = rs;
            want.resize(k);
            want.push_back(make_record(100));
            chk(m && encode_stats(m->live()) == encode_stats(fold(want, want.size())), "reopen after append at cut " + std::to_string(cut));
        }
    }
    // one flipped bit anywhere: everything before the damaged frame is recovered, nothing after it, nothing garbled
    for (size_t at = 0; at < bytes.size(); ++at) {
        std::string b = bytes;
        b[at] = static_cast<char>(b[at] ^ (1 << (at % 8)));
        std::error_code ec;
        fs::remove_all(dst, ec), fs::create_directories(dst, ec);
        write_file(Journal::segment_path(dst, 1), b);
        auto l = Ledger::open(dst, opts_fast());
        size_t frame = 0;
        while (frame < ends.size() && ends[frame] <= at) ++frame;   // the frame holding byte `at`
        if (!l || encode_stats(l->live()) != encode_stats(fold(rs, frame))) chk(false, "flip at " + std::to_string(at));
    }
}

static void test_newer_and_refusal() {
    // a frame from a newer codec is skipped and counted; the seq still advances
    const fs::path d = fresh("newer_frame");
    {
        std::string seg;
        std::string p;
        encode(make_record(1), p);
        frame_encode(seg, kCodecVersion, 1, p);
        frame_encode(seg, kCodecVersion + 1, 2, "future bytes");
        frame_encode(seg, kCodecVersion, 3, p);
        write_file(Journal::segment_path(d, 1), seg);
    }
    auto l = Ledger::open(d, opts_fast());
    chk(l && l->health().recovered == 2 && l->health().replay_newer == 1 && l->health().last_seq == 3, "newer frame skipped and counted");
    l->record(make_record(2));
    chk(l->health().last_seq == 4, "seq goes on past a skipped frame");

    // a snapshot from a newer version: the directory is left alone
    const fs::path s = fresh("newer_snapshot");
    {
        auto m = Ledger::open(s, opts_fast(300));
        for (uint64_t i = 0; i < 40; ++i) m->record(make_record(i));
        m->compact_now();
    }
    const auto snaps = [&] {
        std::vector<fs::path> v;
        for (auto& e : fs::directory_iterator(s))
            if (e.path().extension() == ".dat") v.push_back(e.path());
        return v;
    }();
    chk(snaps.size() == 1, "one snapshot");
    std::string b = read_all(snaps[0]);
    const uint32_t newer = kStatsVersion + 1;
    std::memcpy(&b[4], &newer, 4);
    write_file(snaps[0], b);
    std::string err;
    chk(!Ledger::open(s, opts_fast(300), &err) && err.find("newer") != std::string::npos, "newer snapshot refuses to open");
    chk(read_all(snaps[0]) == b, "and leaves it untouched");

    // a damaged snapshot is skipped and counted
    const fs::path c = fresh("corrupt_snapshot");
    {
        auto m = Ledger::open(c, opts_fast(300));
        for (uint64_t i = 0; i < 40; ++i) m->record(make_record(i));
        m->compact_now();
    }
    for (auto& e : fs::directory_iterator(c))
        if (e.path().extension() == ".dat") {
            std::string x = read_all(e.path());
            x[x.size() / 2] ^= 0x40;
            write_file(e.path(), x);
        }
    auto m = Ledger::open(c, opts_fast(300));
    chk(m && m->health().snapshots_corrupt == 1, "damaged snapshot counted");
}

static std::vector<std::string> listing(const fs::path& d) {
    std::vector<std::string> v;
    for (auto& e : fs::directory_iterator(d)) v.push_back(e.path().filename().string());
    std::sort(v.begin(), v.end());
    return v;
}

static void test_compaction_kills() {
    constexpr uint64_t N = 120;
    std::vector<Record> rs;
    for (uint64_t i = 0; i < N; ++i) rs.push_back(make_record(i));
    const std::string want = encode_stats(fold(rs, rs.size()));

    auto build = [&](const std::string& name) {
        const fs::path d = fresh(name);
        auto l = Ledger::open(d, opts_fast(1500));   // many small segments, no automatic compaction
        for (const auto& r : rs) l->record(r);
        return d;
    };

    // count the steps of an uninterrupted compaction
    int steps = 0;
    {
        const fs::path d = build("kill_count");
        const uint64_t last = Journal::segments(d).back();
        const auto r = Compactor(d, [&](const char*) { return ++steps, true; }).compact(last - 1);
        chk(r.ok && r.segments_removed >= 3, "compaction ran");
        auto l = Ledger::open(d, opts_fast(1500));
        chk(l && encode_stats(l->live()) == want, "clean compaction keeps the fold");
        chk(Journal::segments(d).size() == 1, "only the active segment is left");
    }
    chk(steps > 8, "enough steps to kill at");
    // kill at every step; then reopen; then a second (clean) compaction; the fold never moves
    for (int k = 1; k <= steps; ++k) {
        const fs::path d = build("kill_" + std::to_string(k));
        const uint64_t last = Journal::segments(d).back();
        int n = 0;
        const auto r = Compactor(d, [&](const char*) { return ++n < k; }).compact(last - 1);
        chk(r.aborted, "aborted at " + std::to_string(k));
        auto l = Ledger::open(d, opts_fast(1500));
        chk(l && encode_stats(l->live()) == want, "fold after a kill at step " + std::to_string(k));
        for (auto& f : listing(d)) chk(f.find(".tmp") == std::string::npos, "no temp file after open (step " + std::to_string(k) + ")");
        if (l) {
            l->compact_now();
            chk(encode_stats(l->live()) == want, "live after the next compaction, kill " + std::to_string(k));
            l.reset();
            auto m = Ledger::open(d, opts_fast(1500));
            chk(m && encode_stats(m->live()) == want, "reopen after the next compaction, kill " + std::to_string(k));
            chk(m && Journal::segments(d).size() == 1, "segments retired, kill " + std::to_string(k));
        }
    }
}

static void test_rotation_and_threads() {
    const fs::path d = fresh("rotate");
    LedgerOptions o = opts_fast(2000);
    o.compact_after = 2;
    o.publish_every = 50;
    std::vector<Record> rs;
    std::string want;
    {
        auto l = Ledger::open(d, o);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> reads{0};
        std::thread reader([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto r = l->snapshot();
                if (r) reads += r->cards.size() + 1;
            }
        });
        size_t max_segs = 0;
        for (uint64_t i = 0; i < 3000; ++i) {
            rs.push_back(make_record(i));
            chk(l->record(rs.back()), "record in rotation");
            max_segs = std::max(max_segs, Journal::segments(d).size());
        }
        l->tick();
        stop = true;
        reader.join();
        chk(max_segs <= 4, "segments stay bounded while compacting (max " + std::to_string(max_segs) + ")");
        chk(l->health().compactions > 3 && l->health().compaction_failures == 0, "compactions ran");
        want = encode_stats(fold(rs, rs.size()));
        chk(encode_stats(l->live()) == want, "live == fold");
        auto snap = l->snapshot();
        chk(snap && encode_stats(*snap) == want, "published copy == live after tick");
    }
    auto l = Ledger::open(d, o);
    chk(l && encode_stats(l->live()) == want, "recovered == live after rotation and compaction");
    const uint64_t before = l->health().last_seq;
    l->record(make_record(7));
    chk(l->health().last_seq == before + 1 && before == 3000, "seq continues across a restart");
}

static uint64_t dir_bytes(const fs::path& d) {
    uint64_t n = 0;
    for (auto& e : fs::directory_iterator(d)) n += e.is_regular_file() ? e.file_size() : 0;
    return n;
}

static void test_million() {
    const fs::path d = fresh("million");
    LedgerOptions o = opts_fast(1u << 20);
    o.compact_after = 2;
    o.publish_every = 100000;
    const uint64_t N = std::getenv("DD_LEDGER_N") ? std::strtoull(std::getenv("DD_LEDGER_N"), nullptr, 10) : 1000000;   // sanitizer runs shrink it
    uint64_t max_bytes = 0;
    size_t max_segs = 0;
    std::string want;
    const auto t0 = std::chrono::steady_clock::now();
    {
        auto l = Ledger::open(d, o);
        for (uint64_t i = 0; i < N; ++i) {
            know::Event e;
            e.type = i % 3 == 0 ? know::Event::Type::Offered : know::Event::Type::OpenedByModel;
            e.ts_ns = 1700000000ull * 1000000000ull + i * 1000000000ull, e.turn = static_cast<uint32_t>(i / 10);
            e.uri = {know::Kind::Skill, "card-" + std::to_string(i % 1000), "", ""}, e.reason = "turn", e.key = "k" + std::to_string(i % 97), e.terms = {"a", "b", "c"};
            l->record(EventRec{e});
            if (i % 5000 == 0) {
                max_bytes = std::max(max_bytes, dir_bytes(d));
                max_segs = std::max(max_segs, Journal::segments(d).size());
            }
        }
        want = encode_stats(l->live());
        chk(l->health().appended == N, "all appended");
        chk(l->live().cards.size() <= 1000, "live cards stay at the distinct count");
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("million: %.1f s, max dir %llu KiB, max segments %zu\n", secs, static_cast<unsigned long long>(max_bytes >> 10), max_segs);
    chk(max_segs <= 4, "million: segments bounded");
    chk(max_bytes < 5u * (1u << 20), "million: disk under 5 MiB");
    auto l = Ledger::open(d, o);
    chk(l && encode_stats(l->live()) == want && l->health().last_seq == N, "million: recovery == live");
    chk(l && l->health().recovered < N / 5 + 10000, "million: recovery replays only the tail");
}

static void test_lock_and_health() {
    const fs::path d = fresh("lock");
    std::string err;
    {
        auto a = Ledger::open(d, opts_fast(), &err);
        chk(a != nullptr, "lock: the first writer opens: " + err);
        std::string err2;
        auto b = Ledger::open(d, opts_fast(), &err2);
        chk(!b, "lock: a second writer on the same directory is refused");
        chk(err2.find("in use by another dd process") != std::string::npos && err2.find("pid " + std::to_string(::getpid())) != std::string::npos,
            "lock: the refusal names the holder's pid: " + err2);
        struct stat st {};
        chk(::stat((d / "LOCK").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "lock: LOCK is private (0600)");
        std::ifstream in(d / "LOCK");
        long pid = 0;
        in >> pid;
        chk(pid == ::getpid(), "lock: LOCK holds the pid");
        a->record(make_record(1));
        a->sync();
        Stats s;
        Ledger::Health h;
        chk(Ledger::load_readonly(d, s, &h) && s.records == 1, "lock: the read-only load works beside the writer");
        chk(h.sync_errors == 0 && h.write_errors == 0, "lock: a clean run reports no errors");
        // a refused open touched nothing the holder owns
        chk(a->record(make_record(2)), "lock: the holder still writes after a refused open");
    }
    auto c = Ledger::open(d, opts_fast(), &err);
    chk(c != nullptr, "lock: released with the ledger: " + err);
    chk(c && c->live().records == 2, "lock: the new holder sees both records");

    // a refused open must not clear another run's temp file or its HEALTH
    auto e = Ledger::open(d, opts_fast());
    chk(!e, "lock: still refused while c lives");
    std::ofstream(d / "snap.tmp") << "x";
    chk(!Ledger::open(d, opts_fast()) && fs::exists(d / "snap.tmp"), "lock: the loser of the race does not remove the holder's temp file");
    c.reset();

    // HEALTH: what a run that could not sync leaves for `dd learn status`; a fresh run starts clean
    std::ofstream(d / "HEALTH") << "sync_errors 3\nwrite_errors 2\n";
    Stats s;
    Ledger::Health h;
    chk(Ledger::load_readonly(d, s, &h) && h.sync_errors == 3 && h.write_errors == 2, "health: the read-only load reports the last run's errors");
    auto f = Ledger::open(d, opts_fast(), &err);
    chk(f && !fs::exists(d / "HEALTH") && !fs::exists(d / "snap.tmp"), "health: a fresh holder clears HEALTH and stale temp files");
    chk(f && f->health().sync_errors == 0, "health: counters start at zero");
}

int main() {
    std::error_code ec;
    fs::create_directories(base_dir(), ec);
    test_crc();
    test_codec();
    test_truncation_and_flips();
    test_newer_and_refusal();
    test_compaction_kills();
    test_rotation_and_threads();
    test_lock_and_health();
    test_million();
    fs::remove_all(base_dir(), ec);
    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("learn ledger: all passed\n");
    return 0;
}

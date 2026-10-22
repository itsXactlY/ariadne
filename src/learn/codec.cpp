#include "dd/learn/codec.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

namespace dd {
namespace learn {
namespace {

class Out {
public:
    explicit Out(std::string& s) : s_(s) {}
    void u8(uint8_t v) { s_.push_back(static_cast<char>(v)); }
    void var(uint64_t v) {
        while (v >= 0x80) u8(static_cast<uint8_t>(v) | 0x80), v >>= 7;
        u8(static_cast<uint8_t>(v));
    }
    void svar(int64_t v) { var((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
    void f32(float v) {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        raw(&b, 4);
    }
    void f64(double v) {
        uint64_t b;
        std::memcpy(&b, &v, 8);
        raw(&b, 8);
    }
    void str(std::string_view v, size_t cap = kMaxStr) {
        size_t n = std::min(v.size(), cap);
        while (n > 0 && n < v.size() && (static_cast<uint8_t>(v[n]) & 0xC0) == 0x80) --n;   // not inside a UTF-8 character
        var(n);
        s_.append(v.data(), n);
    }
    void list(const std::vector<std::string>& v) {
        const size_t n = std::min(v.size(), kMaxList);
        var(n);
        for (size_t i = 0; i < n; ++i) str(v[i]);
    }

private:
    void raw(const void* p, size_t n) { s_.append(static_cast<const char*>(p), n); }
    std::string& s_;
};

class In {
public:
    explicit In(std::string_view s) : p_(s.data()), end_(s.data() + s.size()) {}
    bool ok() const { return ok_; }
    bool done() const { return p_ == end_; }
    uint8_t u8() {
        if (p_ == end_) return fail(), 0;
        return static_cast<uint8_t>(*p_++);
    }
    uint64_t var() {
        uint64_t v = 0;
        for (int sh = 0; sh < 70; sh += 7) {
            const uint8_t b = u8();
            if (!ok_) return 0;
            if (sh == 63 && b > 1) return fail(), 0;
            v |= static_cast<uint64_t>(b & 0x7F) << sh;
            if (!(b & 0x80)) return v;
        }
        return fail(), 0;
    }
    int64_t svar() {
        const uint64_t v = var();
        return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
    }
    float f32() {
        uint32_t b = 0;
        raw(&b, 4);
        float v;
        std::memcpy(&v, &b, 4);
        return v;
    }
    double f64() {
        uint64_t b = 0;
        raw(&b, 8);
        double v;
        std::memcpy(&v, &b, 8);
        return v;
    }
    std::string str(size_t cap = kMaxStr) {
        const uint64_t n = var();
        if (!ok_ || n > cap || n > static_cast<uint64_t>(end_ - p_)) return fail(), std::string();
        std::string v(p_, n);
        p_ += n;
        return v;
    }
    size_t count(size_t cap) {
        const uint64_t n = var();
        if (n > cap) return fail(), 0;
        return static_cast<size_t>(n);
    }
    std::vector<std::string> list() {
        std::vector<std::string> v;
        const size_t n = count(kMaxList);
        for (size_t i = 0; i < n && ok_; ++i) v.push_back(str());
        return v;
    }
    uint8_t enumv(uint8_t max) {
        const uint8_t v = u8();
        if (v > max) return fail(), 0;
        return v;
    }

private:
    void raw(void* d, size_t n) {
        if (static_cast<size_t>(end_ - p_) < n) return fail();
        std::memcpy(d, p_, n);
        p_ += n;
    }
    void fail() { ok_ = false, p_ = end_; }
    const char* p_;
    const char* end_;
    bool ok_ = true;
};

void put_uri(Out& o, const know::Uri& u) {
    o.u8(static_cast<uint8_t>(u.kind));
    o.str(u.id), o.str(u.path), o.str(u.page);
}
know::Uri get_uri(In& in) {
    know::Uri u;
    u.kind = static_cast<know::Kind>(in.enumv(static_cast<uint8_t>(know::Kind::Rule)));
    u.id = in.str(), u.path = in.str(), u.page = in.str();
    return u;
}

void put_call(Out& o, const know::CallRecord& c) {
    o.str(c.tool), o.str(c.intent), o.str(c.args), o.str(c.digest);
    o.list(c.paths), o.list(c.binaries);
    o.u8(static_cast<uint8_t>((c.failed ? 1 : 0) | (c.refused ? 2 : 0) | (c.exploring ? 4 : 0) | (c.empty ? 8 : 0)));
    o.svar(c.exit), o.str(c.error), o.var(c.bytes);
}
know::CallRecord get_call(In& in) {
    know::CallRecord c;
    c.tool = in.str(), c.intent = in.str(), c.args = in.str(), c.digest = in.str();
    c.paths = in.list(), c.binaries = in.list();
    const uint8_t f = in.u8();
    c.failed = f & 1, c.refused = f & 2, c.exploring = f & 4, c.empty = f & 8;
    c.exit = static_cast<int>(in.svar()), c.error = in.str(), c.bytes = static_cast<size_t>(in.var());
    return c;
}

void put_event(Out& o, const know::Event& e) {
    o.u8(static_cast<uint8_t>(e.type)), o.var(e.ts_ns), o.var(e.turn), o.var(e.round), o.var(e.epoch);
    put_uri(o, e.uri);
    o.u8(static_cast<uint8_t>(e.where)), o.str(e.reason), o.str(e.key), o.list(e.terms);
    o.f32(e.score), o.f32(e.mass), o.svar(e.matched);
    if (e.type == know::Event::Type::Calls) put_call(o, e.call);
}
know::Event get_event(In& in) {
    know::Event e;
    e.type = static_cast<know::Event::Type>(in.enumv(static_cast<uint8_t>(know::Event::Type::Shadow)));
    e.ts_ns = in.var(), e.turn = static_cast<uint32_t>(in.var()), e.round = static_cast<uint32_t>(in.var()), e.epoch = static_cast<uint32_t>(in.var());
    e.uri = get_uri(in);
    e.where = static_cast<know::Placement>(in.enumv(static_cast<uint8_t>(know::Placement::Anchor)));
    e.reason = in.str(), e.key = in.str(), e.terms = in.list();
    e.score = in.f32(), e.mass = in.f32(), e.matched = static_cast<int>(in.svar());
    if (e.type == know::Event::Type::Calls) e.call = get_call(in);
    return e;
}

void put_decayed(Out& o, const Decayed& d) { o.f64(d.v), o.svar(d.t); }
Decayed get_decayed(In& in) {
    Decayed d;
    d.v = in.f64(), d.t = in.svar();
    return d;
}

}  // namespace

void encode(const Record& r, std::string& out) {
    Out o(out);
    o.u8(static_cast<uint8_t>(r.index()));
    if (auto* v = std::get_if<EventRec>(&r)) {
        put_event(o, v->e);
    } else if (auto* v = std::get_if<OutcomeRec>(&r)) {
        put_uri(o, v->uri);
        o.u8(static_cast<uint8_t>(v->outcome)), o.str(v->trigger), o.f32(v->weight), o.list(v->terms), o.var(v->episode), o.svar(v->ts);
    } else if (auto* v = std::get_if<StateRec>(&r)) {
        put_uri(o, v->uri);
        o.u8(static_cast<uint8_t>(v->from)), o.u8(static_cast<uint8_t>(v->to)), o.str(v->why), o.svar(v->ts);
    } else if (auto* v = std::get_if<DraftRec>(&r)) {
        o.str(v->slug), o.str(v->category), o.str(v->digest), o.str(v->verdict), o.u8(v->accepted ? 1 : 0), o.svar(v->ts);
    } else if (auto* v = std::get_if<TombstoneRec>(&r)) {
        put_uri(o, v->uri);
        o.svar(v->ts);
    }
}

Decode decode(uint8_t version, std::string_view payload, Record& out) {
    if (version > kCodecVersion) return Decode::Newer;
    if (version == 0 || payload.size() > kMaxPayload) return Decode::Corrupt;
    In in(payload);
    const uint8_t tag = in.u8();
    switch (tag) {
        case 0: out = EventRec{get_event(in)}; break;
        case 1: {
            OutcomeRec v;
            v.uri = get_uri(in);
            v.outcome = static_cast<Outcome>(in.enumv(static_cast<uint8_t>(Outcome::Vetoed)));
            v.trigger = in.str(), v.weight = in.f32(), v.terms = in.list(), v.episode = static_cast<uint32_t>(in.var()), v.ts = in.svar();
            out = std::move(v);
            break;
        }
        case 2: {
            StateRec v;
            v.uri = get_uri(in);
            v.from = static_cast<State>(in.enumv(static_cast<uint8_t>(State::Rejected)));
            v.to = static_cast<State>(in.enumv(static_cast<uint8_t>(State::Rejected)));
            v.why = in.str(), v.ts = in.svar();
            out = std::move(v);
            break;
        }
        case 3: {
            DraftRec v;
            v.slug = in.str(), v.category = in.str(), v.digest = in.str(), v.verdict = in.str(), v.accepted = in.enumv(1) != 0, v.ts = in.svar();
            out = std::move(v);
            break;
        }
        case 4: {
            TombstoneRec v;
            v.uri = get_uri(in);
            v.ts = in.svar();
            out = std::move(v);
            break;
        }
        default: return Decode::Corrupt;
    }
    return in.ok() && in.done() ? Decode::Ok : Decode::Corrupt;
}

Record canonical(const Record& r) {
    std::string b;
    encode(r, b);
    Record out;
    decode(kCodecVersion, b, out);
    return out;
}

std::string encode_stats(const Stats& s) {
    std::string out;
    Out o(out);
    o.var(s.records), o.svar(s.now_hint);
    std::vector<const std::pair<const std::string, CardStats>*> cards;
    cards.reserve(s.cards.size());
    for (const auto& kv : s.cards)
        if (kv.first.size() <= kMaxKey) cards.push_back(&kv);
    std::sort(cards.begin(), cards.end(), [](auto* a, auto* b) { return a->first < b->first; });
    o.var(cards.size());
    for (auto* kv : cards) {
        const CardStats& c = kv->second;
        o.str(kv->first, kMaxKey);
        put_decayed(o, c.offered), put_decayed(o, c.opened), put_decayed(o, c.helped), put_decayed(o, c.failed), put_decayed(o, c.shadow);
        o.u8(static_cast<uint8_t>(c.state)), o.svar(c.first_seen), o.svar(c.last_used);
    }
    size_t n = 0;
    for (const auto& kv : s.assoc) n += kv.first.size() <= kMaxKey;
    o.var(n);
    for (const auto& kv : s.assoc) {
        if (kv.first.size() > kMaxKey) continue;
        const Assoc& a = kv.second;
        o.str(kv.first, kMaxKey);
        put_decayed(o, a.weight);
        o.var(a.episodes), o.var(a.last_episode), o.var(a.days), o.svar(a.last_day);
    }
    return out;
}

Decode decode_stats(uint32_t version, std::string_view body, Stats& out) {
    if (version > kStatsVersion) return Decode::Newer;
    if (version == 0) return Decode::Corrupt;
    In in(body);
    Stats s;
    s.records = in.var(), s.now_hint = in.svar();
    size_t n = in.count(1u << 24);
    for (size_t i = 0; i < n && in.ok(); ++i) {
        std::string k = in.str(kMaxKey);
        CardStats c;
        c.offered = get_decayed(in), c.opened = get_decayed(in), c.helped = get_decayed(in), c.failed = get_decayed(in), c.shadow = get_decayed(in);
        c.state = static_cast<State>(in.enumv(static_cast<uint8_t>(State::Rejected)));
        c.first_seen = in.svar(), c.last_used = in.svar();
        s.cards.emplace(std::move(k), c);
    }
    n = in.count(1u << 24);
    for (size_t i = 0; i < n && in.ok(); ++i) {
        std::string k = in.str(kMaxKey);
        Assoc a;
        a.weight = get_decayed(in);
        a.episodes = static_cast<uint32_t>(in.var()), a.last_episode = static_cast<uint32_t>(in.var());
        a.days = static_cast<uint16_t>(in.var()), a.last_day = in.svar();
        s.assoc.emplace(std::move(k), a);
    }
    if (!in.ok() || !in.done()) return Decode::Corrupt;
    out = std::move(s);
    return Decode::Ok;
}

}  // namespace learn
}  // namespace dd

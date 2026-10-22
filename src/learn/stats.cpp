// dd::learn::Stats — the deterministic fold of the journal. It never reads the wall clock.
#include "dd/learn/stats.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dd {
namespace learn {

namespace {

constexpr size_t kMaxAssoc = 65536;   // past 1.25x this, the weakest associations go
constexpr double kZ = 1.96;

std::string card_key(const know::Uri& u) {
    know::Uri k;
    k.kind = u.kind;
    k.id = u.id;
    return k.str();
}

// Wilson lower bound of k successes in n trials; 0.5 with no evidence.
double wilson(double k, double n) {
    if (n < 1e-9) return 0.5;
    k = std::clamp(k, 0.0, n);
    const double p = k / n, z2 = kZ * kZ;
    const double centre = p + z2 / (2 * n);
    const double spread = kZ * std::sqrt(p * (1 - p) / n + z2 / (4 * n * n));
    return std::clamp((centre - spread) / (1 + z2 / n), 0.0, 1.0);
}

CardStats& card(Stats& s, const std::string& key, int64_t ts) {
    auto [it, fresh] = s.cards.try_emplace(key);
    if (fresh) it->second.first_seen = ts;
    return it->second;
}

void prune_assoc(Stats& s, int64_t now) {
    if (s.assoc.size() <= kMaxAssoc + kMaxAssoc / 4) return;
    std::vector<std::pair<double, const std::string*>> w;
    w.reserve(s.assoc.size());
    for (const auto& [k, a] : s.assoc) w.emplace_back(a.weight.at(now, kHlAssoc), &k);
    std::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : *a.second < *b.second; });
    std::vector<std::string> drop;
    for (size_t i = kMaxAssoc; i < w.size(); ++i) drop.push_back(*w[i].second);
    for (const auto& k : drop) s.assoc.erase(k);
}

void erase_card(Stats& s, const std::string& key) {
    s.cards.erase(key);
    const std::string prefix = key + '\x1f';
    auto it = s.assoc.lower_bound(prefix);
    while (it != s.assoc.end() && it->first.compare(0, prefix.size(), prefix) == 0) it = s.assoc.erase(it);
}

}  // namespace

double CardStats::conversion_lcb(int64_t now) const {
    const double n = offered.at(now, kHlConversion);
    return wilson(std::min(opened.at(now, kHlConversion), n), n);
}

double CardStats::efficacy_lcb(int64_t now) const {
    const double h = helped.at(now, kHlEfficacy), f = failed.at(now, kHlEfficacy);
    return wilson(h, h + f);
}

void Stats::apply(const Record& r) {
    ++records;
    if (const auto* e = std::get_if<EventRec>(&r)) {
        const int64_t ts = static_cast<int64_t>(e->e.ts_ns / 1000000000ull);
        now_hint = std::max(now_hint, ts);
        using T = know::Event::Type;
        switch (e->e.type) {
            case T::Offered: {
                auto& c = card(*this, card_key(e->e.uri), ts);
                c.offered.add(1, ts, kHlConversion);
                break;
            }
            case T::OpenedByRouter:
            case T::OpenedByModel: {
                auto& c = card(*this, card_key(e->e.uri), ts);
                c.opened.add(1, ts, kHlConversion);
                c.last_used = std::max(c.last_used, ts);
                break;
            }
            case T::Shadow: {
                auto& c = card(*this, card_key(e->e.uri), ts);
                c.shadow.add(1, ts, kHlConversion);
                break;
            }
            default: break;
        }
    } else if (const auto* o = std::get_if<OutcomeRec>(&r)) {
        now_hint = std::max(now_hint, o->ts);
        auto& c = card(*this, card_key(o->uri), o->ts);
        const double w = o->weight;
        switch (o->outcome) {
            case Outcome::Helped: {
                c.helped.add(w, o->ts, kHlEfficacy);
                c.last_used = std::max(c.last_used, o->ts);
                const std::string prefix = card_key(o->uri) + '\x1f';
                const int64_t day = o->ts / static_cast<int64_t>(kDay);
                for (const auto& t : o->terms) {
                    Assoc& a = assoc[prefix + t];
                    a.weight.add(w, o->ts, kHlAssoc);
                    if (a.episodes == 0 || a.last_episode != o->episode) {
                        a.episodes = std::min<uint32_t>(a.episodes + 1, 255);
                        a.last_episode = o->episode;
                    }
                    if (a.days == 0 || a.last_day != day) {
                        a.days = std::min<uint16_t>(a.days + 1, 65535);
                        a.last_day = day;
                    }
                }
                prune_assoc(*this, now_hint);
                break;
            }
            case Outcome::Failed: c.failed.add(w, o->ts, kHlEfficacy); break;
            case Outcome::Vetoed:
                c.helped.add(w, o->ts, kHlEfficacy);   // the rule worked
                c.last_used = std::max(c.last_used, o->ts);
                break;
            case Outcome::Opened: c.last_used = std::max(c.last_used, o->ts); break;   // conversion was counted by the event
            case Outcome::Ignored: break;                                                // offered without an open: already the denominator
        }
    } else if (const auto* st = std::get_if<StateRec>(&r)) {
        now_hint = std::max(now_hint, st->ts);
        card(*this, card_key(st->uri), st->ts).state = st->to;
    } else if (const auto* d = std::get_if<DraftRec>(&r)) {
        now_hint = std::max(now_hint, d->ts);   // the journal keeps the verdict; no aggregate
    } else if (const auto* t = std::get_if<TombstoneRec>(&r)) {
        now_hint = std::max(now_hint, t->ts);
        erase_card(*this, card_key(t->uri));
    }
}

}  // namespace learn
}  // namespace dd

// src/learn/curator.cpp — impl of dd/learn/curator.hpp.
#include "dd/learn/curator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include "dd/core/par.hpp"

namespace dd {
namespace learn {

namespace {

using Row = std::pair<const std::string, CardStats>;

double target_prior(const CardStats& c, int64_t now, const CuratorParams& p) {
    if (c.state == State::Retired || c.state == State::Rejected) return p.lo;
    const double evidence = c.helped.at(now, kHlEfficacy) + c.failed.at(now, kHlEfficacy);
    double t = 1.0;
    if (evidence >= 0.5) {
        // A shrunk mean, not a Wilson lower bound: the bound grows with the number of draws, so a card offered
        // early out-ranks an equal one offered late and takes every offer (found by the 180-day soak: twenty equally
        // good cards ended with thirteen holding 99% of the offers and seven starved). Two pseudo-observations
        // still keep a card with three lucky wins under one with fifty.
        const double h = c.helped.at(now, kHlEfficacy);
        const double est = (h + 0.5 * p.shrink) / (evidence + p.shrink);
        // ... plus an error bar, so a card that had bad luck early is tried again instead of being buried by one that had good luck
        const double sigma = std::sqrt(std::max(0.0, est * (1.0 - est)) / (evidence + p.shrink));
        t = p.lo + std::clamp(est + p.explore * sigma, 0.0, 1.0) * (p.hi - p.lo);
    } else {   // never exposed enough to judge: near misses say it was under-offered
        const double s = c.shadow.at(now, kHlConversion);
        t = 1.0 + p.shadow_boost * s / (s + 3.0);
    }
    return std::clamp(t, p.lo, p.hi);
}

double cap_of(State s, const CuratorParams& p) {
    if (s == State::Probation) return p.probation_cap;
    if (s == State::Dormant) return p.dormant_cap;
    return p.hi;
}

// The prior the next snapshot gives a card: what the evidence says, bounded by the state's cap and by the
// per-pass step. The cap is folded in BEFORE the step window, not after it — applying it after let a demotion
// jump 1.0 -> probation_cap in a single pass (-30%), which is exactly the move rate the step exists to forbid:
// one noisy round of evidence would strand a card. A card on probation now descends by step per pass and stays
// recoverable. Retired and Rejected are terminal, so they pin to the floor at once: a card that is gone must
// stop being offered now, not six passes later.
double next_prior(const CardStats& c, int64_t now, double was, const CuratorParams& p) {
    const double want = std::min(target_prior(c, now, p), cap_of(c.state, p));
    if (c.state == State::Retired || c.state == State::Rejected) return std::clamp(want, p.lo, p.hi);
    return std::clamp(want, was * (1.0 - p.step), was * (1.0 + p.step));
}

bool same_terms(const std::vector<know::Term>& a, const std::vector<know::Term>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].text != b[i].text) return false;
    return true;
}

}  // namespace

Curator::Curator(std::shared_ptr<LearnOverlay> overlay, CuratorParams p) : overlay_(std::move(overlay)), p_(p) {}

bool Curator::pass(const Stats& s, int64_t now) {
    const auto prev = overlay_->copy();
    std::vector<const Row*> rows;
    rows.reserve(s.cards.size());
    for (const auto& r : s.cards) rows.push_back(&r);
    std::sort(rows.begin(), rows.end(), [](const Row* a, const Row* b) { return a->first < b->first; });

    const long n = static_cast<long>(rows.size());
    const bool par = rows.size() >= p_.par_cards;
    [[maybe_unused]] const int threads = par ? core::par_threads() : 1;
    std::vector<double> next(rows.size(), 1.0);
#pragma omp parallel for schedule(static) num_threads(threads) if (par)
    for (long i = 0; i < n; ++i) {
        const Row& r = *rows[static_cast<size_t>(i)];
        const auto it = prev->prior.find(r.first);
        const double was = it == prev->prior.end() ? 1.0 : it->second;
        next[static_cast<size_t>(i)] = next_prior(r.second, now, was, p_);
    }

    auto snap = std::make_unique<OverlaySnapshot>();
    for (size_t i = 0; i < rows.size(); ++i)
        if (std::fabs(next[i] - 1.0) > 1e-9) snap->prior.emplace(rows[i]->first, next[i]);
    for (auto& [card, terms] : promote(s, now, p_.affinity)) snap->terms.emplace(card, std::move(terms));

    bool moved = false;
    for (const auto& [k, v] : snap->prior) {
        const auto it = prev->prior.find(k);
        if (std::fabs(v - (it == prev->prior.end() ? 1.0 : it->second)) > p_.move) moved = true;
    }
    for (const auto& [k, v] : prev->prior)
        if (!snap->prior.count(k) && std::fabs(1.0 - v) > p_.move) moved = true;
    if (snap->terms.size() != prev->terms.size()) moved = true;
    else
        for (const auto& [k, t] : snap->terms) {
            const auto it = prev->terms.find(k);
            if (it == prev->terms.end() || !same_terms(t, it->second)) {
                moved = true;
                break;
            }
        }
    if (!moved) return false;
    overlay_->publish(std::move(snap));
    return true;
}

bool Curator::write_report(const Stats& s, int64_t now, const std::filesystem::path& dir) const {
    const auto snap = overlay_->copy();
    auto prior_of = [&](const std::string& k) {
        const auto it = snap->prior.find(k);
        return it == snap->prior.end() ? 1.0 : it->second;
    };
    std::vector<const Row*> rows;
    for (const auto& r : s.cards) rows.push_back(&r);
    std::sort(rows.begin(), rows.end(), [](const Row* a, const Row* b) { return a->first < b->first; });

    std::ostringstream o;
    char buf[160];
    o << "# learn report\n\nrecords folded: " << s.records << ", cards tracked: " << s.cards.size() << ", overlay version: " << overlay_->version() << "\n";

    auto movers = rows;
    std::stable_sort(movers.begin(), movers.end(), [&](const Row* a, const Row* b) { return std::fabs(prior_of(a->first) - 1.0) > std::fabs(prior_of(b->first) - 1.0); });
    o << "\n## top movers (prior)\n";
    int shown = 0;
    for (const Row* r : movers) {
        const double v = prior_of(r->first);
        if (std::fabs(v - 1.0) < 1e-9 || shown++ >= 10) break;
        std::snprintf(buf, sizeof buf, "- %s  %.3f  efficacy %.2f  conversion %.2f\n", r->first.c_str(), v, r->second.efficacy_lcb(now), r->second.conversion_lcb(now));
        o << buf;
    }
    if (!shown) o << "- none\n";

    o << "\n## learned terms\n";
    shown = 0;
    for (const auto& [card, terms] : snap->terms) {
        if (shown++ >= 20) break;
        o << "- " << card << " (" << terms.size() << "):";
        for (size_t i = 0; i < terms.size() && i < 8; ++i) o << ' ' << terms[i].text;
        o << "\n";
    }
    if (!shown) o << "- none\n";

    o << "\n## dormant and retired\n";
    shown = 0;
    for (const Row* r : rows)
        if (r->second.state == State::Dormant || r->second.state == State::Retired || r->second.state == State::Rejected) {
            o << "- " << r->first << "  " << state_name(r->second.state) << "\n";
            ++shown;
        }
    if (!shown) o << "- none\n";

    double total = 0;
    for (const Row* r : rows) total += r->second.offered.at(now, kHlConversion);
    auto hubs = rows;
    std::stable_sort(hubs.begin(), hubs.end(), [&](const Row* a, const Row* b) { return a->second.offered.at(now, kHlConversion) > b->second.offered.at(now, kHlConversion); });
    o << "\n## hubs (share of offers)\n";
    shown = 0;
    for (const Row* r : hubs) {
        const double off = r->second.offered.at(now, kHlConversion);
        if (off <= 0 || total <= 0 || shown++ >= 5) break;
        std::snprintf(buf, sizeof buf, "- %s  %.1f%%  conversion %.2f  prior %.3f\n", r->first.c_str(), 100.0 * off / total, r->second.conversion_lcb(now), prior_of(r->first));
        o << buf;
    }
    if (!shown) o << "- none\n";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto tmp = dir / "report.md.tmp", dst = dir / "report.md";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << o.str();
        if (!f) return false;
    }
    std::filesystem::rename(tmp, dst, ec);
    return !ec;
}

}  // namespace learn
}  // namespace dd

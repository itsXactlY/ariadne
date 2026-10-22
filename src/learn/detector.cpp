#include "dd/learn/detector.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

namespace dd {
namespace learn {
namespace {

// tick index of every step: the position of its (turn, round) among the episode's distinct rounds
std::vector<size_t> tick_index(const Episode& ep) {
    std::vector<size_t> idx(ep.steps.size());
    size_t t = 0;
    for (size_t i = 0; i < ep.steps.size(); ++i) {
        if (i > 0 && (ep.steps[i].turn != ep.steps[i - 1].turn || ep.steps[i].round != ep.steps[i - 1].round)) ++t;
        idx[i] = t;
    }
    return idx;
}

Detection cut(const Episode& ep, size_t first, size_t last, std::string kind, std::string sig, double score) {
    Detection d;
    d.kind = std::move(kind);
    d.signature = std::move(sig);
    d.score = std::min(1.0, std::max(0.0, score));
    d.slice.steps.assign(ep.steps.begin() + static_cast<long>(first), ep.steps.begin() + static_cast<long>(last) + 1);
    d.end_turn = ep.steps[last].turn;
    d.end_round = ep.steps[last].round;
    return d;
}

bool worked(const Step& s) { return !s.call.failed && !s.call.refused && !s.call.exploring; }

// what a round was about: its most used binary, else the project of its first path
std::string key_of(const Step& s) {
    if (!s.call.binaries.empty()) return s.call.binaries.front();
    if (s.call.paths.empty()) return {};
    std::filesystem::path p(s.call.paths.front());
    std::vector<std::string> parts;
    for (const auto& c : p)
        if (c != "/") parts.push_back(c.string());
    for (size_t i = 0; i + 1 < parts.size(); ++i)
        if (parts[i] == "projects") return parts[i + 1];
    return p.has_parent_path() ? p.parent_path().filename().string() : std::string();
}

}  // namespace

std::optional<Detection> FixedAfterFailure::detect(const Episode& ep) const {
    if (ep.steps.empty()) return std::nullopt;
    const auto tick = tick_index(ep);
    const size_t last_tick = tick.back();
    std::map<std::string, std::vector<size_t>> seen;
    for (size_t i = 0; i < ep.steps.size(); ++i) {
        const auto& c = ep.steps[i].call;
        if (c.failed && !c.refused && !c.error.empty()) seen[c.error].push_back(i);
    }
    std::optional<Detection> best;
    for (const auto& [sig, at] : seen) {
        if (static_cast<int>(at.size()) < min_seen_) continue;
        const size_t last_fail = at.back();
        const size_t end_tick = tick[last_fail] + static_cast<size_t>(quiet_);
        if (end_tick > last_tick) continue;   // not enough quiet rounds yet
        size_t end = last_fail;
        bool good = false;
        for (size_t i = last_fail + 1; i < ep.steps.size() && tick[i] <= end_tick; ++i) {
            end = i;
            good = good || worked(ep.steps[i]);
        }
        if (!good) continue;
        size_t fixes = 0;
        for (size_t i = last_fail + 1; i <= end; ++i) fixes += worked(ep.steps[i]) ? 1 : 0;
        const double score = 0.4 + 0.15 * static_cast<double>(at.size()) + 0.05 * static_cast<double>(fixes);
        if (!best || score >= best->score) best = cut(ep, at.front(), end, "fixed", sig, score);
    }
    return best;
}

std::optional<Detection> LongSuccess::detect(const Episode& ep) const {
    if (ep.steps.empty()) return std::nullopt;
    const auto tick = tick_index(ep);
    const size_t rounds = tick.back() + 1;
    // per round: first and last step, whether any call failed, the topic
    struct Round { size_t first = 0, last = 0; bool failed = false; std::string key; };
    std::vector<Round> rs(rounds);
    std::vector<std::map<std::string, int>> votes(rounds);
    for (size_t i = 0; i < ep.steps.size(); ++i) {
        auto& r = rs[tick[i]];
        if (i == 0 || tick[i] != tick[i - 1]) r.first = i;
        r.last = i;
        r.failed = r.failed || ep.steps[i].call.failed;
        if (const std::string k = key_of(ep.steps[i]); !k.empty()) ++votes[tick[i]][k];
    }
    for (size_t t = 0; t < rounds; ++t) {
        int top = 0;
        for (const auto& [k, n] : votes[t])
            if (n > top) { top = n; rs[t].key = k; }
    }
    std::optional<Detection> best;
    size_t start = 0;
    std::string key;
    auto close = [&](size_t end_excl) {   // rounds [start, end_excl)
        if (end_excl <= start || key.empty()) return;
        const size_t n = end_excl - start;
        if (static_cast<int>(n) < min_rounds_) return;
        if (rs[end_excl - 1].failed || (n >= 2 && rs[end_excl - 2].failed)) return;   // must end clean
        const double score = 0.3 + 0.7 * std::min(1.0, static_cast<double>(n) / 16.0);
        if (!best || score > best->score) best = cut(ep, rs[start].first, rs[end_excl - 1].last, "long", key, score);
    };
    for (size_t t = 0; t < rounds; ++t) {
        if (rs[t].key.empty() || rs[t].key == key) continue;   // a round with no topic rides along
        if (key.empty()) {
            key = rs[t].key;
            start = t;
            continue;
        }
        close(t);
        key = rs[t].key;
        start = t;
    }
    close(rounds);
    return best;
}

std::optional<Detection> Pinned::detect(const Episode& ep) const {
    if (ep.steps.empty() || !armed_.exchange(false)) return std::nullopt;
    return cut(ep, 0, ep.steps.size() - 1, "pinned", "", 1.0);
}

std::optional<Detection> best_detection(const std::vector<std::unique_ptr<Detector>>& ds, const Episode& window) {
    std::optional<Detection> best;
    for (const auto& d : ds)
        if (auto r = d->detect(window); r && (!best || r->score > best->score)) best = std::move(r);
    return best;
}

}  // namespace learn
}  // namespace dd

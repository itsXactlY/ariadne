// src/know/working_set.cpp — impl of dd/know/working_set.hpp.
#include "dd/know/working_set.hpp"

#include <algorithm>

namespace dd {
namespace know {

void WorkingSet::new_turn(const std::string& question) {
    ++turn_;
    round_ = 0;
    last_note_round_ = -1000;
    question_ = question;
    covered_.clear();
    warned_.clear();
    calls_.clear();
    fail_sig_.clear();
    fail_tool_.clear();
    turn_tokens_ = 0;
    stalled_ = false;
}

void WorkingSet::new_round() { ++round_; }

void WorkingSet::new_epoch(int epoch) { epoch_ = epoch; }

void WorkingSet::offered(const Uri& u, const std::string& title, size_t tokens) {
    Entry e;
    e.uri = u;
    e.uri.page.clear();
    e.title = title;
    e.epoch = epoch_;
    e.turn = turn_;
    e.tokens = tokens;
    entries_.push_back(std::move(e));
}

void WorkingSet::opened(const Uri& u, const std::string& title, size_t tokens) {
    Entry e;
    e.uri = u;
    e.uri.page.clear();
    e.title = title;
    e.opened = true;
    e.epoch = epoch_;
    e.turn = turn_;
    e.tokens = tokens;
    entries_.push_back(std::move(e));
}

namespace {
bool same(const Uri& a, const Uri& b) { return a.kind == b.kind && a.id == b.id; }
}  // namespace

bool WorkingSet::offered_this_turn(const Uri& u) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.turn == turn_ && same(e.uri, u); });
}

bool WorkingSet::offered_this_epoch(const Uri& u) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.epoch == epoch_ && same(e.uri, u); });
}

bool WorkingSet::open_in_window(const Uri& u) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.opened && e.epoch == epoch_ && same(e.uri, u); });
}

std::vector<WorkingSet::Entry> WorkingSet::in_use(Kind kind, int turns) const {
    std::vector<Entry> out;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (!it->opened || it->uri.kind != kind || it->turn < turn_ - (turns - 1)) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Entry& e) { return same(e.uri, it->uri); })) continue;
        out.push_back(*it);
    }
    return out;
}

void WorkingSet::cover(const std::vector<Term>& terms) {
    for (const auto& t : terms) covered_.insert(t.text);
}

int WorkingSet::uncovered(const std::vector<Term>& terms) const {
    int n = 0;
    for (const auto& t : terms)
        if (!covered(t.text)) ++n;
    return n;
}

void WorkingSet::record(const std::vector<CallRecord>& calls) {
    for (const auto& c : calls) {
        if (c.refused) continue;   // it never ran: it says nothing about what the model is doing
        calls_.push_back(c);
        if (calls_.size() > 24) calls_.pop_front();
        if (c.failed) {
            if (!c.error.empty()) ++fail_sig_[c.error];
            ++fail_tool_[c.tool];
        }
    }
}

int WorkingSet::identical_streak() const {
    if (calls_.empty() || calls_.back().digest.empty()) return 0;
    int n = 0;
    for (auto it = calls_.rbegin(); it != calls_.rend() && it->digest == calls_.back().digest; ++it) ++n;
    return n;
}

int WorkingSet::blind_streak() const {
    int n = 0;
    for (auto it = calls_.rbegin(); it != calls_.rend() && it->exploring && (it->empty || it->failed); ++it) ++n;
    return n;
}

std::string WorkingSet::last_blind_summary() const {
    std::vector<std::pair<std::string, int>> counts;
    int n = blind_streak();
    for (auto it = calls_.rbegin(); n-- > 0 && it != calls_.rend(); ++it) {
        const std::string label = it->tool + (it->failed ? " miss" : "");
        auto at = std::find_if(counts.begin(), counts.end(), [&](const auto& p) { return p.first == label; });
        if (at == counts.end()) counts.emplace_back(label, 1);
        else ++at->second;
    }
    std::string s;
    for (const auto& [label, k] : counts) s += (s.empty() ? "" : ", ") + label + (k > 1 ? " x" + std::to_string(k) : "");
    return s;
}

int WorkingSet::failures(const std::string& signature) const {
    auto it = fail_sig_.find(signature);
    return it == fail_sig_.end() ? 0 : it->second;
}

int WorkingSet::family_failures(const std::string& tool) const {
    auto it = fail_tool_.find(tool);
    return it == fail_tool_.end() ? 0 : it->second;
}

}  // namespace know
}  // namespace dd

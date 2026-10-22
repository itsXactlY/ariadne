// src/know/trigger.cpp — impl of dd/know/trigger.hpp.
#include "dd/know/trigger.hpp"

#include <algorithm>
#include <cctype>

namespace dd {
namespace know {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

// what a batch of calls says about the task: the model's own intent, the programs, the files, the errors
std::vector<std::pair<std::string, float>> pieces_of(const std::vector<CallRecord>& calls) {
    std::vector<std::pair<std::string, float>> p;
    for (const auto& c : calls) {
        if (!c.intent.empty()) p.emplace_back(c.intent, 1.5f);
        if (!c.binaries.empty()) p.emplace_back(join(c.binaries), 1.2f);
        for (const auto& path : c.paths) p.emplace_back(path, 1.0f);
        if (c.failed && !c.error.empty()) p.emplace_back(c.error, 1.0f);
    }
    return p;
}

}  // namespace

bool asks_about_the_past(const std::string& text) {
    const std::string t = lower(text);
    static const char* cues[] = {"remember", "recall", "earlier", "last time", "previous", "we did", "we decided", "we talked", "what was",
                                 "what did", "why did", "how did we", "did we", "had we", "erinner", "frueher", "früher", "letzte", "damals",
                                 "was war", "wie war", "haben wir", "hatten wir", "wann war", "bisher", "yesterday", "gestern", "last week"};
    for (const char* c : cues)
        if (t.find(c) != std::string::npos) return true;
    return false;
}

void TurnStartTrigger::plan(const Signal& s, const PlanContext& c, Plan& out) const {
    const auto* t = std::get_if<TurnStarted>(&s);
    if (!t || t->trivial) return;
    const std::string& asked = t->anchored.empty() ? t->text : t->anchored;
    Intent in;
    in.kinds = {Kind::Skill, Kind::Memory, Kind::Tool};
    in.query = c.distill.text(asked, c.idf);
    in.query.text = asked;   // the deep leg searches by the words as written
    in.deep = !t->skip_deep;
    in.wait_ms = asks_about_the_past(t->text) ? c.policy.deep_recall_ms : c.policy.deep_ms;
    in.where = Placement::QuestionNote;
    in.tokens = c.policy.turn_tokens;
    in.reason = "turn";
    out.push_back(std::move(in));
}

void ShiftTrigger::plan(const Signal& s, const PlanContext& c, Plan& out) const {
    const auto* f = std::get_if<CallsFinished>(&s);
    if (!f || !c.policy.shift || !c.policy.auto_route) return;
    if (c.ws.rounds_since_note() < c.policy.shift_gap_rounds) return;   // not on the heels of the last note
    const Query q = c.distill.terms(pieces_of(f->calls), c.idf);
    if (c.ws.uncovered(q.terms) < c.policy.shift_new_terms) return;      // the same topic: nothing new to say
    std::string fresh;
    int n = 0;
    for (const auto& t : q.terms)
        if (!c.ws.covered(t.text) && n++ < 3) fresh += (fresh.empty() ? "" : " ") + t.text;
    Intent in;
    in.kinds = {Kind::Skill, Kind::Memory};
    in.query = q;
    in.where = Placement::AfterTools;
    in.tokens = c.policy.hint_tokens;
    in.reason = "shift: " + fresh;
    out.push_back(std::move(in));
}

void FailureTrigger::plan(const Signal& s, const PlanContext& c, Plan& out) const {
    const auto* f = std::get_if<CallsFinished>(&s);
    if (!f || !c.policy.failure) return;
    for (const auto& call : f->calls) {
        if (!call.failed || call.error.empty() || call.error.rfind("[rule ", 0) == 0) continue;   // a rule's refusal is not an error to look up
        if (c.ws.was_warned(call.error)) continue;   // the router marks a signature once it has spoken
        const int again = c.ws.failures(call.error), family = c.ws.family_failures(call.tool);
        Intent in;
        in.kinds = {Kind::Memory, Kind::Skill};
        in.query = c.distill.terms({{call.error, 1.5f}, {call.intent, 1.0f}, {join(call.binaries), 1.0f}}, c.idf);
        in.deep = again >= 2 || family >= 2;   // a repeat is worth the pod's time
        in.wait_ms = 0;
        in.where = Placement::AfterTools;
        in.tokens = c.policy.failure_tokens;
        in.reason = "failure: " + call.error;
        in.key = call.error;
        out.push_back(std::move(in));
        break;   // one retrieval per batch: the first new error
    }
}

std::string StallTrigger::notice_text(const WorkingSet& ws, const Policy& p) {
    if (ws.identical_streak() >= p.stall_identical)
        return "You made the same call " + std::to_string(ws.identical_streak()) +
               " times in a row. NEVER repeat it: change the arguments, choose another tool, or find() what you are looking for.";
    return std::to_string(ws.blind_streak()) + " calls in a row found nothing (" + ws.last_blind_summary() +
           "). STOP guessing paths and commands: find(\"what you are looking for\"), or open a pointer below, then act.";
}

void StallTrigger::plan(const Signal& s, const PlanContext& c, Plan& out) const {
    const auto* f = std::get_if<CallsFinished>(&s);
    if (!f || !c.policy.stall || c.ws.stalled()) return;
    const bool same = c.ws.identical_streak() >= c.policy.stall_identical;
    const bool blind = c.ws.blind_streak() >= c.policy.stall_blind;
    if (!same && !blind) return;
    Intent in;
    in.kinds = {Kind::Skill, Kind::Memory};
    in.query = c.distill.text(c.ws.question(), c.idf);   // by the question, not by the last call: that is what was lost
    in.query.text = c.ws.question();
    in.deep = true;
    in.wait_ms = 0;
    in.where = Placement::AfterTools;
    in.tokens = c.policy.stall_tokens;
    in.reason = same ? "stall: same call" : "stall: blind";
    in.notice = true;
    out.push_back(std::move(in));
}

std::vector<std::unique_ptr<Trigger>> default_triggers() {
    std::vector<std::unique_ptr<Trigger>> t;
    t.push_back(std::make_unique<TurnStartTrigger>());
    t.push_back(std::make_unique<StallTrigger>());
    t.push_back(std::make_unique<FailureTrigger>());
    t.push_back(std::make_unique<ShiftTrigger>());
    return t;
}

}  // namespace know
}  // namespace dd

#include "dd/learn/lifecycle.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "dd/core/yaml.hpp"
#include "dd/know/types.hpp"

namespace fs = std::filesystem;

namespace dd {
namespace learn {
namespace {

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const char* state_text(State s) {
    switch (s) {
        case State::Draft: return "draft";
        case State::Probation: return "probation";
        case State::Active: return "active";
        case State::Dormant: return "dormant";
        case State::Retired: return "retired";
        case State::Rejected: return "rejected";
    }
    return "draft";
}

State state_from(const std::string& s, State fallback) {
    for (State c : {State::Draft, State::Probation, State::Active, State::Dormant, State::Retired, State::Rejected})
        if (s == state_text(c)) return c;
    return fallback;
}

int64_t to_int(const std::string& s, int64_t fallback) {
    try {
        return s.empty() ? fallback : std::stoll(s);
    } catch (...) {
        return fallback;
    }
}

// Sets `key: value` inside the frontmatter block, adding the line when absent. nullopt: no frontmatter.
std::optional<std::string> set_keys(const std::string& text, const std::vector<std::pair<std::string, std::string>>& kv) {
    if (text.rfind("---\n", 0) != 0) return std::nullopt;
    const size_t end = text.find("\n---", 3);
    if (end == std::string::npos) return std::nullopt;
    std::vector<std::string> lines;
    std::istringstream in(text.substr(4, end - 4 + 1));
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    for (const auto& [k, v] : kv) {
        bool done = false;
        for (auto& l : lines)
            if (l.rfind(k + ":", 0) == 0) {
                l = k + ": " + v;
                done = true;
                break;
            }
        if (!done) lines.push_back(k + ": " + v);
    }
    std::string out = "---\n";
    for (const auto& l : lines) out += l + "\n";
    return out + text.substr(end + 1);
}

bool win_after(const CardStats& c, int64_t t) { return c.helped.v > 0 && c.helped.t > t; }

}  // namespace

int64_t Subject::last_used() const {
    int64_t t = since;
    if (card) t = std::max(t, card->last_used);
    return t;
}

Lifecycle::Lifecycle(fs::path root, LifecycleParams p) : root_(std::move(root)), p_(p) {
    const auto helped = [](const Subject& s) { return s.card ? s.card->helped.at(s.now, kHlEfficacy) : 0.0; };
    const auto failed = [](const Subject& s) { return s.card ? s.card->failed.at(s.now, kHlEfficacy) : 0.0; };
    // first match wins; a failed skill retires before it can be promoted
    table_ = {
        {State::Probation, State::Retired, "failed twice",
         [=](const Subject& s, const LifecycleParams& p) { return s.card && s.card->failed.t >= s.since && failed(s) >= p.retire_failures - 1e-9; }},
        {State::Probation, State::Active, "proven",
         [=](const Subject& s, const LifecycleParams& p) {
             if (!s.card || helped(s) < p.promote_wins * p.decay_allowance - 1e-9 || !win_after(*s.card, s.since)) return false;
             return s.card->failed.v <= 0 || s.now - s.card->failed.t > p.promote_quiet_s;
         }},
        {State::Probation, State::Retired, "no win in probation",
         [=](const Subject& s, const LifecycleParams& p) {
             const int64_t last_win = s.card && win_after(*s.card, s.since) ? s.card->helped.t : s.since;
             return s.now - last_win > p.probation_max_s;
         }},
        {State::Active, State::Probation, "efficacy fell",
         [=](const Subject& s, const LifecycleParams& p) {
             return s.card && !s.demoted && helped(s) + failed(s) >= p.demote_evidence - 1e-9 && s.card->efficacy_lcb(s.now) < p.demote_floor;
         }},
        {State::Active, State::Dormant, "idle",
         [](const Subject& s, const LifecycleParams& p) { return s.now - s.last_used() > p.dormant_after_s; }},
        {State::Dormant, State::Active, "used again",
         [](const Subject& s, const LifecycleParams&) { return s.card && s.card->last_used > s.since; }},
        {State::Dormant, State::Retired, "idle a year",
         [](const Subject& s, const LifecycleParams& p) { return s.now - s.last_used() > p.retire_after_s; }},
    };
}

bool Lifecycle::rewrite_state(const fs::path& md, State to, int64_t since, bool demoted) {
    std::error_code ec;
    if (!fs::is_regular_file(md, ec)) return false;
    const std::string text = read_text(md);
    const auto fm = core::parse_frontmatter(text);
    auto o = fm.find("origin");
    if (o == fm.end() || core::unquote(o->second) != "learned") return false;
    std::vector<std::pair<std::string, std::string>> kv = {{"state", state_text(to)}, {"state_since", std::to_string(since)}};
    if (demoted) kv.emplace_back("demoted", "1");
    const auto out = set_keys(text, kv);
    if (!out) return false;
    const fs::path tmp = md.parent_path() / ".SKILL.md.tmp";   // dot-named: never taken for a skill
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << *out;
        f.flush();
        if (!f) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, md, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

fs::path Lifecycle::retired_dir(const fs::path& root, const std::string& category, const std::string& name) {
    return root / ".retired" / category / name;
}

std::vector<Lifecycle::Change> Lifecycle::step(const Stats& stats, int64_t now, const Notify& record) const {
    std::vector<Change> done;
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) return done;

    struct Found { fs::path md; Subject s; };
    std::vector<Found> found;
    for (const auto& cat : fs::directory_iterator(root_, ec)) {
        if (!cat.is_directory(ec) || cat.path().filename().string().front() == '.') continue;
        std::error_code e2;
        for (const auto& dir : fs::directory_iterator(cat.path(), e2)) {
            const fs::path md = dir.path() / "SKILL.md";
            if (!dir.is_directory(e2) || !fs::is_regular_file(md, e2)) continue;
            const auto fm = core::parse_frontmatter(read_text(md));
            auto o = fm.find("origin");
            if (o == fm.end() || core::unquote(o->second) != "learned") continue;   // not ours: untouched
            Found f;
            f.md = md;
            f.s.name = dir.path().filename().string();
            f.s.category = cat.path().filename().string();
            auto val = [&](const char* k) { auto it = fm.find(k); return it == fm.end() ? std::string() : core::unquote(it->second); };
            f.s.state = state_from(val("state"), State::Probation);
            f.s.demoted = val("demoted") == "1";
            know::Uri u;
            u.kind = know::Kind::Skill;
            u.id = f.s.name;
            auto c = stats.cards.find(u.str());
            f.s.card = c == stats.cards.end() ? nullptr : &c->second;
            f.s.since = to_int(val("state_since"), f.s.card && f.s.card->first_seen ? f.s.card->first_seen : now);
            f.s.now = now;
            found.push_back(std::move(f));
        }
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.md < b.md; });

    for (auto& f : found) {
        Subject& s = f.s;
        know::Uri u;
        u.kind = know::Kind::Skill;
        u.id = s.name;
        const State journaled = s.card ? s.card->state : State::Active;
        if (journaled != s.state) record(StateRec{u, journaled, s.state, "reconcile", now});   // the file leads, the journal follows

        for (const auto& t : table_) {
            if (t.from != s.state || !t.guard(s, p_)) continue;
            const bool demote = t.from == State::Active && t.to == State::Probation;
            if (!rewrite_state(f.md, t.to, now, s.demoted || demote)) break;
            if (t.to == State::Retired) {
                const fs::path src = f.md.parent_path();
                fs::path dst = retired_dir(root_, s.category, s.name);
                std::error_code mec;
                if (fs::exists(dst, mec)) dst += "-" + std::to_string(now);
                fs::create_directories(dst.parent_path(), mec);
                fs::rename(src, dst, mec);
            }
            record(StateRec{u, s.state, t.to, t.why, now});
            done.push_back(Change{s.name, s.category, s.state, t.to, t.why});
            break;
        }
    }
    return done;
}

}  // namespace learn
}  // namespace dd

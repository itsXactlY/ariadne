#include "dd/learn/inspect.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "dd/core/path.hpp"
#include "dd/core/yaml.hpp"
#include "dd/learn/affinity.hpp"
#include "dd/learn/curator.hpp"
#include "dd/learn/ledger.hpp"

namespace fs = std::filesystem;

namespace dd {
namespace learn {

namespace {

struct LearnedSkill {
    std::string name, category, state;
    fs::path dir;
};

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<LearnedSkill> learned_skills(const fs::path& root) {
    std::vector<LearnedSkill> out;
    core::walk(root, "SKILL.md", core::WalkOptions{}, [&](const fs::path& md) {
        const auto fm = core::parse_frontmatter(read_text(md));
        const auto o = fm.find("origin");
        if (o == fm.end() || core::unquote(o->second) != "learned") return;
        LearnedSkill s;
        s.dir = md.parent_path();
        const auto n = fm.find("name");
        s.name = n != fm.end() ? core::unquote(n->second) : s.dir.filename().string();
        const fs::path rel = fs::relative(s.dir.parent_path(), root);
        s.category = rel.empty() || rel == "." ? "" : rel.string();
        const auto st = fm.find("state");
        s.state = st != fm.end() ? core::unquote(st->second) : "draft";
        out.push_back(std::move(s));
    });
    std::sort(out.begin(), out.end(), [](const LearnedSkill& a, const LearnedSkill& b) { return a.name < b.name; });
    return out;
}

std::string uri_of(const std::string& skill) {
    return skill.rfind("skill://", 0) == 0 ? skill : "skill://" + skill;
}

std::string name_of(const std::string& skill) {
    return skill.rfind("skill://", 0) == 0 ? skill.substr(8) : skill;
}

std::string fmt(const char* f, double a) {
    char b[64];
    std::snprintf(b, sizeof b, f, a);
    return b;
}

std::string esc(const std::string& s) {
    std::string o;
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o;
}

bool load(const InspectPaths& p, Stats& s, Ledger::Health& h, Result& r) {
    std::string err;
    if (Ledger::load_readonly(p.learn_dir, s, &h, &err)) return true;
    r.ok = false;
    r.text = err;
    return false;
}

std::string dec(const Decayed& d, int64_t now, double hl) { return fmt("%.2f", d.at(now, hl)); }

}  // namespace

Result status(const InspectPaths& p, int64_t now) {
    Result r;
    Stats s;
    Ledger::Health h;
    if (!load(p, s, h, r)) return r;
    std::ostringstream o;
    o << "learn: " << p.learn_dir.string() << "\n";
    o << "journal: " << s.records << " records folded (" << h.recovered << " replayed after the snapshot, last seq " << h.last_seq << ")";
    if (h.replay_newer || h.replay_undecodable || h.bad_segments || h.snapshots_corrupt)
        o << "; skipped: " << h.replay_newer << " newer, " << h.replay_undecodable << " undecodable, " << h.bad_segments
          << " bad segments, " << h.snapshots_corrupt << " bad snapshots";
    o << "\n";
    if (h.sync_errors || h.write_errors)   // left by the run that saw them (Ledger's HEALTH file)
        o << "WARNING: the last run could not make its records durable: " << h.sync_errors << " failed sync(s), " << h.write_errors
          << " failed write(s); the newest records may be missing\n";
    double offered = 0, opened = 0, helped = 0, failed = 0;
    for (const auto& [k, c] : s.cards) {
        offered += c.offered.at(now, kHlConversion);
        opened += c.opened.at(now, kHlConversion);
        helped += c.helped.at(now, kHlEfficacy);
        failed += c.failed.at(now, kHlEfficacy);
    }
    o << "cards tracked: " << s.cards.size() << "  offered " << fmt("%.1f", offered) << "  opened " << fmt("%.1f", opened)
      << "  helped " << fmt("%.1f", helped) << "  failed " << fmt("%.1f", failed) << "  (decayed)  associations: " << s.assoc.size() << "\n";

    const auto skills = learned_skills(p.skills_root);
    std::map<std::string, int> by_state;
    for (const auto& k : skills) {
        const auto it = s.cards.find("skill://" + k.name);
        by_state[it != s.cards.end() ? state_name(it->second.state) : k.state]++;
    }
    o << "learned skills: " << skills.size();
    for (const auto& [st, n] : by_state) o << "  " << st << " " << n;
    o << "\n";
    for (const auto& k : skills) {
        const auto it = s.cards.find("skill://" + k.name);
        o << "  " << (k.category.empty() ? "" : k.category + "/") << k.name << "  ["
          << (it != s.cards.end() ? state_name(it->second.state) : k.state.c_str()) << "]";
        if (it != s.cards.end()) o << "  conversion " << fmt("%.2f", it->second.conversion_lcb(now)) << "  efficacy " << fmt("%.2f", it->second.efficacy_lcb(now));
        o << "\n";
    }

    auto overlay = std::make_shared<LearnOverlay>();
    Curator(overlay).pass(s, now);
    const auto snap = overlay->copy();
    if (snap) {
        std::vector<std::pair<std::string, double>> movers(snap->prior.begin(), snap->prior.end());
        std::sort(movers.begin(), movers.end(), [](const auto& a, const auto& b) {
            const double da = std::fabs(a.second - 1.0), db = std::fabs(b.second - 1.0);
            return da != db ? da > db : a.first < b.first;
        });
        o << "overlay (the next pass): " << snap->prior.size() << " priors moved, " << snap->terms.size() << " cards with learned terms\n";
        for (size_t i = 0; i < movers.size() && i < 8; ++i) o << "  " << movers[i].first << "  prior " << fmt("%.3f", movers[i].second) << "\n";
    }
    const std::string rp = report_path(p);
    if (!rp.empty()) o << "report: " << rp << "\n";
    r.ok = true;
    r.text = o.str();
    return r;
}

Result why(const InspectPaths& p, const std::string& skill, int64_t now) {
    Result r;
    Stats s;
    Ledger::Health h;
    if (!load(p, s, h, r)) return r;
    const std::string uri = uri_of(skill), name = name_of(skill);
    const auto it = s.cards.find(uri);
    const auto skills = learned_skills(p.skills_root);
    const auto ls = std::find_if(skills.begin(), skills.end(), [&](const LearnedSkill& k) { return k.name == name; });
    if (it == s.cards.end() && ls == skills.end()) {
        r.text = "no record of " + uri;
        return r;
    }
    std::ostringstream o;
    o << uri;
    if (ls != skills.end()) o << "  (learned, " << ls->dir.string() << ")";
    o << "\n";
    if (it != s.cards.end()) {
        const CardStats& c = it->second;
        o << "state: " << state_name(c.state) << "\n";
        o << "offered " << dec(c.offered, now, kHlConversion) << "  opened " << dec(c.opened, now, kHlConversion)
          << "  conversion lower bound " << fmt("%.3f", c.conversion_lcb(now)) << "\n";
        o << "helped " << dec(c.helped, now, kHlEfficacy) << "  failed " << dec(c.failed, now, kHlEfficacy)
          << "  efficacy lower bound " << fmt("%.3f", c.efficacy_lcb(now)) << "\n";
        o << "near misses (shadow) " << dec(c.shadow, now, kHlConversion) << "\n";
    } else {
        o << "state: " << ls->state << " (no outcomes yet)\n";
    }
    const AffinityParams ap;
    const auto promoted = promote(s, now, ap);
    const auto pit = promoted.find(uri);
    const std::string prefix = uri + kAssocSep;
    int n = 0;
    for (auto a = s.assoc.lower_bound(prefix); a != s.assoc.end() && a->first.compare(0, prefix.size(), prefix) == 0; ++a) {
        const std::string term = a->first.substr(prefix.size());
        bool in = false;
        if (pit != promoted.end())
            for (const auto& t : pit->second) in = in || t.text == term;
        if (n++ == 0) o << "associated terms (promoted at " << ap.min_episodes << " episodes on " << ap.min_days << " days):\n";
        o << "  " << (in ? "* " : "  ") << term << "  weight " << dec(a->second.weight, now, kHlAssoc) << "  episodes " << a->second.episodes
          << "  days " << a->second.days << "\n";
    }
    if (n == 0) o << "no associated terms yet\n";
    r.ok = true;
    r.text = o.str();
    return r;
}

Result export_jsonl(const InspectPaths& p, int64_t now) {
    Result r;
    Stats s;
    Ledger::Health h;
    if (!load(p, s, h, r)) return r;
    std::vector<std::string> keys;
    for (const auto& [k, c] : s.cards) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    std::ostringstream o;
    auto d = [&](const Decayed& x, double hl) { return fmt("%.6g", x.at(now, hl)); };
    for (const auto& k : keys) {
        const CardStats& c = s.cards.at(k);
        o << "{\"type\":\"card\",\"uri\":\"" << esc(k) << "\",\"state\":\"" << state_name(c.state) << "\",\"offered\":" << d(c.offered, kHlConversion)
          << ",\"opened\":" << d(c.opened, kHlConversion) << ",\"helped\":" << d(c.helped, kHlEfficacy) << ",\"failed\":" << d(c.failed, kHlEfficacy)
          << ",\"shadow\":" << d(c.shadow, kHlConversion) << ",\"conversion_lcb\":" << fmt("%.6g", c.conversion_lcb(now))
          << ",\"efficacy_lcb\":" << fmt("%.6g", c.efficacy_lcb(now)) << "}\n";
    }
    for (const auto& [k, a] : s.assoc) {
        const size_t sep = k.find(kAssocSep);
        if (sep == std::string::npos) continue;
        o << "{\"type\":\"assoc\",\"uri\":\"" << esc(k.substr(0, sep)) << "\",\"term\":\"" << esc(k.substr(sep + 1)) << "\",\"weight\":"
          << d(a.weight, kHlAssoc) << ",\"episodes\":" << a.episodes << ",\"days\":" << a.days << "}\n";
    }
    r.ok = true;
    r.text = o.str();
    return r;
}

Result forget(const InspectPaths& p, const std::string& skill, int64_t now) {
    Result r;
    const std::string name = name_of(skill);
    const auto skills = learned_skills(p.skills_root);
    const auto ls = std::find_if(skills.begin(), skills.end(), [&](const LearnedSkill& k) { return k.name == name; });
    if (ls == skills.end()) {
        r.text = name + " is not a learned skill (origin: learned): not touched";
        return r;
    }
    std::string err;
    auto ledger = Ledger::open(p.learn_dir, {}, &err);
    if (!ledger) {
        r.text = err;
        return r;
    }
    know::Uri u;
    u.kind = know::Kind::Skill;
    u.id = name;
    if (!ledger->record(Record{TombstoneRec{u, now}})) {
        r.text = "the tombstone could not be written";
        return r;
    }
    ledger->sync();
    fs::path dest = p.skills_root / ".retired" / (ls->category.empty() ? fs::path() : fs::path(ls->category)) / name;
    std::error_code ec;
    if (fs::exists(dest, ec)) dest += "-" + std::to_string(now);
    fs::create_directories(dest.parent_path(), ec);
    fs::rename(ls->dir, dest, ec);
    if (ec) {
        r.text = "forgotten in the ledger, but the folder could not be moved: " + ec.message();
        return r;
    }
    r.ok = true;
    r.text = "forgot skill://" + name + "; moved to " + dest.string();
    return r;
}

std::string report_path(const InspectPaths& p) {
    const fs::path r = p.learn_dir / "report.md";
    std::error_code ec;
    return fs::exists(r, ec) ? r.string() : std::string();
}

}  // namespace learn
}  // namespace dd

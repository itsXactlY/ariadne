#include "dd/learn/skill_sink.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include "dd/core/yaml.hpp"
#include "dd/tools/skill_provenance.hpp"

namespace fs = std::filesystem;

namespace dd {
namespace learn {
namespace {

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

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string date_of(int64_t t) {
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

bool learned(const std::map<std::string, std::string>& fm) {
    auto it = fm.find("origin");
    return it != fm.end() && core::unquote(it->second) == "learned";
}

}  // namespace

SkillSink::SkillSink(fs::path root, Clock clock) : root_(std::move(root)), clock_(std::move(clock)) {}

int64_t SkillSink::now() const {
    if (clock_) return clock_();
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

fs::path SkillSink::path_of(const Draft& d) const { return root_ / d.category / d.name / "SKILL.md"; }

DraftRec SkillSink::reject(const Draft& d, const Verdict& v) const {
    return DraftRec{d.name, d.category, d.digest, v.reason.empty() ? "rejected" : v.reason, false, now()};
}

DraftRec SkillSink::write(const Draft& d, const Verdict& v, State state) const {
    DraftRec rec{d.name, d.category, d.digest, "", false, now()};
    if (!v.ok) return reject(d, v);
    std::error_code ec;
    const fs::path target = path_of(d);

    // a skill of this name that is not ours, in any category, wins
    if (fs::is_directory(root_, ec)) {
        for (const auto& cat : fs::directory_iterator(root_, ec)) {
            if (!cat.is_directory(ec)) continue;
            const fs::path other = cat.path() / d.name / "SKILL.md";
            if (other == target || !fs::exists(other, ec)) continue;
            if (!learned(core::parse_frontmatter(read_text(other)))) {
                rec.verdict = "exists: a skill of this name is not learned (" + cat.path().filename().string() + ")";
                return rec;
            }
        }
    }

    bool existed = false;
    if (fs::exists(target, ec)) {
        existed = true;
        const auto fm = core::parse_frontmatter(read_text(target));
        if (!learned(fm)) {
            rec.verdict = "exists: not learned, left alone";
            return rec;
        }
        // a learned skill the model edited carries who edited it. A draft is a guess from the
        // transcript; an edit the model made through edit_file is work already done. The draft loses.
        if (const auto it = fm.find("edited_by"); it != fm.end()) {
            rec.verdict = "edited_by: " + core::unquote(it->second) + " outranks a draft, left alone";
            return rec;
        }
        if (auto it = fm.find("digest"); it != fm.end() && core::unquote(it->second) == d.digest) {
            rec.accepted = true;
            rec.verdict = "unchanged";
            return rec;
        }
    }

    std::string tags;
    for (const auto& t : d.tags) tags += (tags.empty() ? "" : ", ") + t;
    std::string text = "---\nname: " + d.name + "\ndescription: " + d.gist + "\ntags: [" + tags + "]\norigin: learned\nstate: " + state_text(state) +
                       "\nkind: " + d.kind + "\nepisode: " + d.episode.substr(0, 16) + "\ndigest: " + d.digest + "\ncreated: " + date_of(rec.ts) + "\n---\n\n" + d.body;

    const dd::tools::WriteOriginScope scope("learn_loop");   // this write is ours, not the model's
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        rec.verdict = "write failed: " + ec.message();
        return rec;
    }
    const fs::path tmp = target.parent_path() / ".SKILL.md.tmp";   // dot-named: never taken for a skill
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        f.flush();
        if (!f) {
            rec.verdict = "write failed";
            fs::remove(tmp, ec);
            return rec;
        }
    }
    fs::rename(tmp, target, ec);   // atomic: a reader sees the old file or the new one
    if (ec) {
        rec.verdict = "write failed: " + ec.message();
        fs::remove(tmp, ec);
        return rec;
    }
    rec.accepted = true;
    rec.verdict = existed ? "updated" : "written";
    return rec;
}

}  // namespace learn
}  // namespace dd

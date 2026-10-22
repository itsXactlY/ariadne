#pragma once
// dd/learn/skill_sink.hpp — an accepted draft becomes <skills>/<category>/<name>/SKILL.md.
//
// Idempotent by name and digest; replaces only a file that says `origin: learned`; a skill of the same name
// anywhere else in the tree that is not learned wins and the draft is refused.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "dd/learn/author.hpp"
#include "dd/learn/record.hpp"
#include "dd/learn/reviewer.hpp"

namespace dd {
namespace learn {

class SkillSink {
public:
    using Clock = std::function<int64_t()>;   // unix seconds
    explicit SkillSink(std::filesystem::path skills_root, Clock clock = {});

    // The journal record for the outcome: accepted + verdict "written" | "unchanged" | "updated", or not accepted + the reason.
    DraftRec write(const Draft& d, const Verdict& v, State state = State::Probation) const;
    DraftRec reject(const Draft& d, const Verdict& v) const;   // a draft the reviewer refused: only the record, no file

    std::filesystem::path path_of(const Draft& d) const;

private:
    std::filesystem::path root_;
    Clock clock_;
    int64_t now() const;
};

}  // namespace learn
}  // namespace dd

#pragma once
// A discovered skill: metadata parsed from a SKILL.md plus its location.

#include <map>
#include <string>
#include <filesystem>

namespace dd {
namespace skills {

struct Skill {
    std::string name;
    std::string description;
    std::string category;              // derived from path; may be empty
    std::filesystem::path skill_md;    // absolute path to SKILL.md
    std::filesystem::path skill_dir;   // parent directory
    std::map<std::string, std::string> frontmatter;
};

}  // namespace skills
}  // namespace dd

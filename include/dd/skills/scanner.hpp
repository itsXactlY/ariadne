#pragma once
// Skill scanner: discovers SKILL.md files across roots, FOLLOWS symlinked
// directories (the fix), dedups by name, derives category from path.

#include <vector>
#include <string>
#include <filesystem>
#include "dd/skills/skill.hpp"

namespace dd {
namespace skills {

class Scanner {
public:
    // Roots to scan (SKILLS_DIR first, then external dirs).
    void add_root(const std::filesystem::path& root);

    // Scan all roots; returns skills in discovery order (deduped by name).
    std::vector<Skill> scan() const;

    // Current OS platform id (e.g. "linux") for platform-gated skills.
    void set_platform(const std::string& p) { platform_ = p; }

private:
    std::vector<std::filesystem::path> roots_;
    std::string platform_{"linux"};
};

}  // namespace skills
}  // namespace dd

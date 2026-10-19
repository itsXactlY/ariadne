#pragma once
// Skill registry: indexes scanned skills by name + category and resolves
// `skill://<name>[/<file>]` URIs and `view(name, file)` lookups.

#include <vector>
#include <string>
#include <map>
#include <filesystem>
#include "dd/skills/skill.hpp"

namespace dd {
namespace skills {

class Registry {
public:
    void build(const std::vector<Skill>& skills);

    // Index accessors.
    const Skill* find(const std::string& name) const;
    std::vector<std::string> categories() const;
    std::map<std::string, size_t> category_counts() const;

    // Load a skill's SKILL.md (or a file within its dir). Returns empty on miss.
    std::string view(const std::string& name, const std::string& file = "") const;

    // Resolve a `skill://<name>[/<file>]` URI to file content.
    std::string resolve(const std::string& uri) const;

    const std::vector<Skill>& all() const { return skills_; }

private:
    std::vector<Skill> skills_;
    std::map<std::string, size_t> by_name_;
    std::map<std::string, size_t> by_category_;
};

}  // namespace skills
}  // namespace dd

#include "dd/skills/registry.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace dd {
namespace skills {

static std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

void Registry::build(const std::vector<Skill>& skills) {
    skills_ = skills;
    by_name_.clear();
    by_category_.clear();
    for (size_t i = 0; i < skills_.size(); ++i) {
        by_name_.emplace(skills_[i].name, i);
        if (!skills_[i].category.empty())
            by_category_.emplace(skills_[i].category, i);
    }
}

const Skill* Registry::find(const std::string& name) const {
    auto it = by_name_.find(name);
    if (it == by_name_.end()) return nullptr;
    return &skills_[it->second];
}

std::vector<std::string> Registry::categories() const {
    std::map<std::string, size_t> counts;
    for (const auto& s : skills_) {
        auto key = s.category.empty() ? std::string("uncategorized") : s.category;
        counts[key]++;
    }
    std::vector<std::string> out;
    for (const auto& [k, v] : counts) out.push_back(k);
    return out;
}

std::map<std::string, size_t> Registry::category_counts() const {
    std::map<std::string, size_t> counts;
    for (const auto& s : skills_) {
        auto key = s.category.empty() ? std::string("uncategorized") : s.category;
        counts[key]++;
    }
    return counts;
}

std::string Registry::view(const std::string& name, const std::string& file) const {
    const Skill* s = find(name);
    if (!s) return {};
    if (file.empty()) return read_file(s->skill_md);
    std::filesystem::path target = s->skill_dir / file;
    // Guard: target must stay inside skill_dir.
    std::error_code ec;
    auto dir = std::filesystem::canonical(s->skill_dir, ec);
    auto tgt = std::filesystem::canonical(target, ec);
    if (ec) return {};
    if (tgt.string().rfind(dir.string(), 0) != 0) return {};
    return read_file(tgt);
}

std::string Registry::resolve(const std::string& uri) const {
    const std::string pfx = "skill://";
    if (uri.rfind(pfx, 0) != 0) return {};
    std::string rest = uri.substr(pfx.size());
    auto slash = rest.find('/');
    std::string name = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string file = (slash == std::string::npos) ? "" : rest.substr(slash + 1);
    return view(name, file);
}

}  // namespace skills
}  // namespace dd

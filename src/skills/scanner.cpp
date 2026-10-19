#include "dd/skills/scanner.hpp"
#include "dd/core/path.hpp"
#include "dd/core/yaml.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace dd {
namespace skills {

static constexpr size_t MAX_NAME = 64;
static constexpr size_t MAX_DESC = 1024;

static std::string read_head(const std::filesystem::path& p, size_t max_bytes) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string buf(max_bytes, '\0');
    f.read(&buf[0], max_bytes);
    buf.resize((size_t)f.gcount());
    return buf;
}

static std::string first_body_line(const std::string& content) {
    auto b = dd::core::body(content);
    std::stringstream ss(b);
    std::string line;
    while (std::getline(ss, line)) {
        size_t a = 0, z = line.size();
        while (a < z && std::isspace((unsigned char)line[a])) ++a;
        while (z > a && std::isspace((unsigned char)line[z - 1])) --z;
        if (z > a && line[a] != '#') return line.substr(a, z - a);
    }
    return {};
}

// Python: rel = skill_md.relative_to(root); parts = rel.parts; if len>=3 -> parts[0].
// Implemented by string prefix-strip + '/' split (no path::relative_to here).
static std::string category_from_path(const std::filesystem::path& skill_md,
                                      const std::filesystem::path& root) {
    auto sroot = root.string();
    auto spath = skill_md.string();
    if (spath.rfind(sroot, 0) != 0) return {};
    auto rel = spath.substr(sroot.size());
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    std::vector<std::string> parts;
    std::stringstream ss(rel);
    std::string tok;
    while (std::getline(ss, tok, '/'))
        if (!tok.empty()) parts.push_back(tok);
    if (parts.size() >= 3) return parts.front();
    return {};
}

static bool platform_ok(const std::map<std::string, std::string>& fm,
                        const std::string& platform) {
    auto it = fm.find("platform");
    if (it == fm.end()) return true;
    auto plats = dd::core::parse_list(it->second);
    if (plats.empty()) return true;
    return std::find(plats.begin(), plats.end(), platform) != plats.end();
}

void Scanner::add_root(const std::filesystem::path& root) { roots_.push_back(root); }

std::vector<Skill> Scanner::scan() const {
    std::vector<Skill> out;
    std::set<std::string> seen;

    for (const auto& root : roots_) {
        std::vector<std::filesystem::path> found;
        dd::core::walk(root, "SKILL.md", dd::core::WalkOptions{},
                       [&](const std::filesystem::path& p) { found.push_back(p); });
        std::sort(found.begin(), found.end());

        for (const auto& skill_md : found) {
            auto content = read_head(skill_md, 4000);
            auto fm = dd::core::parse_frontmatter(content);
            if (!platform_ok(fm, platform_)) continue;

            std::string name;
            if (auto it = fm.find("name"); it != fm.end())
                name = dd::core::unquote(it->second);
            if (name.empty()) name = skill_md.parent_path().filename().string();
            if (name.size() > MAX_NAME) name.resize(MAX_NAME);
            if (seen.count(name)) continue;
            seen.insert(name);

            std::string desc;
            if (auto it = fm.find("description"); it != fm.end())
                desc = dd::core::unquote(it->second);
            if (desc.empty()) desc = first_body_line(content);
            if (desc.size() > MAX_DESC) desc = desc.substr(0, MAX_DESC - 3) + "...";

            Skill s;
            s.name = std::move(name);
            s.description = std::move(desc);
            s.category = category_from_path(skill_md, root);
            s.skill_md = skill_md;
            s.skill_dir = skill_md.parent_path();
            s.frontmatter = std::move(fm);
            out.push_back(std::move(s));
        }
    }
    return out;
}

}  // namespace skills
}  // namespace dd

// src/know/wiring.cpp — impl of dd/know/wiring.hpp.
#include "dd/know/wiring.hpp"

#include <algorithm>
#include <cstdlib>

namespace dd {
namespace know {

std::filesystem::path expand_home(const std::string& p) {
    if (p == "~" || p.rfind("~/", 0) == 0) {
        const char* h = std::getenv("HOME");
        if (h && *h) return std::filesystem::path(h) / (p.size() > 2 ? p.substr(2) : std::string());
    }
    return std::filesystem::path(p);
}

SkillSetup skill_setup(const core::Config& cfg, const core::Home& home) {
    SkillSetup s;
    if (cfg.has("skills.enabled")) s.enabled = cfg.get_bool("skills.enabled");
    if (cfg.has("skills.auto_route")) s.auto_route = cfg.get_bool("skills.auto_route");
    if (cfg.has("skills.maze")) s.maze = cfg.get_bool("skills.maze");
    s.roots.push_back(home.skills());
    if (const core::Json* dirs = cfg.get_list("skills.external_dirs"))
        for (const auto& d : dirs->arr)
            if (d && d->type == core::Json::Str && !d->str.empty()) s.roots.push_back(expand_home(d->str));
    if (const core::Json* off = cfg.get_list("skills.disabled"))
        for (const auto& d : off->arr)
            if (d && d->type == core::Json::Str && !d->str.empty()) s.disabled.insert(d->str);
    auto num = [&](const char* key, float& dst) { if (cfg.has(key)) dst = static_cast<float>(cfg.get_double(key)); };
    num("knowledge.rank.name", s.bm25.w_name);
    num("knowledge.rank.tags", s.bm25.w_tags);
    num("knowledge.rank.group", s.bm25.w_group);
    num("knowledge.rank.gist", s.bm25.w_gist);
    num("knowledge.rank.k1", s.bm25.k1);
    if (cfg.has("knowledge.page_bytes")) s.page_bytes = static_cast<size_t>(std::max<long long>(1000, cfg.get_int("knowledge.page_bytes")));
    return s;
}

std::shared_ptr<SkillCatalogue> make_skill_catalogue(const SkillSetup& s, std::shared_ptr<context::Memory> mem) {
    std::vector<std::shared_ptr<SkillSource>> sources;
    sources.push_back(std::make_shared<DiskSkills>(s.roots));
    if (s.maze && mem && mem->configured()) sources.push_back(std::make_shared<MazeSkills>(std::move(mem)));
    SkillCatalogue::Options o;
    o.disabled = s.disabled;
    o.bm25 = s.bm25;
    o.page_bytes = s.page_bytes;
    return std::make_shared<SkillCatalogue>(std::move(sources), std::move(o));
}

std::string maze_url_of(const core::Config& cfg) {
    const core::Json* srvs = cfg.get_map("mcp_servers");
    if (!srvs || srvs->type != core::Json::Obj) return "";
    for (const auto& [name, node] : srvs->obj) {
        if (!node || node->type != core::Json::Obj) continue;
        if (name.find("maze") == std::string::npos && name.find("Maze") == std::string::npos) continue;
        if (const core::Json* e = core::json_get(*node, "enabled"); e && e->type == core::Json::Bool && !e->b) continue;
        if (const core::Json* t = core::json_get(*node, "transport"); t && t->type == core::Json::Str && t->str != "http") continue;
        if (const core::Json* u = core::json_get(*node, "url"); u && u->type == core::Json::Str) return u->str;
    }
    return "";
}

}  // namespace know
}  // namespace dd

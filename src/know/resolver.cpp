// src/know/resolver.cpp — impl of dd/know/resolver.hpp.
#include "dd/know/resolver.hpp"

#include <algorithm>
#include <cstdio>

#include "dd/know/text.hpp"
#include "dd/tools/registry.hpp"

namespace dd {
namespace know {

namespace {

using core::Json;

core::JsonSP sp(Json v) { return std::make_shared<Json>(std::move(v)); }

std::string str_arg(const Json& a, const char* k) {
    const Json* v = core::json_get(a, k);
    return v && v->type == Json::Str ? v->str : "";
}

constexpr size_t kSkillsListPage = 60;   // a page of names: ~200 tokens

std::string kb(size_t bytes) {
    char b[24];
    std::snprintf(b, sizeof b, bytes >= 10240 ? "%.0f KB" : "%.1f KB", static_cast<double>(bytes) / 1024.0);
    return b;
}

}  // namespace

std::string Resolver::opened_json(const Uri& u, const Body& b) const {
    Json o = core::json_obj({{"uri", sp(core::json_str(u.str()))}, {"text", sp(core::json_str(b.text))}});
    core::obj_set(o, "page", core::json_str(std::to_string(b.page) + "/" + std::to_string(b.pages)));
    core::obj_set(o, "size", core::json_str(kb(b.total)));
    if (!b.more.empty()) core::obj_set(o, "more", core::json_str("read_file(\"" + b.more + "\")"));
    return core::json_dump(o);
}

std::string Resolver::missing_json(const Uri& u) const {
    std::string msg = std::string("no ") + kind_name(u.kind) + " '" + u.str() + "'";
    if (u.kind == Kind::Skill) {
        std::string near;
        for (const auto& h : router_.find(u.id, {Kind::Skill}, 3)) near += (near.empty() ? "" : ", ") + h.card.uri.id;
        if (!near.empty()) msg += "; close: " + near;
        else msg += "; find(\"what you need\") lists what exists";
    }
    return tools::tool_error(msg);
}

std::optional<std::string> Resolver::read(const std::string& path) {
    const auto u = Uri::parse(path);
    if (!u) return std::nullopt;
    const auto b = router_.open(*u);
    if (!b) return missing_json(*u);
    return opened_json(*u, *b);
}

std::string Resolver::skill_load(const Json& args) {
    std::string name = str_arg(args, "name");
    if (name.empty()) return tools::tool_error("name required");
    if (name.rfind("skill://", 0) != 0) name = "skill://" + name;
    const auto u = Uri::parse(name);
    if (!u) return tools::tool_error("not a skill name: " + name);
    const auto b = router_.open(*u);
    if (!b) return missing_json(*u);
    Json o = core::json_obj({{"name", sp(core::json_str(u->id))}, {"text", sp(core::json_str(b->text))}});
    core::obj_set(o, "page", core::json_str(std::to_string(b->page) + "/" + std::to_string(b->pages)));
    if (!b->more.empty()) core::obj_set(o, "more", core::json_str("read_file(\"" + b->more + "\")"));
    return core::json_dump(o);
}

std::string Resolver::find(const Json& args) const {
    const std::string query = str_arg(args, "query");
    if (query.empty()) return tools::tool_error("query required");
    std::vector<Kind> kinds;
    if (const Json* k = core::json_get(args, "kinds")) {
        std::vector<std::string> names;
        if (k->type == Json::Arr)
            for (const auto& x : k->arr)
                if (x && x->type == Json::Str) names.push_back(x->str);
        if (k->type == Json::Str) names.push_back(k->str);
        for (const auto& n : names)
            if (const auto kind = kind_from(n)) kinds.push_back(*kind);
    }
    int k = 8;
    if (const Json* n = core::json_get(args, "k"); n && n->type == Json::Num) k = std::clamp(static_cast<int>(n->num), 1, 15);
    const auto hits = router_.find(query, kinds, k);

    Json arr = core::json_arr();
    bool any_strong = false;
    for (const auto& h : hits) {
        const bool strong = router_.offers(h);
        any_strong = any_strong || strong;
        char score[16];
        std::snprintf(score, sizeof score, "%.2f", h.score);
        Uri u = h.card.uri;
        u.page.clear();
        Json one = core::json_obj({{"uri", sp(core::json_str(u.str()))},
                                   {"kind", sp(core::json_str(kind_name(u.kind)))},
                                   {"title", sp(core::json_str(h.card.title))},
                                   {"score", sp(core::json_num(std::stod(score)))},
                                   {"strength", sp(core::json_str(strong ? "strong" : "weak"))}});
        if (!h.card.gist.empty()) core::obj_set(one, "gist", core::json_str(utf8_head(h.card.gist, 160)));
        core::obj_set(one, "matched", core::json_str(h.why));
        if (h.card.uri.kind == Kind::Skill && !h.card.has_body) core::obj_set(one, "note", core::json_str("no body to open"));
        else core::obj_set(one, "read", core::json_str(Deliverer::read_hint(u)));
        core::arr_push(arr, one);
    }
    Json out = core::json_obj({{"query", sp(core::json_str(query))}, {"hits", sp(std::move(arr))}});
    if (hits.empty())
        core::obj_set(out, "hint", core::json_str("nothing matched these words: try the names and terms the task itself uses, or look with a search command"));
    else if (!any_strong)
        core::obj_set(out, "hint", core::json_str("all weak: the words overlap a little, not enough to rely on; widen or change the query before concluding anything"));
    return core::json_dump(out);
}

Json Resolver::find_schema() {
    Json props = core::json_obj(
        {{"query", sp(core::json_obj({{"type", sp(core::json_str("string"))},
                                      {"description", sp(core::json_str("what you are looking for, in plain words: a task, a behaviour, a tool, a project"))}}))},
         {"kinds", sp(core::json_obj({{"type", sp(core::json_str("array"))},
                                      {"items", sp(core::json_obj({{"type", sp(core::json_str("string"))}}))},
                                      {"description", sp(core::json_str("skill, memory, tool; default all"))}}))},
         {"k", sp(core::json_obj({{"type", sp(core::json_str("integer"))}, {"description", sp(core::json_str("how many hits, default 8"))}}))},
         {"intent", sp(core::json_obj({{"type", sp(core::json_str("string"))}, {"description", sp(core::json_str("what you are doing, in a few words"))}}))}});
    return core::json_obj(
        {{"description", sp(core::json_str("Find skills, memories and deferred tools by what they are for. Use it FIRST when you do not know where something "
                                           "is or how it is done: never guess a name or a path. Hits come strongest first with a score; below ~0.3 or marked "
                                           "weak, widen the query. Open a hit with read_file(uri)."))},
         {"parameters", sp(core::json_obj({{"type", sp(core::json_str("object"))},
                                           {"properties", sp(std::move(props))},
                                           {"required", sp(core::json_arr({sp(core::json_str("query"))}))}}))}});
}

Json Resolver::skill_load_schema() {
    Json props = core::json_obj({{"name", sp(core::json_obj({{"type", sp(core::json_str("string"))}}))},
                                 {"intent", sp(core::json_obj({{"type", sp(core::json_str("string"))}, {"description", sp(core::json_str("what you are doing, in a few words"))}}))}});
    return core::json_obj(
        {{"description", sp(core::json_str("Read a skill's instructions by name: the same as read_file(\"skill://name\"). A long skill comes a page at a time; "
                                           "the reply names the next page."))},
         {"parameters", sp(core::json_obj({{"type", sp(core::json_str("object"))},
                                           {"properties", sp(std::move(props))},
                                           {"required", sp(core::json_arr({sp(core::json_str("name"))}))}}))}});
}

std::string Resolver::skills_list(const Json& args) const {
    std::string group = str_arg(args, "group");
    if (group == "*") group.clear();                              // every group
    size_t offset = 0;
    if (const Json* v = core::json_get(args, "offset"); v && v->type == Json::Num && v->num > 0)
        offset = static_cast<size_t>(v->num);
    size_t k = kSkillsListPage;
    if (const Json* v = core::json_get(args, "k"); v && v->type == Json::Num)
        k = static_cast<size_t>(std::clamp(static_cast<int>(v->num), 1, 200));
    const auto cat = router_.skill_catalogue();
    if (!cat || !cat->ready()) return tools::tool_error("no skill catalogue to list yet (off, or still warming up); find(\"what you need\") waits for it");
    std::vector<std::string> names;
    for (const auto& c : cat->cards())
        if (group.empty() || c.group == group)
            names.push_back(group.empty() && !c.group.empty() ? c.group + "/" + c.uri.id : c.uri.id);
    std::sort(names.begin(), names.end());
    if (names.empty())
        return tools::tool_error("no skill carries the group '" + group + "': the groups are named in the catalogue map - match one exactly, or ask with no group for every one");
    if (offset >= names.size())
        return tools::tool_error("nothing behind offset " + std::to_string(offset) + " of " + std::to_string(names.size()) + " listed");
    Json arr = core::json_arr();
    size_t i = offset;
    for (; i < names.size() && i - offset < k; ++i) core::arr_push(arr, core::json_str(names[i]));
    Json out = core::json_obj({{"group", sp(core::json_str(group.empty() ? "all" : group))},
                               {"offset", sp(core::json_num(static_cast<double>(offset)))},
                               {"names", sp(std::move(arr))}});
    core::obj_set(out, "total", core::json_num(static_cast<double>(names.size())));
    if (i < names.size())
        core::obj_set(out, "more", core::json_str("skills_list(group=\"" + group + "\", offset=" + std::to_string(i) + ")"));
    core::obj_set(out, "open", core::json_str("read_file(\"skill://<name>\") - the name is what follows the last /; a long skill continues with #2, its own files with skill://<name>/<path>"));
    return core::json_dump(out);
}

Json Resolver::skills_list_schema() {
    Json props = core::json_obj(
        {{"group", sp(core::json_obj({{"type", sp(core::json_str("string"))},
                                      {"description", sp(core::json_str("one group exactly as the catalogue map prints it; empty for every group"))}}))},
         {"offset", sp(core::json_obj({{"type", sp(core::json_str("integer"))},
                                       {"description", sp(core::json_str("first name to return, from the last reply's more"))}}))},
         {"k", sp(core::json_obj({{"type", sp(core::json_str("integer"))},
                                  {"description", sp(core::json_str("how many names, default 60, max 200"))}}))}});
    return core::json_obj(
        {{"description", sp(core::json_str("List the skill NAMES the catalogue census counts, a page at a time, by group: the census names groups and counts them, this is how the names behind a count are read. For what a skill is FOR, use find instead."))},
         {"parameters", sp(core::json_obj({{"type", sp(core::json_str("object"))},
                                           {"properties", sp(std::move(props))}}))}});
}

}  // namespace know
}  // namespace dd

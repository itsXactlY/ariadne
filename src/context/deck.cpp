// src/context/deck.cpp — impl of dd/context/deck.hpp.
#include "dd/context/deck.hpp"

#include <algorithm>

namespace dd {
namespace context {

using core::Json;

namespace {

core::JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

std::string str_arg(const Json& args, const char* key) {
    const Json* v = core::json_get(args, key);
    if (!v || v->type != Json::Str) return "";
    const size_t b = v->str.find_first_not_of(" \t\n");
    if (b == std::string::npos) return "";
    return v->str.substr(b, v->str.find_last_not_of(" \t\n") - b + 1);
}

}  // namespace

bool ToolDeck::deferrable(const std::string& name) const {
    return std::find(deferred_.begin(), deferred_.end(), name) != deferred_.end();
}

void ToolDeck::arrange(const std::set<std::string>& core) {
    visible_.clear();
    deferred_.clear();
    catalog_.clear();
    for (const auto& name : reg_.get_all_tool_names()) {
        if (tools::is_bridge_tool(name)) continue;
        const bool keep = core.count(name) ||
                          std::find(cfg_.always_load.begin(), cfg_.always_load.end(), name) != cfg_.always_load.end() ||
                          reg_.get_toolset_for_tool(name).empty();
        (keep ? visible_ : deferred_).push_back(name);
    }
    const std::vector<Json> deferred_defs = reg_.get_definitions(deferred_, true);
    deferred_tokens_ = tools::estimate_tokens_from_schemas(deferred_defs);
    active_ = tools::should_activate(cfg_, static_cast<int>(deferred_tokens_), std::nullopt);
    if (!active_) {
        visible_.insert(visible_.end(), deferred_.begin(), deferred_.end());
        deferred_.clear();
    }

    defs_ = core::json_arr();
    for (auto& d : reg_.get_definitions(visible_, true)) core::arr_push(defs_, d);
    if (!active_) return;

    for (const auto& td : deferred_defs) {
        const Json* fn = core::json_get(td, "function");
        if (!fn) continue;
        tools::CatalogEntry e;
        const Json* nm = core::json_get(*fn, "name");
        const Json* ds = core::json_get(*fn, "description");
        e.name = nm && nm->type == Json::Str ? nm->str : "";
        e.description = ds && ds->type == Json::Str ? ds->str : "";
        const std::string toolset = reg_.get_toolset_for_tool(e.name);
        e.source = toolset.empty() ? "other" : "mcp";
        e.source_name = toolset;
        e.tokens = tools::tokenize(tools::entry_search_text(td));
        if (!e.name.empty()) catalog_.push_back(std::move(e));
    }
    const Json bridges = tools::bridge_tool_schemas(static_cast<int>(deferred_.size()));
    for (const auto& b : bridges.arr) core::arr_push(defs_, *b);

    auto add = [&](const std::string& name, std::string (ToolDeck::*fn)(const Json&) const) {
        tools::ToolEntry e;
        e.name = name;
        e.toolset = "";   // core: never deferred itself
        for (const auto& b : bridges.arr)
            if (const Json* f = core::json_get(*b, "function"))
                if (const Json* n = core::json_get(*f, "name"); n && n->str == name) {
                    Json schema = core::json_obj();
                    if (const Json* d = core::json_get(*f, "description")) core::obj_set(schema, "description", *d);
                    if (const Json* p = core::json_get(*f, "parameters")) core::obj_set(schema, "parameters", *p);
                    e.schema = schema;
                }
        e.handler = [this, fn](const Json& a) { return (this->*fn)(a); };
        if (registrar) registrar(e);
        else reg_.register_tool(e);
    };
    add(tools::TOOL_SEARCH_NAME, &ToolDeck::search);
    add(tools::TOOL_DESCRIBE_NAME, &ToolDeck::describe);
    add(tools::TOOL_CALL_NAME, &ToolDeck::call);
}

std::string ToolDeck::search(const Json& args) const {
    const std::string query = str_arg(args, "query");
    if (query.empty()) return tools::tool_error("query is required");
    int limit = cfg_.search_default_limit;
    if (const Json* l = core::json_get(args, "limit"); l && l->type == Json::Num)
        limit = std::max(1, std::min(cfg_.max_search_limit, static_cast<int>(l->num)));
    const auto hits = tools::search_catalog(catalog_, query, limit);
    Json matches = core::json_arr();
    for (const auto& h : hits) core::arr_push(matches, tools::format_search_hit(h));
    Json out = core::json_obj({{"query", jsp(core::json_str(query))},
                               {"total_available", jsp(core::json_num(static_cast<double>(catalog_.size())))},
                               {"matches", jsp(std::move(matches))}});
    if (hits.empty() && !catalog_.empty()) {
        core::obj_set(out, "available_sources", tools::available_source_summary(catalog_));
        core::obj_set(out, "hint", core::json_str(
            "No lexical match was found, but the sources above are connected and their tools remain available. "
            "Retry tool_search with the service name plus a concrete action or object before concluding the "
            "capability is unavailable."));
    }
    return core::json_dump(out);
}

std::string ToolDeck::describe(const Json& args) const {
    const std::string name = str_arg(args, "name");
    if (name.empty()) return tools::tool_error("name is required");
    if (!deferrable(name))
        return tools::tool_error("'" + name + "' is not a deferrable tool. If you see it in the tools list already, "
                                 "call it directly; otherwise check the spelling against tool_search.");
    for (const auto& td : reg_.get_definitions({name}, true))
        if (const Json* fn = core::json_get(td, "function")) {
            Json out = core::json_obj({{"name", jsp(core::json_str(name))}});
            if (const Json* d = core::json_get(*fn, "description")) core::obj_set(out, "description", *d);
            if (const Json* p = core::json_get(*fn, "parameters")) core::obj_set(out, "parameters", *p);
            return core::json_dump(out);
        }
    return tools::tool_error("'" + name + "' is not currently available. Re-run tool_search to refresh.");
}

std::string ToolDeck::call(const Json& args) const {
    const std::string name = str_arg(args, "name");
    if (name.empty()) return tools::tool_error("tool_call requires a 'name' argument");
    if (tools::is_bridge_tool(name))
        return tools::tool_error("tool_call cannot invoke '" + name + "' (it is itself a bridge tool)");
    if (!deferrable(name))
        return tools::tool_error("'" + name + "' is not a deferrable tool. If it appears in the model-facing tools "
                                 "list already, call it directly instead of via tool_call.");
    Json inner = core::json_obj();
    if (const Json* a = core::json_get(args, "arguments")) {
        if (a->type == Json::Str) {
            try {
                inner = core::json_parse(a->str.empty() ? "{}" : a->str);
            } catch (const std::exception& e) {
                return tools::tool_error(std::string("tool_call 'arguments' is not valid JSON: ") + e.what());
            }
        } else {
            inner = *a;
        }
    }
    if (inner.type == Json::Null) inner = core::json_obj();
    if (inner.type != Json::Obj) return tools::tool_error("tool_call 'arguments' must be an object");
    if (const Json* schema = reg_.get_schema(name))
        if (const Json* params = core::json_get(*schema, "parameters"))
            if (const Json* req = core::json_get(*params, "required"); req && req->type == Json::Arr) {
                std::string missing;
                for (const auto& r : req->arr)
                    if (r && r->type == Json::Str && !core::json_get(inner, r->str))
                        missing += (missing.empty() ? "" : ", ") + r->str;
                if (!missing.empty())
                    return core::json_dump(core::json_obj(
                        {{"error", jsp(core::json_str("tool_call to '" + name + "' is missing required argument(s): " +
                                                      missing + ". The tool was NOT invoked."))},
                         {"parameters", jsp(*params)},
                         {"hint", jsp(core::json_str("Retry tool_call with 'arguments' matching the parameters schema above."))}}));
            }
    return reg_.dispatch(name, inner);
}

}  // namespace context
}  // namespace dd

// Port of the pure lexical + config pieces of tools/tool_search.py.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/tools/tool_search.hpp"

using namespace dd;
using namespace dd::core;

namespace dd {
namespace tools {

static std::string py_str_of(const Json& v);

static const std::vector<std::string> BRIDGE_NAMES = {TOOL_SEARCH_NAME,
                                                      TOOL_DESCRIBE_NAME,
                                                      TOOL_CALL_NAME};

bool is_bridge_tool(const std::string& name) {
    for (const auto& n : BRIDGE_NAMES) {
        if (n == name) return true;
    }
    return false;
}

// ---- small python-semantics helpers -------------------------------------
static std::string py_strip(const std::string& s) {
    size_t a = 0, b = s.size();
    auto ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string py_lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Python int(value) for the value shapes that appear in config.
static bool py_int_of(const Json* v, long long& out) {
    if (!v) return false;
    switch (v->type) {
        case Json::Bool: out = v->b ? 1 : 0; return true;
        case Json::Num: out = (long long)v->num; return true;  // trunc toward zero
        case Json::Str: {
            std::string s = py_strip(v->str);
            if (s.empty()) return false;
            size_t i = 0;
            bool neg = false;
            if (s[i] == '+' || s[i] == '-') { neg = (s[i] == '-'); ++i; }
            if (i >= s.size()) return false;
            long long acc = 0;
            for (; i < s.size(); ++i) {
                if (!std::isdigit((unsigned char)s[i])) return false;
                acc = acc * 10 + (s[i] - '0');
            }
            out = neg ? -acc : acc;
            return true;
        }
        default: return false;
    }
}

static bool py_float_of(const Json* v, double& out) {
    if (!v) return false;
    switch (v->type) {
        case Json::Bool: out = v->b ? 1.0 : 0.0; return true;
        case Json::Num: out = v->num; return true;
        case Json::Str: {
            std::string s = py_strip(v->str);
            if (s.empty()) return false;
            std::istringstream is(s);
            double d;
            is >> d;
            if (is.fail()) return false;
            std::string rest;
            if (is >> rest) return false;  // trailing garbage -> ValueError
            out = d;
            return true;
        }
        default: return false;
    }
}

static int safe_int(const Json* v, int fallback) {
    long long x;
    if (py_int_of(v, x)) return (int)x;
    return fallback;
}

static double safe_float(const Json* v, double fallback) {
    double x;
    if (py_float_of(v, x)) return x;
    return fallback;
}

// ---- config -------------------------------------------------------------
static ToolSearchConfig defaults_auto() {
    return ToolSearchConfig{"auto", 5.0, 5, 20, {}};
}

ToolSearchConfig ToolSearchConfig::from_raw(const core::Json& raw) {
    if (raw.type == Json::Bool && !raw.b) {
        return ToolSearchConfig{"off", 5.0, 5, 20, {}};
    }
    if (raw.type != Json::Obj) {
        return defaults_auto();  // True, non-dict, None -> auto
    }

    std::string enabled_raw = "auto";
    if (const Json* e = json_get(raw, "enabled")) enabled_raw = py_lower(py_strip(py_str_of(*e)));

    std::string enabled;
    if (enabled_raw == "true" || enabled_raw == "1" || enabled_raw == "yes") enabled = "on";
    else if (enabled_raw == "false" || enabled_raw == "0" || enabled_raw == "no") enabled = "off";
    else if (enabled_raw == "auto" || enabled_raw == "on" || enabled_raw == "off") enabled = enabled_raw;
    else enabled = "auto";

    double threshold_pct = safe_float(json_get(raw, "threshold_pct"), 5.0);
    threshold_pct = std::max(0.0, std::min(100.0, threshold_pct));

    int max_search_limit = std::max(1, std::min(50, safe_int(json_get(raw, "max_search_limit"), 20)));
    int search_default_limit = std::max(1, std::min(max_search_limit, safe_int(json_get(raw, "search_default_limit"), 5)));

    std::vector<std::string> always_load;
    if (const Json* a = json_get(raw, "always_load")) {
        if (a->type == Json::Str) {
            std::string s = py_strip(a->str);
            if (!s.empty()) always_load.push_back(s);
        } else if (a->type == Json::Arr) {
            for (const auto& sp : a->arr) {
                if (sp && sp->type == Json::Str) {
                    std::string s = py_strip(sp->str);
                    if (!s.empty()) always_load.push_back(s);
                }
            }
        }
    }

    return ToolSearchConfig{enabled, threshold_pct, search_default_limit, max_search_limit, always_load};
}

// ---- lexical helpers ----------------------------------------------------
static std::string py_str_of(const Json& v) {
    if (v.type == Json::Str) return v.str;
    if (v.type == Json::Bool) return v.b ? "true" : "false";
    if (v.type == Json::Null) return "None";
    return json_dump(v);
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            cur.push_back((char)std::tolower((unsigned char)c));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string entry_search_text(const core::Json& tool_def) {
    const Json* fn = json_get(tool_def, "function");
    Json fn_null;
    if (!fn) fn = &fn_null;  // {} fallback
    std::string name;
    if (const Json* n = json_get(*fn, "name")) name = py_str_of(*n);
    std::string desc;
    if (const Json* d = json_get(*fn, "description")) desc = (d->type == Json::Str) ? d->str : std::string();

    std::string param_names;
    if (const Json* params = json_get(*fn, "parameters")) {
        if (const Json* props = json_get(*params, "properties")) {
            for (size_t i = 0; i < props->obj.size(); ++i) {
                if (i) param_names += " ";
                param_names += props->obj[i].first;
            }
        }
    }

    std::string name_words = name;
    for (char& c : name_words) {
        if (c == '_' || c == '.' || c == '-' || c == ':') c = ' ';
    }

    return name_words + " " + desc + " " + param_names;
}

// ---- BM25 + affinity ----------------------------------------------------
static double bm25_score(const std::vector<std::string>& query_tokens,
                         const std::vector<std::string>& doc_tokens,
                         double avg_dl,
                         const std::map<std::string, int>& doc_freq, int n_docs,
                         double k1 = 1.5, double b = 0.75) {
    if (doc_tokens.empty()) return 0.0;
    double score = 0.0;
    long long dl = (long long)doc_tokens.size();
    std::map<std::string, int> doc_tf;
    for (const auto& t : doc_tokens) doc_tf[t]++;
    for (const auto& q : query_tokens) {
        auto dfit = doc_freq.find(q);
        int df = (dfit != doc_freq.end()) ? dfit->second : 0;
        if (df == 0) continue;
        double idf = std::log(1.0 + ((double)n_docs - df + 0.5) / (df + 0.5));
        auto tfit = doc_tf.find(q);
        int tf = (tfit != doc_tf.end()) ? tfit->second : 0;
        if (tf == 0) continue;
        double denom = (double)tf + k1 * (1.0 - b + b * (double)dl / std::max(avg_dl, 1.0));
        double norm = (double)tf * (k1 + 1.0) / denom;
        score += idf * norm;
    }
    return score;
}

double name_affinity(const std::string& query,
                            const std::vector<std::string>& query_tokens,
                            const std::string& name) {
    std::string ql = py_strip(query);
    ql = py_lower(ql);
    std::string nl = py_lower(name);
    if (ql == nl) return 3.0;
    if (!ql.empty() && nl.find(ql) != std::string::npos) return 2.0;

    std::vector<std::string> nt = tokenize(name);
    std::set<std::string> name_tokens(nt.begin(), nt.end());
    if (query_tokens.empty()) return 0.0;
    std::set<std::string> qset(query_tokens.begin(), query_tokens.end());
    if (std::includes(name_tokens.begin(), name_tokens.end(), qset.begin(), qset.end())) return 1.0;
    size_t inter = 0;
    for (const auto& q : qset) {
        if (name_tokens.count(q)) ++inter;
    }
    return (double)inter / (double)qset.size();
}

static std::vector<CatalogEntry> search_catalog_impl(
    const std::vector<CatalogEntry>& catalog, const std::string& query, int limit) {
    if (catalog.empty() || limit <= 0) return {};
    std::vector<std::string> query_tokens = tokenize(query);
    if (query_tokens.empty()) return {};

    std::vector<int> doc_lengths;
    doc_lengths.reserve(catalog.size());
    long long total = 0;
    for (const auto& e : catalog) {
        doc_lengths.push_back((int)e.tokens.size());
        total += (long long)e.tokens.size();
    }
    double avg_dl = (double)total / (double)std::max((size_t)1, catalog.size());

    std::map<std::string, int> doc_freq;
    for (const auto& e : catalog) {
        std::set<std::string> distinct(e.tokens.begin(), e.tokens.end());
        for (const auto& t : distinct) doc_freq[t]++;
    }
    int n_docs = (int)catalog.size();

    struct Row { double aff, rel; std::string name; size_t idx; };
    std::vector<Row> rows;
    for (size_t i = 0; i < catalog.size(); ++i) {
        double aff = name_affinity(query, query_tokens, catalog[i].name);
        double rel = bm25_score(query_tokens, catalog[i].tokens, avg_dl, doc_freq, n_docs);
        if (aff || rel > 0) rows.push_back({aff, rel, catalog[i].name, i});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.aff != b.aff) return a.aff > b.aff;
        if (a.rel != b.rel) return a.rel > b.rel;
        return a.name < b.name;
    });
    std::vector<CatalogEntry> out;
    for (size_t i = 0; i < rows.size() && (int)i < limit; ++i) out.push_back(catalog[rows[i].idx]);
    return out;
}

std::vector<CatalogEntry> search_catalog(const std::vector<CatalogEntry>& catalog,
                                         const std::string& query, int limit) {
    return search_catalog_impl(catalog, query, limit);
}

std::vector<std::pair<std::string, double>> rank_documents(
    const std::vector<std::pair<std::string, std::string>>& documents,
    const std::string& query, int limit) {
    if (documents.empty() || limit <= 0) return {};
    std::vector<std::string> query_tokens = tokenize(query);
    if (query_tokens.empty()) return {};

    std::vector<std::vector<std::string>> token_docs;
    token_docs.reserve(documents.size());
    for (const auto& nd : documents) {
        std::string name = nd.first;
        for (char& c : name) {
            if (c == '-' || c == '_') c = ' ';
        }
        std::string text = name + " " + nd.second;
        token_docs.push_back(tokenize(text));
    }

    std::vector<int> doc_lengths;
    long long total = 0;
    for (const auto& d : token_docs) {
        doc_lengths.push_back((int)d.size());
        total += (long long)d.size();
    }
    double avg_dl = (double)total / (double)std::max((size_t)1, documents.size());

    std::map<std::string, int> doc_freq;
    for (const auto& d : token_docs) {
        std::set<std::string> distinct(d.begin(), d.end());
        for (const auto& t : distinct) doc_freq[t]++;
    }
    int n_docs = (int)documents.size();

    struct Row { double aff, rel; std::string name; };
    std::vector<Row> rows;
    for (size_t i = 0; i < documents.size(); ++i) {
        double aff = name_affinity(query, query_tokens, documents[i].first);
        double rel = bm25_score(query_tokens, token_docs[i], avg_dl, doc_freq, n_docs);
        if (aff || rel > 0) rows.push_back({aff, rel, documents[i].first});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.aff != b.aff) return a.aff > b.aff;
        if (a.rel != b.rel) return a.rel > b.rel;
        return a.name < b.name;
    });
    std::vector<std::pair<std::string, double>> out;
    for (size_t i = 0; i < rows.size() && (int)i < limit; ++i)
        out.push_back({rows[i].name, rows[i].aff + rows[i].rel});
    return out;
}

std::string source_label(const std::string& source_name) {
    std::string label = source_name.empty() ? "other" : source_name;
    if (label.rfind("mcp-", 0) == 0) label = label.substr(4);
    return label;
}

core::Json format_search_hit(const CatalogEntry& entry) {
    std::string desc = entry.description;
    if (desc.size() > 400) desc = desc.substr(0, 400);
    Json o = json_obj();
    obj_set(o, "name", json_str(entry.name));
    obj_set(o, "source", json_str(entry.source));
    obj_set(o, "source_name", json_str(entry.source_name));
    obj_set(o, "description", json_str(desc));
    return o;
}

core::Json available_source_summary(const std::vector<CatalogEntry>& catalog) {
    std::map<std::string, int> counts;  // map is sorted by key (matches sorted(counts))
    for (const auto& e : catalog) counts[source_label(e.source_name)]++;
    Json arr = json_arr();
    for (const auto& kv : counts) {
        Json o = json_obj();
        obj_set(o, "name", json_str(kv.first));
        obj_set(o, "tool_count", json_num((double)kv.second));
        arr_push(arr, o);
    }
    return arr;
}

int estimate_tokens_from_schemas(const std::vector<core::Json>& tool_defs) {
    long long total_chars = 0;
    for (const auto& td : tool_defs) total_chars += (long long)json_dump(td).size();
    return (int)std::ceil((double)total_chars / 4.0);
}

bool should_activate(const ToolSearchConfig& config, int deferrable_tokens,
                     const std::optional<long long>& /*context_length*/) {
    if (config.enabled == "off") return false;
    if (deferrable_tokens <= 0) return false;
    return true;
}

core::Json bridge_tool_schemas(int deferred_count) {
    const std::string n = std::to_string(deferred_count);
    const std::string desc_search =
        "Search the " + n +
        " tools that are not loaded in this context. Returns matching names with their descriptions; "
        "follow with `tool_describe` for a tool's parameter schema, then `tool_call` to invoke it. All " + n +
        " are connected and callable \xe2\x80\x94 never report a capability as missing until a search for it "
        "returned no match. Tools already listed in this context are loaded and must be called directly, not searched.";
    const std::string desc_describe =
        "Load the full JSON schema for one tool returned by `tool_search`. Required before `tool_call` if the tool's parameters are unknown.";
    const std::string desc_call =
        "Invoke a deferred tool by name with the given arguments. Argument shape matches the tool's schema (see `tool_describe`). Policy, hooks, and approvals run exactly as for any directly-listed tool.";

    Json arr = json_arr();

    {  // search
        Json td = json_obj();
        obj_set(td, "type", json_str("function"));
        Json fn = json_obj();
        obj_set(fn, "name", json_str(TOOL_SEARCH_NAME));
        obj_set(fn, "description", json_str(desc_search));
        Json params = json_obj();
        obj_set(params, "type", json_str("object"));
        Json props = json_obj();
        Json qp = json_obj();
        obj_set(qp, "type", json_str("string"));
        obj_set(qp, "description", json_str("Keywords describing the capability you need (e.g. 'create github issue')."));
        obj_set(props, "query", qp);
        Json lp = json_obj();
        obj_set(lp, "type", json_str("integer"));
        obj_set(lp, "description", json_str("Maximum number of results to return. Default 5."));
        obj_set(props, "limit", lp);
        obj_set(params, "properties", props);
        Json req = json_arr();
        arr_push(req, json_str("query"));
        obj_set(params, "required", req);
        obj_set(fn, "parameters", params);
        obj_set(td, "function", fn);
        arr_push(arr, td);
    }
    {  // describe
        Json td = json_obj();
        obj_set(td, "type", json_str("function"));
        Json fn = json_obj();
        obj_set(fn, "name", json_str(TOOL_DESCRIBE_NAME));
        obj_set(fn, "description", json_str(desc_describe));
        Json params = json_obj();
        obj_set(params, "type", json_str("object"));
        Json props = json_obj();
        Json np = json_obj();
        obj_set(np, "type", json_str("string"));
        obj_set(np, "description", json_str("Exact tool name (as returned by tool_search)."));
        obj_set(props, "name", np);
        obj_set(params, "properties", props);
        Json req = json_arr();
        arr_push(req, json_str("name"));
        obj_set(params, "required", req);
        obj_set(fn, "parameters", params);
        obj_set(td, "function", fn);
        arr_push(arr, td);
    }
    {  // call
        Json td = json_obj();
        obj_set(td, "type", json_str("function"));
        Json fn = json_obj();
        obj_set(fn, "name", json_str(TOOL_CALL_NAME));
        obj_set(fn, "description", json_str(desc_call));
        Json params = json_obj();
        obj_set(params, "type", json_str("object"));
        Json props = json_obj();
        Json np = json_obj();
        obj_set(np, "type", json_str("string"));
        obj_set(np, "description", json_str("Exact tool name to invoke."));
        obj_set(props, "name", np);
        Json ap = json_obj();
        obj_set(ap, "type", json_str("object"));
        obj_set(ap, "description", json_str("Arguments for the tool, matching its schema."));
        obj_set(props, "arguments", ap);
        obj_set(params, "properties", props);
        Json req = json_arr();
        arr_push(req, json_str("name"));
        arr_push(req, json_str("arguments"));
        obj_set(params, "required", req);
        obj_set(fn, "parameters", params);
        obj_set(td, "function", fn);
        arr_push(arr, td);
    }

    return arr;
}

}  // namespace tools
}  // namespace dd

#pragma once
// Pure lexical engine + config for the tool_search subsystem
// (port of tools/tool_search.py). Only the registry-independent pieces are
// ported here: tokenize/entry-text, BM25 + name-affinity ranking over a
// provided catalog, catalog source summaries, the bridge tool schemas, the
// ToolSearchConfig coercion, token estimation, and activation gating.
// The registry-coupled functions (build_catalog source classification,
// is_deferrable/classify/assemble/dispatch/resolve/validate) are NOT ported.
#include <optional>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace tools {

inline const std::string TOOL_SEARCH_NAME = "tool_search";
inline const std::string TOOL_DESCRIBE_NAME = "tool_describe";
inline const std::string TOOL_CALL_NAME = "tool_call";

struct ToolSearchConfig {
    std::string enabled;
    double threshold_pct = 5.0;
    int search_default_limit = 5;
    int max_search_limit = 20;
    std::vector<std::string> always_load;

    static ToolSearchConfig from_raw(const core::Json& raw);
};

bool is_bridge_tool(const std::string& name);

// Lexical helpers.
std::vector<std::string> tokenize(const std::string& text);
std::string entry_search_text(const core::Json& tool_def);
double name_affinity(const std::string& query,
                     const std::vector<std::string>& query_tokens,
                     const std::string& name);

core::Json bridge_tool_schemas(int deferred_count);

// Catalog entry: the registry-coupled build_catalog is not ported; the caller
// supplies name/description/source/source_name and the entry tokens.
struct CatalogEntry {
    std::string name;
    std::string description;
    std::string source;
    std::string source_name;
    std::vector<std::string> tokens;
};

// Ranking engine (BM25 + name affinity), faithful to search_catalog/rank_documents.
std::vector<CatalogEntry> search_catalog(const std::vector<CatalogEntry>& catalog,
                                         const std::string& query, int limit = 5);
std::vector<std::pair<std::string, double>> rank_documents(
    const std::vector<std::pair<std::string, std::string>>& documents,
    const std::string& query, int limit = 20);

std::string source_label(const std::string& source_name);
core::Json format_search_hit(const CatalogEntry& entry);
core::Json available_source_summary(const std::vector<CatalogEntry>& catalog);

// Token estimation + activation gate.
int estimate_tokens_from_schemas(const std::vector<core::Json>& tool_defs);
bool should_activate(const ToolSearchConfig& config, int deferrable_tokens,
                     const std::optional<long long>& context_length);

}  // namespace tools
}  // namespace dd

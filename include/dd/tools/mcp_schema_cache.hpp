#pragma once
// Pure helpers ported from tools/mcp_schema_cache.py.
// NOT ported (fs/IO-coupled, not claimed here): _cache_path, _load_all,
// _save_all, get_cached_entry, has_cached_entry, write_cache_entry,
// clear_cache_entry (all read/write the on-disk cache file via atomic_json_write).
#include "dd/core/json.hpp"
#include <string>
#include <vector>

namespace dd {
namespace tools {

using core::Json;
using core::JsonSP;

// Stable hash of the connection-defining parts of an MCP server config.
// Mirrors config_fingerprint(): canonical compact JSON (sort_keys, separators
// (",",":")) of {command,args,url,transport,tools_include,tools_exclude}, then
// sha256 hex truncated to 16 chars. NOTE: Python json.dumps uses ensure_ascii=True;
// the core json_dump passes UTF-8 through, so non-ASCII config values would
// diverge. Realistic MCP configs (commands/URLs/tool names) are ASCII.
std::string config_fingerprint(const Json& config);

// entry["tools"] if it is a list, else empty.
std::vector<JsonSP> tools_from_cache_entry(const Json& entry);

// entry["utility_tools"] if it is a list, else empty.
std::vector<JsonSP> utility_tools_from_cache_entry(const Json& entry);

}  // namespace tools
}  // namespace dd

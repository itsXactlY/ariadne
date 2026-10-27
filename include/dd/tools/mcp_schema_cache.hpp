#pragma once
// Ported from tools/mcp_schema_cache.py: the fingerprint, the entry accessors,
// and the on-disk half (_cache_path, _load_all, _save_all, get_cached_entry,
// has_cached_entry, write_cache_entry, clear_cache_entry).
//
// The whole thing is one rule: a cached manifest may only be served while the
// fingerprint of the connection config still matches it. Everything below is
// keyed on that, and nothing reads the file without going through it.
#include "dd/core/json.hpp"
#include <filesystem>
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

// The fingerprint for a dd_tui server node. dd gates tools with a per-name
// `tools: {name: {enabled}}` map instead of Python's include/exclude lists, so
// the map is folded into the payload as the disabled names: a config that
// disables a tool has to miss, and a config that changes nothing else does not.
// Headers are not part of the key, as in Python — they do not change the manifest.
std::string server_fingerprint(const Json& server_config);

// entry["tools"] if it is a list, else empty.
std::vector<JsonSP> tools_from_cache_entry(const Json& entry);

// entry["utility_tools"] if it is a list, else empty.
std::vector<JsonSP> utility_tools_from_cache_entry(const Json& entry);

// cache_dir / "mcp_schema_cache.json" — the filename Python calls _CACHE_FILENAME.
std::filesystem::path cache_path(const std::filesystem::path& cache_dir);

// The whole cache as one object, server name -> entry. A file that is missing,
// unreadable, unparsable, or not an object reads as empty: a corrupt cache must
// cost a live list, never a boot failure.
Json load_all(const std::filesystem::path& file);

// Write the whole cache: temp file in the same directory, fsync, chmod 0600,
// rename. A reader sees the old file or the new one, never half of either.
// False means nothing was written and the old file is still there.
bool save_all(const std::filesystem::path& file, const Json& data);

// The entry only when its fingerprint matches; Json{} (Null) on a miss.
// A changed config is a miss, so a stale manifest can never be registered.
Json get_cached_entry(const std::filesystem::path& file, const std::string& server_name,
                      const std::string& fingerprint);

bool has_cached_entry(const std::filesystem::path& file, const std::string& server_name,
                      const std::string& fingerprint);

// Persist a manifest after a successful live connect. An entry identical to what
// is already on disk is not rewritten — the file keeps its mtime, which is how a
// served boot is told apart from a boot that listed.
bool write_cache_entry(const std::filesystem::path& file, const std::string& server_name,
                       const std::string& fingerprint, const Json& tools,
                       const Json& utility_tools);

// Drop one server's entry. A server that is gone from the config leaves nothing behind.
bool clear_cache_entry(const std::filesystem::path& file, const std::string& server_name);

}  // namespace tools
}  // namespace dd

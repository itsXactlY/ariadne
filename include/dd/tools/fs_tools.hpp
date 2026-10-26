#pragma once
// dd/tools/fs_tools.hpp — read_file / write_file / list_dir, shared by dd_tui and dd_agent.

#include <cstddef>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace tools {

struct FsLimits {
    size_t max_read_bytes = 1u << 20;   // bytes a read returns in one call
    size_t max_entries = 2000;          // directory entries one list returns
};

// {path, content as "     N|text" lines, total_lines, offset, lines} or {"error"}.
std::string fs_read(const core::Json& args, const FsLimits& limits = {});
// {path, bytes} or {"error"}.
std::string fs_write(const core::Json& args);
// {path, entries} or {"error"}.
std::string fs_list(const core::Json& args, const FsLimits& limits = {});
// The raw bytes of a file, no line numbering. Same guards fs_read applies: a directory, a
// non-regular file, a file over `cap`, and a file whose first bytes hold a NUL are refused with the
// reason, not with empty content. The patch applier needs the file exactly as it is; this is the
// only place the binary probe lives, so the two readers cannot drift apart.
std::string fs_read_raw(const std::string& path, std::size_t cap, std::string& err);
// The OpenAI-style schema of a tool with these string properties (plus the optional `intent`).
core::Json fs_tool_schema(const std::vector<std::string>& props, const std::string& desc);

}  // namespace tools
}  // namespace dd

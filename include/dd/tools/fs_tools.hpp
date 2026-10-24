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
// The OpenAI-style schema of a tool with these string properties (plus the optional `intent`).
core::Json fs_tool_schema(const std::vector<std::string>& props, const std::string& desc);

}  // namespace tools
}  // namespace dd

#pragma once
// dd::tools::terminal_hints — output-pattern failure hints for the terminal tool.
// Port of tools/terminal_hints.py.
#include <map>
#include <optional>
#include <string>

namespace dd {
namespace tools {

// Return one short recovery hint for a failed command, or nullopt.
// exit_code == 0 -> nullopt. Only the first 4000 chars of output are scanned;
// at most one hint is returned.
std::optional<std::string> annotate_failure(const std::string& command,
                                            int exit_code,
                                            const std::string& output);

}  // namespace tools
}  // namespace dd

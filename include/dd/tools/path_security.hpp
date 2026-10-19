// tools/path_security.py -> dd::tools
#ifndef DD_TOOLS_PATH_SECURITY_HPP
#define DD_TOOLS_PATH_SECURITY_HPP
#include <filesystem>
#include <optional>
#include <string>

namespace dd {
namespace tools {

// validate_within_dir: returns an error message if *path* resolves outside
// *root*, or std::nullopt when safe. resolve() follows symlinks + normalizes
// '..' (weakly_canonical matches Python resolve() on non-existent paths).
std::optional<std::string> validate_within_dir(const std::filesystem::path& path,
                                               const std::filesystem::path& root);

// has_traversal_component: True if path_str has a '..' path component.
bool has_traversal_component(const std::string& path_str);

}  // tools
}  // dd
#endif

// tools/path_security.py -> dd::tools
#include "dd/tools/path_security.hpp"

#include <filesystem>

namespace dd {
namespace tools {

namespace {

// Python Path.relative_to: succeeds iff root's components are a leading
// prefix of resolved's components (resolved == root is allowed -> ".").
bool is_within(const std::filesystem::path& resolved,
               const std::filesystem::path& root) {
    auto ri = resolved.begin();
    const auto re = resolved.end();
    for (auto rr = root.begin(), rre = root.end(); rr != rre; ++rr) {
        if (ri == re) return false;  // resolved shorter than root
        if (*ri != *rr) return false;
        ++ri;
    }
    return true;
}

}  // anon

// Path.resolve() -> weakly_canonical (normalizes, follows symlinks for the
// existing prefix, works on non-existent paths like Python resolve()).
std::optional<std::string> validate_within_dir(const std::filesystem::path& path,
                                               const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
    if (ec) return "Path escapes allowed directory: " + ec.message();
    std::error_code ec2;
    std::filesystem::path root_resolved = std::filesystem::weakly_canonical(root, ec2);
    if (ec2) return "Path escapes allowed directory: " + ec2.message();
    if (!is_within(resolved, root_resolved)) {
        return "Path escapes allowed directory: " + resolved.string() +
               " is not within " + root_resolved.string();
    }
    return std::nullopt;
}

bool has_traversal_component(const std::string& path_str) {
    std::filesystem::path p(path_str);
    for (auto it = p.begin(), e = p.end(); it != e; ++it) {
        if (*it == "..") return true;
    }
    return false;
}

}  // tools
}  // dd

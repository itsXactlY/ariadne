#include "dd/core/path.hpp"

#include <algorithm>
#include <set>
#include <system_error>

namespace dd {
namespace core {

std::filesystem::path canonical(const std::filesystem::path& p) {
    std::error_code ec;
    auto out = std::filesystem::canonical(p, ec);
    if (ec) throw std::system_error(ec);
    return out;
}

static bool has_excluded_part(const std::filesystem::path& p,
                              const std::vector<std::string>& excluded) {
    for (const auto& part : p) {
        auto s = part.string();
        if (std::find(excluded.begin(), excluded.end(), s) != excluded.end())
            return true;
    }
    return false;
}

// Iterative DFS with an explicit stack; follows symlinked directories once.
void walk(const std::filesystem::path& root,
          const std::string& filename,
          const WalkOptions& opts,
          std::function<void(const std::filesystem::path&)> visitor) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;

    std::vector<std::filesystem::path> stack{root};
    std::set<std::filesystem::path> visited{dd::core::canonical(root)};

    while (!stack.empty()) {
        auto dir = stack.back();
        stack.pop_back();

        for (std::filesystem::directory_iterator it(dir, ec), end;
             !ec && it != end; it.increment(ec)) {
            const auto entry = it->path();
            std::error_code lec;
            auto lt = std::filesystem::symlink_status(entry, lec);
            if (lec) continue;

            bool is_dir = std::filesystem::is_directory(entry);
            bool is_link = std::filesystem::is_symlink(entry);

            bool recurse = is_dir ||
                           (is_link && opts.follow_symlinks &&
                            std::filesystem::is_directory(
                                std::filesystem::read_symlink(entry, ec)));
            if (recurse) {
                std::error_code vec;
                auto canon = std::filesystem::canonical(entry, vec);
                if (vec) continue;
                if (visited.insert(canon).second) {
                    if (!has_excluded_part(entry, opts.exclude_parts))
                        stack.push_back(entry);
                }
                continue;
            }

            if (entry.filename() == filename) {
                if (!has_excluded_part(entry, opts.exclude_parts))
                    visitor(entry);
            }
        }
    }
}

}  // namespace core
}  // namespace dd

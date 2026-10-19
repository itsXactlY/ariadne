#pragma once
// dd::core::path — filesystem walking that FOLLOWS directory symlinks.
// This is the C++ replacement for Python's pathlib.rglob(), which silently
// skips symlinked directories whose real target is outside the scan root.
// That rglob limitation is the root cause of "daedalus rarely uses a skill".

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dd {
namespace core {

struct WalkOptions {
    // Names that, if present as any path component, exclude the entry.
    std::vector<std::string> exclude_parts{".git", ".github", ".hub",
                                           ".curator_backups"};
    // Follow directory symlinks (default true — the fix).
    bool follow_symlinks = true;
};

// Visit every regular file under `root` whose filename == `filename`
// (e.g. "SKILL.md"). Symlinked directories are followed exactly once
// (cycle-safe via a canonical-visited set). `visitor` receives the
// absolute path of each match.
void walk(const std::filesystem::path& root,
          const std::string& filename,
          const WalkOptions& opts,
          std::function<void(const std::filesystem::path&)> visitor);

// Resolve a possibly-symlinked path to its canonical form (throws on error).
std::filesystem::path canonical(const std::filesystem::path& p);

}  // namespace core
}  // namespace dd

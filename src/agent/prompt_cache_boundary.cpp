#include "dd/agent/prompt_cache_boundary.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace dd {
namespace agent {

static const std::size_t MAX_ENTRIES = 32;
static const std::size_t MAX_CHARS = 4 * 1024 * 1024;

// front = oldest (LRU), back = newest. Python OrderedDict + move_to_end, guarded by a
// threading.Lock; here the registry is thread-local instead, which is the same guarantee with no
// lock at all. A stable prefix is registered while a prompt is being built and matched while the
// next prompt on that same thread is built, so the registry never crosses a thread and there is
// no shared mutable state for a mutex to guard. The caps are per thread, and the threads that
// build prompts are few and bounded, so the 4 MiB ceiling stays a ceiling.
static thread_local std::vector<std::string> prefixes_;

static std::size_t total_chars() {
    std::size_t s = 0;
    for (const auto& p : prefixes_) s += p.size();
    return s;
}

void register_stable_prefix(const std::string& prefix) {
    if (prefix.empty()) return;
    auto it = std::find(prefixes_.begin(), prefixes_.end(), prefix);
    if (it != prefixes_.end()) prefixes_.erase(it);  // move_to_end
    prefixes_.push_back(prefix);
    while (prefixes_.size() > MAX_ENTRIES) prefixes_.erase(prefixes_.begin());  // popitem(last=False)
    while (prefixes_.size() > 1 && total_chars() > MAX_CHARS) prefixes_.erase(prefixes_.begin());
}

std::optional<std::string> find_stable_prefix(const std::string& content) {
    std::string best;
    bool found = false;
    for (const auto& p : prefixes_) {
        if (content.size() > p.size() && content.compare(0, p.size(), p) == 0) {
            if (!found || p.size() > best.size()) {
                best = p;
                found = true;
            }
        }
    }
    if (found) {
        auto it = std::find(prefixes_.begin(), prefixes_.end(), best);
        if (it != prefixes_.end()) {
            prefixes_.erase(it);
            prefixes_.push_back(best);  // move_to_end
        }
        return best;
    }
    return std::nullopt;
}

void clear_stable_prefixes() {
    prefixes_.clear();
}

}  // namespace agent
}  // namespace dd

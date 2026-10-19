#include "dd/agent/prompt_cache_boundary.hpp"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace dd {
namespace agent {

static const std::size_t MAX_ENTRIES = 32;
static const std::size_t MAX_CHARS = 4 * 1024 * 1024;

static std::mutex lock_;
// front = oldest (LRU), back = newest. Python OrderedDict + move_to_end.
static std::vector<std::string> prefixes_;

static std::size_t total_chars() {
    std::size_t s = 0;
    for (const auto& p : prefixes_) s += p.size();
    return s;
}

void register_stable_prefix(const std::string& prefix) {
    if (prefix.empty()) return;
    std::lock_guard<std::mutex> g(lock_);
    auto it = std::find(prefixes_.begin(), prefixes_.end(), prefix);
    if (it != prefixes_.end()) prefixes_.erase(it);  // move_to_end
    prefixes_.push_back(prefix);
    while (prefixes_.size() > MAX_ENTRIES) prefixes_.erase(prefixes_.begin());  // popitem(last=False)
    while (prefixes_.size() > 1 && total_chars() > MAX_CHARS) prefixes_.erase(prefixes_.begin());
}

std::optional<std::string> find_stable_prefix(const std::string& content) {
    std::lock_guard<std::mutex> g(lock_);
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
    std::lock_guard<std::mutex> g(lock_);
    prefixes_.clear();
}

}  // namespace agent
}  // namespace dd

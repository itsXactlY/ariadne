#include "dd/agent/payload_tape.hpp"
#include "dd/core/json.hpp"

#include <algorithm>
#include <memory>
#include <vector>

namespace dd {
namespace agent {

using JsonSP = std::shared_ptr<core::Json>;

void PayloadTape::start(const std::vector<JsonSP>& sources,
                        const std::vector<core::Json>& rendered, int gen) {
    blocks.clear();
    Block b;
    b.sources = sources;
    b.rendered.reserve(rendered.size());
    for (const auto& r : rendered) b.rendered.push_back(core::json_deep_copy(r));
    blocks.push_back(std::move(b));
    generation = gen;
}

std::pair<int, int> PayloadTape::matched_blocks(const std::vector<JsonSP>& view, int gen) const {
    if (!generation || *generation != gen) return {0, 0};
    int nblocks = 0;
    std::size_t pos = 0;
    for (const auto& blk : blocks) {
        std::size_t end = pos + blk.sources.size();
        if (end > view.size()) break;
        bool ok = true;
        for (std::size_t i = 0; i < blk.sources.size(); ++i) {
            if (view[pos + i] != blk.sources[i]) { ok = false; break; }  // identity
        }
        if (!ok) break;
        ++nblocks;
        pos = end;
    }
    return {nblocks, static_cast<int>(pos)};
}

void PayloadTape::truncate(int n) {
    if (n < 0) n = 0;
    if (static_cast<std::size_t>(n) < blocks.size())
        blocks.erase(blocks.begin() + n, blocks.end());
}

void PayloadTape::append(JsonSP source, const std::vector<core::Json>& rendered) {
    Block b;
    b.sources.push_back(std::move(source));
    b.rendered.reserve(rendered.size());
    for (const auto& r : rendered) b.rendered.push_back(core::json_deep_copy(r));
    blocks.push_back(std::move(b));
}

std::vector<core::Json> PayloadTape::payload() const {
    std::vector<core::Json> out;
    for (const auto& blk : blocks)
        for (const auto& r : blk.rendered)
            out.push_back(core::json_deep_copy(r));
    return out;
}

}  // namespace agent
}  // namespace dd

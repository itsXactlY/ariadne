#pragma once
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include "dd/core/json.hpp"

namespace dd {
namespace agent {

// Faithful port of agent/payload_tape.py::PayloadTape.
// Source messages are identified by object identity (shared_ptr pointer equality),
// mirroring Python's `is not` in matched_blocks.
class PayloadTape {
public:
    struct Block {
        std::vector<std::shared_ptr<core::Json>> sources;
        std::vector<core::Json> rendered;
    };

    std::vector<Block> blocks;
    std::optional<int> generation;

    explicit operator bool() const { return !blocks.empty(); }

    std::shared_ptr<core::Json> first_source() const {
        if (blocks.empty() || blocks[0].sources.empty()) return nullptr;
        return blocks[0].sources[0];
    }

    void reset() {
        blocks.clear();
        generation.reset();
    }

    void start(const std::vector<std::shared_ptr<core::Json>>& sources,
               const std::vector<core::Json>& rendered, int gen);

    // (blocks, pos) of the longest tape prefix `view` still starts with.
    std::pair<int, int> matched_blocks(const std::vector<std::shared_ptr<core::Json>>& view,
                                       int gen) const;

    void truncate(int n);

    void append(std::shared_ptr<core::Json> source, const std::vector<core::Json>& rendered);

    std::vector<core::Json> payload() const;
};

}  // namespace agent
}  // namespace dd

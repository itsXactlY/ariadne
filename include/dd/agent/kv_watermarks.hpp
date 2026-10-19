#pragma once
// Pure port of agent/kv_watermarks.py: the deterministic watermark math and
// string/slot helpers. The network readers (read_slots, erase_slot) require
// urllib + a live llama-server and are intentionally NOT ported here.
#include <optional>
#include <string>
#include "dd/core/json.hpp"

namespace dd {
namespace agent {

struct Watermarks {
    int fast_layer_tokens = 0;
    double low = 0.60;
    double high = 0.80;
    double hard = 0.90;

    std::string level(int pool_tokens) const;
    int hard_tokens() const;
    double high_ratio_of_hard() const;
};

// llama-server root for a local base_url, or nullopt for anything remote
// (Python Optional[str]; None -> nullopt).
std::optional<std::string> server_root(const std::string& base_url);

// slots is a JSON array of slot objects (as parsed from /slots).
int slot_tokens(const core::Json& slots, int slot_id);
bool slot_busy(const core::Json& slots, int slot_id);

}  // namespace agent
}  // namespace dd

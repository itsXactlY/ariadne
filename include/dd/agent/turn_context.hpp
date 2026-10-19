#pragma once
// Pure subset of agent/turn_context.py: the two pure compression-progress
// decision helpers. The turn prologue itself (build_turn_context, the
// TurnContext dataclass) and every helper that reaches model_metadata
// (build_memory_context_block), plan_focus, or estimate_messages_tokens_rough
// is NOT ported. Float thresholds mirror Python's IEEE double arithmetic.
#include <cstdint>

namespace dd {
namespace agent {

// compression_made_progress: True if a compression pass materially reduced the
// request — row-count drop OR a >5% token reduction.
inline bool compression_made_progress(
    std::int64_t orig_len, std::int64_t new_len,
    std::int64_t orig_tokens, std::int64_t new_tokens) {
    if (new_len < orig_len) return true;
    return orig_tokens > 0 && static_cast<double>(new_tokens) < static_cast<double>(orig_tokens) * 0.95;
}

// _compression_warrants_another_preflight_pass: still over threshold AND the
// previous pass materially reduced token pressure (>5%).
inline bool compression_warrants_another_preflight_pass(
    std::int64_t orig_tokens, std::int64_t new_tokens, std::int64_t threshold_tokens) {
    return new_tokens >= threshold_tokens &&
           orig_tokens > 0 &&
           static_cast<double>(new_tokens) < static_cast<double>(orig_tokens) * 0.95;
}

}  // namespace agent
}  // namespace dd

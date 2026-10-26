// src/know/policy.cpp — Policy::from: the knowledge: and skills: sections of config.yaml.
#include "dd/know/policy.hpp"

namespace dd {
namespace know {

Policy Policy::from(const core::Config& cfg) {
    Policy p;
    auto num = [&](const char* key, auto& dst) {
        if (cfg.has(key)) dst = static_cast<std::decay_t<decltype(dst)>>(cfg.get_double(key));
    };
    auto flag = [&](const char* key, bool& dst) {
        if (cfg.has(key)) dst = cfg.get_bool(key);
    };
    num("knowledge.deep_ms", p.deep_ms);
    num("knowledge.deep_recall_ms", p.deep_recall_ms);
    num("knowledge.budget.turn_tokens", p.turn_tokens);
    num("knowledge.budget.hint_tokens", p.hint_tokens);
    num("knowledge.budget.max_turn_tokens", p.max_turn_tokens);
    if (cfg.has("knowledge.budget.hint_tokens")) {   // a failure or a stall may say a little more than a shift
        p.failure_tokens = p.hint_tokens + p.hint_tokens / 5;
        p.stall_tokens = p.hint_tokens + p.hint_tokens * 2 / 5;
    }
    num("knowledge.confidence.offer_score", p.skills.offer_score);
    num("knowledge.confidence.offer_mass", p.skills.offer_mass);
    num("knowledge.confidence.open_score", p.skills.open_score);
    num("knowledge.confidence.memory_offer_score", p.memory.offer_score);
    num("knowledge.confidence.memory_offer_mass", p.memory.offer_mass);
    num("knowledge.open_max_bytes", p.open_max_bytes);
    num("knowledge.top_k", p.top_k);
    flag("knowledge.refusal.enabled", p.refusal.on);
    num("knowledge.refusal.offers", p.refusal.refuse_offers);
    num("knowledge.refusal.floor", p.refusal.refuse_floor);
    num("knowledge.refusal.probe_turns", p.refusal.probe_turns);
    flag("knowledge.auto_open", p.auto_open);
    flag("knowledge.mid_turn.shift", p.shift);
    flag("knowledge.mid_turn.failure", p.failure);
    flag("knowledge.mid_turn.stall", p.stall);
    flag("skills.auto_route", p.auto_route);
    return p;
}

}  // namespace know
}  // namespace dd

// src/context/endpoint.cpp — impl of dd/context/endpoint.hpp.
#include "dd/context/endpoint.hpp"

#include "dd/net/http.hpp"

#include <algorithm>
#include <cctype>

namespace dd {
namespace context {

using core::Json;

namespace {

const char* const kLadder[] = {"off", "low", "medium", "high", "xhigh", "max", "ultra"};

double num(const Json& o, const char* k) {
    const Json* v = core::json_get(o, k);
    return v && v->type == Json::Num ? v->num : 0.0;
}

}  // namespace

Endpoint::Endpoint(std::string base_url) : base_url_(std::move(base_url)) {
    std::string b = base_url_;
    while (!b.empty() && b.back() == '/') b.pop_back();
    if (b.size() >= 3 && b.compare(b.size() - 3, 3, "/v1") == 0) b.resize(b.size() - 3);
    std::string low = b;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low.find("127.0.0.1") != std::string::npos || low.find("localhost") != std::string::npos) root_ = b;
    info_.local = !root_.empty();
}

const ServerInfo& Endpoint::probe() {
    if (root_.empty()) return info_;
    const net::Response p = net::get(root_ + "/props", 3000);
    if (p.ok()) {
        try {
            const Json j = core::json_parse(p.body);
            info_.reachable = true;
            if (const Json* g = core::json_get(j, "default_generation_settings"); g && g->type == Json::Obj)
                info_.n_ctx = static_cast<long>(num(*g, "n_ctx"));
            if (info_.n_ctx <= 0) info_.n_ctx = static_cast<long>(num(j, "n_ctx"));
            if (const Json* t = core::json_get(j, "chat_template"); t && t->type == Json::Str) info_.chat_template = t->str;
            if (const Json* bi = core::json_get(j, "build_info"); bi && bi->type == Json::Str) info_.build = bi->str;
            info_.strata = info_.build.rfind("Strata", 0) == 0;
            if (const Json* ts = core::json_get(j, "total_slots"); ts && ts->type == Json::Num && ts->num >= 1)
                info_.slots = static_cast<int>(ts->num);
        } catch (...) {
        }
    }
    if (const auto s = slots(); !s.empty()) info_.slots = static_cast<int>(s.size());
    main_ = std::min(main_.load(), std::max(0, info_.slots - 1));
    return info_;
}

int Endpoint::main_slot() const { return std::min(main_.load(), std::max(0, info_.slots - 1)); }

int Endpoint::sidekick_slot() const {
    if (info_.slots < 2) return 0;
    return main_slot() != 0 ? 0 : 1;
}

void Endpoint::promote(int slot) {
    if (slot >= 0 && slot < info_.slots) main_ = slot;
}

std::vector<Endpoint::Slot> Endpoint::slots() const {
    std::vector<Slot> out;
    if (root_.empty()) return out;
    const net::Response r = net::get(root_ + "/slots", 1500);
    if (!r.ok()) return out;
    try {
        const Json j = core::json_parse(r.body);
        if (j.type != Json::Arr) return out;
        for (const auto& sp : j.arr) {
            if (!sp || sp->type != Json::Obj) continue;
            Slot s;
            s.id = static_cast<int>(num(*sp, "id"));
            s.tokens = static_cast<long>(num(*sp, "n_prompt_tokens"));
            if (s.tokens <= 0) s.tokens = static_cast<long>(num(*sp, "n_past"));
            if (const Json* b = core::json_get(*sp, "is_processing"); b && b->type == Json::Bool) s.busy = b->b;
            out.push_back(s);
        }
    } catch (...) {
    }
    return out;
}

long Endpoint::pool_tokens(long main_tokens) const {
    if (!multi_slot()) return main_tokens;
    const int side = sidekick_slot();
    for (const auto& s : slots())
        if (s.id == side) return main_tokens + s.tokens;
    return main_tokens;
}

bool Endpoint::erase(int slot) const {
    if (root_.empty()) return false;
    return net::post(root_ + "/slots/" + std::to_string(slot) + "?action=erase", "{}", 5000).ok();
}

void Endpoint::shape(Json& body, int slot, bool thinking) const {
    if (root_.empty()) return;   // a remote API gets the OpenAI fields only
    core::obj_set(body, "n_cache_reuse", core::json_num(256));   // a spliced middle is relocated, not re-read
    if (!single_sequence() && slot >= 0) core::obj_set(body, "id_slot", core::json_num(slot));
    if (thinking && thinking_budget > 0) {
        // llama-server reads it as thinking_budget_tokens; Strata knows only this name
        core::obj_set(body, "reasoning_budget_tokens", core::json_num(thinking_budget));
        core::obj_set(body, "reasoning_budget_message", core::json_str(kThinkingBudgetMessage));
    }
}

std::vector<std::string> Endpoint::efforts() const {
    std::vector<std::string> out;
    const std::string& t = info_.chat_template;
    if (!info_.reachable || t.empty()) {
        for (const char* e : kLadder) out.emplace_back(e);
        return out;
    }
    const bool levels = t.find("reasoning_effort") != std::string::npos;
    if (!levels && t.find("enable_thinking") == std::string::npos) return out;   // no switch at all
    out.emplace_back("off");
    for (int i = 1; i < 7; ++i) {
        const std::string e = kLadder[i];
        if (!levels ? i <= 3 : t.find("'" + e + "'") != std::string::npos || t.find("\"" + e + "\"") != std::string::npos)
            out.push_back(e);
    }
    return out;
}

}  // namespace context
}  // namespace dd

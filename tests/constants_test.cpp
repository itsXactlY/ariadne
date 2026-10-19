// Differential test for constants.hpp vs Python ground truth (daedalus_constants.py).
#include "dd/core/constants.hpp"
#include <cstdio>
#include <string>
#include <vector>
#include <optional>

using namespace dd::core;

static int failures = 0;

static void chk(const std::string& what, bool ok) {
    if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

// json_dump of expected compact forms
static std::string jdump(const Json& j) { return json_dump(j); }

int main() {
    // ---- parse_reasoning_effort ----
    chk("parse false", jdump(parse_reasoning_effort(json_bool(false))) == "{\"enabled\":false}");
    chk("parse true", parse_reasoning_effort(json_bool(true)).type == Json::Null);
    chk("parse null", parse_reasoning_effort(json_null()).type == Json::Null);
    chk("parse empty", parse_reasoning_effort(json_str("")).type == Json::Null);
    chk("parse spaces", parse_reasoning_effort(json_str("  ")).type == Json::Null);
    chk("parse none", jdump(parse_reasoning_effort(json_str("none"))) == "{\"enabled\":false}");
    chk("parse FALSE", jdump(parse_reasoning_effort(json_str("FALSE"))) == "{\"enabled\":false}");
    chk("parse Disabled", jdump(parse_reasoning_effort(json_str("Disabled"))) == "{\"enabled\":false}");
    chk("parse High", jdump(parse_reasoning_effort(json_str(" High "))) == "{\"enabled\":true,\"effort\":\"high\"}");
    chk("parse minimal", jdump(parse_reasoning_effort(json_str("minimal"))) == "{\"enabled\":true,\"effort\":\"minimal\"}");
    chk("parse ultra", jdump(parse_reasoning_effort(json_str("ultra"))) == "{\"enabled\":true,\"effort\":\"ultra\"}");
    chk("parse bogus", parse_reasoning_effort(json_str("bogus")).type == Json::Null);
    chk("parse MAX", jdump(parse_reasoning_effort(json_str("MAX"))) == "{\"enabled\":true,\"effort\":\"max\"}");

    // ---- _canonical_model_variants ----
    {
        std::vector<std::string> gx = {
            "x","anthropic/x","openai/x","google/x","openrouter/x","groq/x","mistral/x","xai/x","cohere/x","perplexity/x","together/x","fireworks/x","deepseek/x",
            "openrouter/anthropic/x","opencode/anthropic/x","fireworks/anthropic/x","groq/anthropic/x","together/anthropic/x",
            "openrouter/openai/x","opencode/openai/x","fireworks/openai/x","groq/openai/x","together/openai/x",
            "openrouter/google/x","opencode/google/x","fireworks/google/x","groq/google/x","together/google/x",
            "openrouter/openrouter/x","opencode/openrouter/x","fireworks/openrouter/x","groq/openrouter/x","together/openrouter/x",
            "openrouter/groq/x","opencode/groq/x","fireworks/groq/x","groq/groq/x","together/groq/x",
            "openrouter/mistral/x","opencode/mistral/x","fireworks/mistral/x","groq/mistral/x","together/mistral/x",
            "openrouter/xai/x","opencode/xai/x","fireworks/xai/x","groq/xai/x","together/xai/x",
            "openrouter/cohere/x","opencode/cohere/x","fireworks/cohere/x","groq/cohere/x","together/cohere/x",
            "openrouter/perplexity/x","opencode/perplexity/x","fireworks/perplexity/x","groq/perplexity/x","together/perplexity/x",
            "openrouter/together/x","opencode/together/x","fireworks/together/x","groq/together/x","together/together/x",
            "openrouter/fireworks/x","opencode/fireworks/x","fireworks/fireworks/x","groq/fireworks/x","together/fireworks/x",
            "openrouter/deepseek/x","opencode/deepseek/x","fireworks/deepseek/x","groq/deepseek/x","together/deepseek/x"};
        auto got = _canonical_model_variants("x");
        chk("x size", got.size() == gx.size());
        bool eq = (got == gx);
        chk("x full", eq);
    }
    {
        auto g = _canonical_model_variants("gpt-4");
        chk("gpt-4 size", g.size() == 146);
        chk("gpt-4 h0", g[0] == "gpt-4");
        chk("gpt-4 h1", g[1] == "gpt.4");
        chk("gpt-4 h2", g[2] == "anthropic/gpt-4");
        chk("gpt-4 t-1", g[145] == "together/deepseek/gpt.4");
    }
    {
        auto g = _canonical_model_variants("anthropic/claude-opus-4.5");
        chk("anth size", g.size() == 292);
        chk("anth h0", g[0] == "anthropic/claude-opus-4.5");
        chk("anth h1", g[1] == "anthropic/claude-opus-4-5");
        chk("anth h4", g[4] == "claude-opus-4.5");
        chk("anth t", g[291] == "together/deepseek/claude.opus.4-5");
    }
    {
        auto g = _canonical_model_variants("openrouter/claude-3");
        chk("orsize", g.size() == 146);
        chk("or h0", g[0] == "openrouter/claude-3");
        chk("or h1", g[1] == "openrouter/claude.3");
        chk("or h2", g[2] == "claude-3");
        chk("or t", g[145] == "together/deepseek/claude.3");
    }

    // ---- resolve_per_model_reasoning_effort ----
    Json ov = json_obj();
    obj_set(ov, "claude-opus-4-5", json_str("high"));
    obj_set(ov, "gpt-4", json_bool(false));
    obj_set(ov, "bad", json_str("bogus"));
    chk("pm claude", jdump(resolve_per_model_reasoning_effort("claude-opus-4.5", ov)) == "{\"enabled\":true,\"effort\":\"high\"}");
    chk("pm gpt-4", jdump(resolve_per_model_reasoning_effort("gpt-4", ov)) == "{\"enabled\":false}");
    chk("pm gpt-4o", resolve_per_model_reasoning_effort("gpt-4o", ov).type == Json::Null);
    chk("pm x", resolve_per_model_reasoning_effort("x", ov).type == Json::Null);
    chk("pm empty-ov", resolve_per_model_reasoning_effort("gpt-4", json_obj()).type == Json::Null);
    chk("pm null-ov", resolve_per_model_reasoning_effort("gpt-4", json_null()).type == Json::Null);

    // ---- resolve_reasoning_config ----
    {
        Json cfg = json_obj();
        Json ag = json_obj();
        Json ror = json_obj();
        obj_set(ror, "gpt-4", json_str("max"));
        obj_set(ag, "reasoning_overrides", ror);
        obj_set(cfg, "agent", ag);
        obj_set(cfg, "model", json_str("gpt-4"));
        chk("cfg per-model", jdump(resolve_reasoning_config(cfg)) == "{\"enabled\":true,\"effort\":\"max\"}");
    }
    {
        Json cfg = json_obj();
        Json ag = json_obj();
        obj_set(ag, "reasoning_effort", json_str("high"));
        obj_set(cfg, "agent", ag);
        chk("cfg high", jdump(resolve_reasoning_config(cfg)) == "{\"enabled\":true,\"effort\":\"high\"}");
    }
    {
        Json cfg = json_obj();
        Json ag = json_obj();
        obj_set(ag, "reasoning_effort", json_bool(false));
        obj_set(cfg, "agent", ag);
        chk("cfg false", jdump(resolve_reasoning_config(cfg)) == "{\"enabled\":false}");
    }
    {
        Json cfg = json_obj();
        Json ag = json_obj();
        Json ror = json_obj();
        obj_set(ror, "x", json_str("high"));
        obj_set(ag, "reasoning_overrides", ror);
        obj_set(cfg, "agent", ag);
        Json mc = json_obj();
        obj_set(mc, "default", json_str("x"));
        obj_set(cfg, "model", mc);
        chk("cfg model-dict", jdump(resolve_reasoning_config(cfg)) == "{\"enabled\":true,\"effort\":\"high\"}");
    }
    {
        Json cfg = json_obj();
        Json ag = json_obj();
        obj_set(ag, "reasoning_effort", json_str("bogus"));
        obj_set(cfg, "agent", ag);
        chk("cfg bogus", resolve_reasoning_config(cfg).type == Json::Null);
    }
    chk("cfg empty", resolve_reasoning_config(json_obj()).type == Json::Null);

    // ---- windows_path_to_wsl ----
    auto ok = [](const std::optional<std::string>& o, const std::string& want) { return o && *o == want; };
    chk("w C\\Users\\x", ok(windows_path_to_wsl("C:\\Users\\x"), "/mnt/c/Users/x"));
    chk("w c:/tmp/y", ok(windows_path_to_wsl("c:/tmp/y"), "/mnt/c/tmp/y"));
    chk("w Cbs", ok(windows_path_to_wsl("C:\\"), "/mnt/c/"));
    chk("w nopath", !windows_path_to_wsl("/mnt/c/x"));
    chk("w D:foo", !windows_path_to_wsl("D:foo"));
    chk("w C:\\a\\b", ok(windows_path_to_wsl("C:\\a\\b"), "/mnt/c/a/b"));
    chk("w space", !windows_path_to_wsl(" "));

    // ---- wsl_unc_path_to_posix ----
    chk("u ubuntu", ok(wsl_unc_path_to_posix("\\\\wsl.localhost\\Ubuntu\\home\\x"), "/home/x"));
    chk("u wsl$", ok(wsl_unc_path_to_posix("\\\\wsl$\\Ubuntu\\a"), "/a"));
    chk("u fwdslash", ok(wsl_unc_path_to_posix("//wsl.localhost/Ubuntu/"), "/"));
    chk("u ci", ok(wsl_unc_path_to_posix("\\\\WSL.LOCALHOST\\U\\"), "/"));
    chk("u x", !wsl_unc_path_to_posix("x"));
    chk("u empty", !wsl_unc_path_to_posix(""));
    chk("u nodistro", !wsl_unc_path_to_posix("\\\\wsl.localhost\\d"));

    // ---- is_first_party_module ----
    chk("ip agent", is_first_party_module("agent"));
    chk("ip agent.foo", is_first_party_module("agent.foo"));
    chk("ip toolsets", is_first_party_module("toolsets"));
    chk("!ip agentops", !is_first_party_module("agentops"));
    chk("ip daedalus_x", is_first_party_module("daedalus_x"));
    chk("!ip empty", !is_first_party_module(""));
    chk("ip daedalus_", is_first_party_module("daedalus_"));
    chk("ip utils", is_first_party_module("utils"));
    chk("!ip agents", !is_first_party_module("agents"));

    if (failures == 0) { std::printf("constants: 60 tests: PASS\n"); }
    return failures;
}

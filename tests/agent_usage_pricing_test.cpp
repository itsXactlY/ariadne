// Differential test: agent/usage_pricing.py pure subset vs Python ground truth.
#include "dd/agent/usage_pricing.hpp"
#include <string>
#include <iostream>

using namespace dd;
using namespace dd::agent;

static int g_fail = 0;
static void chk(const std::string& what, bool ok) {
    if (!ok) { std::cout << "FAIL " << what << "\n"; ++g_fail; }
}

int main() {
    // format_token_count_compact
    chk("tok:0", format_token_count_compact(0) == "0");
    chk("tok:5", format_token_count_compact(5) == "5");
    chk("tok:999", format_token_count_compact(999) == "999");
    chk("tok:1000", format_token_count_compact(1000) == "1K");
    chk("tok:1001", format_token_count_compact(1001) == "1K");
    chk("tok:1500", format_token_count_compact(1500) == "1.5K");
    chk("tok:9999", format_token_count_compact(9999) == "10K");
    chk("tok:10000", format_token_count_compact(10000) == "10K");
    chk("tok:15000", format_token_count_compact(15000) == "15K");
    chk("tok:99999", format_token_count_compact(99999) == "100K");
    chk("tok:100000", format_token_count_compact(100000) == "100K");
    chk("tok:123456", format_token_count_compact(123456) == "123K");
    chk("tok:999999", format_token_count_compact(999999) == "1000K");
    chk("tok:1e6", format_token_count_compact(1000000) == "1M");
    chk("tok:1.5e6", format_token_count_compact(1500000) == "1.5M");
    chk("tok:12345678", format_token_count_compact(12345678) == "12.3M");
    chk("tok:1e9", format_token_count_compact(1000000000LL) == "1B");
    chk("tok:neg1500", format_token_count_compact(-1500) == "-1.5K");
    chk("tok:neg999", format_token_count_compact(-999) == "-999");
    chk("tok:1234", format_token_count_compact(1234) == "1.23K");

    // format_duration_compact
    chk("dur:0", format_duration_compact(0) == "0s");
    chk("dur:30", format_duration_compact(30) == "30s");
    chk("dur:30.5", format_duration_compact(30.5) == "30s");
    chk("dur:59.9", format_duration_compact(59.9) == "60s");
    chk("dur:59.6", format_duration_compact(59.6) == "60s");
    chk("dur:60", format_duration_compact(60) == "1m");
    chk("dur:90", format_duration_compact(90) == "2m");
    chk("dur:1.4", format_duration_compact(1.4) == "1s");
    chk("dur:120", format_duration_compact(120) == "2m");
    chk("dur:3599", format_duration_compact(3599) == "60m");
    chk("dur:3600", format_duration_compact(3600) == "1h");
    chk("dur:3660", format_duration_compact(3660) == "1h 1m");
    chk("dur:3661", format_duration_compact(3661) == "1h 1m");
    chk("dur:7200", format_duration_compact(7200) == "2h");
    chk("dur:86400", format_duration_compact(86400) == "1.0d");
    chk("dur:90000", format_duration_compact(90000) == "1.0d");
    chk("dur:86460", format_duration_compact(86460) == "1.0d");

    // resolve_billing_route
    struct C { const char* m, *p, *b, *r_p, *r_m, *r_b, *r_mode; };
    const C cases[] = {
        {"gpt-4o", "", "", "unknown", "gpt-4o", "", "unknown"},
        {"anthropic/claude-3", "anthropic", "", "anthropic", "claude-3", "", "official_docs_snapshot"},
        {"openrouter/meta/llama", "openrouter", "", "openrouter", "openrouter/meta/llama", "", "official_models_api"},
        {"x", "openai-codex", "", "openai-codex", "x", "", "subscription_included"},
        {"Anthropic/foo", "", "", "unknown", "foo", "", "unknown"},
        {"google/gemini", "", "", "google", "gemini", "", "unknown"},
        {"meta/llama", "", "https://openrouter.ai/v1", "openrouter", "meta/llama", "https://openrouter.ai/v1", "official_models_api"},
        {"x", "", "http://localhost:8080", "custom", "x", "http://localhost:8080", "unknown"},
        {"", "", "", "unknown", "", "", "unknown"},
        {"a/b/c", "openai", "", "openai", "c", "", "official_docs_snapshot"},
        {" a/b ", " Anthropic ", " X ", "anthropic", "b", " X ", "official_docs_snapshot"},
        {"x", "CUSTOM", "", "custom", "x", "", "unknown"},
        {"m", "local", "", "local", "m", "", "unknown"},
        {"d", "weird", "u", "weird", "d", "u", "unknown"},
        {"p/q", "unknown", "", "unknown", "q", "", "unknown"},
        {"deepseek/deepseek-coder", "", "", "unknown", "deepseek-coder", "", "unknown"},
        {"x", "", "OPENROUTER.AI", "openrouter", "x", "OPENROUTER.AI", "official_models_api"},
    };
    for (const auto& c : cases) {
        BillingRoute r = resolve_billing_route(c.m, c.p, c.b);
        std::string id = std::string("route:") + c.m + "/" + c.p + "/" + c.b;
        bool ok = r.provider == c.r_p && r.model == c.r_m && r.base_url == c.r_b && r.billing_mode == c.r_mode;
        chk(id, ok);
    }

    if (g_fail) { std::cout << "usage_pricing: FAIL " << g_fail << "\n"; return 1; }
    std::cout << "all usage_pricing tests passed\n";
    return 0;
}

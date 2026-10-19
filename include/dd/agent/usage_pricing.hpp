#pragma once
// Pure subset of agent/usage_pricing.py: string-routed billing resolution and
// compact number formatters. The pricing tables (_OFFICIAL_DOCS_PRICING),
// metadata fetch, Decimal cost math (estimate_usage_cost / get_pricing /
// estimate_cost_usd) and normalize_usage are NOT ported here.
#include <string>
#include <cmath>
#include <cstdio>

namespace dd {
namespace agent {

struct BillingRoute {
    std::string provider;
    std::string model;
    std::string base_url;
    std::string billing_mode;
};

namespace detail {
inline std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline std::string last_seg(const std::string& m) {
    auto p = m.rfind('/');
    return (p == std::string::npos) ? m : m.substr(p + 1);
}
inline bool contains(const std::string& hay, const std::string& needle) {
    return !needle.empty() && hay.find(needle) != std::string::npos;
}
}  // namespace detail
using namespace detail;

// resolve_billing_route: normalize provider/base/model, infer provider from a
// "<known>/" model prefix, then map to a billing route.
inline BillingRoute resolve_billing_route(
    const std::string& model_name,
    const std::string& provider = "",
    const std::string& base_url = "") {
    std::string provider_name = lower(trim(provider));
    std::string base = lower(trim(base_url));
    std::string model = trim(model_name);
    if (provider_name.empty() && model.find('/') != std::string::npos) {
        auto pos = model.find('/');
        std::string inferred = model.substr(0, pos);  // case-sensitive (Python)
        if (inferred == "anthropic" || inferred == "openai" || inferred == "google") {
            provider_name = inferred;
            model = model.substr(pos + 1);
        }
    }
    if (provider_name == "openai-codex")
        return {"openai-codex", model, base_url, "subscription_included"};
    if (provider_name == "openrouter" || contains(base, "openrouter.ai"))
        return {"openrouter", model, base_url, "official_models_api"};
    if (provider_name == "anthropic")
        return {"anthropic", last_seg(model), base_url, "official_docs_snapshot"};
    if (provider_name == "openai")
        return {"openai", last_seg(model), base_url, "official_docs_snapshot"};
    if (provider_name == "custom" || provider_name == "local" ||
        (!base.empty() && contains(base, "localhost")))
        return {provider_name.empty() ? "custom" : provider_name, model, base_url, "unknown"};
    return {provider_name.empty() ? "unknown" : provider_name,
            model.empty() ? "" : last_seg(model), base_url, "unknown"};
}

// format_duration_compact: seconds -> "Ns" / "Nm" / "Nh[ Nm]" / "N.Nd".
inline std::string format_duration_compact(double seconds) {
    char buf[64];
    if (seconds < 60) { std::snprintf(buf, sizeof buf, "%.0fs", seconds); return buf; }
    double minutes = seconds / 60;
    if (minutes < 60) { std::snprintf(buf, sizeof buf, "%.0fm", minutes); return buf; }
    double hours = minutes / 60;
    if (hours < 24) {
        long long ih = static_cast<long long>(hours);
        long long rem = static_cast<long long>(std::fmod(minutes, 60.0));
        if (rem) std::snprintf(buf, sizeof buf, "%lldh %lldm", ih, rem);
        else std::snprintf(buf, sizeof buf, "%lldh", ih);
        return buf;
    }
    double days = hours / 24;
    std::snprintf(buf, sizeof buf, "%.1fd", days);
    return buf;
}

// format_token_count_compact: int -> "0"/"999"/"1.5K"/"100K"/"1M"/"1B" etc.
inline std::string format_token_count_compact(long long value) {
    long long av = value < 0 ? -value : value;
    if (av < 1000) return std::to_string(value);
    std::string sign = value < 0 ? "-" : "";
    const long long thresholds[3] = {1000000000LL, 1000000LL, 1000LL};
    const char* suffixes[3] = {"B", "M", "K"};
    char buf[64];
    for (int i = 0; i < 3; ++i) {
        if (av >= thresholds[i]) {
            double scaled = static_cast<double>(av) / static_cast<double>(thresholds[i]);
            if (scaled < 10) std::snprintf(buf, sizeof buf, "%.2f", scaled);
            else if (scaled < 100) std::snprintf(buf, sizeof buf, "%.1f", scaled);
            else std::snprintf(buf, sizeof buf, "%.0f", scaled);
            std::string text(buf);
            if (text.find('.') != std::string::npos) {
                while (!text.empty() && text.back() == '0') text.pop_back();
                if (!text.empty() && text.back() == '.') text.pop_back();
            }
            return sign + text + suffixes[i];
        }
    }
    return std::to_string(value);
}

}  // namespace agent
}  // namespace dd

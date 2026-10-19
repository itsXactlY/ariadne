#pragma once
// Pure SQL-clause builders + row helpers from daedalus_state.py / daedalus_state_common.py:
// escape_like, workspace_key, _delegate_from_json, _cwd_prefix_clause, _workspace_key_clause.
// NOT ported (config/filesystem/sqlite-bound): resolved_max_resume_messages,
// resolved_max_export_messages, _system_prompt_hash (sha256 of live prompt), _compression_lock_*
// (os.getpid/psutil), _collect_delegate_child_ids (sqlite conn), production-state-root fs probes.
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace core {

inline std::string escape_like(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '%')
            out += "\\%";
        else if (c == '_')
            out += "\\_";
        else
            out += c;
    }
    return out;
}

static inline std::string trim_ws(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static inline std::string rstrip_slash(const std::string& p) {
    std::size_t b = p.size();
    while (b > 0 && (p[b - 1] == '/' || p[b - 1] == '\\')) --b;
    std::string r = p.substr(0, b);
    return r.empty() ? p : r;
}

inline std::optional<std::string> workspace_key(const Json& row) {
    const Json* g = json_get(row, "git_repo_root");
    std::string root = (g && g->type == Json::Str) ? g->str : "";
    root = trim_ws(root);
    if (!root.empty()) return root;
    const Json* c = json_get(row, "cwd");
    std::string cwd = (c && c->type == Json::Str) ? c->str : "";
    cwd = trim_ws(cwd);
    if (cwd.empty()) return std::nullopt;
    return cwd;
}

inline std::string delegate_from_json(const std::string& col = "model_config") {
    return "json_extract(COALESCE(" + col + ", '{}'), '$._delegate_from')";
}

inline std::pair<std::string, std::vector<std::string>> cwd_prefix_clause(
    const std::string& cwd_prefix) {
    const std::string prefix = rstrip_slash(cwd_prefix);
    const std::string esc = escape_like(prefix);
    const std::string clause =
        R"q((s.cwd = ? OR s.cwd LIKE ? ESCAPE '\' OR s.cwd LIKE ? ESCAPE '\'))q";
    return {clause, std::vector<std::string>{prefix, esc + "/%", esc + "\\%"}};
}

inline std::pair<std::string, std::vector<std::string>> workspace_key_clause(
    const std::string& key) {
    const std::string prefix = rstrip_slash(key);
    const auto [cc, cp] = cwd_prefix_clause(prefix);
    const std::string clause =
        "(s.git_repo_root = ? OR (COALESCE(s.git_repo_root, '') = '' AND " + cc + "))";
    std::vector<std::string> params{prefix};
    for (const auto& p : cp) params.push_back(p);
    return {clause, params};
}

}  // namespace core
}  // namespace dd

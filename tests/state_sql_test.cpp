// Differential test for the daedalus_state SQL-clause cluster (escape_like, workspace_key,
// delegate_from_json, cwd_prefix_clause, workspace_key_clause) vs captured Python truth.
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/core/state_sql.hpp"

using namespace dd;
using namespace dd::core;

static int failures = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << what << "\n";
    }
}

static Json mkobj(std::initializer_list<std::pair<std::string, Json>> kv) {
    Json o = json_obj();
    for (const auto& p : kv) obj_set(o, p.first, p.second);
    return o;
}

static void chk_vec(const std::vector<std::string>& got,
                    const std::vector<std::string>& want, const std::string& tag) {
    chk(got == want, tag);
}

int main() {
    // ---- escape_like ----
    chk(escape_like("") == "", "esc:empty");
    chk(escape_like("abc") == "abc", "esc:plain");
    chk(escape_like("a\\b") == "a\\\\b", "esc:backslash");
    chk(escape_like("50%") == "50\\%", "esc:percent");
    chk(escape_like("a_b") == "a\\_b", "esc:underscore");
    chk(escape_like("a%\\_") == "a\\%\\\\\\_", "esc:mixed");
    chk(escape_like("/home/x") == "/home/x", "esc:path");
    chk(escape_like("/") == "/", "esc:slash");

    // ---- workspace_key ----
    chk(workspace_key(mkobj({{"git_repo_root", json_str("/x ")}})).value_or("") == "/x", "wk:root");
    chk(workspace_key(mkobj({{"git_repo_root", json_str("")}, {"cwd", json_str("/y")}}))
            .value_or("") == "/y", "wk:cwd-fallback");
    chk(workspace_key(mkobj({{"cwd", json_str(" /y ")}})).value_or("") == "/y", "wk:cwd-trim");
    chk(!workspace_key(mkobj({})).has_value(), "wk:empty-none");
    chk(workspace_key(mkobj({{"git_repo_root", json_str("  ")}, {"cwd", json_str("z")}}))
            .value_or("") == "z", "wk:blank-root");
    chk(workspace_key(mkobj({{"git_repo_root", json_str("/r/")}, {"cwd", json_str("/z")}}))
            .value_or("") == "/r/", "wk:root-wins");

    // ---- delegate_from_json ----
    chk(delegate_from_json() ==
            "json_extract(COALESCE(model_config, '{}'), '$._delegate_from')", "dj:default");
    chk(delegate_from_json("col2") ==
            "json_extract(COALESCE(col2, '{}'), '$._delegate_from')", "dj:custom");

    // ---- cwd_prefix_clause ----
    const std::string CWD_CLAUSE =
        "(s.cwd = ? OR s.cwd LIKE ? ESCAPE '\\' OR s.cwd LIKE ? ESCAPE '\\')";
    {
        auto r = cwd_prefix_clause("/home/x");
        chk(r.first == CWD_CLAUSE, "cwd:clause1");
        chk_vec(r.second, {"/home/x", "/home/x/%", "/home/x\\%"}, "cwd:home-x");
    }
    {
        auto r = cwd_prefix_clause("/home/x/");
        chk(r.first == CWD_CLAUSE, "cwd:clause2");
        chk_vec(r.second, {"/home/x", "/home/x/%", "/home/x\\%"}, "cwd:home-x-slash");
    }
    {
        auto r = cwd_prefix_clause("/");
        chk(r.first == CWD_CLAUSE, "cwd:clause3");
        chk_vec(r.second, {"/", "//%", "/\\%"}, "cwd:bare-slash");
    }
    {
        auto r = cwd_prefix_clause("/a_b");
        chk(r.first == CWD_CLAUSE, "cwd:clause4");
        chk_vec(r.second, {"/a_b", "/a\\_b/%", "/a\\_b\\%"}, "cwd:underscore");
    }
    {
        auto r = cwd_prefix_clause("a_b%");
        chk(r.first == CWD_CLAUSE, "cwd:clause5");
        chk_vec(r.second, {"a_b%", "a\\_b\\%/%", "a\\_b\\%\\%"}, "cwd:pct-underscore");
    }

    // ---- workspace_key_clause ----
    const std::string WK_CLAUSE =
        "(s.git_repo_root = ? OR (COALESCE(s.git_repo_root, '') = '' AND " + CWD_CLAUSE + "))";
    {
        auto r = workspace_key_clause("/repo");
        chk(r.first == WK_CLAUSE, "wkq:clause1");
        chk_vec(r.second, {"/repo", "/repo", "/repo/%", "/repo\\%"}, "wkq:repo");
    }
    {
        auto r = workspace_key_clause("/repo/");
        chk(r.first == WK_CLAUSE, "wkq:clause2");
        chk_vec(r.second, {"/repo", "/repo", "/repo/%", "/repo\\%"}, "wkq:repo-slash");
    }
    {
        auto r = workspace_key_clause("/");
        chk(r.first == WK_CLAUSE, "wkq:clause3");
        chk_vec(r.second, {"/", "/", "//%", "/\\%"}, "wkq:bare-slash");
    }
    {
        auto r = workspace_key_clause("a_b");
        chk(r.first == WK_CLAUSE, "wkq:clause4");
        chk_vec(r.second, {"a_b", "a_b", "a\\_b/%", "a\\_b\\%"}, "wkq:underscore");
    }

    if (failures == 0) {
        std::cout << "all state-clause tests passed\n";
        return 0;
    }
    std::cout << failures << " state-clause tests FAILED\n";
    return 1;
}

// Ground-truth test for the osv_check.py parsing subsystem.
#include "dd/tools/osv_package_parse.hpp"
#include <string>
#include <optional>
#include <vector>
#include <cstdio>
#include <iostream>

using namespace dd::tools;

static int g_fail = 0;

static std::string optstr(const std::optional<std::string>& o) {
    return o ? *o : std::string("<null>");
}

static void chk(bool ok, const std::string& label) {
    if (!ok) { std::cerr << "FAIL: " << label << "\n"; ++g_fail; }
}

int main() {
    // infer_ecosystem
    chk(infer_ecosystem("npx") == "npm", "eco npx");
    chk(infer_ecosystem("npx.cmd") == "npm", "eco npx.cmd");
    chk(infer_ecosystem("Npx") == "npm", "eco Npx");
    chk(infer_ecosystem("/usr/bin/npx") == "npm", "eco /usr/bin/npx");
    chk(infer_ecosystem("uvx") == "PyPI", "eco uvx");
    chk(infer_ecosystem("uvx.cmd") == "PyPI", "eco uvx.cmd");
    chk(infer_ecosystem("pipx") == "PyPI", "eco pipx");
    chk(infer_ecosystem("PIPX") == "PyPI", "eco PIPX");
    chk(!infer_ecosystem("node"), "eco node");
    chk(!infer_ecosystem("npxy"), "eco npxy");
    chk(!infer_ecosystem(""), "eco empty");

    // parse_npm_package
    {
        auto r = parse_npm_package("@scope/name");
        chk(optstr(r.first) == "@scope/name" && !r.second, "npm @scope/name");
    }
    {
        auto r = parse_npm_package("@scope/name@1.2.3");
        chk(optstr(r.first) == "@scope/name" && optstr(r.second) == "1.2.3", "npm scoped ver");
    }
    {
        auto r = parse_npm_package("@scope/name@latest");
        chk(optstr(r.first) == "@scope/name" && optstr(r.second) == "latest", "npm scoped latest stays");
    }
    {
        auto r = parse_npm_package("foo@1.0");
        chk(optstr(r.first) == "foo" && optstr(r.second) == "1.0", "npm foo@1.0");
    }
    {
        auto r = parse_npm_package("foo");
        chk(optstr(r.first) == "foo" && !r.second, "npm foo");
    }
    {
        auto r = parse_npm_package("foo@");
        chk(optstr(r.first) == "foo" && optstr(r.second) == "", "npm foo@ empty ver");
    }
    {
        auto r = parse_npm_package("@bad");
        chk(optstr(r.first) == "@bad" && !r.second, "npm @bad");
    }
    {
        auto r = parse_npm_package("@a/b/c");
        chk(optstr(r.first) == "@a/b/c" && !r.second, "npm @a/b/c");
    }
    {
        auto r = parse_npm_package("@a/@b");
        chk(optstr(r.first) == "@a/@b" && !r.second, "npm @a/@b");
    }
    {
        auto r = parse_npm_package("pkg@latest");
        chk(optstr(r.first) == "pkg" && !r.second, "npm pkg@latest -> null ver");
    }

    // parse_pypi_package
    {
        auto r = parse_pypi_package("foo");
        chk(optstr(r.first) == "foo" && !r.second, "pypi foo");
    }
    {
        auto r = parse_pypi_package("foo==1.0");
        chk(optstr(r.first) == "foo" && optstr(r.second) == "1.0", "pypi foo==1.0");
    }
    {
        auto r = parse_pypi_package("foo[extra]==2.0");
        chk(optstr(r.first) == "foo" && optstr(r.second) == "2.0", "pypi extras ver");
    }
    {
        auto r = parse_pypi_package("foo[extra]");
        chk(optstr(r.first) == "foo" && !r.second, "pypi extras only");
    }
    {
        auto r = parse_pypi_package("foo==");
        chk(optstr(r.first) == "foo==" && !r.second, "pypi foo== no match");
    }
    {
        auto r = parse_pypi_package("foo bar");
        chk(optstr(r.first) == "foo bar" && !r.second, "pypi foo bar no match");
    }
    {
        auto r = parse_pypi_package("-pkg");
        chk(optstr(r.first) == "-pkg" && !r.second, "pypi -pkg");
    }
    {
        auto r = parse_pypi_package("pkg.name-x");
        chk(optstr(r.first) == "pkg.name-x" && !r.second, "pypi dotted-dash");
    }
    {
        auto r = parse_pypi_package("pkg==a==b");
        chk(optstr(r.first) == "pkg" && optstr(r.second) == "a==b", "pypi double eq");
    }

    // parse_package_from_args
    {
        auto r = parse_package_from_args({"-y", "pkg@1.0"}, "npm");
        chk(optstr(r.first) == "pkg" && optstr(r.second) == "1.0", "args npm -y pkg@1.0");
    }
    {
        auto r = parse_package_from_args({}, "npm");
        chk(!r.first && !r.second, "args empty");
    }
    {
        auto r = parse_package_from_args({"-y"}, "npm");
        chk(!r.first && !r.second, "args only flag");
    }
    {
        auto r = parse_package_from_args({"--flag", "foo==1.0"}, "PyPI");
        chk(optstr(r.first) == "foo" && optstr(r.second) == "1.0", "args pypi flag foo==1.0");
    }
    {
        auto r = parse_package_from_args({"pkg"}, "npm");
        chk(optstr(r.first) == "pkg" && !r.second, "args npm pkg");
    }
    {
        auto r = parse_package_from_args({"", "x"}, "npm");
        chk(!r.first && !r.second, "args empty-token");
    }

    // parse_package (check_package_for_malware prefix)
    {
        auto r = parse_package("npx", {"-y", "@scope/name@1.2"});
        bool ok = r && std::get<0>(*r) == "npm" && optstr(std::get<1>(*r)) == "@scope/name"
                  && optstr(std::get<2>(*r)) == "1.2";
        chk(ok, "pkg npx scoped");
    }
    {
        auto r = parse_package("uvx", {"foo==1.0"});
        bool ok = r && std::get<0>(*r) == "PyPI" && optstr(std::get<1>(*r)) == "foo"
                  && optstr(std::get<2>(*r)) == "1.0";
        chk(ok, "pkg uvx foo==1.0");
    }
    chk(!parse_package("node", {"x"}), "pkg node -> null");
    chk(!parse_package("npx", {"-y"}), "pkg npx only-flag -> null");
    {
        auto r = parse_package("pipx", {"pkg[ex]==2"});
        bool ok = r && std::get<0>(*r) == "PyPI" && optstr(std::get<1>(*r)) == "pkg"
                  && optstr(std::get<2>(*r)) == "2";
        chk(ok, "pkg pipx extras");
    }

    if (g_fail == 0) { std::cout << "all osv-parse tests passed\n"; return 0; }
    std::cerr << g_fail << " failures\n";
    return 1;
}

// Ground-truth checks for tools/website_policy.py pure host helpers.
#include <iostream>
#include <string>
#include <optional>
#include "dd/core/json.hpp"
#include "dd/tools/website_policy.hpp"

using namespace dd;
using namespace dd::tools;
using namespace dd::core;

static int g_fail = 0;
#define CHECK(cond, label) do { if (!(cond)) { ++g_fail; std::cout << "FAIL " << (label) << "\n"; } } while (0)

int main() {
    // ---- normalize_host ----
    CHECK(normalize_host("  Example.COM.. ") == "example.com", "nh.strip");
    CHECK(normalize_host("") == "", "nh.empty");
    CHECK(normalize_host("a.b.") == "a.b", "nh.rdot");
    CHECK(normalize_host("  ") == "", "nh.ws");
    CHECK(normalize_host("www.") == "www", "nh.wwwdot");
    CHECK(normalize_host("a..b") == "a..b", "nh.innerdot");

    // ---- normalize_rule (type + normalization) ----
    {
        auto r = normalize_rule(json_num(5));   CHECK(!r, "nr.int");
        r = normalize_rule(json_null());        CHECK(!r, "nr.none");
        r = normalize_rule(json_bool(true));    CHECK(!r, "nr.bool");
        r = normalize_rule(json_arr());         CHECK(!r, "nr.list");
        r = normalize_rule(json_str(""));        CHECK(!r, "nr.empty");
        r = normalize_rule(json_str("  Example.COM.. ")); CHECK(r && *r == "example.com", "nr.rdot");
        r = normalize_rule(json_str("#comment")); CHECK(!r, "nr.comment");
        r = normalize_rule(json_str("http://foo.bar/baz?x")); CHECK(r && *r == "foo.bar", "nr.url");
        r = normalize_rule(json_str("www.Foo.com/")); CHECK(r && *r == "foo.com", "nr.wwwslash");
        r = normalize_rule(json_str("  foo  ")); CHECK(r && *r == "foo", "nr.padspace");
        r = normalize_rule(json_str("*.a.b"));   CHECK(r && *r == "*.a.b", "nr.globstar");
        r = normalize_rule(json_str("//"));      CHECK(!r, "nr.doubleslash");
        r = normalize_rule(json_str("https://www.Example.com/a/b")); CHECK(r && *r == "example.com", "nr.wwwup");
    }

    // ---- match_host_against_rule ----
    CHECK(match_host_against_rule("foo.com", "foo.com") == true, "mm.eq");
    CHECK(match_host_against_rule("a.foo.com", "foo.com") == true, "mm.sub");
    CHECK(match_host_against_rule("xfoo.com", "foo.com") == false, "mm.notsub");
    CHECK(match_host_against_rule("a.b", "*.b") == true, "mm.globstar");
    CHECK(match_host_against_rule("ab", "a?b") == false, "mm.globq");
    CHECK(match_host_against_rule("a.b.c", "*.c") == true, "mm.globstar2");
    CHECK(match_host_against_rule("", "x") == false, "mm.emptyhost");
    CHECK(match_host_against_rule("x", "") == false, "mm.emptypat");

    // ---- extract_host_from_urlish ----
    CHECK(extract_host_from_urlish("http://Foo.Bar:8080/x/y?z=1") == "foo.bar", "ex.port");
    CHECK(extract_host_from_urlish("//example.com/p") == "example.com", "ex.schemefull");
    CHECK(extract_host_from_urlish("foo.com/p") == "foo.com", "ex.barepath");
    CHECK(extract_host_from_urlish("https://a.b/c") == "a.b", "ex.https");
    CHECK(extract_host_from_urlish("ftp://host/x") == "host", "ex.ftp");
    CHECK(extract_host_from_urlish("a.b") == "a.b", "ex.bare");
    CHECK(extract_host_from_urlish("www.example.com") == "www.example.com", "ex.wwwkeep");
    CHECK(extract_host_from_urlish("") == "", "ex.empty");

    std::cout << "website_policy tests: " << (g_fail == 0 ? "PASS" : "FAIL")
              << " (" << g_fail << " failed)\n";
    return g_fail == 0 ? 0 : 1;
}

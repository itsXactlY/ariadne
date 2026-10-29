// Ground-truth test for tools/url_safety.py port.
#include "dd/tools/url_safety.hpp"
#include <string>
#include <utility>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, label) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s\n", label); ++failures; } } while (0)

int main() {
    using namespace dd::tools;

    // is_blocked_ip ground truth (captured from real Python _is_blocked_ip).
    static const std::pair<const char*, bool> tbl[] = {
        {"0.0.0.0", true}, {"127.0.0.1", true}, {"127.0.0.2", true},
        {"10.0.0.1", true}, {"10.255.255.255", true}, {"172.168.0.1", false},
        {"172.169.0.1", false}, {"169.254.0.1", true}, {"169.254.169.254", true},
        {"1.0.0.0", false}, {"1.1.0.0", false}, {"255.255.255.255", true},
        {"255.255.255.254", true}, {"100.64.0.0", true}, {"100.64.0.1", true},
        {"100.127.255.255", true}, {"100.128.0.0", false}, {"8.8.8.8", false},
        {"192.168.1.1", true}, {"0.0.0.1", true}, {"128.0.0.0", false},
        {"224.0.0.0", true}, {"224.0.0.1", true}, {"240.0.0.0", true},
        {"240.0.0.1", true}, {"192.0.0.9", false}, {"192.0.0.10", false},
        {"192.0.0.11", true}, {"198.18.0.0", true}, {"198.19.0.0", true},
        {"172.16.0.0", true}, {"172.17.0.0", true}, {"172.16.15.255", true},
        {"192.0.2.0", true}, {"192.0.2.255", true}, {"198.51.100.0", true},
        {"203.0.113.145", true}, {"203.0.113.146", true},
        {"::", true}, {"::1", true}, {"::ffff:0:0", true},
        {"::ffff:ffff:ffff", true}, {"::ffff:1:1", true}, {"1::", true},
        {"100::", true}, {"200::", true}, {"400::", true}, {"fe80::", true},
        {"fec0::", false}, {"ff00::", true}, {"ff01::", true},
        {"feff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", false},
        {"0:0:0:0:0:0:0:1", true}, {"64:ff9b:1::", true}, {"2001:db8::", true},
        {"2002::", true}, {"3fff::", true}, {"fc00::", true}, {"fc07::", true},
        {"::ffff:1.2.3.4", false}, {"0:0:0:0:0:ffff:0:0", true},
        {"0:0:0:0:0:ffff:ffff:ffff", true}, {"2001:1::1", false},
        {"2001:1::2", false}, {"2001:3::", false}, {"2001:4:112::", false},
        {"2001:20::", false}, {"2001:30::", false}, {"2001:2::", true},
        {"2001:3:1::", false},
    };
    for (auto& e : tbl) {
        bool got = false;
        try { got = is_blocked_ip(e.first); }
        catch (...) { std::fprintf(stderr, "THROW %s\n", e.first); ++failures; continue; }
        CHECK(got == e.second, e.first);
    }

    // is_safe_url deterministic paths (no network DNS needed).
    CHECK(is_safe_url("http://metadata.google.internal/x") == false, "blocked-hostname");
    CHECK(is_safe_url("http://metadata.goog/x") == false, "blocked-hostname-goog");
    CHECK(is_safe_url("") == false, "empty-host");
    CHECK(is_safe_url("http://169.254.169.254/") == false, "numeric-blocked");

    // A host the operator configured passes; its neighbours on the same private net do not.
    CHECK(is_safe_url("http://192.168.0.2:8080/v1/chat/completions") == false, "lan model refused before the config names it");
    admit_configured_url("http://192.168.0.2:8080/v1");
    CHECK(is_safe_url("http://192.168.0.2:8080/v1/chat/completions") == true, "configured lan model admitted");
    CHECK(is_safe_url("http://192.168.0.3:8080/") == false, "a neighbour on the same net stays refused");
    admit_configured_url("http://metadata.google.internal/x");
    CHECK(is_safe_url("http://metadata.google.internal/x") == false, "metadata is never admitted, configured or not");

    // NAT64 carries an IPv4: as blocked as the address inside; 6to4 and Teredo are blocked wholesale
    CHECK(is_blocked_ip("64:ff9b::a9fe:a9fe"), "nat64 metadata address");
    CHECK(is_blocked_ip("64:ff9b::7f00:1"), "nat64 loopback");
    CHECK(is_blocked_ip("64:ff9b::10.0.0.1"), "nat64 private, dotted tail");
    CHECK(!is_blocked_ip("64:ff9b::808:808"), "nat64 public stays reachable");
    CHECK(is_blocked_ip("2002:a9fe:a9fe::1"), "6to4 around the metadata address");
    CHECK(is_blocked_ip("2001:0:4136:e378:8000:63bf:3fff:fdd2"), "teredo");

    // url_hostname: the host a client connects to, never the userinfo in front of it
    struct H { const char* url; const char* host; };
    static const H hosts[] = {
        {"http://example.invalid/x", "example.invalid"},
        {"http://example.invalid./x", "example.invalid"},
        {"HTTP://Example.INVALID:8080/x?q=1#f", "example.invalid"},
        {"http://pub.example:80@169.254.169.254/", "169.254.169.254"},
        {"http://user:pa:ss@host.example:81/p", "host.example"},
        {"http://a@b@c.example/", "c.example"},
        {"http://example.invalid#@169.254.169.254/", "example.invalid"},
        {"http://example.invalid?@169.254.169.254/", "example.invalid"},
        {"http://[::1]:8080/", "::1"},
        {"http://user@[::ffff:10.0.0.1]/", "::ffff:10.0.0.1"},
        {"http://[::ffff:127.0.0.1]", "::ffff:127.0.0.1"},
        {"//host.example/p", "host.example"},
        {"host.example/p", "host.example"},
        {"http://[::1/", ""},
        {"http://ho st.example/", ""},
        {"http://", ""},
        {"", ""},
    };
    for (const auto& h : hosts) {
        const std::string got = url_hostname(h.url);
        if (got != h.host) std::fprintf(stderr, "FAIL url_hostname(%s) = '%s', want '%s'\n", h.url, got.c_str(), h.host);
        if (got != h.host) ++failures;
    }
    CHECK(url_hostname("http://example.invalid\\@169.254.169.254/") == "example.invalid", "a backslash ends the authority");

    // userinfo tricks and literals: decided without DNS
    CHECK(is_safe_url("http://pub.example:80@169.254.169.254/") == false, "userinfo in front of the metadata address");
    CHECK(is_safe_url("http://169.254.169.254:80@pub.example/") == false, "metadata address as userinfo only: nothing resolves");
    CHECK(is_safe_url("http://[::1]/") == false, "ipv6 loopback literal");
    CHECK(is_safe_url("http://user@[::ffff:10.0.0.1]/") == false, "mapped private address behind userinfo");
    CHECK(is_safe_url("http://[::ffff:127.0.0.1]:9/") == false, "mapped loopback");
    CHECK(is_safe_url("http://[64:ff9b::a9fe:a9fe]/") == false, "nat64 metadata literal");
    CHECK(is_safe_url("http://metadata.google.internal./x") == false, "blocked hostname with a trailing dot");
    CHECK(is_safe_url("http://METADATA.GOOG/x") == false, "blocked hostname in capitals");
    CHECK(is_safe_url("http://2130706433/") == false, "decimal loopback");
    CHECK(is_safe_url("http://0x7f.1/") == false, "hex loopback");

    if (failures == 0) std::fprintf(stderr, "all url-safety tests passed\n");
    return failures == 0 ? 0 : 1;
}

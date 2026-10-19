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

    if (failures == 0) std::fprintf(stderr, "all url-safety tests passed\n");
    return failures == 0 ? 0 : 1;
}

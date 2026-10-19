// Port of tools/url_safety.py — SSRF gate.
#include "dd/tools/url_safety.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <netdb.h>
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace dd {
namespace tools {

// ---- IPv4 range membership (32-bit) -------------------------------------
static bool in_net4(std::uint32_t ip, std::uint32_t base, int bits) {
    if (bits <= 0) return true;
    std::uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : (~0u << (32 - bits));
    return (ip & mask) == (base & mask);
}

// ---- IPv6 range membership (128-bit as hi:lo) ---------------------------
static bool in_net6(std::uint64_t ihi, std::uint64_t ilo,
                    std::uint64_t bhi, std::uint64_t blo, int bits) {
    if (bits <= 0) return true;
    if (bits <= 64) {
        std::uint64_t mask = (~0ULL << (64 - bits));
        return (ihi & mask) == (bhi & mask);
    }
    std::uint64_t mask = (~0ULL << (128 - bits));
    return (ihi == bhi) && ((ilo & mask) == (blo & mask));
}

// ---- IPv4 registries ----------------------------------------------------
static bool v4_is_private(std::uint32_t ip) {
    static const std::pair<std::uint32_t,int> priv[] = {
        {0x00000000u,8},{0x0A000000u,8},{0x7F000000u,8},{0xA9FE0000u,16},
        {0xAC100000u,12},{0xC0000000u,24},{0xC00000AAu,31},{0xC0000200u,24},
        {0xC0A80000u,16},{0xC6120000u,15},{0xC6336400u,24},{0xCB007100u,24},
        {0xF0000000u,4},{0xFFFFFFFFu,32}};
    static const std::pair<std::uint32_t,int> exc[] = {
        {0xC0000009u,32},{0xC000000Au,32}};
    bool any = false;
    for (auto& n : priv) if (in_net4(ip, n.first, n.second)) { any = true; break; }
    if (!any) return false;
    for (auto& n : exc) if (in_net4(ip, n.first, n.second)) return false;
    return true;
}
static bool v4_is_loopback(std::uint32_t ip)   { return in_net4(ip, 0x7F000000u, 8); }
static bool v4_is_link_local(std::uint32_t ip){ return in_net4(ip, 0xA9FE0000u, 16); }
static bool v4_is_reserved(std::uint32_t ip)   { return in_net4(ip, 0xF0000000u, 4); }
static bool v4_is_multicast(std::uint32_t ip)  { return in_net4(ip, 0xE0000000u, 4); }
static bool v4_is_unspecified(std::uint32_t ip){ return ip == 0u; }
static bool v4_cgnat(std::uint32_t ip)         { return in_net4(ip, 0x64400000u, 10); }

static bool v4_blocked(std::uint32_t ip) {
    return v4_is_private(ip) || v4_is_loopback(ip) || v4_is_link_local(ip)
        || v4_is_reserved(ip) || v4_is_multicast(ip) || v4_is_unspecified(ip)
        || v4_cgnat(ip);
}

// ---- IPv6 registries ----------------------------------------------------
static bool v6_is_private(std::uint64_t hi, std::uint64_t lo) {
    struct N { std::uint64_t hi, lo; int bits; };
    static const N priv[] = {
        {0,1,128},{0,0,128},{0,0xFFFF00000000ULL,96},
        {0x0064FF9B00010000ULL,0,48},{0x0100000000000000ULL,0,64},
        {0x2001000000000000ULL,0,23},{0x20010DB800000000ULL,0,32},
        {0x2002000000000000ULL,0,16},{0x3FFF000000000000ULL,0,20},
        {0xFC00000000000000ULL,0,7},{0xFE80000000000000ULL,0,10}};
    static const N exc[] = {
        {0x2001000100000000ULL,1,128},{0x2001000100000000ULL,2,128},
        {0x2001000300000000ULL,0,32},{0x2001000401120000ULL,0,48},
        {0x2001002000000000ULL,0,28},{0x2001003000000000ULL,0,28}};
    bool any = false;
    for (auto& n : priv) if (in_net6(hi, lo, n.hi, n.lo, n.bits)) { any = true; break; }
    if (!any) return false;
    for (auto& n : exc) if (in_net6(hi, lo, n.hi, n.lo, n.bits)) return false;
    return true;
}
static bool v6_is_loopback(std::uint64_t hi, std::uint64_t lo) {
    return hi == 0 && lo == 1;
}
static bool v6_is_link_local(std::uint64_t hi, std::uint64_t lo) {
    return in_net6(hi, lo, 0xFE80000000000000ULL, 0, 10);
}
static bool v6_is_reserved(std::uint64_t hi, std::uint64_t lo) {
    struct N { std::uint64_t hi, lo; int bits; };
    static const N res[] = {
        {0,0,8},{0x0100000000000000ULL,0,8},{0x0200000000000000ULL,0,7},
        {0x0400000000000000ULL,0,6},{0x0800000000000000ULL,0,5},
        {0x1000000000000000ULL,0,4},{0x4000000000000000ULL,0,3},
        {0x6000000000000000ULL,0,3},{0x8000000000000000ULL,0,3},
        {0xA000000000000000ULL,0,3},{0xC000000000000000ULL,0,3},
        {0xE000000000000000ULL,0,4},{0xF000000000000000ULL,0,5},
        {0xF800000000000000ULL,0,6},{0xFE00000000000000ULL,0,9}};
    for (auto& n : res) if (in_net6(hi, lo, n.hi, n.lo, n.bits)) return true;
    return false;
}
static bool v6_is_multicast(std::uint64_t hi, std::uint64_t lo) {
    return in_net6(hi, lo, 0xFF00000000000000ULL, 0, 8);
}
static bool v6_is_unspecified(std::uint64_t hi, std::uint64_t lo) {
    return hi == 0 && lo == 0;
}

static bool v6_blocked(std::uint64_t hi, std::uint64_t lo) {
    // ipv4_mapped: non-None iff top 96 bits == 0xFFFF (hi==0 && (lo>>32)==0xFFFF).
    if (hi == 0 && (lo >> 32) == 0xFFFFu) {
        std::uint32_t mapped = static_cast<std::uint32_t>(lo & 0xFFFFFFFFULL);
        return v4_blocked(mapped);
    }
    return v6_is_private(hi, lo) || v6_is_loopback(hi, lo) || v6_is_link_local(hi, lo)
        || v6_is_reserved(hi, lo) || v6_is_multicast(hi, lo) || v6_is_unspecified(hi, lo);
}

// ---- Parsers ------------------------------------------------------------
static bool parse_ipv4(const std::string& s, std::uint32_t& out) {
    if (s.find(':') != std::string::npos) return false;
    std::uint32_t parts[4];
    int idx = 0;
    std::size_t pos = 0;
    while (idx < 4) {
        std::size_t dot = s.find('.', pos);
        std::string tok = (dot == std::string::npos) ? s.substr(pos) : s.substr(pos, dot - pos);
        if (tok.empty() || tok.size() > 3) return false;
        int val = 0;
        for (char c : tok) {
            if (c < '0' || c > '9') return false;
            val = val * 10 + (c - '0');
        }
        if (val > 255) return false;
        parts[idx++] = static_cast<std::uint32_t>(val);
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    if (idx != 4) return false;
    out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

// Parse IPv6 into 8 groups with '::' compression.
static bool parse_ipv6(const std::string& s_in, std::uint64_t& hi, std::uint64_t& lo) {
    std::string s = s_in;
    // Embedded IPv4 tail (e.g. "::ffff:1.2.3.4"): last group is a dotted quad
    // -> two 16-bit hex groups, then parse the rest as ordinary IPv6.
    if (s.find('.') != std::string::npos) {
        std::size_t c = s.rfind(':');
        std::string tail = (c == std::string::npos) ? s : s.substr(c + 1);
        std::string head = (c == std::string::npos) ? std::string() : s.substr(0, c + 1);
        std::uint32_t ip4;
        if (!parse_ipv4(tail, ip4)) return false;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%X:%X", (ip4 >> 16) & 0xFFFFu, ip4 & 0xFFFFu);
        s = head + buf;
    }
    auto hex_group = [](const std::string& tok, std::uint16_t& g) -> bool {
        if (tok.empty() || tok.size() > 4) return false;
        int val = 0;
        for (char c : tok) {
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            val = val * 16 + d;
        }
        g = static_cast<std::uint16_t>(val);
        return true;
    };
    auto split_groups = [&](const std::string& part, std::vector<std::uint16_t>& dst) -> bool {
        if (part.empty()) return true;
        std::size_t q = 0;
        while (true) {
            std::size_t c = part.find(':', q);
            std::string tok = (c == std::string::npos) ? part.substr(q) : part.substr(q, c - q);
            std::uint16_t g;
            if (!hex_group(tok, g)) return false;
            dst.push_back(g);
            if (c == std::string::npos) break;
            q = c + 1;
        }
        return true;
    };
    std::vector<std::uint16_t> full;
    std::size_t p = s.find("::");
    if (p != std::string::npos) {
        if (s.find("::", p + 1) != std::string::npos) return false;  // only one '::'
        std::string hs = s.substr(0, p);
        std::string ts = (p + 2 <= s.size()) ? s.substr(p + 2) : std::string();
        std::vector<std::uint16_t> head, tail;
        if (!split_groups(hs, head) || !split_groups(ts, tail)) return false;
        if (head.size() + tail.size() > 7) return false;
        full = head;
        for (std::size_t k = head.size() + tail.size(); k < 8; ++k) full.push_back(0);
        for (auto g : tail) full.push_back(g);
    } else {
        if (!split_groups(s, full)) return false;
        if (full.size() != 8) return false;
    }
    if (full.size() != 8) return false;
    hi = 0; lo = 0;
    for (int k = 0; k < 4; ++k) hi = (hi << 16) | full[k];
    for (int k = 4; k < 8; ++k) lo = (lo << 16) | full[k];
    return true;
}

// ---- Public API ---------------------------------------------------------
bool is_blocked_ip(const std::string& ip_str) {
    std::uint32_t ip4;
    if (parse_ipv4(ip_str, ip4)) return v4_blocked(ip4);
    std::uint64_t hi, lo;
    if (parse_ipv6(ip_str, hi, lo)) return v6_blocked(hi, lo);
    throw std::invalid_argument("invalid ip: " + ip_str);
}

// URL hostname parser: scheme://[user@]host[:port] -> host (lowercased).
static std::string url_hostname(const std::string& url) {
    std::string u = url;
    std::size_t start = 0;
    std::size_t colon = u.find("://");
    if (colon != std::string::npos) {
        start = colon + 3;
    } else {
        start = 0;
    }
    while (start < u.size() && u[start] == '/') ++start;
    std::size_t end = start;
    while (end < u.size() && u[end] != '/' && u[end] != ':' && u[end] != '?') ++end;
    std::string host = u.substr(start, end - start);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return host;
}

bool is_safe_url(const std::string& url) {
    std::string host = url_hostname(url);
    if (host.empty()) return false;
    if (host == "metadata.google.internal" || host == "metadata.goog") return false;
    addrinfo hint;
    hint.ai_flags = 0;
    hint.ai_family = AF_UNSPEC;
    hint.ai_socktype = 0;
    hint.ai_protocol = 0;
    hint.ai_addrlen = 0;
    hint.ai_canonname = nullptr;
    hint.ai_addr = nullptr;
    hint.ai_next = nullptr;
    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), nullptr, &hint, &res);
    if (rc != 0) return false;
    bool safe = true;
    for (addrinfo* p = res; p; p = p->ai_next) {
        sockaddr* sa = p->ai_addr;
        if (sa->sa_family == AF_INET) {
            sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(sa);
            std::uint32_t ip = ntohl(sin->sin_addr.s_addr);
            if (v4_blocked(ip)) { safe = false; break; }
        } else if (sa->sa_family == AF_INET6) {
            sockaddr_in6* s6 = reinterpret_cast<sockaddr_in6*>(sa);
            std::uint64_t hi = 0, lo = 0;
            for (int k = 0; k < 4; ++k)
                hi = (hi << 16) | (static_cast<unsigned char>(s6->sin6_addr.s6_addr[2*k]) << 8)
                     | s6->sin6_addr.s6_addr[2*k+1];
            for (int k = 0; k < 4; ++k)
                lo = (lo << 16) | (static_cast<unsigned char>(s6->sin6_addr.s6_addr[8+2*k]) << 8)
                     | s6->sin6_addr.s6_addr[8+2*k+1];
            if (v6_blocked(hi, lo)) { safe = false; break; }
        }
    }
    if (res) ::freeaddrinfo(res);
    return safe;
}

}  // namespace tools
}  // namespace dd

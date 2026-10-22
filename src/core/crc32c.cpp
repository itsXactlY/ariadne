#include "dd/core/crc32c.hpp"

#include <atomic>
#include <cstring>
#include <string_view>

#if defined(__x86_64__)
#include <nmmintrin.h>
#define DD_CRC_HW 1
#endif

namespace dd {
namespace core {
namespace {

struct Table {
    uint32_t t[256];
    constexpr Table() : t() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
            t[i] = c;
        }
    }
};
constexpr Table kTable;

uint32_t run_table(const uint8_t* p, size_t n, uint32_t c) {
    while (n--) c = kTable.t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c;
}

#ifdef DD_CRC_HW
__attribute__((target("sse4.2"))) uint32_t run_hw(const uint8_t* p, size_t n, uint32_t c) {
    uint64_t c64 = c;
    while (n >= 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        c64 = _mm_crc32_u64(c64, v);
        p += 8, n -= 8;
    }
    uint32_t c32 = static_cast<uint32_t>(c64);
    while (n--) c32 = _mm_crc32_u8(c32, *p++);
    return c32;
}
bool has_hw() {
    __builtin_cpu_init();
    return __builtin_cpu_supports("sse4.2");
}
#else
uint32_t run_hw(const uint8_t* p, size_t n, uint32_t c) { return run_table(p, n, c); }
bool has_hw() { return false; }
#endif

using Fn = uint32_t (*)(const uint8_t*, size_t, uint32_t);

// First use, not static init.
std::atomic<Fn>& slot() {
    static std::atomic<Fn> g{has_hw() ? run_hw : run_table};
    return g;
}

}  // namespace

uint32_t crc32c(const void* data, size_t n, uint32_t crc) {
    return ~slot().load(std::memory_order_relaxed)(static_cast<const uint8_t*>(data), n, ~crc);
}

uint32_t crc32c_table(const void* data, size_t n, uint32_t crc) { return ~run_table(static_cast<const uint8_t*>(data), n, ~crc); }

const char* crc32c_level() { return slot().load(std::memory_order_relaxed) == run_table ? "table" : "sse4.2"; }

bool crc32c_force(const char* level) {
    const std::string_view l = level ? level : "";
    if (l == "table") return slot().store(run_table, std::memory_order_relaxed), true;
    if (l == "sse4.2" && has_hw()) return slot().store(run_hw, std::memory_order_relaxed), true;
    return false;
}

}  // namespace core
}  // namespace dd

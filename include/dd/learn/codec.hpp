#pragma once
// dd/learn/codec.hpp — Records and Stats as bytes.
//
// Versioned and bounded: a string is cut at kMaxStr (on a character boundary) and a list at kMaxList
// when written, and a longer one on the wire is Corrupt when read, so a damaged length can never ask
// for a huge allocation. A payload from a newer codec is Newer: skipped and counted, never guessed at.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dd/learn/record.hpp"
#include "dd/learn/stats.hpp"

namespace dd {
namespace learn {

constexpr uint8_t kCodecVersion = 1;
constexpr uint32_t kStatsVersion = 1;
constexpr size_t kMaxStr = 1024, kMaxList = 32, kMaxKey = 4096, kMaxPayload = 1u << 20;

enum class Decode { Ok, Newer, Corrupt };

void encode(const Record& r, std::string& out);                        // appends the payload (the frame carries the version)
Decode decode(uint8_t version, std::string_view payload, Record& out);

// What the codec would keep of `r`: encode then decode. The ledger folds this form, so replay equals live.
Record canonical(const Record& r);

// Deterministic: equal Stats give equal bytes (cards are written in key order).
std::string encode_stats(const Stats& s);
Decode decode_stats(uint32_t version, std::string_view body, Stats& out);

}  // namespace learn
}  // namespace dd

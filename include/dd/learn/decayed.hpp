#pragma once
// dd/learn/decayed.hpp — a count that forgets: every read and add first decays it to `now`.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dd {
namespace learn {

constexpr double kDay = 86400.0;

struct Decayed {
    double v = 0;
    int64_t t = 0;   // unix seconds of the last update

    // A step is clamped to [0, 30 d]: a suspend or a clock jump must not zero or inflate the evidence.
    double at(int64_t now, double half_life_s) const {
        const double dt = std::clamp(static_cast<double>(now - t), 0.0, 30.0 * kDay);
        return v * std::exp2(-dt / half_life_s);
    }
    void add(double w, int64_t now, double half_life_s) {
        v = at(now, half_life_s) + w;
        t = std::max(t, now);
    }
};

}  // namespace learn
}  // namespace dd

#include "dd/learn/episode.hpp"

#include <algorithm>

#include "dd/core/sha256.hpp"
#include "dd/know/text.hpp"

namespace dd {
namespace learn {

std::vector<std::pair<uint32_t, uint32_t>> Episode::ticks() const {
    std::vector<std::pair<uint32_t, uint32_t>> t;
    for (const auto& s : steps) {
        const std::pair<uint32_t, uint32_t> k{s.turn, s.round};
        if (t.empty() || t.back() != k) t.push_back(k);
    }
    return t;
}

std::string Episode::digest() const {
    std::string acc;
    for (const auto& s : steps) {
        acc += s.call.digest;
        acc += s.call.failed ? '!' : '.';
        acc += s.call.error;
        acc += '\n';
    }
    return core::sha256_hex(acc);
}

void EpisodeRecorder::feed(const know::Event& e) {
    using T = know::Event::Type;
    if (e.type == T::TurnStart) {
        last_turn_ = std::max(last_turn_, e.turn);
        trim();
    } else if (e.type == T::Calls) {
        Step s;
        s.call = e.call;
        s.call.args = know::utf8_head(s.call.args, opt_.max_args);
        s.call.error = know::utf8_head(s.call.error, opt_.max_error);
        s.turn = e.turn;
        s.round = e.round;
        last_turn_ = std::max(last_turn_, e.turn);
        steps_.push_back(std::move(s));
        trim();
    }
}

void EpisodeRecorder::trim() {
    const uint32_t keep_from = last_turn_ >= opt_.max_turns ? last_turn_ - opt_.max_turns + 1 : 0;
    while (!steps_.empty() && (steps_.front().turn < keep_from || steps_.size() > opt_.max_steps)) {
        steps_.pop_front();
        ++evicted_;
    }
}

Episode EpisodeRecorder::window() const {
    Episode e;
    e.steps.assign(steps_.begin(), steps_.end());
    return e;
}

void EpisodeRecorder::drop_through(uint32_t turn, uint32_t round) {
    while (!steps_.empty() && (steps_.front().turn < turn || (steps_.front().turn == turn && steps_.front().round <= round))) steps_.pop_front();
}

}  // namespace learn
}  // namespace dd

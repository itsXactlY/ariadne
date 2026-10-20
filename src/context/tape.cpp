// src/context/tape.cpp — impl of dd/context/tape.hpp.
#include "dd/context/tape.hpp"

#include <chrono>
#include <cstdio>
#include <random>

namespace dd {
namespace context {

std::string Session::new_id() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    std::random_device rd;
    char b[48];
    std::snprintf(b, sizeof b, "dd-%llx-%04x",
                  static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::seconds>(now).count()),
                  rd() & 0xFFFFu);
    return b;
}

ItemPtr Session::append(core::Message m, bool hidden) {
    auto it = std::make_shared<Item>();
    it->msg = std::move(m);
    it->hidden = hidden;
    items_.push_back(it);
    return it;
}

void Session::replace(std::vector<ItemPtr> items) {
    items_ = std::move(items);
    ++generation_;
}

long Session::current_user() const {
    for (size_t i = items_.size(); i-- > 0;)
        if (items_[i]->msg.role == "user" && !items_[i]->hidden) return static_cast<long>(i);
    return -1;
}

ItemPtr Tape::first_source() const {
    if (blocks_.empty() || blocks_[0].sources.empty()) return nullptr;
    return blocks_[0].sources[0];
}

size_t Tape::messages() const {
    size_t n = 0;
    for (const auto& b : blocks_) n += b.rendered.size();
    return n;
}

void Tape::reset() {
    blocks_.clear();
    generation_ = -1;
}

void Tape::start(std::vector<ItemPtr> sources, std::vector<core::Message> rendered, int generation) {
    blocks_.clear();
    blocks_.push_back(Block{std::move(sources), std::move(rendered)});
    generation_ = generation;
}

std::pair<size_t, size_t> Tape::matched(const std::vector<ItemPtr>& view, int generation) const {
    if (generation != generation_) return {0, 0};
    size_t blocks = 0, pos = 0;
    for (const auto& b : blocks_) {
        const size_t end = pos + b.sources.size();
        if (end > view.size()) break;
        bool same = true;
        for (size_t i = 0; i < b.sources.size() && same; ++i) same = view[pos + i] == b.sources[i];
        if (!same) break;
        ++blocks;
        pos = end;
    }
    return {blocks, pos};
}

void Tape::truncate(size_t blocks) {
    if (blocks < blocks_.size()) blocks_.resize(blocks);
}

void Tape::append(ItemPtr source, std::vector<core::Message> rendered) {
    blocks_.push_back(Block{{std::move(source)}, std::move(rendered)});
}

std::vector<core::Message> Tape::payload() const {
    std::vector<core::Message> out;
    out.reserve(messages());
    for (const auto& b : blocks_) out.insert(out.end(), b.rendered.begin(), b.rendered.end());
    return out;
}

}  // namespace context
}  // namespace dd

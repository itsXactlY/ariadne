// src/know/memory_catalogue.cpp — impl of dd/know/memory_catalogue.hpp.
#include "dd/know/memory_catalogue.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "dd/core/json.hpp"
#include "dd/know/text.hpp"

namespace dd {
namespace know {

namespace {

std::string squash(const std::string& s) {
    std::string o;
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            sp = !o.empty();
            continue;
        }
        if (sp) o.push_back(' ');
        sp = false;
        o.push_back(c);
    }
    return o;
}

}  // namespace

const std::vector<std::string>& MemoryCatalogue::prefixes() {
    static const std::vector<std::string> p = {"decision:", "bug:", "invariant:", "ops:", "fact:", "status:", "signal:", "feedback:", "user:", "reference:"};
    return p;
}

std::string MemoryCatalogue::gist_of_row(const std::string& content) {
    // the first paragraph that is prose: not a header line of the kind "fact: A skill was created"
    size_t pos = 0;
    while (pos < content.size()) {
        size_t e = content.find("\n\n", pos);
        if (e == std::string::npos) e = content.size();
        const std::string para = squash(content.substr(pos, e - pos));
        pos = e + 2;
        if (!para.empty()) return utf8_head(para, 400);
    }
    return "";
}

MemoryCatalogue::MemoryCatalogue(std::shared_ptr<context::Memory> mem, std::filesystem::path cache_file, size_t page_bytes)
    : mem_(std::move(mem)), cache_(std::move(cache_file)), page_bytes_(page_bytes) {}

void MemoryCatalogue::load_cache() {
    cache_loaded_ = true;
    if (cache_.empty()) return;
    std::ifstream in(cache_);
    std::string line;
    while (std::getline(in, line)) {
        try {
            const core::Json j = core::json_parse(line);
            Row r;
            if (const core::Json* v = core::json_get(j, "id"); v && v->type == core::Json::Num) r.id = static_cast<long>(v->num);
            if (const core::Json* v = core::json_get(j, "label"); v && v->type == core::Json::Str) r.label = v->str;
            if (const core::Json* v = core::json_get(j, "gist"); v && v->type == core::Json::Str) r.gist = v->str;
            if (r.id > 0 && !r.label.empty()) rows_[r.id] = std::move(r);
        } catch (...) {
        }
    }
}

void MemoryCatalogue::save_cache() const {
    if (cache_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(cache_.parent_path(), ec);
    const std::filesystem::path tmp = cache_.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return;
        for (const auto& [id, r] : rows_)
            out << core::json_dump(core::json_obj({{"id", std::make_shared<core::Json>(core::json_num(static_cast<double>(id)))},
                                                   {"label", std::make_shared<core::Json>(core::json_str(r.label))},
                                                   {"gist", std::make_shared<core::Json>(core::json_str(r.gist))}}))
                << "\n";
    }
    std::filesystem::rename(tmp, cache_, ec);
}

void MemoryCatalogue::publish() {
    std::vector<Card> cards;
    auto snap = std::make_shared<Snapshot>();
    for (const auto& [id, r] : rows_) {
        Card c;
        c.uri.kind = Kind::Memory;
        c.uri.id = std::to_string(id);
        c.title = r.label;
        c.gist = r.gist;
        const size_t colon = r.label.find(':');
        c.group = colon == std::string::npos ? "other" : r.label.substr(0, colon);
        c.source = "maze";
        c.has_body = true;
        ++snap->by_group[c.group];
        cards.push_back(std::move(c));
    }
    snap->generation = gen_.fetch_add(1) + 1;
    snap->index = std::make_unique<Bm25f>(std::move(cards));
    snap_.store(snap);
}

void MemoryCatalogue::refresh() {
    if (!mem_ || !mem_->configured()) return;
    if (!cache_loaded_) {
        load_cache();
        if (!rows_.empty()) publish();   // searchable at once, from what the last run knew
    }
    if (!mem_->up()) mem_->probe();

    std::map<long, std::string> live;   // id -> label, the maze now
    bool reached = true;
    for (const auto& prefix : prefixes()) {
        for (int off = 0; off < 6000; off += 200) {
            const auto page = mem_->labels(prefix, off, 200);
            if (page.empty() && off == 0 && !mem_->up()) {
                reached = false;
                break;
            }
            for (const auto& h : page) live[h.id] = h.label;
            if (page.size() < 200) break;
        }
        if (!reached) break;
    }
    // a listing that came back far shorter than what we know is a pod that flaked mid-way, not a maze that lost its rows
    const bool short_listing = !rows_.empty() && live.size() < rows_.size() / 2;
    healthy_.store(reached && !live.empty() && !short_listing);
    if (!reached || live.empty() || short_listing) return;   // keep what we have

    bool changed = false;
    for (auto it = rows_.begin(); it != rows_.end();) {
        if (!live.count(it->first)) {
            it = rows_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    std::vector<long> fresh;
    for (const auto& [id, label] : live)
        if (!rows_.count(id)) fresh.push_back(id);
    for (size_t i = 0; i < fresh.size(); i += 25) {
        const std::vector<long> batch(fresh.begin() + static_cast<long>(i), fresh.begin() + static_cast<long>(std::min(fresh.size(), i + 25)));
        for (const auto& h : mem_->get(batch)) {
            Row r;
            r.id = h.id;
            r.label = h.label;
            r.gist = gist_of_row(h.content);
            rows_[r.id] = std::move(r);
            changed = true;
        }
    }
    if (changed || !snap_.load()) {
        publish();
        save_cache();
    }
}

uint64_t MemoryCatalogue::generation() const {
    const auto s = snap_.load();
    return s ? s->generation : 0;
}

size_t MemoryCatalogue::size() const {
    const auto s = snap_.load();
    return s ? s->index->size() : 0;
}

std::vector<Hit> MemoryCatalogue::search(const Query& q, int k) const {
    const auto s = snap_.load();
    return s ? s->index->search(q, k) : std::vector<Hit>{};
}

float MemoryCatalogue::idf(const std::string& term) const {
    const auto s = snap_.load();
    return s ? s->index->idf(term) : 0.0f;
}

std::optional<Body> MemoryCatalogue::open(const Uri& u) {
    if (u.kind != Kind::Memory || !mem_) return std::nullopt;
    long id = 0;
    try {
        id = std::stol(u.id);
    } catch (...) {
        return std::nullopt;
    }
    const auto rows = mem_->get({id});
    if (rows.empty() || rows[0].content.empty()) return std::nullopt;
    std::string text = "[" + rows[0].label + "]\n" + rows[0].content;
    return paginate(u, text, page_bytes_);
}

std::string MemoryCatalogue::map() const {
    const auto s = snap_.load();
    if (!s) return "";
    std::vector<std::pair<std::string, size_t>> groups(s->by_group.begin(), s->by_group.end());
    std::stable_sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out = "memory " + std::to_string(s->index->size()) + " curated:";
    size_t n = 0;
    for (const auto& [g, k] : groups) {
        if (n++ == 6) break;
        out += (n == 1 ? " " : " · ") + g + " " + std::to_string(k);
    }
    return out;
}

}  // namespace know
}  // namespace dd

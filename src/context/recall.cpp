// src/context/recall.cpp — impl of dd/context/recall.hpp.
#include "dd/context/recall.hpp"

#include "dd/context/maze.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace dd {
namespace context {

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

std::string utf8_cut(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "…";
}

// "#id": a snippet is cut, mazemaker_get(memory_ids=[id]) reads the rest
std::string line(const Hit& h, size_t max) {
    return "- " + (h.id > 0 ? "#" + std::to_string(h.id) + " " : std::string()) +
           (h.label.empty() ? std::string() : "[" + h.label + "] ") + utf8_cut(squash(h.content), max);
}

}  // namespace

// ---------------- Recaller ----------------

Prefetch Recaller::run(const std::string& query, const std::set<long>& in_window, const std::vector<long>& earlier,
                       size_t budget) const {
    Prefetch out;
    if (!mem_ || !mem_->configured()) return out;
    const auto t0 = std::chrono::steady_clock::now();
    out.intent = mem_->intent(query);
    std::vector<Hit> hits;
    for (auto& h : mem_->recall(query, 6)) {
        if (in_window.count(h.id)) ++out.dropped;
        else if (!h.content.empty()) hits.push_back(std::move(h));
    }

    // the graph around it, in parallel: each branch writes only its own result
    std::vector<Hit> linked, facts, before;
    std::set<long> seen(in_window);
    for (const auto& h : hits) seen.insert(h.id);
    std::thread walk([&] {
        if (hits.empty()) return;
        std::vector<long> ids;
        for (const auto& a : mem_->think(hits[0].id, 2)) {
            if (seen.count(a.id) || a.label.rfind("skill", 0) == 0) continue;
            ids.push_back(a.id);
            if (ids.size() == 3) break;
        }
        if (!ids.empty()) linked = mem_->get(ids);
    });
    std::thread extract([&] {
        for (size_t i = 0; i < std::min<size_t>(2, hits.size()); ++i) {
            auto f = mem_->facts(hits[i].id, 3);
            facts.insert(facts.end(), f.begin(), f.end());
        }
    });
    std::thread history([&] {
        if (out.intent != "temporal" || earlier.empty()) return;
        std::vector<long> ids(earlier.end() - static_cast<long>(std::min<size_t>(3, earlier.size())), earlier.end());
        before = mem_->get(ids);
    });
    walk.join();
    extract.join();
    history.join();

    std::string body;
    auto add = [&](char kind, const std::string& head, const std::vector<Hit>& rows, size_t max, int& n) {
        bool headed = false;
        for (const auto& h : rows) {
            if (h.content.empty()) continue;
            const std::string l = line(h, max) + "\n";
            if (body.size() + l.size() > budget) return;
            if (!headed) {
                body += head + "\n";
                headed = true;
            }
            body += l;
            out.ids.push_back(h.id);
            Hit s = h;
            s.content = utf8_cut(squash(h.content), 240);
            out.shown.emplace_back(kind, std::move(s));
            ++n;
        }
    };
    add('r', "Recalled for this turn:", hits, 420, out.hits);
    add('l', "Connected in the graph:", linked, 300, out.linked);
    add('f', "Extracted facts:", facts, 200, out.facts);
    add('e', "Earlier in this session (out of the window):", before, 420, out.earlier);
    out.block = context_block(body);
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

}  // namespace context
}  // namespace dd

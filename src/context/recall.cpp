// src/context/recall.cpp — impl of dd/context/recall.hpp.
#include "dd/context/recall.hpp"

#include "dd/context/maze.hpp"
#include "dd/tools/tool_search.hpp"

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

std::string line(const Hit& h, size_t max) {
    return "- " + (h.label.empty() ? std::string() : "[" + h.label + "] ") + utf8_cut(squash(h.content), max);
}

// The summary row's own description: frontmatter description:, else the
// first paragraph after its header lines.
std::string describe(const std::string& content) {
    if (const size_t d = content.find("\ndescription:"); d != std::string::npos) {
        size_t b = d + 13;
        const size_t e = content.find('\n', b);
        std::string v = squash(content.substr(b, e == std::string::npos ? std::string::npos : e - b));
        if (v.size() > 1 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
        if (!v.empty()) return v;
    }
    size_t pos = 0;
    while (pos < content.size()) {
        size_t e = content.find("\n\n", pos);
        if (e == std::string::npos) e = content.size();
        const std::string para = squash(content.substr(pos, e - pos));
        pos = e + 2;
        if (para.empty() || para.rfind("Skill:", 0) == 0 || para.rfind("Hermes skill:", 0) == 0 ||
            para.rfind("Full text in", 0) == 0 || para.rfind("---", 0) == 0 || para.rfind("Category:", 0) == 0)
            continue;
        return para;
    }
    return "";
}

std::string field(const std::string& content, const char* key) {
    const std::string k = std::string("\n") + key + ": ";
    const size_t p = ("\n" + content).find(k);
    if (p == std::string::npos) return "";
    const size_t b = p + k.size() - 1, e = content.find('\n', b);
    return squash(content.substr(b, e == std::string::npos ? std::string::npos : e - b));
}

}  // namespace

// ---------------- SkillIndex ----------------

SkillIndex::~SkillIndex() { slot_->alive.store(false); }

size_t SkillIndex::size() const {
    const Data* d = data();
    return d ? d->skills.size() : 0;
}

void SkillIndex::publish(Slot& slot, const std::vector<Hit>& rows) {
    auto* d = new Data;
    for (const auto& r : rows) {
        // skill:<source>:<name> (or skill:<name>); the catalogue also holds a few notes under skill:
        std::string rest = r.label.rfind("skill:", 0) == 0 ? r.label.substr(6) : r.label;
        Skill s;
        s.id = r.id;
        if (const size_t c = rest.find(':'); c != std::string::npos) {
            s.source = rest.substr(0, c);
            s.name = rest.substr(c + 1);
        } else {
            s.name = rest;
        }
        if (s.name.empty() || s.name.find(' ') != std::string::npos) continue;
        s.category = field(r.content, "Category");
        s.description = describe(r.content);
        const auto same = std::find_if(d->skills.begin(), d->skills.end(), [&](const Skill& o) { return o.name == s.name; });
        if (same != d->skills.end()) {
            if (same->source == "local" || s.source != "local") continue;   // the operator's own copy wins
            *same = s;
            continue;
        }
        d->skills.push_back(std::move(s));
    }
    for (const auto& s : d->skills) d->docs.emplace_back(s.name, s.category + " " + s.description);
    const Data* none = nullptr;   // first one wins: a reader may hold the published data
    if (!slot.data.compare_exchange_strong(none, d, std::memory_order_acq_rel)) delete d;
}

void SkillIndex::load_rows(const std::vector<Hit>& rows) { publish(*slot_, rows); }

void SkillIndex::load_async() {
    if (!mem_ || !mem_->configured()) return;
    std::thread([mem = mem_, slot = slot_] {
        std::vector<Hit> labels;
        for (int off = 0; off < 4000 && slot->alive.load(); off += 200) {
            auto page = mem->labels("skill:", off, 200);
            labels.insert(labels.end(), page.begin(), page.end());
            if (page.size() < 200) break;
        }
        std::vector<long> ids;
        for (const auto& h : labels) ids.push_back(h.id);
        if (!slot->alive.load() || ids.empty()) return;
        auto rows = mem->get(ids);
        if (slot->alive.load()) publish(*slot, rows);
    }).detach();
}

std::vector<std::pair<SkillIndex::Skill, double>> SkillIndex::search(const std::string& text, int limit) const {
    std::vector<std::pair<Skill, double>> out;
    const Data* d = data();
    if (!d || d->docs.empty()) return out;
    for (const auto& [name, score] : tools::rank_documents(d->docs, text, limit))
        for (const auto& s : d->skills)
            if (s.name == name) {
                out.emplace_back(s, score);
                break;
            }
    return out;
}

std::optional<SkillIndex::Skill> SkillIndex::match(const std::string& text, double min_score) const {
    // paths name places, not tasks: "…/include/dd/context" must not pull a "context" skill
    std::string words, w;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ' ';
        if (c == ' ' || c == '\n' || c == '\t') {
            if (!w.empty() && w.find('/') == std::string::npos) words += w + " ";
            w.clear();
        } else {
            w.push_back(c);
        }
    }
    const auto best = search(words, 1);
    if (best.empty() || best[0].second < min_score) return std::nullopt;
    return best[0].first;
}

std::string SkillIndex::load(const std::string& name, std::string* resolved) const {
    const Data* d = data();
    if (!d || !mem_) return "";
    const Skill* s = nullptr;
    for (const auto& k : d->skills)
        if (k.name == name) s = &k;
    if (!s) {   // a near name: the best search hit
        const auto hits = search(name, 1);
        if (hits.empty()) return "";
        for (const auto& k : d->skills)
            if (k.name == hits[0].first.name) s = &k;
    }
    if (!s) return "";
    if (resolved) *resolved = s->name;
    for (const std::string& src : {s->source, std::string("local"), std::string("builtin")}) {
        if (src.empty()) continue;
        auto parts = mem_->labels("skillsrc:" + src + ":" + s->name + "#", 0, 100);
        if (parts.empty()) continue;
        std::sort(parts.begin(), parts.end(), [](const Hit& a, const Hit& b) { return a.label < b.label; });
        std::vector<long> ids;
        for (const auto& p : parts) ids.push_back(p.id);
        auto bodies = mem_->get(ids);
        std::sort(bodies.begin(), bodies.end(), [](const Hit& a, const Hit& b) { return a.label < b.label; });
        std::string text;
        for (const auto& b : bodies) text += b.content;
        if (!text.empty()) return text;
    }
    const auto row = mem_->get({s->id});   // no parts: the summary is the skill
    return row.empty() ? "" : row[0].content;
}

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
    auto add = [&](const std::string& head, const std::vector<Hit>& rows, size_t max, int& n) {
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
            ++n;
        }
    };
    add("Recalled for this turn:", hits, 420, out.hits);
    add("Connected in the graph:", linked, 300, out.linked);
    add("Extracted facts:", facts, 200, out.facts);
    add("Earlier in this session (out of the window):", before, 420, out.earlier);
    if (skills_)
        if (auto s = skills_->match(query, skill_min)) {
            out.skill = s->name;
            const std::string l = "Skill that fits: " + s->name + " — " + utf8_cut(s->description, 200) +
                                  " (skill_load(\"" + s->name + "\") reads it)\n";
            if (body.size() + l.size() <= budget + 400) body += l;
        }
    out.block = context_block(body);
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

}  // namespace context
}  // namespace dd

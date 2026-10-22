// src/know/skills.cpp — impl of dd/know/skills.hpp.
#include "dd/know/skills.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <system_error>

#include "dd/core/par.hpp"
#include "dd/core/sha256.hpp"
#include "dd/core/yaml.hpp"
#include "dd/skills/scanner.hpp"

namespace dd {
namespace know {

namespace fs = std::filesystem;

namespace {

constexpr size_t kParSkills = 64;   // fewer skills than this are read in turn: a parallel region costs more than it saves

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

// One line, at most n bytes, cut at a word and never inside a UTF-8 sequence.
std::string gist_of(const std::string& s, size_t n = 400) {
    std::string t = squash(s);
    if (t.size() <= n) return t;
    size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;
    if (const size_t sp = t.rfind(' ', cut); sp != std::string::npos && sp > n / 2) cut = sp;
    return t.substr(0, cut) + "…";
}

std::optional<std::string> read_all(const fs::path& p, size_t cap = 4u << 20) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > cap) text.resize(cap);
    return text;
}

std::string rev_of(const std::string& text) { return core::sha256_hex(text).substr(0, 12); }

uint64_t fnv(uint64_t h, const std::string& s) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return (h ^ 0xFF) * 1099511628211ull;
}

// "\nKey: value" in a card, squashed; "" when absent.
std::string field_of(const std::string& content, const char* key) {
    const std::string k = std::string("\n") + key + ": ";
    const size_t p = ("\n" + content).find(k);
    if (p == std::string::npos) return "";
    const size_t b = p + k.size() - 1, e = content.find('\n', b);
    return squash(content.substr(b, e == std::string::npos ? std::string::npos : e - b));
}

// The card's own description: frontmatter description:, else the first paragraph after its header lines.
std::string describe(const std::string& content) {
    if (std::string v = field_of(content, "Description"); !v.empty()) return v;   // a legacy pointer card
    if (const size_t d = content.find("\ndescription:"); d != std::string::npos) {
        const size_t b = d + 13, e = content.find('\n', b);
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
            para.rfind("[CANONICAL]", 0) == 0 || para.rfind("Full text in", 0) == 0 || para.rfind("---", 0) == 0 ||
            para.rfind("Category:", 0) == 0)
            continue;
        return para;
    }
    return "";
}

std::vector<std::string> list_of(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v) {
        if (c == '[' || c == ']') continue;
        if (c == ',' || c == ';') {
            if (const std::string t = squash(cur); !t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (const std::string t = squash(cur); !t.empty()) out.push_back(t);
    return out;
}

bool skill_name(const std::string& n) {
    if (n.empty() || n.size() > 80) return false;
    return std::all_of(n.begin(), n.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'; });
}

}  // namespace

// ---------------- DiskSkills ----------------

std::vector<Card> DiskSkills::list() {
    skills::Scanner sc;
    std::error_code ec;
    for (const auto& r : roots_)
        if (fs::is_directory(r, ec)) sc.add_root(r);
    sc.set_platform("linux");
    const auto found = sc.scan();
    // each skill's file is read and hashed on its own: the loop runs on all cores, the cards are gathered in order below
    std::vector<std::optional<Card>> slots(found.size());
    const long n = static_cast<long>(found.size());
    [[maybe_unused]] const bool par = found.size() >= kParSkills;   // only the OpenMP clause reads it
#pragma omp parallel for schedule(dynamic, 8) num_threads(par ? core::par_threads() : 1) if (par)
    for (long i = 0; i < n; ++i) {
        try {
            const auto& s = found[static_cast<size_t>(i)];
            const auto text = read_all(s.skill_md);
            if (!text) continue;
            Card c;
            c.uri.kind = Kind::Skill;
            c.uri.id = s.name;
            c.title = s.name;
            c.gist = gist_of(s.description);
            c.group = s.category;
            if (auto it = s.frontmatter.find("tags"); it != s.frontmatter.end()) c.tags = core::parse_list(it->second);
            c.source = "disk";
            for (const auto& r : roots_)
                if (s.skill_md.string().rfind(r.string(), 0) == 0) {
                    c.source = "disk:" + (r.has_parent_path() ? r.parent_path().filename().string() + "/" : std::string()) + r.filename().string();
                    break;
                }
            c.body_bytes = text->size();
            c.rev = rev_of(*text);
            slots[static_cast<size_t>(i)] = std::move(c);
        } catch (...) {   // a skill that cannot be read is left out, as one with no file is
        }
    }
    auto places = std::make_unique<Places>();
    std::vector<Card> out;
    for (size_t i = 0; i < found.size(); ++i) {
        if (!slots[i]) continue;
        places->emplace(found[i].name, Place{found[i].skill_md, found[i].skill_dir});
        out.push_back(std::move(*slots[i]));
    }
    places_.publish(std::move(places));
    return out;
}

std::optional<std::string> DiskSkills::body(const Card& c) {
    fs::path md;
    {
        const auto places = places_.read();
        if (!places) return std::nullopt;
        auto it = places->find(c.uri.id);
        if (it == places->end()) return std::nullopt;
        md = it->second.md;   // copied out: the file is read with the pin released
    }
    return read_all(md);
}

std::optional<std::string> DiskSkills::file(const Card& c, const std::string& rel) {
    fs::path root;
    {
        const auto places = places_.read();
        if (!places) return std::nullopt;
        auto it = places->find(c.uri.id);
        if (it == places->end()) return std::nullopt;
        root = it->second.dir;
    }
    std::error_code ec;
    const fs::path dir = fs::weakly_canonical(root, ec);
    const fs::path tgt = fs::weakly_canonical(root / rel, ec);
    if (ec) return std::nullopt;
    const std::string d = dir.string(), t = tgt.string();
    if (t != d && t.compare(0, d.size() + 1, d + "/") != 0) return std::nullopt;   // never out of the skill's folder
    if (fs::is_directory(tgt, ec)) {
        std::vector<std::string> names;
        for (const auto& e : fs::directory_iterator(tgt, ec)) names.push_back(e.path().filename().string() + (e.is_directory(ec) ? "/" : ""));
        std::sort(names.begin(), names.end());
        std::string listing = rel + "/\n";
        for (const auto& n : names) listing += "  " + n + "\n";
        return listing;
    }
    return read_all(tgt, 1u << 20);
}

// ---------------- MazeSkills ----------------

std::vector<Card> MazeSkills::list() {
    skipped_.store(0);
    std::vector<context::Hit> labels;
    for (int off = 0; off < 4000; off += 200) {
        if (cancel_.load()) {
            ok_.store(false);
            return {};
        }
        auto page = mem_->labels("skill:", off, 200);
        labels.insert(labels.end(), page.begin(), page.end());
        if (page.size() < 200) break;
    }
    if (labels.empty()) {
        ok_.store(false);
        return {};
    }
    std::set<std::string> with_body;   // "<source>:<name>" of every skillsrc:<source>:<name>#NNN
    for (int off = 0; off < 8000; off += 200) {
        if (cancel_.load()) {
            ok_.store(false);
            return {};
        }
        auto page = mem_->labels("skillsrc:", off, 200);
        for (const auto& h : page)
            if (const size_t hash = h.label.rfind('#'); hash != std::string::npos && h.label.size() > 9) with_body.insert(h.label.substr(9, hash - 9));
        if (page.size() < 200) break;
    }
    std::vector<long> ids;
    for (const auto& h : labels) ids.push_back(h.id);
    std::vector<context::Hit> rows;
    for (size_t i = 0; i < ids.size(); i += 100) {   // in chunks, so a cancel is heard within one call
        if (cancel_.load()) {
            ok_.store(false);
            return {};
        }
        const std::vector<long> chunk(ids.begin() + static_cast<long>(i), ids.begin() + static_cast<long>(std::min(ids.size(), i + 100)));
        auto part = mem_->get(chunk);
        rows.insert(rows.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
    ok_.store(mem_->up() && !rows.empty());

    std::vector<Card> out;
    size_t skipped = 0;
    auto paths = std::make_unique<Paths>();
    for (const auto& r : rows) {
        // skill:<source>:<name>; a note filed under skill: ("fact: a skill was created ...") has no source
        const std::string rest = r.label.rfind("skill:", 0) == 0 ? r.label.substr(6) : r.label;
        const size_t colon = rest.find(':');
        if (colon == std::string::npos) {
            ++skipped;
            continue;
        }
        const std::string src = rest.substr(0, colon), name = rest.substr(colon + 1);
        if (src.empty() || !skill_name(name)) {   // also an ::afe:: fragment
            ++skipped;
            continue;
        }
        std::string content = r.content;
        if (const size_t prev = content.find("\n[PREVIOUSLY]"); prev != std::string::npos) content.resize(prev);   // an older version of the same card
        Card c;
        c.uri.kind = Kind::Skill;
        c.uri.id = name;
        c.title = name;
        c.source = "maze:" + src;
        c.group = field_of(content, "Category");
        if (c.group == "(top-level)") c.group.clear();
        c.gist = gist_of(describe(content));
        c.tags = list_of(field_of(content, "Tags"));
        for (const auto& t : list_of(field_of(content, "Triggers"))) c.tags.push_back(t);
        if (const size_t sha = content.find("sha256 "); sha != std::string::npos) {
            size_t e = sha + 7;
            while (e < content.size() && std::isxdigit(static_cast<unsigned char>(content[e]))) ++e;
            if (e - (sha + 7) >= 8) c.rev = content.substr(sha + 7, std::min<size_t>(12, e - (sha + 7)));
        }
        if (const size_t bytes = content.find(" bytes"); bytes != std::string::npos && bytes > 0) {
            size_t b = bytes;
            while (b > 0 && std::isdigit(static_cast<unsigned char>(content[b - 1]))) --b;
            if (b < bytes) c.body_bytes = static_cast<size_t>(std::stoul(content.substr(b, bytes - b)));
        }
        c.has_body = with_body.count(src + ":" + name) > 0;
        if (!c.has_body) c.body_bytes = 0;
        // a legacy pointer card keeps its body where it always was: Path: /home/x/.hermes/skills/.../SKILL.md
        if (const std::string path = field_of(content, "Path"); !c.has_body && !path.empty()) {
            std::error_code ec;
            const fs::path p = fs::path(path);
            if (fs::is_regular_file(p, ec)) {
                c.has_body = true;
                c.body_bytes = static_cast<size_t>(fs::file_size(p, ec));
                (*paths)[c.source + "/" + name] = p;
            }
        }
        out.push_back(std::move(c));
    }
    paths_.publish(std::move(paths));
    skipped_.store(skipped);
    auto rank = [](const std::string& s) { return s == "maze:local" ? 0 : s == "maze:maze" ? 1 : s == "maze:builtin" ? 3 : 2; };
    std::stable_sort(out.begin(), out.end(), [&](const Card& a, const Card& b) { return rank(a.source) < rank(b.source); });
    return out;
}

std::optional<std::string> MazeSkills::body(const Card& c) {
    fs::path pointed;   // a pointer card's own file
    if (const auto paths = paths_.read()) {
        if (auto it = paths->find(c.source + "/" + c.uri.id); it != paths->end()) pointed = it->second;
    }
    if (!pointed.empty()) return read_all(pointed);
    const std::string src = c.source.rfind("maze:", 0) == 0 ? c.source.substr(5) : c.source;
    for (const std::string& s : {src, std::string("local"), std::string("builtin")}) {
        if (s.empty()) continue;
        auto parts = mem_->labels("skillsrc:" + s + ":" + c.uri.id + "#", 0, 100);
        if (parts.empty()) continue;
        std::sort(parts.begin(), parts.end(), [](const context::Hit& a, const context::Hit& b) { return a.label < b.label; });
        std::vector<long> ids;
        for (const auto& p : parts) ids.push_back(p.id);
        auto bodies = mem_->get(ids);
        std::sort(bodies.begin(), bodies.end(), [](const context::Hit& a, const context::Hit& b) { return a.label < b.label; });
        std::string text;
        for (const auto& b : bodies) text += b.content;
        if (!text.empty()) return text;
    }
    return std::nullopt;
}

// ---------------- SkillCatalogue ----------------

SkillCatalogue::SkillCatalogue(std::vector<std::shared_ptr<SkillSource>> sources, Options o)
    : sources_(std::move(sources)), opt_(std::move(o)) {}

void SkillCatalogue::refresh() {
    // a source that is down keeps the cards it last gave: a blip must not empty the catalogue
    if (last_good_.size() != sources_.size()) last_good_.assign(sources_.size(), {});

    std::vector<Card> shown;
    std::vector<Owned> owned;
    std::map<std::string, size_t> by_id;
    Census census;
    bool any_ok = false, all_ok = true;

    // makes a snapshot of what the sources have given so far searchable (unless it is the one already published)
    auto publish = [&](std::vector<Card> cards, std::vector<Owned> own, Census cen) {
        uint64_t fp = 1469598103934665603ull;
        if (overlay_) fp = fnv(fp, "overlay|" + std::to_string(overlay_->version()));
        for (auto& c : cards) {
            if (const auto p = opt_.priors.find(c.uri.id); p != opt_.priors.end()) c.prior = p->second;
            if (overlay_) {
                c.prior *= overlay_->prior(c.uri);
                overlay_->learned_terms(c.uri, c.learned);
            }
            fp = fnv(fp, c.uri.id + "|" + c.rev + "|" + c.source + "|" + (c.has_body ? "1" : "0") + "|" + c.gist);
        }
        if (const auto old = snap_.read(); old && old->fingerprint == fp) return;   // nothing changed: same generation
        auto snap = std::make_unique<Snapshot>();
        snap->fingerprint = fp;
        snap->generation = gen_.fetch_add(1) + 1;
        cen.cards = cards.size();
        for (const auto& c : cards) {
            if (!c.has_body) ++cen.bodyless;
            ++cen.by_group[c.group.empty() ? "other" : c.group];
            ++cen.by_source[c.source];
        }
        snap->census = std::move(cen);
        snap->owned = std::move(own);
        for (size_t i = 0; i < cards.size(); ++i) snap->by_id[cards[i].uri.id] = i;
        snap->index = std::make_unique<Bm25f>(std::move(cards), opt_.bm25);
        snap_.publish(std::move(snap));
    };

    for (size_t si = 0; si < sources_.size(); ++si) {
        auto& src = sources_[si];
        std::vector<Card> cards;
        try {
            cards = src->list();
        } catch (...) {
        }
        const bool ok = src->ok();
        any_ok = any_ok || ok;
        all_ok = all_ok && ok;
        if (ok && !cards.empty()) last_good_[si] = cards;
        else if (!ok && !last_good_[si].empty()) cards = last_good_[si];
        census.skipped += src->skipped();

        for (auto& c : cards) {
            if (opt_.disabled.count(c.uri.id)) continue;
            auto it = by_id.find(c.uri.id);
            if (it == by_id.end()) {
                by_id[c.uri.id] = shown.size();
                shown.push_back(c);
                owned.push_back({src, c});
                continue;
            }
            const size_t at = it->second;
            // maze:local is an index of this host's disk skills: a snapshot of the skill on disk, older than it
            if (shown[at].source.rfind("disk:", 0) == 0 && c.source == "maze:local") {
                ++census.superseded;
                continue;
            }
            const std::string& have = shown[at].rev;
            const size_t n = std::min(have.size(), c.rev.size());
            const bool differs = n > 0 && have.compare(0, n, c.rev, 0, n) != 0;   // both hashes known and unequal
            if (!differs) {
                ++census.collapsed;
                if (!shown[at].has_body && c.has_body) {   // an identical copy with a body lends it
                    shown[at].has_body = true;
                    shown[at].body_bytes = c.body_bytes;
                    owned[at] = {src, c};
                }
                continue;
            }
            Card alias = c;   // a different skill under the same name: kept, reachable as name@source
            std::string tag = c.source;
            if (const size_t colon = tag.find(':'); colon != std::string::npos) tag = tag.substr(colon + 1);
            alias.uri.id = c.uri.id + "@" + tag;
            alias.title = alias.uri.id;
            if (by_id.count(alias.uri.id)) continue;
            by_id[alias.uri.id] = shown.size();
            shown.push_back(alias);
            owned.push_back({src, c});
            ++census.aliased;
        }
        // the first source is the local one: searchable now, while the maze is still being listed (the pod is the slow leg)
        if (si == 0 && sources_.size() > 1 && snap_.empty() && !shown.empty()) publish(shown, owned, census);
    }
    healthy_.store(all_ok);
    if (!any_ok && shown.empty()) return;   // nothing reachable yet: stay unready rather than publish an empty set
    publish(std::move(shown), std::move(owned), std::move(census));
}

uint64_t SkillCatalogue::generation() const {
    const auto s = snap_.read();
    return s ? s->generation : 0;
}

size_t SkillCatalogue::size() const {
    const auto s = snap_.read();
    return s ? s->index->size() : 0;
}

std::vector<Hit> SkillCatalogue::search(const Query& q, int k) const {
    const auto s = snap_.read();
    return s ? s->index->search(q, k) : std::vector<Hit>{};
}

float SkillCatalogue::idf(const std::string& term) const {
    const auto s = snap_.read();
    return s ? s->index->idf(term) : 0.0f;
}

std::optional<Card> SkillCatalogue::find(const std::string& name) const {
    const auto s = snap_.read();
    if (!s) return std::nullopt;
    auto it = s->by_id.find(name);
    if (it == s->by_id.end()) return std::nullopt;
    return s->index->cards()[it->second];
}

std::vector<Card> SkillCatalogue::cards() const {
    const auto s = snap_.read();
    return s ? s->index->cards() : std::vector<Card>{};
}

SkillCatalogue::Census SkillCatalogue::census() const {
    const auto s = snap_.read();
    return s ? s->census : Census{};
}

std::optional<Body> SkillCatalogue::open(const Uri& u) {
    Owned o;
    {
        const auto s = snap_.read();
        if (!s || u.kind != Kind::Skill) return std::nullopt;
        auto it = s->by_id.find(u.id);
        if (it == s->by_id.end()) return std::nullopt;
        o = s->owned[it->second];   // copied out: the source is opened (a file, the pod) with the pin released
    }
    std::optional<std::string> text = u.path.empty() ? o.src->body(o.orig) : o.src->file(o.orig, u.path);
    if (!text) return std::nullopt;
    if (u.path.empty() && text->rfind("---", 0) == 0) {   // the SKILL.md's own frontmatter is the card's business, not the model's tokens
        std::string rest = core::body(*text);
        if (!rest.empty()) text = std::move(rest);
    }
    return paginate(u, *text, opt_.page_bytes);
}

std::string SkillCatalogue::map() const {
    const auto s = snap_.read();
    if (!s) return "";
    std::vector<std::pair<std::string, size_t>> groups(s->census.by_group.begin(), s->census.by_group.end());
    std::stable_sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out = "skills " + std::to_string(s->census.cards) + ":";
    size_t shown = 0;
    for (const auto& [g, n] : groups) {
        if (shown++ == 10) break;
        out += (shown == 1 ? " " : " · ") + g + " " + std::to_string(n);
    }
    if (groups.size() > 10) out += " · +" + std::to_string(groups.size() - 10) + " groups";
    return out;
}

}  // namespace know
}  // namespace dd

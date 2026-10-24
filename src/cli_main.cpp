// dd — daedalus-cpp harness CLI (the skill catalogue, and the tools to keep it honest).
//
//   dd list [--category C] [--limit N] [--names-only]   the merged catalogue: ./skills, external dirs, the maze
//   dd view NAME [FILE] [--page N|all]                  a skill, a page at a time (or one of its files)
//   dd resolve skill://NAME[/FILE][#PAGE]               the same, by uri
//   dd find QUERY... [--k N]                            what the harness would offer for this text
//   dd skills check [--strict]                          the census: bodyless, duplicate, note-like, no-gist cards
//   dd replay FILE [--k N] [--show]                     prompts (jsonl {"text","want":[...]}, or one per line)
//                                                       through the catalogue: offers, hits and misses per threshold
//   dd learn status|why SKILL|report|forget SKILL|export   what the router has learned (read-only, except forget)
//   dd import-daedalus [--from DIR] [--skills] [--skins] [--with-config]   copy from the Python Daedalus's home, once
//
// flags: --home DIR  --config FILE  --maze URL  --no-maze  (config.yaml sits beside the binary)
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "dd/context/maze.hpp"
#include "dd/core/config.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"
#include "dd/know/policy.hpp"
#include "dd/learn/inspect.hpp"
#include "dd/know/skills.hpp"
#include "dd/know/text.hpp"
#include "dd/know/wiring.hpp"

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::know;

namespace {

struct Args {
    std::vector<std::string> pos;
    std::string home, config, maze, from, page, k, strict, show;
    bool no_maze = false, skills = false, skins = false, cfg = false;
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        auto val = [&](std::string& dst) { if (i + 1 < argc) dst = argv[++i]; };
        if (s == "--home") val(a.home);
        else if (s == "--config") val(a.config);
        else if (s == "--maze") val(a.maze);
        else if (s == "--no-maze") a.no_maze = true;
        else if (s == "--from") val(a.from);
        else if (s == "--page") val(a.page);
        else if (s == "--k") val(a.k);
        else if (s == "--strict") a.strict = "1";
        else if (s == "--show") a.show = "1";
        else if (s == "--skills") a.skills = true;
        else if (s == "--skins") a.skins = true;
        else if (s == "--with-config") a.cfg = true;
        else a.pos.push_back(s);
    }
    return a;
}

struct Env {
    core::Home home;
    Policy policy;
    SkillSetup setup;
    std::shared_ptr<context::Maze> maze;
    std::shared_ptr<SkillCatalogue> cat;
};

Env open_env(const Args& a) {
    Env e;
    std::string why;
    e.home = core::resolve_home(a.home, a.config, &why);
    if (e.home.root.empty()) { std::cerr << why << "\n"; std::exit(2); }
    core::Config cfg{core::Json{}};
    const fs::path cfg_path = a.config.empty() ? e.home.config() : fs::path(a.config);
    std::error_code ec;
    if (fs::exists(cfg_path, ec)) {
        try { cfg = core::load_config(cfg_path.string()); } catch (const std::exception& ex) { std::cerr << "config: " << ex.what() << "\n"; }
    }
    e.setup = skill_setup(cfg, e.home);
    e.policy = Policy::from(cfg);
    const std::string url = a.no_maze ? std::string() : !a.maze.empty() ? a.maze : maze_url_of(cfg);
    if (!url.empty() && e.setup.maze) {
        e.maze = std::make_shared<context::Maze>(url);
        e.maze->probe();
        if (!e.maze->up()) std::cerr << "dd: the maze at " << url << " is not answering: the catalogue is the disk's alone\n";
    }
    e.cat = make_skill_catalogue(e.setup, e.maze);
    e.cat->refresh();
    return e;
}

// A whole decimal number in [lo, hi]; false for anything else (empty, trailing junk, out of range).
bool parse_int(const std::string& text, int lo, int hi, int& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

int bad_number(const char* flag, const std::string& got) {
    std::cerr << "dd: " << flag << " takes a whole number, got '" << got << "'\n";
    return 1;
}

std::string kb(size_t bytes) {
    char b[32];
    std::snprintf(b, sizeof b, bytes >= 10240 ? "%.0f KB" : "%.1f KB", static_cast<double>(bytes) / 1024.0);
    return b;
}

Query query_of(const Env& e, const std::string& text) {
    return Distiller().text(text, [&](const std::string& t) { return e.cat->idf(t); });
}

int cmd_list(const Args& a) {
    std::string category;
    int limit = 20;
    bool names_only = false;
    for (size_t i = 0; i < a.pos.size(); ++i) {
        if (a.pos[i] == "--category" && i + 1 < a.pos.size()) category = a.pos[++i];
        else if (a.pos[i] == "--limit" && i + 1 < a.pos.size()) {
            if (!parse_int(a.pos[i + 1], 0, 1000000, limit)) return bad_number("--limit", a.pos[i + 1]);
            ++i;
        } else if (a.pos[i] == "--names-only") names_only = true;
    }
    Env e = open_env(a);
    const auto c = e.cat->census();
    std::cout << "total=" << c.cards << " groups=" << c.by_group.size() << "\n";
    for (const auto& [g, n] : c.by_group) std::cout << "  " << g << ": " << n << "\n";
    int shown = 0;
    for (const auto& card : e.cat->cards()) {
        if (!category.empty() && card.group != category) continue;
        if (limit > 0 && shown++ >= limit) break;
        std::cout << "  [" << (card.group.empty() ? "-" : card.group) << "] " << card.uri.id;
        if (!names_only) std::cout << " — " << utf8_head(card.gist, 120);
        std::cout << "\n";
    }
    return 0;
}

int cmd_view(const std::string& uri_text, const Args& a) {
    Env e = open_env(a);
    auto u = Uri::parse(uri_text);
    if (!u) {
        Uri v;
        v.id = uri_text;
        if (a.pos.size() > 1) v.path = a.pos[1];
        u = v;
    }
    if (!a.page.empty()) u->page = a.page;
    const auto body = e.cat->open(*u);
    if (!body) {
        std::cerr << "skill '" << u->str() << "' not found or has no body\n";
        for (const auto& h : e.cat->search(query_of(e, u->id), 3)) std::cerr << "  close: " << h.card.uri.id << "\n";
        return 1;
    }
    std::cout << body->text;
    if (!body->more.empty()) std::cerr << "\n[page " << body->page << "/" << body->pages << " of " << kb(body->total) << " — next: " << body->more << "]\n";
    return 0;
}

int cmd_find(const Args& a) {
    if (a.pos.empty()) { std::cerr << "usage: dd find QUERY...\n"; return 1; }
    int k = 8;
    if (!a.k.empty() && !parse_int(a.k, 1, 1000, k)) return bad_number("--k", a.k);
    Env e = open_env(a);
    std::string text;
    for (const auto& w : a.pos) text += (text.empty() ? "" : " ") + w;
    const Query q = query_of(e, text);
    std::cout << "terms:";
    for (const auto& t : q.terms) std::cout << " " << t.text;
    std::cout << "\n";
    for (const auto& h : e.cat->search(q, k)) {
        std::printf("%5.2f  %-42s [%s · %s%s]  %s\n", h.score, h.card.uri.id.c_str(), h.card.group.empty() ? "-" : h.card.group.c_str(),
                    h.card.source.c_str(), h.card.has_body ? (", " + kb(h.card.body_bytes)).c_str() : ", no body", utf8_head(h.card.gist, 90).c_str());
        std::printf("       matched: %s\n", h.why.c_str());
    }
    return 0;
}

int cmd_check(const Args& a) {
    Env e = open_env(a);
    const auto c = e.cat->census();
    std::vector<std::string> bodyless, nogist;
    for (const auto& card : e.cat->cards()) {
        if (!card.has_body) bodyless.push_back(card.uri.id);
        if (card.gist.empty()) nogist.push_back(card.uri.id);
    }
    auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 12; ++i) s += (i ? ", " : "") + v[i];
        return v.size() > 12 ? s + ", ... (+" + std::to_string(v.size() - 12) + ")" : s;
    };
    std::cout << "skills: " << c.cards << "   catalogue " << (e.cat->healthy() ? "healthy" : "NOT healthy (a source did not answer)") << "\n";
    for (const auto& [s, n] : c.by_source) std::cout << "  source " << s << ": " << n << "\n";
    std::cout << "  groups: " << c.by_group.size() << "\n";
    std::cout << "  identical copies collapsed: " << c.collapsed << "   maze snapshots older than their disk skill: " << c.superseded << "\n";
    std::cout << "  differing copies kept as name@source: " << c.aliased << "\n";
    std::cout << "  rows that are not skills (notes, fragments): " << c.skipped << "\n";
    std::cout << "  listings that hit their cap (more behind them): " << c.truncated << "\n";
    std::cout << "  no body: " << bodyless.size() << (bodyless.empty() ? "" : "   " + list(bodyless)) << "\n";
    std::cout << "  no gist: " << nogist.size() << (nogist.empty() ? "" : "   " + list(nogist)) << "\n";
    const bool problems = !bodyless.empty() || !nogist.empty() || c.skipped > 0 || c.truncated > 0;
    return (a.strict.empty() || !problems) ? 0 : 1;
}

// ---- replay: a corpus through the catalogue, offers and misses at each threshold ----
struct Prompt {
    std::string text;
    std::vector<std::string> want;
    bool labeled = false;
};

std::vector<Prompt> read_corpus(const std::string& path) {
    std::vector<Prompt> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Prompt p;
        if (line.front() == '{') {
            try {
                const core::Json j = core::json_parse(line);
                if (const core::Json* t = core::json_get(j, "text"); t && t->type == core::Json::Str) p.text = t->str;
                if (const core::Json* w = core::json_get(j, "want"); w && w->type == core::Json::Arr) {
                    p.labeled = true;
                    for (const auto& x : w->arr)
                        if (x && x->type == core::Json::Str) p.want.push_back(x->str);
                }
            } catch (...) {
                continue;
            }
        } else {
            p.text = line;
        }
        if (!p.text.empty()) out.push_back(std::move(p));
    }
    return out;
}

int cmd_replay(const Args& a) {
    if (a.pos.empty()) { std::cerr << "usage: dd replay FILE [--k N] [--show]\n"; return 1; }
    int k = 3;
    if (!a.k.empty() && !parse_int(a.k, 1, 1000, k)) return bad_number("--k", a.k);
    Env e = open_env(a);
    const auto corpus = read_corpus(a.pos[0]);
    struct Row {
        const Prompt* p;
        std::vector<Hit> hits;
    };
    std::vector<Row> rows;
    for (const auto& p : corpus) rows.push_back({&p, e.cat->search(query_of(e, p.text), k)});
    int fit = 0, none = 0;
    for (const auto& r : rows) (r.p->want.empty() ? none : fit) += r.p->labeled ? 1 : 0;
    std::printf("%zu prompts: %d with a skill, %d with none (labeled)\n", rows.size(), fit, none);
    std::printf("%6s %7s %9s %7s %8s %8s\n", "offer", "offers", "right@1", "wrong", "hit@k", "false");
    for (double t = 0.20; t <= 0.80001; t += 0.05) {
        int offers = 0, right1 = 0, wrong = 0, hitk = 0, falsep = 0;
        for (const auto& r : rows) {
            const bool offered = !r.hits.empty() && r.hits[0].score >= t;
            if (offered) ++offers;
            if (!r.p->labeled) continue;
            auto in_want = [&](const Hit& h) { return std::find(r.p->want.begin(), r.p->want.end(), h.card.uri.id) != r.p->want.end(); };
            if (r.p->want.empty()) {
                if (offered) ++falsep;
                continue;
            }
            if (offered) (in_want(r.hits[0]) ? right1 : wrong)++;
            for (const auto& h : r.hits)
                if (h.score >= t && in_want(h)) { ++hitk; break; }
        }
        std::printf("%6.2f %7d %9d %7d %8d %8d\n", t, offers, right1, wrong, hitk, falsep);
    }
    {   // the shipped rule: what the harness would really do with these prompts
        const size_t n = e.cat->size();
        const Confidence& c = e.policy.skills;
        int offers = 0, hit3 = 0, right1 = 0, wrong = 0, falsep = 0, opened = 0, opened_right = 0, with_skill = 0, without = 0;
        for (const auto& r : rows) {
            const bool labeled = r.p->labeled;
            if (labeled) ++(r.p->want.empty() ? without : with_skill);
            if (r.hits.empty() || !c.offers(r.hits[0], n)) continue;
            ++offers;
            auto in_want = [&](const Hit& h) { return std::find(r.p->want.begin(), r.p->want.end(), h.card.uri.id) != r.p->want.end(); };
            const bool opens = c.opens(r.hits[0], n);
            if (opens) ++opened;
            if (!labeled) continue;
            if (r.p->want.empty()) { ++falsep; continue; }
            if (in_want(r.hits[0])) { ++right1; if (opens) ++opened_right; } else ++wrong;
            for (const auto& h : r.hits)
                if (in_want(h)) { ++hit3; break; }
        }
        std::printf("\nthe policy (offer: matched>=%d, mass>=%.1f at %zu skills -> %.1f here, score>=%.2f; open: score>=%.2f):\n", c.min_matched, c.offer_mass,
                    c.calibrated_at, c.needed(n), c.offer_score, c.open_score);
        std::printf("  %d offers over %zu prompts: the right skill in the top %d for %d of %d that have one; right at 1: %d, wrong at 1: %d; "
                    "false offers: %d of %d without one; opened for the model: %d (%d right)\n",
                    offers, rows.size(), k, hit3, with_skill, right1, wrong, falsep, without, opened, opened_right);
    }
    if (!a.show.empty())
        for (const auto& r : rows) {
            std::string head = utf8_head(r.p->text, 100);
            std::replace(head.begin(), head.end(), '\n', ' ');
            std::printf("\n\"%s\"\n", head.c_str());
            if (r.p->labeled) {
                std::printf("  want:");
                for (const auto& w : r.p->want) std::printf(" %s", w.c_str());
                std::printf("%s\n", r.p->want.empty() ? " (nothing)" : "");
            }
            for (const auto& h : r.hits) std::printf("  %.2f m%d %.1f/%.1f %s  (%s)\n", h.score, h.matched, h.mass, h.ideal, h.card.uri.id.c_str(), h.why.c_str());
        }
    return 0;
}

// ---- import-daedalus: copy once, never share ----
size_t copy_tree(const fs::path& from, const fs::path& to, std::set<fs::path>& seen, int depth = 0) {
    std::error_code ec;
    if (depth > 12) return 0;
    const fs::path real = fs::canonical(from, ec);
    if (ec || !seen.insert(real).second) return 0;   // a symlink loop
    fs::create_directories(to, ec);
    size_t files = 0;
    for (const auto& ent : fs::directory_iterator(from, ec)) {
        const fs::path dest = to / ent.path().filename();
        if (fs::is_directory(ent.path(), ec)) {
            files += copy_tree(ent.path(), dest, seen, depth + 1);
        } else if (fs::is_regular_file(ent.path(), ec) && !fs::exists(dest, ec)) {
            fs::copy_file(ent.path(), dest, ec);
            if (!ec) ++files;
        }
    }
    seen.erase(real);
    return files;
}

int cmd_import(const Args& a) {
    std::string why;
    const core::Home home = core::resolve_home(a.home, a.config, &why);
    if (home.root.empty()) { std::cerr << why << "\n"; return 1; }
    const fs::path from = a.from.empty() ? core::platform_default_daedalus_home() : fs::path(a.from);
    std::error_code ec;
    if (!fs::is_directory(from, ec)) { std::cerr << "dd: no " << from << "\n"; return 1; }
    if (fs::weakly_canonical(from, ec) == fs::weakly_canonical(home.root, ec)) { std::cerr << "dd: " << from << " is this home already\n"; return 1; }
    core::ensure_home(home, &why);
    const bool all = !a.skills && !a.skins && !a.cfg;
    std::set<fs::path> seen;
    if (a.skills || all) std::cout << "skills: " << copy_tree(from / "skills", home.skills(), seen) << " files -> " << home.skills() << "\n";
    if (a.skins || all) std::cout << "skins:  " << copy_tree(from / "skins", home.skins(), seen) << " files -> " << home.skins() << "\n";
    if (a.cfg || all) {
        if (!fs::exists(home.config(), ec) && fs::exists(from / "config.yaml", ec)) {
            fs::copy_file(from / "config.yaml", home.config(), ec);
            std::cout << "config: " << (ec ? "failed: " + ec.message() : std::string("copied to ") + home.config().string()) << "\n";
        } else {
            std::cout << "config: kept " << home.config() << "\n";
        }
    }
    std::cout << "copied once; nothing is shared at runtime.\n";
    return 0;
}

// ---- learn: the ledger beside the binary, shown or (forget) edited ----
int cmd_learn(const Args& a) {
    const char* usage = "usage: dd learn status | why SKILL | report | forget SKILL | export\n";
    if (a.pos.empty()) { std::cerr << usage; return 1; }
    std::string why;
    const core::Home home = core::resolve_home(a.home, a.config, &why);
    if (home.root.empty()) { std::cerr << why << "\n"; return 1; }
    const learn::InspectPaths p{home.learn(), home.skills()};
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const std::string& sub = a.pos[0];
    learn::Result r;
    if (sub == "status") r = learn::status(p, now);
    else if (sub == "export") r = learn::export_jsonl(p, now);
    else if (sub == "why" || sub == "forget") {
        if (a.pos.size() < 2) { std::cerr << usage; return 1; }
        r = sub == "why" ? learn::why(p, a.pos[1], now) : learn::forget(p, a.pos[1], now);
    } else if (sub == "report") {
        const std::string path = learn::report_path(p);
        if (path.empty()) { std::cerr << "no report yet (the first curator pass writes " << (home.learn() / "report.md") << ")\n"; return 1; }
        std::ifstream f(path);
        std::cout << f.rdbuf();
        return 0;
    } else { std::cerr << usage; return 1; }
    (r.ok ? std::cout : std::cerr) << r.text << (r.text.empty() || r.text.back() == '\n' ? "" : "\n");
    return r.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: dd <list|view|resolve|find|skills check|replay|learn|import-daedalus> ...\n";
        return 1;
    }
    const std::string cmd = argv[1];
    Args a = parse(argc, argv);
    if (cmd == "list") return cmd_list(a);
    if (cmd == "view" || cmd == "resolve") {
        if (a.pos.empty()) { std::cerr << "usage: dd " << cmd << " <name|skill://...> [file]\n"; return 1; }
        return cmd_view(a.pos[0], a);
    }
    if (cmd == "find") return cmd_find(a);
    if (cmd == "skills") {
        if (!a.pos.empty() && a.pos[0] == "check") { a.pos.erase(a.pos.begin()); return cmd_check(a); }
        std::cerr << "usage: dd skills check [--strict]\n";
        return 1;
    }
    if (cmd == "replay") return cmd_replay(a);
    if (cmd == "learn") return cmd_learn(a);
    if (cmd == "import-daedalus") return cmd_import(a);
    std::cerr << "unknown command: " << cmd << "\n";
    return 1;
}

// dd::know — the catalogue core: words, ranking, skill sources, merging, paging.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dd/context/memory.hpp"
#include "dd/core/sha256.hpp"
#include "dd/know/bm25.hpp"
#include "dd/know/catalogue.hpp"
#include "dd/know/skills.hpp"
#include "dd/know/text.hpp"
#include "dd/know/types.hpp"

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::know;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}
static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
}
static bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }
// the term a plain word becomes in an index (stemmed, folded)
static std::string T(const char* w) {
    const auto v = terms_of(w, true);
    return v.empty() ? std::string() : v[0];
}
static bool utf8_valid(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0) return false;
        for (int k = 1; k <= n; ++k)
            if (i + static_cast<size_t>(k) >= s.size() || (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0xC0) != 0x80) return false;
        i += static_cast<size_t>(n) + 1;
    }
    return true;
}

// ---- a pod stand-in: rows by label, up or down on demand ----
struct FakeMemory : context::Memory {
    std::vector<context::Hit> rows;
    std::atomic<bool> is_up{true};
    std::atomic<int> label_calls{0}, get_calls{0};
    void add(const std::string& label, const std::string& content) {
        context::Hit h;
        h.id = static_cast<long>(rows.size()) + 100;
        h.label = label;
        h.content = content;
        rows.push_back(h);
    }
    bool configured() const override { return true; }
    bool up() const override { return is_up.load(); }
    bool probe() override { return is_up.load(); }
    std::vector<context::Hit> recall(const std::string&, int, const std::string&) override { return {}; }
    long remember(const std::string&, const std::string&) override { return -1; }
    std::vector<context::Hit> think(long, int) override { return {}; }
    std::vector<context::Hit> facts(long, int) override { return {}; }
    std::string intent(const std::string&) override { return ""; }
    int connect(const std::vector<context::Edge>&) override { return 0; }
    std::vector<context::Hit> labels(const std::string& prefix, int offset, int limit) override {
        ++label_calls;
        std::vector<context::Hit> out;
        if (!is_up) return out;
        int skip = offset;
        for (const auto& r : rows) {
            if (r.label.rfind(prefix, 0) != 0) continue;
            if (skip-- > 0) continue;
            if (static_cast<int>(out.size()) >= limit) break;
            context::Hit h;
            h.id = r.id;
            h.label = r.label;
            out.push_back(h);
        }
        return out;
    }
    std::vector<context::Hit> get(const std::vector<long>& ids) override {
        ++get_calls;
        std::vector<context::Hit> out;
        if (!is_up) return out;
        for (long id : ids)
            for (const auto& r : rows)
                if (r.id == id) out.push_back(r);
        return out;
    }
};

static void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
static std::string rev12(const std::string& text) { return core::sha256_hex(text).substr(0, 12); }

static std::string skill_md(const std::string& name, const std::string& desc, const std::string& tags = "", const std::string& body = "Steps go here.\n") {
    return "---\nname: " + name + "\ndescription: " + desc + "\n" + (tags.empty() ? "" : "tags: [" + tags + "]\n") + "---\n\n# " + name + "\n\n" + body;
}

static std::string maze_card(const std::string& name, const std::string& cat, const std::string& src, const std::string& desc, const std::string& rev,
                             size_t bytes, int parts, const std::string& tags = "") {
    return "Skill: " + name + "\nCategory: " + cat + "\nSource: " + src + "\n" + (tags.empty() ? "" : "Tags: " + tags + "\n") + "\n" + desc +
           "\n\nFull text in mazemaker: skillsrc:" + src + ":" + name + " — " + std::to_string(parts) + " part, " + std::to_string(bytes) +
           " bytes, sha256 " + rev + "\n";
}

// ================= types =================
static void test_uri() {
    auto u = Uri::parse("skill://podman-deep-dive");
    chk(u && u->kind == Kind::Skill && u->id == "podman-deep-dive" && u->path.empty() && u->page.empty(), "uri: skill");
    u = Uri::parse("skill://podman-deep-dive/references/units.md#2");
    chk(u && u->path == "references/units.md" && u->page == "2" && u->str() == "skill://podman-deep-dive/references/units.md#2", "uri: path + page round trip");
    u = Uri::parse("  mem://1290233  ");
    chk(u && u->kind == Kind::Memory && u->id == "1290233", "uri: memory, trimmed");
    chk(!Uri::parse("mem://abc"), "uri: a memory id is a number");
    chk(Uri::parse("rule://podman-not-docker") && Uri::parse("tool://mazemaker_prune") && Uri::parse("sess://dd-1"), "uri: rule, tool, session");
    chk(!Uri::parse("skill://x/../../etc/passwd"), "uri: no ..");
    chk(!Uri::parse("skill://x//etc/passwd") || Uri::parse("skill://x//etc/passwd")->path.front() != '/', "uri: no absolute path");
    chk(!Uri::parse("https://example.com/x") && !Uri::parse("/etc/passwd") && !Uri::parse("skill://") && !Uri::parse("nope://x"), "uri: not ours");
    chk(Uri::parse("skill://x#all") && Uri::parse("skill://x#all")->page == "all", "uri: #all");
    chk(!Uri::parse("skill://x#two"), "uri: a page is a number or all");
    chk(Uri::parse("skill://ascii-art@builtin") && Uri::parse("skill://ascii-art@builtin")->id == "ascii-art@builtin", "uri: name@source");
}

static void test_paginate() {
    std::string text;
    for (int i = 0; i < 40; ++i) text += "paragraph " + std::to_string(i) + " " + std::string(180, 'x') + "\n\n";
    Uri u;
    u.id = "big";
    auto p1 = paginate(u, text, 2000);
    chk(p1 && p1->page == 1 && p1->pages > 3 && p1->text.size() <= 2000 && p1->more == "skill://big#2", "paginate: page 1 names page 2");
    chk(p1 && p1->text.size() > 1000 && p1->text.substr(p1->text.size() - 2) == "\n\n", "paginate: cut between paragraphs");
    Uri u2 = u;
    u2.page = "2";
    auto p2 = paginate(u2, text, 2000);
    chk(p2 && p2->page == 2 && p2->text.rfind("paragraph", 0) == 0, "paginate: page 2 starts on a paragraph");
    std::string joined;
    for (int k = 1; k <= p1->pages; ++k) {
        Uri uk = u;
        uk.page = std::to_string(k);
        auto pk = paginate(uk, text, 2000);
        chk(pk.has_value(), "paginate: page " + std::to_string(k));
        if (pk) joined += pk->text;
    }
    chk(joined == text, "paginate: the pages are the text, in order, none lost");
    Uri ul = u;
    ul.page = std::to_string(p1->pages);
    auto last = paginate(ul, text, 2000);
    chk(last && last->more.empty(), "paginate: the last page has no more");
    Uri ux = u;
    ux.page = "99";
    chk(!paginate(ux, text, 2000), "paginate: no such page");
    Uri ua = u;
    ua.page = "all";
    auto all = paginate(ua, text, 2000, 100000);
    chk(all && all->text == text && all->more.empty(), "paginate: #all is the whole text under the cap");
    auto capped = paginate(ua, text, 2000, 5000);
    chk(capped && capped->text.size() <= 5000 && !capped->more.empty(), "paginate: #all stops at the cap and names where to go on");
    auto small = paginate(u, "short", 2000);
    chk(small && small->pages == 1 && small->text == "short" && small->more.empty(), "paginate: a short text is one page");
    std::string utf;
    for (int i = 0; i < 300; ++i) utf += "äöü€";
    bool all_valid = true;
    for (int k = 1; k <= 40; ++k) {
        Uri uk = u;
        uk.page = std::to_string(k);
        auto pk = paginate(uk, utf, 100);
        if (!pk) break;
        all_valid = all_valid && utf8_valid(pk->text);
    }
    chk(all_valid, "paginate: never inside a UTF-8 sequence");
}

// ================= words =================
static void test_terms() {
    auto t = terms_of("Wie kann ich die Datenbank für Übungen prüfen?");
    chk(has(t, T("datenbank")) && !has(t, "fuer") && !has(t, "die") && has(t, T("uebungen")) && has(t, T("pruefen")), "terms: German folded, stopwords out: " + join(t));
    t = terms_of("readFile HTTPServer parse_args");
    chk(has(t, T("read")) && has(t, T("file")) && has(t, T("http")) && has(t, T("server")) && has(t, T("parse")) && has(t, T("args")), "terms: camelCase and snake_case split: " + join(t));
    t = terms_of("strata 0.1.45 failed with ENOENT and E0433");
    chk(has(t, "0.1.45") && has(t, "enoent") && has(t, "e0433") && has(t, "strata"), "terms: version and error codes kept whole: " + join(t));
    t = terms_of("the and of to a in is it");
    chk(t.empty(), "terms: English stopwords out: " + join(t));
    t = terms_of("the and", true);
    chk(t.size() == 2, "terms: keep_stop keeps them");
    t = terms_of("╭─dev@box ❯ 🔴 ⇶ done");
    chk(has(t, "dev") && has(t, "box") && has(t, "done") && !has(t, "─"), "terms: box drawing and emoji break words: " + join(t));
    t = terms_of("5 slots 8080 400");
    chk(!has(t, "5") && has(t, T("slots")) && has(t, "8080") && has(t, "400"), "terms: lone digits out, numbers of 3+ digits in: " + join(t));
    t = terms_of("naïve café ÉCOLE");
    chk(has(t, T("naive")) && has(t, T("cafe")) && has(t, T("ecole")), "terms: accents folded, an accented capital does not split its word: " + join(t));
    chk(is_stopword("fuer") && is_stopword("the") && !is_stopword("podman"), "terms: is_stopword");
    // plural, -ing, -ed, final e: one stem on both sides
    auto one = [](const char* w) { auto v = terms_of(w); return v.empty() ? std::string() : v[0]; };
    chk(one("containers") == one("container") && one("Containers") == one("container"), "terms: containers = container");
    chk(one("deployed") == one("deploy") && one("deploying") == one("deploy"), "terms: deployed = deploying = deploy");
    chk(one("caches") == one("cache") && one("changed") == one("change"), "terms: caches = cache, changed = change");
    chk(one("queries") == one("query") && one("settings") == one("setting"), "terms: queries = query");
    chk(one("status") == "status" && one("access") == "access" && one("process") == "process", "terms: us/ss/is endings are not plurals");
    chk(one("tui") == "tui" && one("dae") == "dae" && one("kv") == "kv", "terms: short words stay");
    t = terms_of("What about BTQuant lately");
    chk(has(t, T("btquant")) && has(t, T("bt")) && has(t, T("quant")), "terms: BTQuant is btquant and its parts: " + join(t));
    t = terms_of("readFile");
    chk(has(t, T("readfile")) && has(t, T("read")) && has(t, T("file")), "terms: a split identifier is also kept whole: " + join(t));
}

static void test_distiller() {
    Distiller d;
    // pasted terminal and code are not a question
    const std::string pasted =
        "why does this fail?\n"
        "╭─dev@box in ~ via v3.14.7 took 0s [🔴]\n"
        "╰─λ sudo ufw status\n"
        "[sudo] Passwort für alca:\n"
        "{\"models\":[{\"name\":\"x\"}]}\n"
        "```\nint main() { return 0; }\n```\n"
        "the quadlet unit for podman never starts";
    const std::string prose = Distiller::prose(pasted, 1200);
    chk(prose.find("why does this fail") != std::string::npos && prose.find("quadlet") != std::string::npos, "prose: the question stays: " + prose);
    chk(prose.find("ufw") == std::string::npos && prose.find("int main") == std::string::npos && prose.find("models") == std::string::npos,
        "prose: terminal lines and code fences go: " + prose);
    chk(Distiller::prose("see <pasted_content id=\"1\">lots of log lines here</pasted_content> and fix it", 1200).find("lots") == std::string::npos, "prose: paste tags and what is in them go");
    chk(Distiller::prose("read https://example.com/a/b?c=d then fix", 1200).find("example") == std::string::npos, "prose: urls go");
    const std::string long_msg(5000, 'x');
    chk(Distiller::prose(long_msg + " THE ASK", 1200).find("THE ASK") != std::string::npos && Distiller::prose(long_msg + " THE ASK", 1200).size() <= 1210,
        "prose: capped, and the end (the ask) survives");

    // paths name the project and the file, never the generic folders on the way
    Query q = d.text("build /home/dev/projects/quark-messenger/archives/tor-rework/src/main.cpp now");
    std::vector<std::string> terms;
    for (const auto& t : q.terms) terms.push_back(t.text);
    chk(has(terms, "quark") && has(terms, "messenger") && has(terms, "tor") && has(terms, "rework") && has(terms, "cpp"), "distill: project folders kept: " + join(terms));
    chk(!has(terms, "home") && !has(terms, "src") && !has(terms, "projects"), "distill: generic folders dropped: " + join(terms));

    // a quoted identifier is said on purpose
    q = d.text("what about `fast_layer_tokens` here");
    float w_quoted = 0, w_plain = 0;
    for (const auto& t : q.terms) {
        if (t.text == "fast") w_quoted = t.weight;
        if (t.text == "layer") w_plain = t.weight;
    }
    chk(w_quoted > 1.0f && w_quoted == w_plain, "distill: a quoted identifier weighs more");

    // too many terms: the rarest survive when the index can say which are rare
    std::string many;
    for (int i = 0; i < 40; ++i) many += "word" + std::to_string(i) + "x ";
    IdfFn idf = [](const std::string& t) { return t == "word7x" || t == "word9x" ? 5.0f : 1.0f; };
    q = d.text(many, idf);
    terms.clear();
    for (const auto& t : q.terms) terms.push_back(t.text);
    chk(q.terms.size() == 12 && has(terms, "word7x") && has(terms, "word9x"), "distill: 12 terms, the rare ones among them");
    // an unknown term (idf 0) cannot match anything: not worth a slot
    q = d.text("podman zzzunknownzzz", [](const std::string& t) { return t == "podman" ? 2.0f : 0.0f; });
    chk(q.terms.size() == 1 && q.terms[0].text == "podman", "distill: terms the index lacks are dropped");
    q = d.terms({{"mazemaker_recall failed", 1.0f}, {"Reading model role settings", 1.5f}});
    terms.clear();
    for (const auto& t : q.terms) terms.push_back(t.text);
    chk(has(terms, T("mazemaker")) && has(terms, T("recall")) && has(terms, T("reading")) && has(terms, T("settings")), "distill: free pieces (tool args, an intent) become terms: " + join(terms));
}

// ================= ranking =================
static Card card(const std::string& name, const std::string& group, const std::string& gist, std::vector<std::string> tags = {}) {
    Card c;
    c.uri.id = name;
    c.title = name;
    c.group = group;
    c.gist = gist;
    c.tags = std::move(tags);
    c.body_bytes = 3000;
    return c;
}

static std::vector<Card> small_catalogue() {
    return {card("podman-quadlet-ops", "devops", "Operate rootless podman containers and quadlet units: restart, logs, drop-ins", {"podman", "quadlet", "systemd"}),
            card("podman-deep-dive", "devops", "Debug podman networking, storage and symlink mess in containers", {"podman", "containers"}),
            card("git-branch-workflow", "software-development", "Branch, rebase and merge safely; never force-push to main", {"git"}),
            card("songwriting", "creative", "Write song lyrics and structure verses and choruses", {"music"}),
            card("jack-in-a-box", "devops",
                 "A long description that mentions the harness, the memory, the engine, the pod, the tui, the router, the build, the test, the deploy, "
                 "the site, the container, the model, the context, the tape and many other common words of the everyday work",
                 {})};
}

static void test_bm25() {
    Bm25f idx(small_catalogue());
    Distiller d;
    auto top = [&](const std::string& q, int k = 3) { return idx.search(d.text(q, [&](const std::string& t) { return idx.idf(t); }), k); };

    auto hits = top("podman quadlet unit never starts");
    chk(!hits.empty() && hits[0].card.title == "podman-quadlet-ops", "bm25: the quadlet question finds the quadlet skill");
    chk(hits.size() >= 2 && hits[1].card.title == "podman-deep-dive", "bm25: its sibling is next");
    chk(!hits.empty() && hits[0].score > 0.3 && hits[0].score <= 1.0, "bm25: a real match scores well inside 0..1");
    chk(!hits.empty() && hits[0].why.find("podman") != std::string::npos, "bm25: why names the matched terms: " + (hits.empty() ? "" : hits[0].why));
    chk(!hits.empty() && hits[0].margin >= 0, "bm25: margin is the lead on the next hit");

    // the name is the strongest field: asking for a skill by name finds it at the top, near 1
    hits = top("git-branch-workflow");
    chk(!hits.empty() && hits[0].card.title == "git-branch-workflow" && hits[0].score > 0.55, "bm25: asking by name scores high");

    // length must not buy a score: a long message with the same ask stays in range and finds the same skill
    hits = top("so basically i was looking at it for a while and anyway the podman quadlet unit still does not come up after all of that");
    chk(!hits.empty() && hits[0].card.title == "podman-quadlet-ops" && hits[0].score <= 1.0 && hits[0].score > 0.2,
        "bm25: filler around the ask does not move the answer: " + (hits.empty() ? "" : hits[0].card.title));

    // no overlap: nothing, never a false best
    chk(top("zzzz qqqq xxxx").empty(), "bm25: no shared term, no hit");
    chk(idx.search(Query{}, 3).empty(), "bm25: an empty query finds nothing");
    chk(Bm25f(std::vector<Card>{}).search(d.text("podman"), 3).empty(), "bm25: an empty index finds nothing");

    // a prior demotes
    auto cards = small_catalogue();
    cards[0].prior = 0.5;
    Bm25f demoted(cards);
    const auto base = idx.search(d.text("podman quadlet"), 5), low = demoted.search(d.text("podman quadlet"), 5);
    double s_base = 0, s_low = 0;
    for (const auto& h : base) if (h.card.title == "podman-quadlet-ops") s_base = h.score;
    for (const auto& h : low) if (h.card.title == "podman-quadlet-ops") s_low = h.score;
    chk(s_base > 0 && s_low > 0 && s_low < s_base * 0.6, "bm25: a prior below 1 lowers a card's score");

    // k and ordering
    hits = top("podman", 1);
    chk(hits.size() == 1, "bm25: k limits the hits");
    hits = top("podman", 10);
    bool sorted = true;
    for (size_t i = 1; i < hits.size(); ++i) sorted = sorted && hits[i - 1].score >= hits[i].score;
    chk(sorted, "bm25: hits are sorted by score");

    // many calls leave no residue in the shared scratch space
    const auto first = top("podman quadlet");
    for (int i = 0; i < 50; ++i) top("git branch " + std::to_string(i));
    const auto again = top("podman quadlet");
    chk(!first.empty() && !again.empty() && first[0].score == again[0].score && first[0].card.title == again[0].card.title, "bm25: repeated searches are stable");

    // speed: a 700-card catalogue answers in about a millisecond
    std::vector<Card> big;
    for (int i = 0; i < 700; ++i)
        big.push_back(card("skill-" + std::to_string(i) + "-" + (i % 7 == 0 ? "podman" : "thing"), i % 2 ? "devops" : "creative",
                           "does the number " + std::to_string(i) + " job with words like deploy build logs rootless and restart", {"tag" + std::to_string(i % 13)}));
    Bm25f bigidx(big);
    const auto t0 = std::chrono::steady_clock::now();
    size_t found = 0;
    for (int i = 0; i < 200; ++i) found += bigidx.search(d.text("podman rootless restart logs deploy"), 5).size();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 200.0;
    chk(found > 0 && ms < 2.0, "bm25: a search over 700 cards is fast (" + std::to_string(ms) + " ms)");
}

// ================= sources and the merged catalogue =================
static void test_disk_skills(const fs::path& tmp) {
    const fs::path root = tmp / "disk" / "skills";
    write(root / "devops" / "podman-quadlet-ops" / "SKILL.md", skill_md("podman-quadlet-ops", "Operate rootless podman and quadlet units", "podman, quadlet"));
    write(root / "devops" / "podman-quadlet-ops" / "references" / "units.md", "# units\nthe unit files\n");
    write(root / "devops" / "podman-quadlet-ops" / "references" / "deep" / "more.md", "more\n");
    write(root / "software-development" / "git-branch-workflow" / "SKILL.md", skill_md("git-branch-workflow", "Branch and merge safely"));
    write(root / "loose-skill" / "SKILL.md", skill_md("loose-skill", "Lives at the top"));
    write(tmp / "secret.txt", "do not leak");

    DiskSkills disk({root});
    auto cards = disk.list();
    chk(cards.size() == 3, "disk: three skills found, nested or not (" + std::to_string(cards.size()) + ")");
    const Card* podman = nullptr;
    for (const auto& c : cards)
        if (c.uri.id == "podman-quadlet-ops") podman = &c;
    chk(podman && podman->group == "devops" && podman->has_body && podman->body_bytes > 50 && podman->rev.size() == 12, "disk: group, size and a 12-char revision");
    chk(podman && podman->tags.size() == 2 && podman->tags[0] == "podman", "disk: tags from the frontmatter");
    chk(podman && podman->source.rfind("disk:", 0) == 0, "disk: the source says where it came from");
    chk(podman && disk.body(*podman) && disk.body(*podman)->find("podman-quadlet-ops") != std::string::npos, "disk: body reads the SKILL.md");
    auto ref = podman ? disk.file(*podman, "references/units.md") : std::nullopt;
    chk(ref && ref->find("unit files") != std::string::npos, "disk: a file inside the skill's folder");
    auto dirlist = podman ? disk.file(*podman, "references") : std::nullopt;
    chk(dirlist && dirlist->find("units.md") != std::string::npos && dirlist->find("deep/") != std::string::npos, "disk: a folder lists its entries");
    chk(podman && !disk.file(*podman, "../../../secret.txt"), "disk: no way out of the skill's folder with ..");
    chk(podman && !disk.file(*podman, "/etc/passwd"), "disk: no absolute path");
    // a symlink inside the skill that points out is refused too
    std::error_code ec;
    fs::create_symlink(tmp / "secret.txt", root / "devops" / "podman-quadlet-ops" / "references" / "leak.txt", ec);
    chk(podman && !disk.file(*podman, "references/leak.txt"), "disk: a symlink that leaves the folder is refused");
    chk(!disk.list().empty() && disk.ok(), "disk: listing again works and is ok");
    DiskSkills none({tmp / "nowhere"});
    chk(none.list().empty() && none.ok(), "disk: a missing root is an empty source, not an error");
}

struct World {
    fs::path root;
    std::shared_ptr<FakeMemory> mem = std::make_shared<FakeMemory>();
    std::string podman_md, shared_md;
};

static World make_world(const fs::path& tmp) {
    World w;
    w.root = tmp / "world" / "skills";
    w.podman_md = skill_md("podman-quadlet-ops", "Operate rootless podman and quadlet units", "podman, quadlet");
    write(w.root / "devops" / "podman-quadlet-ops" / "SKILL.md", w.podman_md);
    write(w.root / "devops" / "podman-quadlet-ops" / "references" / "units.md", "the unit files\n");
    // maze: the same skill (identical sha: collapses), maze-only skills, a differing duplicate, a bodyless card with an identical copy that has a body
    auto& m = *w.mem;
    m.add("skill:local:podman-quadlet-ops", maze_card("podman-quadlet-ops", "devops", "local", "Operate rootless podman and quadlet units", rev12(w.podman_md), w.podman_md.size(), 1));
    m.add("skillsrc:local:podman-quadlet-ops#000", w.podman_md);
    m.add("skill:local:maze-only", maze_card("maze-only", "debugging", "local", "Debug a flaky pod with a flashlight", "aaaaaaaaaaaa", 21, 1, "pods, torch"));
    m.add("skillsrc:local:maze-only#000", "Maze-only body text.\n");
    m.add("skill:builtin:maze-only", maze_card("maze-only", "debugging", "builtin", "Debug a flaky pod with a flashlight", "aaaaaaaaaaaa", 21, 1));
    m.add("skillsrc:builtin:maze-only#000", "Maze-only body text.\n");
    m.add("skill:local:ascii-art", maze_card("ascii-art", "creative", "local", "Draw ascii art, local flavour", "111111111111", 30, 1));
    m.add("skillsrc:local:ascii-art#000", "local ascii art\n");
    m.add("skill:builtin:ascii-art", maze_card("ascii-art", "creative", "builtin", "Draw ascii art, builtin flavour", "222222222222", 31, 1));
    m.add("skillsrc:builtin:ascii-art#000", "builtin ascii art\n");
    m.add("skill:local:bodyless", maze_card("bodyless", "misc", "local", "A card whose body the maze lost", "333333333333", 0, 0));
    m.add("skill:builtin:bodyless", maze_card("bodyless", "misc", "builtin", "A card whose body the maze lost", "333333333333", 18, 1));
    m.add("skillsrc:builtin:bodyless#000", "borrowed body\n");
    m.add("skill:local:multi", maze_card("multi", "misc", "local", "A skill in two parts", "444444444444", 11, 2));
    m.add("skillsrc:local:multi#001", "second part\n");
    m.add("skillsrc:local:multi#000", "first part, ");
    // not skills: a note filed under skill:, an AFE fragment
    m.add("skill:fix-neural-banner", "fact: A skill `fix-neural-banner` was created on 2026-10-16");
    m.add("skill:builtin:minecraft-modpack-server::afe::A3f90f65d93b6", "a fragment cut out of a transcript");
    m.add("skill:builtin:a name with spaces", "not a name");
    return w;
}

static void test_catalogue_merge(const fs::path& tmp) {
    World w = make_world(tmp);
    auto disk = std::make_shared<DiskSkills>(std::vector<fs::path>{w.root});
    auto maze = std::make_shared<MazeSkills>(w.mem);
    SkillCatalogue cat({disk, maze});
    chk(!cat.ready() && cat.size() == 0 && cat.generation() == 0, "catalogue: empty before the first refresh");
    cat.refresh();
    chk(cat.ready() && cat.healthy() && cat.generation() == 1, "catalogue: ready, healthy, generation 1");

    auto c = cat.census();
    // podman (disk+maze identical), maze-only, ascii-art + ascii-art@builtin, bodyless, multi = 6 cards
    chk(cat.size() == 6, "catalogue: six skills (" + std::to_string(cat.size()) + ")");
    chk(c.collapsed == 2, "catalogue: identical copies collapse (maze-only local+builtin, bodyless local+builtin): " + std::to_string(c.collapsed));
    chk(c.superseded == 1, "catalogue: the maze's snapshot of a disk skill yields to the disk copy (podman): " + std::to_string(c.superseded));
    chk(c.aliased == 1, "catalogue: one differing copy kept as an alias");
    chk(c.skipped == 3, "catalogue: a note, an AFE fragment and a bad name are not skills (" + std::to_string(c.skipped) + ")");
    chk(cat.find("fix-neural-banner") == std::nullopt, "catalogue: the note is not a skill");

    auto podman = cat.find("podman-quadlet-ops");
    chk(podman && podman->source.rfind("disk:", 0) == 0, "catalogue: disk comes first in precedence: " + (podman ? podman->source : ""));
    auto a1 = cat.find("ascii-art"), a2 = cat.find("ascii-art@builtin");
    chk(a1 && a1->source == "maze:local" && a2 && a2->source == "maze:builtin", "catalogue: the differing copy is reachable as ascii-art@builtin");

    Uri u;
    u.id = "ascii-art@builtin";
    auto b = cat.open(u);
    chk(b && b->text == "builtin ascii art\n", "catalogue: the alias opens its own body");
    u.id = "ascii-art";
    chk(cat.open(u) && cat.open(u)->text == "local ascii art\n", "catalogue: the bare name opens the winner's body");
    u.id = "podman-quadlet-ops";
    {
        const auto fm = cat.open(u);
        chk(fm && fm->text.rfind("---", 0) != 0 && fm->text.find("name: podman-quadlet-ops") == std::string::npos && fm->text.find("# podman-quadlet-ops") != std::string::npos,
            "catalogue: a SKILL.md opens without its frontmatter");
    }
    u.id = "ascii-art";

    u.id = "bodyless";
    auto bl = cat.find("bodyless");
    chk(bl && bl->has_body && bl->body_bytes == 18, "catalogue: a bodyless card borrows an identical copy's body");
    chk(cat.open(u) && cat.open(u)->text == "borrowed body\n", "catalogue: ... and opens it");

    u.id = "multi";
    auto mu = cat.open(u);
    chk(mu && mu->text == "first part, second part\n", "catalogue: maze parts are joined in label order");

    u.id = "podman-quadlet-ops";
    u.path = "references/units.md";
    auto ref = cat.open(u);
    chk(ref && ref->text == "the unit files\n", "catalogue: skill://name/references/x.md reads the skill's own file from disk");
    u.path = "../../etc/passwd";
    chk(!cat.open(u), "catalogue: no way out");
    u.path.clear();
    u.id = "no-such-skill";
    chk(!cat.open(u), "catalogue: an unknown skill does not open");
    u.id = "podman-quadlet-ops";
    u.kind = Kind::Memory;
    chk(!cat.open(u), "catalogue: only skills open here");

    // search sees the merged set, including maze-only skills and tags
    Distiller d;
    auto hits = cat.search(d.text("flashlight pods torch", [&](const std::string& t) { return cat.idf(t); }), 3);
    chk(!hits.empty() && hits[0].card.uri.id == "maze-only", "catalogue: a maze-only skill is found by its description and tags");
    chk(cat.map().rfind("skills 6:", 0) == 0 && cat.map().find("devops 1") != std::string::npos, "catalogue: map names the groups: " + cat.map());

    // nothing changed: same generation
    cat.refresh();
    chk(cat.generation() == 1, "catalogue: a refresh that finds the same cards keeps the generation");
    w.mem->add("skill:local:brand-new", maze_card("brand-new", "misc", "local", "Just authored in the maze", "555555555555", 9, 1));
    w.mem->add("skillsrc:local:brand-new#000", "new body\n");
    cat.refresh();
    chk(cat.generation() == 2 && cat.find("brand-new"), "catalogue: a new card bumps the generation and is found");

    // disabled names are hidden
    SkillCatalogue::Options o;
    o.disabled = {"maze-only"};
    SkillCatalogue hidden({disk, maze}, o);
    hidden.refresh();
    chk(!hidden.find("maze-only") && hidden.find("multi"), "catalogue: skills.disabled hides a name");
    // priors
    SkillCatalogue::Options po;
    po.priors = {{"podman-quadlet-ops", 0.1}};
    SkillCatalogue priored({disk, maze}, po);
    priored.refresh();
    auto pc = priored.find("podman-quadlet-ops");
    chk(pc && pc->prior == 0.1, "catalogue: a prior from the options reaches the card");
}

static void test_catalogue_pod_down_and_up(const fs::path& tmp) {
    World w = make_world(tmp);
    // the pod is down at launch, the disk tree is there: the catalogue serves from disk at once
    w.mem->is_up = false;
    auto disk = std::make_shared<DiskSkills>(std::vector<fs::path>{w.root});
    auto maze = std::make_shared<MazeSkills>(w.mem);
    SkillCatalogue cat({disk, maze});
    cat.refresh();
    chk(cat.ready() && cat.size() == 1 && !cat.healthy(), "pod down: the disk skill is served, the catalogue says it is not healthy");
    const uint64_t g1 = cat.generation();
    // the pod comes back: a later refresh brings the maze in and bumps the generation
    w.mem->is_up = true;
    cat.refresh();
    chk(cat.healthy() && cat.size() == 6 && cat.generation() > g1, "pod up later: the maze's skills arrive, generation bumps");
    // a blip: the pod goes down again; what it gave stays
    w.mem->is_up = false;
    cat.refresh();
    chk(cat.size() == 6 && !cat.healthy(), "pod blip: the catalogue keeps what the maze last gave");

    // no disk and the pod down: unready, not an empty "ready"
    auto only_maze = std::make_shared<MazeSkills>(w.mem);
    SkillCatalogue cold({only_maze});
    cold.refresh();
    chk(!cold.ready() && cold.size() == 0, "pod down, nothing on disk: not ready");
    w.mem->is_up = true;
    cold.refresh();
    chk(cold.ready() && cold.size() > 0, "... and ready once the pod answers");

    // the refresher picks a late pod up on its own
    w.mem->is_up = false;
    auto cat2 = std::make_shared<SkillCatalogue>(std::vector<std::shared_ptr<SkillSource>>{std::make_shared<MazeSkills>(w.mem)});
    Refresher r({cat2}, std::chrono::seconds(60), std::chrono::milliseconds(80));
    r.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    chk(!cat2->ready(), "refresher: nothing while the pod is down");
    w.mem->is_up = true;
    for (int i = 0; i < 60 && !cat2->ready(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    chk(cat2->ready() && cat2->size() > 0, "refresher: retries soon and picks the pod up when it returns");
    r.stop();
}

// a catalogue says when its first snapshot is there: the caller sleeps on it, it does not poll
static void test_wait_ready(const fs::path& tmp) {
    using Clock = std::chrono::steady_clock;
    auto ms_since = [](Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
    World w = make_world(tmp);
    w.mem->is_up = false;
    auto cat = std::make_shared<SkillCatalogue>(std::vector<std::shared_ptr<SkillSource>>{std::make_shared<MazeSkills>(w.mem)});
    auto t0 = Clock::now();
    chk(!cat->wait_ready(std::chrono::milliseconds(60)) && ms_since(t0) >= 55, "wait_ready: a catalogue that is not ready times out");
    Refresher r({cat}, std::chrono::seconds(60), std::chrono::milliseconds(40));
    r.start();
    std::thread pod_returns([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        w.mem->is_up = true;
    });
    t0 = Clock::now();
    const bool ok = cat->wait_ready(std::chrono::seconds(10));
    chk(ok && ms_since(t0) < 1500 && cat->size() > 0, "wait_ready: returns when the first snapshot lands (" + std::to_string(ms_since(t0)) + " ms)");
    t0 = Clock::now();
    chk(cat->wait_ready(std::chrono::seconds(10)) && ms_since(t0) < 25, "wait_ready: a ready catalogue returns at once");
    pod_returns.join();
    r.stop();
}

static void test_open_pages(const fs::path& tmp) {
    const fs::path root = tmp / "pages" / "skills";
    std::string body;
    for (int i = 0; i < 60; ++i) body += "## Step " + std::to_string(i) + "\n" + std::string(150, 'y') + "\n\n";
    write(root / "big-skill" / "SKILL.md", skill_md("big-skill", "A long procedure", "", body));
    SkillCatalogue::Options o;
    o.page_bytes = 3000;
    SkillCatalogue cat({std::make_shared<DiskSkills>(std::vector<fs::path>{root})}, o);
    cat.refresh();
    Uri u;
    u.id = "big-skill";
    auto p1 = cat.open(u);
    chk(p1 && p1->pages > 2 && p1->page == 1 && p1->text.size() <= 3000 && p1->more == "skill://big-skill#2", "pages: page 1 of a long skill names page 2");
    u.page = "2";
    auto p2 = cat.open(u);
    chk(p2 && p2->page == 2 && p2->more == "skill://big-skill#3", "pages: skill://x#2");
    u.page = "all";
    auto all = cat.open(u);
    chk(all && all->text.size() > p1->text.size(), "pages: skill://x#all gives more at once");
}

// ================= the replay: real failure modes, synthetic catalogue =================
struct Case {
    const char* prompt;
    std::vector<std::string> want;   // any of these in the top 3; empty: nothing should be offered
};

static std::vector<Card> replay_catalogue() {
    return {
        card("podman-quadlet-ops", "devops", "Operate rootless podman containers and quadlet systemd units: restart, logs, drop-ins", {"podman", "quadlet", "systemd", "container"}),
        card("podman-deep-dive", "devops", "Debug podman networking, storage and symlink mess in containers", {"podman", "containers", "symlink"}),
        card("never-touch-live-pods-or-postgres", "devops", "Rule: never restart, stop or rebuild live pods or the postgres database while debugging", {"pods", "postgres", "live"}),
        card("site-deploy-ops", "devops", "Deploy the marketing site: build, publish, verify the pages", {"deploy", "site", "pages"}),
        card("tui-verbose-animation", "software-development", "Make the terminal ui panels dynamic: reasoning, tool and answer boxes animate while streaming", {"tui", "animation", "panels"}),
        card("clean-config-setup", "software-development", "Create a clean sample config and place it properly after install and setup", {"config", "install", "setup"}),
        card("trading-bot-maintenance", "trading", "Maintain the BTQuant trading bot: backtests, broker adapter, mcp server for strategies", {"btquant", "trading", "mcp"}),
        card("messenger-crypto-audit", "security", "Audit the messenger project: tor rework, key handling, archives", {"messenger", "tor", "crypto"}),
        card("llm-server-flags", "llm-serving", "Explain llama server flags: slots, context size, kv cache, batch and parallel decoding", {"llama", "slots", "kv", "context"}),
        card("vram-budget-audit", "llm-serving", "Audit gpu vram and ram budgets of the model server and the embedder at idle", {"vram", "gpu", "ram", "embedder"}),
        card("memory-first", "mazemaker", "The memory protocol: recall first, save decisions and bugs to the graph after each turn", {"mazemaker", "memory", "recall"}),
        card("dream-engine-ops", "mazemaker", "Operate the dream engine: nightly runs, phases, scheduling and limits", {"dream", "dae", "nightly"}),
        card("songwriting", "creative", "Write song lyrics and structure verses and choruses", {"music", "lyrics"}),
        card("git-branch-workflow", "software-development", "Branch, rebase and merge safely; never force-push main; short commits", {"git", "branch", "rebase"}),
        card("jack-in-a-box", "devops",
             "Everything about the harness: the memory, the engine, the pod, the tui, the router, the build, the test, the deploy, the site, the container, "
             "the model, the context, the tape, the config, the install, the setup and many other common words of everyday work",
             {}),
        card("single-source-of-truth", "software-development",
             "Keep one source of truth for the config, the skills, the docs and the state so that nothing is copied or shared twice between the tools", {}),
    };
}

static const std::vector<Case>& replay_cases() {
    static const std::vector<Case> cases = {
        // short prompts that must reach their skill
        {"deploy the site", {"site-deploy-ops"}},
        {"podman symlink container mess needs to stop", {"podman-deep-dive", "podman-quadlet-ops"}},
        {"what did we fix in BTQuant lately?", {"trading-bot-maintenance"}},
        {"clean sample config, properly placed after install", {"clean-config-setup"}},
        {"the tui is not dynamic, i want reasoning and tool boxes animated", {"tui-verbose-animation"}},
        {"restart the quadlet unit for the pod", {"podman-quadlet-ops"}},
        {"save all of this to mazemaker", {"memory-first"}},
        {"less vram at idle for the embedder", {"vram-budget-audit"}},
        {"stop dae, it should run once a week at night", {"dream-engine-ops"}},
        {"how many slots does the llama server flag give with this context size", {"llm-server-flags"}},
        {"write a song about the sea", {"songwriting"}},
        {"rebase the branch and merge it", {"git-branch-workflow"}},
        // German and mixed
        {"die podman container sind ein chaos, symlinks überall", {"podman-deep-dive", "podman-quadlet-ops"}},
        {"deploye bitte die site, build und publish", {"site-deploy-ops"}},
        {"wie viel vram braucht der embedder im idle", {"vram-budget-audit"}},
        // the project is only named inside a path
        {"ok after now we build /home/dev/projects/messenger/archives/tor-rework/ and push it", {"messenger-crypto-audit"}},
        {"based on last work whats missing here? /home/dev/projects/llama-server/", {"llm-server-flags"}},
        // a long message: the ask is buried in filler and pasted terminal
        {"so basically i was looking at all of this for a while and it is not what i expected, but fine\n"
         "╭─dev@box in ~ via v3.14.7 took 0s\n╰─λ podman ps\nCONTAINER ID  IMAGE  COMMAND  CREATED  STATUS  PORTS  NAMES\n"
         "anyway the podman symlink container mess is back again and i need it gone",
         {"podman-deep-dive", "podman-quadlet-ops"}},
        // nothing in the catalogue is about these: no offer, and never the hub
        {"what happend?", {}},
        {"ok", {}},
        {"who are u?", {}},
        {"thanks, that was it", {}},
        {"why is it slow", {}},
        {"reply with just: wired", {}},
        {"read src/tools/registry.cpp and name three tools", {}},
        {"json error fixed?", {}},
        {"what should we do next?", {}},
        {"wieso geht das nicht", {}},
        {"i think that is enough for today, the rest can wait until tomorrow morning", {}},
    };
    return cases;
}

// The rule's shape (two terms matched, a fair share of the question) at a scale this 16-card catalogue can reach;
// the shipped thresholds are calibrated on the real catalogue (dd replay) and live in Policy.
static bool offered(const Hit& h) { return h.matched >= 2 && h.score >= 0.25; }

static void test_replay() {
    Bm25f idx(replay_catalogue());
    Distiller d;
    int fit = 0, found = 0, none = 0, false_offers = 0;
    std::string misses, noise;
    for (const auto& c : replay_cases()) {
        const auto hits = idx.search(d.text(c.prompt, [&](const std::string& t) { return idx.idf(t); }), 3);
        if (c.want.empty()) {
            ++none;
            if (!hits.empty() && offered(hits[0])) {
                ++false_offers;
                noise += std::string("\n   \"") + c.prompt + "\" -> " + hits[0].card.title;
            }
            continue;
        }
        ++fit;
        bool ok = false;
        for (const auto& h : hits)
            for (const auto& w : c.want) ok = ok || (h.card.title == w && offered(h));
        if (ok) ++found;
        else misses += std::string("\n   \"") + c.prompt + "\"";
    }
    std::printf("replay: %d/%d prompts with a skill found, %d/%d without one offered nothing%s%s\n", found, fit, none - false_offers, none,
                misses.empty() ? "" : "\n  missed:", misses.c_str());
    chk(static_cast<double>(found) / fit >= 0.80, "replay: top-3 recall >= 80% on prompts that have a skill; missed:" + misses);
    chk(static_cast<double>(false_offers) / none <= 0.10, "replay: false offers <= 10% on prompts that have none; offered:" + noise);
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / ("dd_know_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    test_uri();
    test_paginate();
    test_terms();
    test_distiller();
    test_bm25();
    test_disk_skills(tmp);
    test_catalogue_merge(tmp);
    test_catalogue_pod_down_and_up(tmp);
    test_wait_ready(tmp);
    test_open_pages(tmp);
    test_replay();

    fs::remove_all(tmp);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all know tests passed\n");
    return g_fail ? 1 : 0;
}

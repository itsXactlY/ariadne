// dd::know::Bm25f — the index built on all cores is the index built in turn: the same hits to the last bit and the same
// idf, at one, two, four and eight threads and in the serial single-shard path. And what a score means, on a small corpus.
#include <omp.h>

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "dd/know/bm25.hpp"
#include "dd/know/text.hpp"

using namespace dd::know;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) {
        ++g_fail;
        if (g_fail <= 12) std::printf("FAIL %s\n", label.c_str());
    }
}

// ---- a corpus with the shape of the real ones: a vocabulary with a long tail, tags, groups, camelCase and umlauts ----
static std::vector<std::string> make_vocab(std::mt19937& rng) {
    std::vector<std::string> v;
    const char* stems[] = {"podman", "quadlet", "systemd", "mazemaker", "recall", "daedalus", "strata", "tape", "cache", "kernel", "vulkan", "llama", "router",
                           "skill", "memory", "deploy", "build", "audit", "debug", "render", "stream", "socket", "token", "prefix", "window", "slot", "pool"};
    const char* tails[] = {"", "s", "ing", "ed", "er", "Ops", "Debug", "X", "2", "_v1", "-notes"};
    for (const char* s : stems)
        for (const char* t : tails) v.push_back(std::string(s) + t);
    for (int i = 0; i < 400; ++i) {
        std::string w;
        for (int k = 3 + static_cast<int>(rng() % 6); k > 0; --k) w.push_back(static_cast<char>('a' + rng() % 26));
        v.push_back(w);
    }
    v.push_back("Müller");
    v.push_back("über");
    v.push_back("straße");
    return v;
}

static std::string pick_zipf(std::mt19937& rng, const std::vector<std::string>& v) {
    const double u = std::uniform_real_distribution<double>(0, 1)(rng);
    return v[static_cast<size_t>(std::pow(u, 2.2) * static_cast<double>(v.size() - 1))];
}

static std::vector<Card> make_cards(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    const auto vocab = make_vocab(rng);
    std::vector<Card> cards(n);
    for (size_t i = 0; i < n; ++i) {
        Card& c = cards[i];
        c.uri.id = "card-" + std::to_string(i);
        c.title = pick_zipf(rng, vocab) + "-" + pick_zipf(rng, vocab) + "-" + std::to_string(i);
        c.group = pick_zipf(rng, vocab);
        for (int k = static_cast<int>(rng() % 4); k > 0; --k) c.tags.push_back(pick_zipf(rng, vocab));
        for (int k = 5 + static_cast<int>(rng() % 40); k > 0; --k) c.gist += pick_zipf(rng, vocab) + (rng() % 9 == 0 ? ". " : " ");
        c.prior = (i % 97 == 0) ? 0.5 : 1.0;
    }
    return cards;
}

static std::vector<Query> make_queries(unsigned seed, size_t n) {
    std::mt19937 rng(seed);
    const auto vocab = make_vocab(rng);
    std::vector<Query> qs(n);
    for (auto& q : qs) {
        for (int k = 1 + static_cast<int>(rng() % 10); k > 0; --k) {
            Term t;
            t.text = rng() % 8 == 0 ? "nosuchterm" + std::to_string(rng() % 50) : pick_zipf(rng, vocab);
            for (auto& ch : t.text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            t.weight = 0.5f + static_cast<float>(rng() % 30) / 10.0f;
            q.terms.push_back(std::move(t));
        }
    }
    return qs;
}

static bool same_hits(const std::vector<Hit>& a, const std::vector<Hit>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const Hit &x = a[i], &y = b[i];
        if (x.card.uri.id != y.card.uri.id || x.score != y.score || x.raw != y.raw || x.margin != y.margin || x.matched != y.matched || x.mass != y.mass ||
            x.ideal != y.ideal || x.why != y.why)
            return false;
    }
    return true;
}

static void test_parallel_equals_serial() {
    const size_t n = 2600;
    const auto cards = make_cards(n, 7);
    const auto queries = make_queries(11, 300);

    Bm25fParams serial;
    serial.par_docs = ~size_t{0};   // never parallel: one shard, built in turn
    const Bm25f ref(cards, serial);
    chk(ref.size() == n, "serial index has every card");

    int hits_seen = 0;
    for (int threads : {1, 2, 4, 8}) {
        omp_set_num_threads(threads);
        Bm25fParams par;
        par.par_docs = 1;   // always parallel: sixteen shards
        const Bm25f idx(cards, par);
        chk(idx.size() == n, "parallel index has every card");
        for (size_t qi = 0; qi < queries.size(); ++qi) {
            const auto want = ref.search(queries[qi], 8);
            const auto got = idx.search(queries[qi], 8);
            hits_seen += static_cast<int>(want.size());
            chk(same_hits(want, got), "threads " + std::to_string(threads) + " query " + std::to_string(qi) + " hits differ");
        }
        for (const auto& q : queries)
            for (const auto& t : q.terms) chk(ref.idf(t.text) == idx.idf(t.text), "idf of " + t.text + " at " + std::to_string(threads) + " threads");
    }
    chk(hits_seen > 300 * 4, "the queries find something (" + std::to_string(hits_seen) + " hits)");
    omp_set_num_threads(omp_get_num_procs());
}

static void test_below_threshold_is_serial() {
    // the default threshold: a small catalogue builds without a parallel region, and gives the same index
    const auto cards = make_cards(120, 3);
    Bm25f small(cards);   // 120 < 512
    Bm25fParams par;
    par.par_docs = 1;
    Bm25f forced(cards, par);
    for (const auto& q : make_queries(5, 80)) chk(same_hits(small.search(q, 8), forced.search(q, 8)), "small catalogue: serial and parallel agree");
}

static void test_score_meaning() {
    std::vector<Card> cards(4);
    cards[0].title = "podman-quadlet-ops";
    cards[0].gist = "operate rootless podman containers and quadlet units";
    cards[0].tags = {"podman", "quadlet"};
    cards[1].title = "kernel-tuning";
    cards[1].gist = "sysctl and cache pressure for the kernel";
    cards[2].title = "recall-router";
    cards[2].gist = "route a question to memory and skills";
    cards[3].title = "container-basics";
    cards[3].gist = "what a container is";
    for (size_t i = 0; i < cards.size(); ++i) cards[i].uri.id = cards[i].title;
    const Bm25f idx(cards);

    Query q;
    q.terms = {{"podman", 1.0f}, {"quadlet", 1.0f}};
    auto hits = idx.search(q, 3);
    chk(!hits.empty() && hits[0].card.title == "podman-quadlet-ops", "the card with both terms is first");
    chk(!hits.empty() && hits[0].matched == 2 && hits[0].why == "podman quadlet", "it matched both, in query order");
    chk(!hits.empty() && hits[0].score > 0.5 && hits[0].score <= 1.0, "a card that has all of what was asked scores high");

    Query none;
    none.terms = {{"zzzz", 1.0f}};
    chk(idx.search(none, 3).empty(), "nothing matches an unknown term");
    chk(idx.idf("zzzz") == 0.0f && idx.idf("podman") > 0.0f, "idf: 0 for an unknown term, positive for a known one");

    const Bm25f empty(std::vector<Card>{});
    chk(empty.search(q, 3).empty() && empty.size() == 0, "an empty catalogue answers nothing");
}

int main() {
    test_score_meaning();
    test_below_threshold_is_serial();
    test_parallel_equals_serial();
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all know bm25 tests passed\n");
    return g_fail ? 1 : 0;
}

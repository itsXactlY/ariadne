// tests/know_stopwords_test.cpp — stopword languages are opt-in, and the other languages never eat an English or tech word.
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "dd/core/config.hpp"
#include "dd/know/text.hpp"
#include "dd/know/wiring.hpp"

using namespace dd::know;

static int g_fail = 0, g_pass = 0;
static void chk(bool ok, const std::string& what) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::printf("FAIL %s\n", what.c_str());
    }
}
static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}
static bool has(const std::vector<std::string>& v, const std::string& w) {
    for (const auto& x : v)
        if (x == w) return true;
    return false;
}

int main() {
    // the default is English and German, as it always was
    auto t = terms_of("fix the CI pipeline");
    chk(has(t, "ci") && !has(t, "the"), "default: ci survives, the is out: " + join(t));
    t = terms_of("var x = min(a, b)");
    chk(has(t, "var") && has(t, "min"), "default: var and min survive: " + join(t));
    chk(terms_of("AI agent memory").size() == 3, "default: ai agent memory");
    chk(is_stopword("und") && is_stopword("the") && !is_stopword("och") && !is_stopword("della") && !is_stopword("nao"), "default: en+de only");

    // every foreign language on: the words a coder types still count
    set_stopword_languages(stopword_languages());
    for (const char* w : {"ai", "ci", "io", "os", "var", "min", "max", "lo", "see", "sin", "del", "su", "vi", "em", "op", "id", "con", "par", "pod", "sub", "per", "come", "take", "plus", "era", "hay"})
        chk(!is_stopword(w), std::string("all languages: ") + w + " is not a stopword");
    chk(is_stopword("du") && is_stopword("ne") && is_stopword("man"), "en/de words stay as they were (du ne man)");

    // a few languages: the foreign stopwords go, the coder's words stay
    set_stopword_languages({"en", "de", "pt", "it", "ro", "sv"});
    t = terms_of("fix the CI pipeline");
    chk(has(t, "ci") && has(t, "pipelin"), "pt it ro sv: ci survives: " + join(t));
    t = terms_of("var x = min(a, b)");
    chk(has(t, "var") && has(t, "min"), "pt it ro sv: var and min survive: " + join(t));
    t = terms_of("os.path.join and io_uring with AI");
    chk(has(t, "os") && has(t, "io") && has(t, "ai") && has(t, "uring"), "pt it ro sv: os io ai uring survive: " + join(t));
    t = terms_of("och della nao");
    chk(t.empty(), "pt it ro sv: och (sv) della (it) nao (pt) are out: " + join(t));
    chk(!is_stopword("ikke") && !is_stopword("jeg"), "no/da stay off when not asked for");

    // the table is folded the way the tokenizer folds: être -> etre
    set_stopword_languages({"fr"});
    chk(terms_of("être").empty(), "fr: être (folded to etre) is out");
    set_stopword_languages({"en", "de"});
    chk(terms_of("être") == std::vector<std::string>{"etre"}, "en de: être is a word");

    // names: case, aliases, unknowns
    set_stopword_languages({"NB", "xx", "bogus"});
    chk(is_stopword("ikke") && !is_stopword("the"), "NB is no; unknown names are ignored; nothing else on");
    set_stopword_languages({});
    chk(!is_stopword("the") && terms_of("the and of").size() == 3, "an empty list switches stopwords off");

    // configure() reads knowledge.stopword_languages
    dd::core::Config cfg(dd::core::parse_yaml("knowledge:\n  stopword_languages: [en, de, sv]\n"));
    configure(cfg);
    chk(is_stopword("the") && is_stopword("och") && !is_stopword("della"), "configure: list");
    configure(dd::core::Config(dd::core::parse_yaml("knowledge:\n  stopword_languages: en, it\n")));
    chk(is_stopword("the") && is_stopword("della") && !is_stopword("und"), "configure: scalar");
    configure(dd::core::Config(dd::core::parse_yaml("knowledge:\n  rank:\n    name: 8\n")));
    chk(is_stopword("the") && is_stopword("und") && !is_stopword("och"), "configure: key absent = en+de");

    // readers while the table is swapped: no crash, every answer is one of the two tables'
    std::atomic<bool> stop{false};
    std::atomic<long> bad{0}, reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const bool the = is_stopword("the");   // en is in both tables below
                if (!the) ++bad;
                (void)terms_of("fix the CI pipeline och della");
                ++reads;
            }
        });
    for (int i = 0; i < 200; ++i) set_stopword_languages(i % 2 ? std::vector<std::string>{"en", "sv"} : std::vector<std::string>{"en", "de", "it"});
    stop = true;
    for (auto& th : readers) th.join();
    chk(bad == 0 && reads > 0, "readers during swaps saw a table every time");

    set_stopword_languages({"en", "de"});
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all know stopword tests passed\n");
    return g_fail ? 1 : 0;
}

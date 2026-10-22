#pragma once
// dd/know/types.hpp — the vocabulary of retrieval.
//
// A Card is what a catalogue knows about an item without opening it: cheap,
// local, always searchable. A Body is what opening it returns, a page at a
// time. A Uri names either: skill://name[/file][#page], mem://123,
// rule://name, tool://name, sess://id.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dd {
namespace know {

enum class Kind { Skill, Memory, Tool, Session, Rule };
const char* kind_name(Kind k);                        // skill memory tool session rule
std::optional<Kind> kind_from(const std::string& s);  // those, and "mem"

struct Uri {
    Kind kind = Kind::Skill;
    std::string id;     // skill name (name@source for a differing copy), memory id, rule name, tool name
    std::string path;   // skill://name/references/x.md
    std::string page;   // "" (the first), "2", "all"

    std::string str() const;
    // nullopt: not an internal URI. Absolute paths and ".." are refused.
    static std::optional<Uri> parse(const std::string& text);
    bool operator==(const Uri& o) const {
        return kind == o.kind && id == o.id && path == o.path && page == o.page;
    }
};

struct Term {
    std::string text;
    float weight = 1.0f;
};

struct Card {
    Uri uri;
    std::string title;    // skill name / memory label / tool name
    std::string gist;     // one line: when to use it (<= 200 chars)
    std::string group;    // skill category / label namespace / toolset
    std::string source;   // disk:<root> | maze:local | maze:builtin | ...
    std::string rev;      // content hash prefix; "" when unknown
    std::vector<std::string> tags;
    size_t body_bytes = 0;
    bool has_body = true;
    double prior = 1.0;   // a curated weight on the final score
    std::vector<Term> learned;   // terms the harness learned for this card (Overlay); the Bm25f `learned` field
};

struct Query {
    std::vector<Term> terms;   // distilled, weighted
    std::string text;          // as written: the deep leg's query
    std::vector<Kind> kinds;   // empty: every catalogue
};

struct Hit {
    Card card;
    double score = 0;    // 0..1: the share of the best score this query could have reached
    double raw = 0;
    double margin = 0;   // to the next hit
    int matched = 0;     // query terms found in this card
    double mass = 0;     // idf x weight of those terms: how much evidence, in absolute terms
    double ideal = 0;    // the same for every term of the query the index knows
    std::string why;     // the matched terms
};

struct Body {
    Uri uri;
    std::string text;
    size_t total = 0;    // bytes in the whole text
    int page = 1, pages = 1;
    std::string more;    // the Uri of the next page, "" on the last
};

// Pages of at most page_bytes, cut between paragraphs. uri.page: "" or "1".."n" or "all"
// ("all": whole pages up to all_cap bytes). nullopt: no such page.
std::optional<Body> paginate(const Uri& uri, const std::string& text, size_t page_bytes, size_t all_cap = 24000);

}  // namespace know
}  // namespace dd

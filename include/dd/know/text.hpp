#pragma once
// dd/know/text.hpp — words for the index and for the query.
//
// terms_of folds, splits and filters: lowercase; ä ö ü ß and the accented
// Latin letters folded to ae oe ue ss and their base letters; camelCase and
// every non-alphanumeric split; a version or an error code (0.1.45, ENOENT)
// also kept whole; stopwords out (English and German unless knowledge.stopword_languages says otherwise).
//
// The Distiller turns what a person wrote into a Query: pasted terminal and
// code stripped, paths reduced to the project and file they name (never the
// generic folders on the way), identifiers weighted up, and at most
// max_terms of the rarest terms kept, so a long message cannot buy a score
// with common words.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "dd/know/types.hpp"

namespace dd {
namespace know {

std::vector<std::string> terms_of(std::string_view text, bool keep_stop = false);
bool is_stopword(std::string_view folded);
// Which languages' stopwords are out. Names are ISO 639-1 codes (stopword_languages() lists them; nb/nn count as no, bs as
// hr; unknown names are ignored). The default is {"en", "de"}. The other languages are opt-in and never stop a word that
// is also an English word or a common tech token (ai, ci, io, os, var, min ...): en and de are taken as they are.
// Call it at setup, before the catalogues are built: it swaps one table in; a build running meanwhile may see either.
void set_stopword_languages(const std::vector<std::string>& names);
std::vector<std::string> stopword_languages();
bool is_generic_path_word(std::string_view lower);

// the first / last n bytes, never cutting inside a UTF-8 sequence
std::string utf8_head(const std::string& s, size_t n);
std::string utf8_tail(const std::string& s, size_t n);

using IdfFn = std::function<float(const std::string&)>;   // 0: the index has no such term

class Distiller {
public:
    struct Options {
        size_t max_terms = 12;
        size_t max_chars = 1200;   // of prose kept from one message
    };
    Distiller() = default;
    explicit Distiller(Options o) : opt_(o) {}

    // What a person wrote. idf (optional) picks the rarest terms when there are too many.
    Query text(const std::string& s, const IdfFn& idf = {}) const;
    // The prose of a message: pasted terminal output, code fences, urls and paste tags removed, capped.
    static std::string prose(const std::string& s, size_t max_chars);
    // Weighted terms of free-form material (tool arguments, an error line, an intent).
    Query terms(const std::vector<std::pair<std::string, float>>& pieces, const IdfFn& idf = {}) const;

private:
    Options opt_;
    Query pick(std::vector<Term> all, std::string text, const IdfFn& idf) const;
};

}  // namespace know
}  // namespace dd

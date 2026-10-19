#ifndef DD_TOOLS_FUZZY_MATCH_HPP
#define DD_TOOLS_FUZZY_MATCH_HPP
// Multi-strategy fuzzy find-and-replace. Faithful port of tools/fuzzy_match.py:
// the 8-strategy matching chain (exact, line-trimmed, whitespace-normalized,
// indentation-flexible, escape-normalized, trimmed-boundary, block-anchor,
// context-aware) plus difflib.SequenceMatcher.ratio() reimplemented inline.
// Byte-based slicing is byte-identical to Python because every match/line
// boundary falls on a UTF-8 codepoint boundary; seq_ratio is codepoint-aware.
#include <string>
#include <optional>

namespace dd {
namespace tools {

struct FuzzyResult {
    std::string content;
    int match_count = 0;
    std::optional<std::string> error;
};

FuzzyResult fuzzy_find_and_replace(const std::string& content,
                                   const std::string& old_string,
                                   const std::string& new_string,
                                   bool replace_all = false);

}  // tools
}  // dd
#endif

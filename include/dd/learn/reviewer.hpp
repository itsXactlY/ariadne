#pragma once
// dd/learn/reviewer.hpp — the gate a draft passes before it exists on disk. A learned skill is a prompt the
// model will trust, so it is checked for size, secrets, private paths, instructions aimed at the harness,
// commands it must never suggest (remote code, exfiltration, sudo, force-push ...) and near-duplicates of what
// the catalogue already has.

#include <functional>
#include <string>
#include <vector>

#include "dd/know/types.hpp"
#include "dd/learn/author.hpp"

namespace dd {
namespace learn {

struct Verdict {
    bool ok = false;
    std::string reason;   // "" when ok; else the first rule that held it and what matched (never the secret itself)
};

class Reviewer {
public:
    using SearchFn = std::function<std::vector<know::Hit>(const know::Query&)>;
    struct Options {
        size_t min_body = 40, max_body = 6000, max_gist = 200;
        size_t max_tags = 12;
        double dup_score = 0.6;   // a hit this close (and with dup_matched terms) is a duplicate
        int dup_matched = 3;
        std::string home_dir;      // e.g. "/home/someone"
        std::string project_dir;   // paths under it are fine
    };
    Reviewer(SearchFn search, Options o) : search_(std::move(search)), opt_(std::move(o)) {}
    explicit Reviewer(SearchFn search) : Reviewer(std::move(search), Options{}) {}

    Verdict review(const Draft& d) const;

    // the single rules, public for tests
    static std::string find_secret(const std::string& text);                  // "" or a label
    static std::string find_instruction(const std::string& text);             // "" or a label
    static std::string find_command(const std::string& text);                 // "" or a label
    std::string find_private_path(const std::string& text) const;             // "" or the offending prefix

private:
    SearchFn search_;
    Options opt_;
};

}  // namespace learn
}  // namespace dd

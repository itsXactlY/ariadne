#pragma once
// dd/learn/author.hpp — a detection becomes a draft skill. TemplateAuthor is deterministic: no model,
// the same detection always gives the same draft.

#include <optional>
#include <string>
#include <vector>

#include "dd/learn/detector.hpp"

namespace dd {
namespace learn {

struct Draft {
    std::string name;       // the skill name and folder: kebab, stable for the same problem
    std::string category;   // the folder above it
    std::string kind;       // fixed | long | pinned
    std::string gist;       // one line, when to use it (<= 200 chars)
    std::string body;       // the SKILL.md text after the frontmatter
    std::string digest;     // sha256 (16 hex) over name and body: the idempotency key
    std::string episode;    // the episode's digest
    std::vector<std::string> tags;
};

class Author {
public:
    virtual ~Author() = default;
    virtual std::optional<Draft> author(const Detection& d) const = 0;
};

class TemplateAuthor final : public Author {
public:
    // home_dir: replaced by "~" in anything quoted from the work
    explicit TemplateAuthor(std::string home_dir = "") : home_(std::move(home_dir)) {}
    std::optional<Draft> author(const Detection& d) const override;

private:
    std::string home_;
};

// the folder a draft belongs in, from the binary most used ("podman" -> "ops"); "learned" when none is known
std::string category_for(const std::vector<std::string>& binaries_by_use);

}  // namespace learn
}  // namespace dd

#pragma once
// Minimal YAML frontmatter parser for SKILL.md metadata.
// Handles the leading `---` block with single-line `key: value` scalars and
// inline lists (`[a, b]`). Enough for discovery metadata (name, description,
// category, platform, tags). Not a full YAML implementation.

#include <map>
#include <string>
#include <vector>

namespace dd {
namespace core {

// Parse the frontmatter block of a markdown document. Returns key->value
// (values are raw strings; use helpers below for lists). Returns empty map
// if there is no leading `---` block.
std::map<std::string, std::string> parse_frontmatter(const std::string& content);

// Split a frontmatter value that is an inline list `[a, b]` or comma list
// `a, b` into trimmed, quote-stripped tokens.
std::vector<std::string> parse_list(const std::string& value);

// Strip surrounding quotes from a scalar.
std::string unquote(const std::string& value);

// The markdown body (everything after the frontmatter block).
std::string body(const std::string& content);

}  // namespace core
}  // namespace dd

#ifndef DD_TOOLS_PATCH_PARSER_HPP
#define DD_TOOLS_PATCH_PARSER_HPP
// V4A patch-format parser (pure). Faithful port of the parsing core of
// tools/patch_parser.py. The application layer (apply_v4a_operations / _apply_*)
// is NOT ported: it is injected with a duck-typed file_ops object doing
// filesystem IO, subprocess (rm/mv), and runtime lint introspection.
#include <string>
#include <vector>
#include <optional>

namespace dd {
namespace tools {

enum class OperationType { ADD, UPDATE, DELETE, MOVE };

struct HunkLine {
    std::string prefix;
    std::string content;
};

struct Hunk {
    std::optional<std::string> context_hint;
    std::vector<HunkLine> lines;
};

struct PatchOperation {
    OperationType operation;
    std::string file_path;
    std::optional<std::string> new_path;
    std::vector<Hunk> hunks;
    std::optional<std::string> content;
};

// Parse a V4A patch. Returns (operations, error_message). On success the
// error_message is nullopt; on failure the vector is empty and the message
// describes the problem. (The Python source never produces a non-None error
// in parse_v4a_patch itself, but the signature carries the slot.)
std::pair<std::vector<PatchOperation>, std::optional<std::string>>
parse_v4a_patch(const std::string& patch_content);

}  // tools
}  // dd
#endif

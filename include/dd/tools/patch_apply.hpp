#ifndef DD_TOOLS_PATCH_APPLY_HPP
#define DD_TOOLS_PATCH_APPLY_HPP
// The application half of the V4A patch format. parse_v4a_patch turns patch text into operations;
// this turns operations into writes.
//
// Two phases on purpose: every operation is planned against the file as it is NOW, and nothing is
// written until the whole patch has planned clean. A patch with one bad hunk therefore leaves every
// file untouched. Individual writes then go through the same atomic path write_file uses (temp file
// in the parent dir, rename), so a file never holds half an edit.
//
// Context matching is fuzzy_match's strategy ladder (exact -> line-trimmed -> whitespace-normalized
// -> indentation-flexible -> escape-normalized -> trimmed-boundary -> block-anchor -> context-aware).
// The ladder absorbs whitespace drift; it is not a licence to guess. An ambiguous match is reported
// as an error naming the operation and the hunk, never resolved by picking one candidate.
//
// A hunk's context_hint is a locator the model offers for the failing case only: it is echoed back
// in the error so the model can widen the context. It is not used to choose between candidates,
// because fuzzy_match reports no match positions.
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace tools {

struct ApplyOptions {
    std::string root;              // DELETE and MOVE must stay inside it; "" = no extra bound
    bool allow_delete = false;     // DELETE and MOVE are refused unless this is set
    std::size_t max_bytes = 8u << 20;   // a file the patch reads must be smaller than this
    std::optional<std::string> skills_root;  // when set, a learned skill the model edited is annotated
    std::string origin;            // who is writing: skill_provenance's stack top
    long long now = 0;             // epoch seconds, for the annotation
};

struct AppliedFile {
    std::string path;
    std::string op;      // Add | Update | Delete | Move
    std::size_t bytes = 0;
};

struct ApplyReport {
    std::vector<AppliedFile> files;      // empty when error is set: nothing was written
    std::vector<std::string> notes;      // one per hunk that matched: places, and the hint it was given
    std::optional<std::string> error;    // names the operation and the hunk
};

// Parse, plan, commit. A report carrying an error wrote nothing.
ApplyReport apply_v4a_patch(const std::string& patch_text, const ApplyOptions& opt);

// The tool-facing entry: {patch, allow_delete?} -> {applied, files, notes} | {error}.
// Anything the caller leaves unset is resolved here: root = the launch directory, origin =
// skill_provenance's stack top, now = the wall clock.
std::string fs_edit(const core::Json& args, ApplyOptions opt = {});

// edit_file's schema. `patch` is the whole V4A document; one call can touch several files.
core::Json edit_tool_schema();

}  // namespace tools
}  // namespace dd
#endif

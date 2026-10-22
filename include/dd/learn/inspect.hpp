#pragma once
// dd/learn/inspect.hpp — what `dd learn` shows: read-only views over a learn directory, and `forget`.
//
// Every view replays the ledger without touching it (Ledger::load_readonly), so it is safe beside a
// running TUI. Only forget writes: a tombstone to the journal, and a learned skill's folder moved to
// <skills>/.retired/ (never deleted; the scanner skips that folder).

#include <cstdint>
#include <filesystem>
#include <string>

namespace dd {
namespace learn {

struct InspectPaths {
    std::filesystem::path learn_dir, skills_root;
};

struct Result {
    bool ok = false;
    std::string text;   // the output, or the reason it failed
};

Result status(const InspectPaths& p, int64_t now);
Result why(const InspectPaths& p, const std::string& skill, int64_t now);
Result export_jsonl(const InspectPaths& p, int64_t now);   // one JSON object per card and per association, sorted
Result forget(const InspectPaths& p, const std::string& skill, int64_t now);
std::string report_path(const InspectPaths& p);            // <learn>/report.md ("" when none yet)

}  // namespace learn
}  // namespace dd

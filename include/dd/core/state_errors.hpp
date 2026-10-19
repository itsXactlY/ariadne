#pragma once
// dd/core/state_errors.hpp — daedalus_state.py error-classifier helpers.
// Ported (string / marker paths, pure): is_disk_full_error (the marker-membership branch — the
//   OSError/errno.ENOSPC special-case is caller-side in C++), classify_persistence_error (the
//   marker bucketing; a CompressionSessionBusyError instance maps to "locked" at the caller). NOT
//   ported: is_malformed_db_error — its isinstance(exc, sqlite3.DatabaseError) gate makes it always
//   False on a plain string, so the whole function is exception-type coupled (no string analogue).
// Python None-input maps to "" here: is_disk_full_error("")=False and classify("")="unknown" match
// the Python None branches exactly.
#include <algorithm>
#include <cctype>
#include <string>

namespace dd {
namespace core {

namespace {
constexpr const char* DISK_FULL_MARKERS[] = {
    "no space left on device",
    "not enough space",
    "database or disk is full",
    "disk full",
    "full disk",
    "enospc",
};
}  // anon

inline std::string lower_copy(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// daedalus_state.py: is_disk_full_error (string path). Lower-cased marker membership.
inline bool is_disk_full_error(const std::string& exc) {
    const std::string lowered = lower_copy(exc);
    for (const char* m : DISK_FULL_MARKERS) {
        if (contains(lowered, m)) return true;
    }
    return false;
}

// daedalus_state.py: classify_persistence_error (string path). Returns "locked" | "disk" |
// "unknown". An exception that is a CompressionSessionBusyError instance is handled by callers
// (they pass the literal "locked" bucket); every other input is classified from its string form.
inline std::string classify_persistence_error(const std::string& exc_or_str) {
    const std::string text = lower_copy(exc_or_str);
    if (contains(text, "locked") || contains(text, "busy") ||
        contains(text, "being compressed") || contains(text, "compression lease")) {
        return "locked";
    }
    if (is_disk_full_error(exc_or_str) || contains(text, "disk") ||
        contains(text, "readonly") || contains(text, "read-only")) {
        return "disk";
    }
    return "unknown";
}

}  // namespace core
}  // namespace dd

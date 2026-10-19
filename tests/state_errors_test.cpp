// Differential test for daedalus_state.py error classifiers vs captured Python truth (string path).
#include <iostream>
#include <string>

#include "dd/core/state_errors.hpp"

using namespace dd;
using namespace dd::core;

static int failures = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << what << "\n";
    }
}

int main() {
    // ---- is_disk_full_error ----
    chk(!is_disk_full_error(""), "df:empty");
    chk(is_disk_full_error("No Space Left On Device"), "df:noslpace");
    chk(is_disk_full_error("ENOSPC"), "df:enospc");
    chk(!is_disk_full_error("disk or DISK is FULL"), "df:not-contiguous");
    chk(!is_disk_full_error("read-only filesystem"), "df:readonly-not-disk-full");
    chk(!is_disk_full_error("database is locked"), "df:locked");
    chk(!is_disk_full_error("another process holds the lock"), "df:lock-not-disk");
    chk(!is_disk_full_error("Being compressed now"), "df:compressed");
    chk(!is_disk_full_error("compression lease active"), "df:lease");
    chk(is_disk_full_error("database or disk is full"), "df:sq-full");
    chk(!is_disk_full_error("unknown error"), "df:unknown");
    chk(is_disk_full_error("Errno 28: not enough space"), "df:errno-space");
    chk(!is_disk_full_error("readonly"), "df:readonly");

    // ---- classify_persistence_error ----
    chk(classify_persistence_error("") == "unknown", "cp:empty");
    chk(classify_persistence_error("No Space Left On Device") == "disk", "cp:nspace");
    chk(classify_persistence_error("ENOSPC") == "disk", "cp:enospc");
    chk(classify_persistence_error("disk or DISK is FULL") == "disk", "cp:disk-word");
    chk(classify_persistence_error("read-only filesystem") == "disk", "cp:read-only");
    chk(classify_persistence_error("database is locked") == "locked", "cp:locked");
    chk(classify_persistence_error("another process holds the lock") == "unknown", "cp:lock-not-locked");
    chk(classify_persistence_error("Being compressed now") == "locked", "cp:compressed");
    chk(classify_persistence_error("compression lease active") == "locked", "cp:lease");
    chk(classify_persistence_error("database or disk is full") == "disk", "cp:sq-full");
    chk(classify_persistence_error("unknown error") == "unknown", "cp:unknown");
    chk(classify_persistence_error("Errno 28: not enough space") == "disk", "cp:errno-space");
    chk(classify_persistence_error("readonly") == "disk", "cp:readonly");

    if (failures == 0) {
        std::cout << "all state-error tests passed\n";
        return 0;
    }
    std::cout << failures << " state-error tests FAILED\n";
    return 1;
}

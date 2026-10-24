#pragma once
// Where a learn test keeps its files: $DD_TEST_TMP (`make test` sets it) else ./build, then <tag>_<pid>. Never /tmp, and never
// shared between two runs, so the plain, ASan and TSan binaries of one test can run at the same time.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

inline std::filesystem::path learn_test_dir(const std::string& tag) {
    const char* e = std::getenv("DD_TEST_TMP");
    return (e && *e ? std::filesystem::path(e) : std::filesystem::path("build")) / (tag + "_" + std::to_string(::getpid()));
}

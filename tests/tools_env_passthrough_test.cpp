// tests/tools_env_passthrough_test.cpp -> env_passthrough.py
#include "dd/tools/env_passthrough.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace dd::tools;
using namespace dd;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL: %s\n", #cond); } } while (0)

static void write_config(const std::string& dir, const std::string& body) {
    std::ofstream f(dir + "/config.yaml");
    f << body;
    f.close();
}

int main() {
    std::string dir = "/tmp/dd_env_passthrough_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    setenv("DAEDALUS_HOME", dir.c_str(), 1);

    // config-based passthrough
    write_config(dir,
        "terminal:\n"
        "  env_passthrough:\n"
        "    - FOO\n"
        "    - BAR\n"
        "    - \"  BAZ  \"\n");
    reset_config_cache();
    CHECK(is_env_passthrough("FOO"));
    CHECK(is_env_passthrough("BAR"));
    CHECK(is_env_passthrough("BAZ"));  // stripped
    CHECK(!is_env_passthrough("QUX"));

    // skill-registered
    register_env_passthrough({"MYVAR", "  ", "  SPACED  "});
    CHECK(is_env_passthrough("MYVAR"));
    CHECK(is_env_passthrough("SPACED"));  // stripped
    CHECK(!is_env_passthrough(""));       // empty never registered

    // union
    std::set<std::string> all = get_all_passthrough();
    CHECK(all.count("FOO") && all.count("BAR") && all.count("BAZ"));
    CHECK(all.count("MYVAR") && all.count("SPACED"));
    CHECK(all.count("QUX") == 0);

    // clear skill-scoped: config vars remain
    clear_env_passthrough();
    CHECK(!is_env_passthrough("MYVAR"));
    CHECK(is_env_passthrough("FOO"));

    // config not-a-list / missing -> empty
    write_config(dir, "terminal:\n  env_passthrough: \"not-a-list\"\n");
    reset_config_cache();
    CHECK(!is_env_passthrough("FOO"));

    write_config(dir, "other: 1\n");
    reset_config_cache();
    CHECK(!is_env_passthrough("FOO"));

    // no config file -> empty
    std::filesystem::remove(dir + "/config.yaml");
    reset_config_cache();
    CHECK(!is_env_passthrough("FOO"));

    if (failures == 0) {
        std::printf("all env-passthrough tests passed\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}

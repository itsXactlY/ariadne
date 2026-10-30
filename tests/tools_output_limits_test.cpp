// tests/tools_output_limits_test.cpp -> tool_output_limits.py
#include "dd/tools/tool_output_limits.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using namespace dd::tools;
using namespace dd;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL: %s\n", #cond); } } while (0)

static core::Json mk_num(double v) { core::Json j; j.type = core::Json::Num; j.num = v; return j; }
static core::Json mk_str(const std::string& s) { core::Json j; j.type = core::Json::Str; j.str = s; return j; }
static core::Json mk_bool(bool b) { core::Json j; j.type = core::Json::Bool; j.b = b; return j; }

// Named-local coercion so we can take a stable address.
static int c_num(double v, int def) { core::Json j = mk_num(v); return _coerce_positive_int(&j, def); }
static int c_str(const std::string& s, int def) { core::Json j = mk_str(s); return _coerce_positive_int(&j, def); }
static int c_bool(bool b, int def) { core::Json j = mk_bool(b); return _coerce_positive_int(&j, def); }
static int c_null(int def) { core::Json j; return _coerce_positive_int(&j, def); }

static void write_config(const std::string& dir, const std::string& body) {
    std::ofstream f(dir + "/config.yaml");
    f << body;
    f.close();
}

int main() {
    // ---- _coerce_positive_int ----
    CHECK(c_num(5000, 1) == 5000);
    CHECK(c_num(3.7, 1) == 3);
    CHECK(c_num(0, 7) == 7);
    CHECK(c_num(-5, 7) == 7);
    CHECK(c_bool(true, 7) == 1);
    CHECK(c_bool(false, 7) == 7);
    CHECK(c_str("5000", 7) == 5000);
    CHECK(c_str("  42 ", 7) == 42);
    CHECK(c_str("+7", 7) == 7);
    CHECK(c_str("5000.0", 7) == 7);
    CHECK(c_str("abc", 7) == 7);
    CHECK(c_str("", 7) == 7);
    CHECK(c_str("-3", 7) == 7);  // -3 <= 0 -> default
    CHECK(c_null(7) == 7);
    CHECK(_coerce_positive_int(nullptr, 7) == 7);

    // ---- get_tool_output_limits via config.yaml ----
    std::string dir = "/tmp/dd_tool_limits_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    setenv("DD_HOME", dir.c_str(), 1);

    write_config(dir,
        "tool_output:\n"
        "  max_bytes: 100000\n"
        "  max_lines: 5000\n"
        "  max_line_length: 2000\n");
    _reset_tool_output_limits_cache();
    CHECK(get_max_bytes() == 100000);
    CHECK(get_max_lines() == 5000);
    CHECK(get_max_line_length() == 2000);

    // invalid / non-positive values fall back to defaults
    write_config(dir,
        "tool_output:\n"
        "  max_bytes: \"abc\"\n"
        "  max_lines: -1\n"
        "  max_line_length: 0\n");
    _reset_tool_output_limits_cache();
    CHECK(get_max_bytes() == DEFAULT_MAX_BYTES);
    CHECK(get_max_lines() == DEFAULT_MAX_LINES);
    CHECK(get_max_line_length() == DEFAULT_MAX_LINE_LENGTH);

    // partial section: only max_lines set
    write_config(dir,
        "tool_output:\n"
        "  max_lines: 9999\n");
    _reset_tool_output_limits_cache();
    CHECK(get_max_bytes() == DEFAULT_MAX_BYTES);
    CHECK(get_max_lines() == 9999);
    CHECK(get_max_line_length() == DEFAULT_MAX_LINE_LENGTH);

    // no config file at all -> defaults
    std::filesystem::remove(dir + "/config.yaml");
    _reset_tool_output_limits_cache();
    CHECK(get_max_bytes() == DEFAULT_MAX_BYTES);
    CHECK(get_max_lines() == DEFAULT_MAX_LINES);
    CHECK(get_max_line_length() == DEFAULT_MAX_LINE_LENGTH);

    // caching: first read wins until reset
    write_config(dir, "tool_output:\n  max_bytes: 1\n");
    _reset_tool_output_limits_cache();
    int first = get_max_bytes();
    CHECK(first == 1);
    write_config(dir, "tool_output:\n  max_bytes: 2\n");
    CHECK(get_max_bytes() == 1);  // still cached
    _reset_tool_output_limits_cache();
    CHECK(get_max_bytes() == 2);  // re-read after reset

    if (failures == 0) {
        std::printf("all tool-output-limits tests passed\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}

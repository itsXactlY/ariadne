// tests/tools_output_envelope_test.cpp — the tool-result envelope.
//
// read_file and run_terminal are the two places a tool result enters the tape, so the two questions
// this envelope has to answer honestly are: what did I hand back, and what did I drop. Every check
// here asserts a field the reply carries, not a feeling about the reply.
#include "dd/core/config.hpp"
#include "dd/core/json.hpp"
#include "dd/tools/fs_tools.hpp"
#include "dd/tools/shell.hpp"
#include "dd/tools/tool_output_limits.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace dd;
using namespace dd::tools;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL: %s\n", #cond); } } while (0)

static core::Json parse(const std::string& reply) { return core::json_parse(reply); }
static long num(const core::Json& j, const char* k) {
    const core::Json* v = core::json_get(j, k);
    return (v && v->type == core::Json::Num) ? static_cast<long>(v->num) : -1;
}
static std::string str(const core::Json& j, const char* k) {
    const core::Json* v = core::json_get(j, k);
    return (v && v->type == core::Json::Str) ? v->str : std::string();
}
static bool truth(const core::Json& j, const char* k) {
    const core::Json* v = core::json_get(j, k);
    return v && v->type == core::Json::Bool && v->b;
}
static bool absent(const core::Json& j, const char* k) { return core::json_get(j, k) == nullptr; }
static size_t array_size(const core::Json& j, const char* k) {
    const core::Json* v = core::json_get(j, k);
    return (v && v->type == core::Json::Arr) ? v->arr.size() : 0;
}
static bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

static core::Config cfg_of(const std::string& yaml) { return core::Config(core::parse_yaml(yaml)); }
static core::Json args_of(const std::string& path) {
    return core::json_obj({{"path", std::make_shared<core::Json>(core::json_str(path))}});
}

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dd_envelope_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    // ---- the envelope is resolved from the config this process loaded ----
    const auto unset = resolve_tool_output_from(cfg_of(""));
    CHECK(unset.at("max_bytes") == SHIPPED_MAX_BYTES);      // 8000: what run_terminal shipped
    CHECK(unset.at("max_lines") == SHIPPED_MAX_LINES);      // 2000: what read_file shipped
    CHECK(unset.at("max_line_length") == SHIPPED_MAX_LINE_LENGTH);   // 4000: the old kMaxLine
    CHECK(unset.at("read_bytes") == SHIPPED_READ_BYTES);    // 1 MiB per call
    // An unset config must not widen the tape: the parity defaults above mirror the Python module,
    // and 50000 bytes per result would be a sixfold rise nobody asked for.
    CHECK(SHIPPED_MAX_BYTES < DEFAULT_MAX_BYTES);

    const auto set = resolve_tool_output_from(cfg_of(
        "tool_output:\n"
        "  max_bytes: 12000\n"
        "  max_lines: 300\n"
        "  max_line_length: 150\n"
        "  read_bytes: 4096\n"));
    CHECK(set.at("max_bytes") == 12000);
    CHECK(set.at("max_lines") == 300);
    CHECK(set.at("max_line_length") == 150);
    CHECK(set.at("read_bytes") == 4096);

    // Garbage, zero and negatives fall back to the shipped numbers, never to 0.
    const auto junk = resolve_tool_output_from(cfg_of(
        "tool_output:\n"
        "  max_bytes: abc\n"
        "  max_lines: 0\n"
        "  max_line_length: -5\n"
        "  read_bytes: 100000000000000000000000\n"));
    CHECK(junk.at("max_bytes") == SHIPPED_MAX_BYTES);
    CHECK(junk.at("max_lines") == SHIPPED_MAX_LINES);
    CHECK(junk.at("max_line_length") == SHIPPED_MAX_LINE_LENGTH);
    CHECK(junk.at("read_bytes") == SHIPPED_READ_BYTES);

    // ---- read_file: the window it took and the lines it did not show ----
    const fs::path many = dir / "many.txt";
    {
        std::ofstream f(many);
        for (int i = 1; i <= 2500; ++i) f << "line " << i << "\n";
    }
    const core::Json whole = parse(fs_read(args_of(many.string()), FsLimits{}));
    CHECK(num(whole, "lines") == 2000);
    CHECK(num(whole, "total_lines") == 2500);
    CHECK(num(whole, "lines_dropped") == 500);
    CHECK(num(whole, "next_offset") == 2001);   // resumable: the next call starts where this one stopped
    CHECK(truth(whole, "truncated"));

    // An explicit limit still wins, and the dropped count follows it rather than the config default.
    const core::Json capped = parse(fs_read(
        core::json_obj({{"path", std::make_shared<core::Json>(core::json_str(many.string()))},
                        {"limit", std::make_shared<core::Json>(core::json_num(10))}}),
        FsLimits{}));
    CHECK(num(capped, "lines") == 10);
    CHECK(num(capped, "lines_dropped") == 2490);
    CHECK(num(capped, "next_offset") == 11);

    // Resuming from next_offset lands on the line the first call stopped at, not one past it.
    const core::Json resumed = parse(fs_read(
        core::json_obj({{"path", std::make_shared<core::Json>(core::json_str(many.string()))},
                        {"offset", std::make_shared<core::Json>(core::json_num(num(whole, "next_offset")))},
                        {"limit", std::make_shared<core::Json>(core::json_num(100))}}),
        FsLimits{}));
    CHECK(num(resumed, "offset") == 2001);
    CHECK(num(resumed, "lines") == 100);
    CHECK(num(resumed, "lines_dropped") == 400);   // lines past the window, not the ones before it
    CHECK(truth(resumed, "truncated"));            // stopping inside a file is a partial read
    CHECK(num(resumed, "next_offset") == 2101);    // and it names where to continue
    CHECK(contains(str(resumed, "content"), "line 2001"));

    // A line longer than the cap is cut and says how long it really was.
    const fs::path longline = dir / "long.txt";
    {
        std::ofstream f(longline);
        f << std::string(300, 'x') << "\nsecond\n";
    }
    const core::Json cut = parse(fs_read(args_of(longline.string()),
                                         FsLimits{1u << 20, 2000, 2000, 100}));
    CHECK(num(cut, "lines_cut") == 1);
    CHECK(contains(str(cut, "content"), "[line cut: 300 bytes]"));
    CHECK(num(cut, "total_lines") == 2);
    CHECK(absent(cut, "lines_dropped"));   // both lines came back; only one was shortened

    // The byte budget cuts by bytes and still names where to continue.
    const fs::path wide = dir / "wide.txt";
    {
        std::ofstream f(wide);
        for (int i = 1; i <= 200; ++i) f << std::string(300, 'a') << "\n";
    }
    const core::Json budget = parse(fs_read(args_of(wide.string()),
                                            FsLimits{1000, 2000, 2000, 4000}));
    CHECK(truth(budget, "truncated"));
    CHECK(num(budget, "next_offset") > 1);
    CHECK(num(budget, "lines") < 200);
    CHECK(num(budget, "lines_dropped") == 200 - num(budget, "lines"));

    // ---- list_dir: the entry cap is stated, not silently applied ----
    const fs::path entries = dir / "entries";
    fs::create_directories(entries);
    for (int i = 0; i < 50; ++i) std::ofstream(entries / ("f" + std::to_string(i))).write("x", 1);
    const core::Json listed = parse(fs_list(args_of(entries.string()), FsLimits{1u << 20, 10}));
    CHECK(array_size(listed, "entries") == 10);
    CHECK(truth(listed, "truncated"));
    CHECK(num(listed, "total") == 50);

    // ---- run_terminal: bytes handed back, bytes dropped, and why ----
    const fs::path big = dir / "big.txt";
    {
        std::ofstream f(big);
        for (int i = 1; i <= 4000; ++i) f << "line " << i << " of a large command result\n";
    }
    ShellOptions opt;
    opt.max_output = 1000;
    const core::Json trunc = parse(run_shell("cat " + big.string(), opt));
    CHECK(num(trunc, "exit") == 0);
    CHECK(truth(trunc, "truncated"));
    const long total = num(trunc, "output_bytes");
    CHECK(total > 1000);
    CHECK(num(trunc, "bytes_dropped") == total - 1000);
    CHECK(contains(str(trunc, "stdout"), "[truncated: " + std::to_string(total - 1000) + " of " +
                                                    std::to_string(total) + " bytes dropped]"));
    CHECK(contains(str(trunc, "hint"), "Narrow"));   // says to narrow, not to rerun the same thing
    CHECK(str(trunc, "stdout").size() < static_cast<size_t>(total));   // the reply is the cap, not the output

    // A cut result that also failed says both: the failure and that the reason may be past the cut.
    const core::Json fail_cut = parse(run_shell("cat " + big.string() + "; exit 3", opt));
    CHECK(num(fail_cut, "exit") == 3);
    CHECK(truth(fail_cut, "truncated"));
    CHECK(contains(str(fail_cut, "hint"), "cut at 1000 bytes"));
    CHECK(contains(str(fail_cut, "hint"), "past the cut"));

    // A failure the output explains gets the recovery hint, with no drop noise around it.
    const core::Json missing = parse(run_shell("definitely-not-a-command-xyz", ShellOptions{}));
    CHECK(num(missing, "exit") == 127);
    CHECK(absent(missing, "truncated"));
    CHECK(absent(missing, "bytes_dropped"));
    CHECK(contains(str(missing, "hint"), "not installed or not on PATH"));

    // Silence from a query verb is information, not success.
    const fs::path empty = dir / "empty";
    fs::create_directories(empty);
    const core::Json quiet = parse(run_shell("ls " + empty.string(), ShellOptions{}));
    CHECK(num(quiet, "exit") == 0);
    CHECK(str(quiet, "stdout").empty());
    CHECK(truth(quiet, "stdout_empty"));
    CHECK(contains(str(quiet, "hint"), "printed nothing"));

    // A verb whose silence is normal gets no hint: `true` succeeding quietly is not a finding.
    const core::Json silent_ok = parse(run_shell("true", ShellOptions{}));
    CHECK(num(silent_ok, "exit") == 0);
    CHECK(absent(silent_ok, "hint"));
    CHECK(absent(silent_ok, "stdout_empty"));

    // An output under the cap carries no drop fields at all.
    const core::Json small = parse(run_shell("echo hello", ShellOptions{}));
    CHECK(num(small, "exit") == 0);
    CHECK(str(small, "stdout") == "hello\n");
    CHECK(absent(small, "truncated"));
    CHECK(absent(small, "bytes_dropped"));
    CHECK(absent(small, "hint"));

    // The shipped default is the shipped number: 8000 bytes, from the struct, not from a config file.
    ShellOptions def;
    CHECK(def.max_output == SHIPPED_MAX_BYTES);
    CHECK(FsLimits{}.max_lines == SHIPPED_MAX_LINES);
    CHECK(FsLimits{}.max_line_length == SHIPPED_MAX_LINE_LENGTH);

    fs::remove_all(dir, ec);
    std::printf(failures == 0 ? "envelope: all checks passed\n" : "envelope: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}

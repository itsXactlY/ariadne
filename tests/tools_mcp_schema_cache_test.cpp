// Tests for mcp_schema_cache: the fingerprint and accessors, and the on-disk half.
// The disk tests run in a temp directory — the cache file is a path argument, so no
// real home is touched and the runs are independent of whatever ~/.daedalus holds.
#include "dd/tools/mcp_schema_cache.hpp"
#include "dd/core/json.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace dd;
using namespace dd::core;   // json_parse/json_obj/json_arr/obj_set are core builders
using core::Json;
using core::JsonSP;

static int g_fail = 0;
static void chk(bool ok, const char* label) {
    if (!ok) { std::printf("FAIL %s\n", label); ++g_fail; }
}

static long mtime_ns(const std::filesystem::path& p) {
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) return -1;
    return static_cast<long>(st.st_mtim.tv_sec) * 1000000000L + st.st_mtim.tv_nsec;
}

static bool mode_is(const std::filesystem::path& p, mode_t want) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == want;
}

// Nothing in the directory but the cache file itself: a failed or half-done write
// would leave a .*.dd-tmp-* behind, which is exactly what atomicity must not do.
static bool no_temp_leftovers(const std::filesystem::path& dir) {
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().filename().string().find(".dd-tmp-") != std::string::npos) return false;
    return true;
}

static void disk_tests() {
    char tmpl[] = "/tmp/dd-schema-cache-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    chk(dir != nullptr, "temp_dir_created");
    if (!dir) return;
    const std::filesystem::path root(dir);
    const std::filesystem::path file = tools::cache_path(root);

    chk(file.filename() == "mcp_schema_cache.json", "cache_path_filename");
    chk(tools::load_all(file).type == Json::Obj && tools::load_all(file).obj.empty(), "load_missing_is_empty");

    // save then read back: the whole object survives, mode 0600, no temp file left over.
    Json one = json_obj();
    obj_set(one, "pulse", json_obj());
    obj_set(one, "pulse", json_obj());   // replaced, not duplicated: insertion order is preserved
    Json entry = json_obj();
    obj_set(entry, "fingerprint", json_str("aaaa0000bbbb1111"));
    obj_set(entry, "tools", json_parse("[{\"name\":\"search\",\"description\":\"d\"}]"));
    obj_set(entry, "utility_tools", json_arr());
    obj_set(one, "pulse", entry);
    chk(tools::save_all(file, one), "save_ok");
    chk(mode_is(file, 0600), "saved_mode_0600");
    chk(no_temp_leftovers(root), "save_leaves_no_temp");
    Json back = tools::load_all(file);
    chk(back.type == Json::Obj && back.obj.size() == 1, "roundtrip_one_server");
    chk(core::json_dump(back) == core::json_dump(one), "roundtrip_byte_equal");

    // A fingerprint is the whole staleness contract: same key served, changed key missed.
    chk(tools::get_cached_entry(file, "pulse", "aaaa0000bbbb1111").type == Json::Obj, "entry_served_on_match");
    chk(tools::get_cached_entry(file, "pulse", "ffff0000eeee1111").type == Json::Null, "entry_missed_on_changed_config");
    chk(tools::get_cached_entry(file, "other", "aaaa0000bbbb1111").type == Json::Null, "entry_missed_on_absent_server");
    chk(tools::has_cached_entry(file, "pulse", "aaaa0000bbbb1111"), "has_entry_true");
    chk(!tools::has_cached_entry(file, "pulse", "nope"), "has_entry_false");
    // The served entry hands back its tools, and a copy: mutating it must not poison the file.
    Json served = tools::get_cached_entry(file, "pulse", "aaaa0000bbbb1111");
    chk(tools::tools_from_cache_entry(served).size() == 1, "served_tools_present");
    obj_set(served, "tools", json_arr());
    chk(tools::tools_from_cache_entry(tools::get_cached_entry(file, "pulse", "aaaa0000bbbb1111")).size() == 1,
        "served_entry_is_a_copy");

    // An identical write is not a write: the file keeps its mtime, which is how a boot
    // that was served from cache is told apart from a boot that listed and re-saved.
    const long before = mtime_ns(file);
    chk(tools::write_cache_entry(file, "pulse", "aaaa0000bbbb1111",
                                 json_parse("[{\"name\":\"search\",\"description\":\"d\"}]"), json_arr()), "write_identical_ok");
    chk(mtime_ns(file) == before, "identical_write_does_not_rewrite");

    // A changed manifest does rewrite, and a second server survives it.
    chk(tools::write_cache_entry(file, "pulse", "aaaa0000bbbb1111",
                                 json_parse("[{\"name\":\"search\"},{\"name\":\"dig\"}]"), json_arr()), "write_changed_ok");
    chk(tools::tools_from_cache_entry(tools::get_cached_entry(file, "pulse", "aaaa0000bbbb1111")).size() == 2,
        "changed_manifest_visible");
    chk(tools::write_cache_entry(file, "wurm", "cccc0000dddd1111", json_parse("[{\"name\":\"deep\"}]"), json_arr()),
        "write_second_server_ok");
    chk(tools::load_all(file).obj.size() == 2, "read_modify_write_keeps_both");
    chk(tools::clear_cache_entry(file, "wurm"), "clear_ok");
    chk(tools::load_all(file).obj.size() == 1, "clear_drops_only_that_server");
    chk(tools::clear_cache_entry(file, "wurm"), "clear_absent_is_not_an_error");

    // Corrupt or partial files read as empty, so the next boot lists live instead of failing.
    const std::filesystem::path broken = root / "broken.json";   // written raw: this is about reading junk back
    for (const char* junk : {"", "not json at all", "{\"pulse\": {\"fingerprint\": \"", "[1,2,3]", "\"a scalar\""}) {
        FILE* f = std::fopen(broken.c_str(), "w");
        if (f) { std::fwrite(junk, 1, std::strlen(junk), f); std::fclose(f); }
        chk(tools::load_all(broken).obj.empty(), "corrupt_cache_reads_empty");
        chk(tools::get_cached_entry(broken, "pulse", "aaaa0000bbbb1111").type == Json::Null, "corrupt_cache_is_a_miss");
    }
    ::unlink(broken.c_str());

    // An entry whose fingerprint matches but whose tools node is not a list must not be served
    // as an empty manifest — dd_tui treats that shape as a miss and lists live.
    Json weird = json_obj();
    obj_set(weird, "pulse", json_obj());
    obj_set(weird, "pulse", json_obj());
    Json w = json_obj();
    obj_set(w, "fingerprint", json_str("aaaa0000bbbb1111"));
    obj_set(w, "tools", json_str("a string, not a list"));
    obj_set(weird, "pulse", w);
    chk(tools::save_all(file, weird), "save_weird_entry");
    chk(tools::get_cached_entry(file, "pulse", "aaaa0000bbbb1111").type == Json::Obj, "weird_entry_keyed_on_fingerprint");
    chk(tools::tools_from_cache_entry(tools::get_cached_entry(file, "pulse", "aaaa0000bbbb1111")).empty(),
        "weird_entry_yields_no_tools");

    // The cache directory may not exist yet on a fresh home; a save must make it.
    const std::filesystem::path fresh = root / "cache" / "mcp_schema_cache.json";
    chk(tools::save_all(fresh, one), "save_creates_parent_dir");
    chk(std::filesystem::exists(fresh), "created_cache_exists");

    // server_fingerprint: dd's per-name `tools: {name: {enabled}}` is the same rule Python
    // spells as an exclude list, so disabling a tool has to change the key.
    const Json base = json_parse("{\"url\":\"http://127.0.0.1:9/mcp\",\"transport\":\"http\"}");
    const Json same_as_absent = json_parse(
        "{\"url\":\"http://127.0.0.1:9/mcp\",\"transport\":\"http\",\"tools\":{\"search\":{\"enabled\":true}}}");
    const Json disabled = json_parse(
        "{\"url\":\"http://127.0.0.1:9/mcp\",\"transport\":\"http\",\"tools\":{\"search\":{\"enabled\":false}}}");
    const Json moved = json_parse("{\"url\":\"http://127.0.0.1:9/mcp2\",\"transport\":\"http\"}");
    const Json rekeyed = json_parse(
        "{\"url\":\"http://127.0.0.1:9/mcp\",\"transport\":\"http\",\"headers\":{\"Authorization\":\"Bearer x\"}}");
    chk(tools::server_fingerprint(base) == tools::server_fingerprint(same_as_absent), "enabled_true_equals_absent");
    chk(tools::server_fingerprint(base) != tools::server_fingerprint(disabled), "disabling_a_tool_changes_key");
    chk(tools::server_fingerprint(base) != tools::server_fingerprint(moved), "moving_the_url_changes_key");
    chk(tools::server_fingerprint(base) == tools::server_fingerprint(rekeyed), "rotating_a_header_keeps_key");

    // Two threads writing two servers: the read-modify-write is one step, so neither is lost.
    const std::filesystem::path shared = root / "threads.json";
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i)
        ts.push_back(std::thread([&, i] {
            tools::write_cache_entry(shared, "s" + std::to_string(i), "fp" + std::to_string(i),
                                     json_parse("[{\"name\":\"t\"}]"), json_arr());
        }));
    for (auto& t : ts) t.join();
    chk(tools::load_all(shared).obj.size() == 8, "concurrent_writes_keep_every_server");
    bool every_fingerprint = true;
    for (int i = 0; i < 8; ++i)
        if (tools::get_cached_entry(shared, "s" + std::to_string(i), "fp" + std::to_string(i)).type != Json::Obj)
            every_fingerprint = false;
    chk(every_fingerprint, "concurrent_entries_all_keyed");

    std::filesystem::remove_all(root);
}

int main() {
    // config_fingerprint: parse config JSON, compare 16-char hex to Python ground truth.
    struct Case { const char* cfg; const char* want; };
    const Case cases[] = {
        {"{}", "38953fe0cbe0ee5f"},
        {"{\"command\":\"npx\",\"args\":[\"-y\",\"foo\"],\"url\":null,\"transport\":null,\"tools\":{\"include\":[\"b\",\"a\"],\"exclude\":[\"c\"]}}",
         "5b6e77ec72f1d114"},
        {"{\"command\":\"node\",\"url\":\"http://x:1/mcp\",\"transport\":\"streamable-http\",\"tools\":{\"include\":[\"z\",\"a\",\"m\"]}}",
         "35b14cbb52e4ca7e"},
        {"{\"command\":\"c\",\"args\":[],\"tools\":{\"include\":[],\"exclude\":[]}}", "ec18b8165f845555"},
        {"{\"command\":\"c\",\"tools\":{\"exclude\":[\"q\",\"p\"],\"include\":[\"s\"]}}", "f5ac012ffd21d60e"},
    };
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        Json cfg = core::json_parse(cases[i].cfg);
        std::string got = tools::config_fingerprint(cfg);
        chk(got == cases[i].want, cases[i].cfg);
    }

    // tools_from_cache_entry: list -> elements; non-list / absent -> empty.
    Json withTools = core::json_parse("{\"tools\":[{\"name\":\"a\"},{\"name\":\"b\"}],\"utility_tools\":[{\"name\":\"u\"}]}");
    std::vector<JsonSP> t = tools::tools_from_cache_entry(withTools);
    chk(t.size() == 2, "tools_present");
    std::vector<JsonSP> u = tools::utility_tools_from_cache_entry(withTools);
    chk(u.size() == 1, "utility_present");

    Json noTools = core::json_parse("{\"command\":\"x\"}");
    chk(tools::tools_from_cache_entry(noTools).size() == 0, "tools_absent");
    chk(tools::utility_tools_from_cache_entry(noTools).size() == 0, "utility_absent");

    Json wrongType = core::json_parse("{\"tools\":\"notalist\"}");
    chk(tools::tools_from_cache_entry(wrongType).size() == 0, "tools_wrongtype");

    disk_tests();

    if (g_fail == 0) { std::printf("all mcp-schema-cache tests passed\n"); return 0; }
    std::printf("%d failures\n", g_fail);
    return 1;
}

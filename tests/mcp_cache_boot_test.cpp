// tests/mcp_cache_boot_test.cpp — W6: a boot reads a server's manifest from disk while
// the config still hashes to what was cached, and pays the live list when it does not.
//
// The proof runs the shipped binary three times against one temp home and one server
// whose tools/list costs real milliseconds. The box's request log is the datum: "the
// second boot listed nothing" is read off the wire, not inferred from a log line the
// binary happens to print. Wall time is the second datum — a cache that saved nothing
// would show up in the clock. No tokens are spent: the endpoint answers a canned stream.
#include "fake_mcp_box.hpp"

#include <cstdio>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace {

int g_fail = 0;
void chk(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

using fake::FakeBox;

const char* kSlowMs = "400";   // tools/list costs this much; initialize does not

// The entry's key, read as text: the test asserts on what is on disk, so it does not
// need to re-derive a fingerprint the binary derived from a config it parsed itself.
std::string fingerprint_in(const std::string& cache) {
    const size_t at = cache.find("\"fingerprint\":\"");
    if (at == std::string::npos) return {};
    return cache.substr(at + 14, 16);
}

std::string config_for(int port, const std::string& mcp_path) {
    const std::string base = "http://127.0.0.1:" + std::to_string(port) + "/v1";
    // tool_search off, as in the masking proof: a deferred tool never rides the prompt,
    // so a manifest served from cache would be invisible in the body we assert on.
    return "model:\n  base_url: " + base + "\n  name: fake-model\n"
           "tool_search:\n  enabled: false\n"
           "mcp_servers:\n  fake:\n    enabled: true\n    transport: http\n"
           "    url: http://127.0.0.1:" + std::to_string(port) + mcp_path + "\n"
           "    instructions: verbatim\n";
}

void write_config(const std::string& home, const std::string& text) {
    const int f = ::open((home + "/config.yaml").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (f >= 0) { ::write(f, text.data(), text.size()); ::close(f); }
}

// The chat body is where a manifest becomes real: a cached list that registers nothing
// would pass a file-level assertion and still leave the model without the server's tools.
std::string first_chat_body(FakeBox& box) {
    std::lock_guard<std::mutex> lk(box.mu);
    return box.chat_bodies.empty() ? std::string() : box.chat_bodies.front();
}

}  // namespace

int main() {
    FakeBox box;
    box.slow_tools_ms = std::atoi(kSlowMs);
    box.start();
    if (box.port == 0) return 1;

    const std::string home = "/tmp/dd_cache_boot_home";
    ::system("rm -rf /tmp/dd_cache_boot_home && mkdir -p /tmp/dd_cache_boot_home");
    const std::string base = "http://127.0.0.1:" + std::to_string(box.port) + "/v1";
    const std::string cache = home + "/cache/mcp_schema_cache.json";
    const std::string out_path = "/tmp/dd_cache_boot.out";
    write_config(home, config_for(box.port, "/mcp"));

    // ── boot 1: nothing on disk yet, so the server is asked ──
    int status = -1;
    box.reset();
    const long t1 = fake::run_dd_tui(home, base, out_path, &status);
    const std::string cache1 = fake::slurp(cache);
    chk(box.count_tools_lists() == 1, "first boot asked the server for its tools");
    chk(box.count_initializes() >= 1, "first boot handshook");
    chk(!cache1.empty(), "first boot wrote the cache");
    chk(cache1.find("\"fake_tool\"") != std::string::npos, "the manifest is the server's own");
    chk(fingerprint_in(cache1).size() == 16, "the entry is keyed by a 16-hex fingerprint");
    struct stat st1 {};
    chk(::stat(cache.c_str(), &st1) == 0 && (st1.st_mode & 0777) == 0600, "the cache file is 0600");
    chk(t1 >= 350, "first boot paid the slow tools/list");
    const std::string body1 = first_chat_body(box);
    chk(body1.find("\"fake_tool\"") != std::string::npos, "first boot registered the tool");

    // ── boot 2: same config, so the entry on disk is the answer ──
    box.reset();
    const long t2 = fake::run_dd_tui(home, base, out_path, &status);
    const std::string cache2 = fake::slurp(cache);
    chk(box.count_tools_lists() == 0, "second boot paid no tools/list — it was served from cache");
    chk(box.count_initializes() >= 1, "second boot still handshakes: the cache saves the list, not the connect");
    chk(cache2 == cache1, "a served boot rewrites nothing");
    struct stat st2 {};
    chk(::stat(cache.c_str(), &st2) == 0 && st2.st_mtim.tv_nsec == st1.st_mtim.tv_nsec &&
        st2.st_mtim.tv_sec == st1.st_mtim.tv_sec, "the cache file kept its mtime");
    std::printf("-- boot1 %ldms · boot2 %ldms (tools/list costs %s ms)\n", t1, t2, kSlowMs);
    chk(t1 - t2 >= std::atoi(kSlowMs) / 2, "the served boot is measurably faster");
    const std::string body2 = first_chat_body(box);
    chk(body2.find("\"fake_tool\"") != std::string::npos, "a cached manifest registers the same tool");
    chk(body2.find("\"required\":[\"q\"]") != std::string::npos, "the cached schema rides untouched");
    chk(body2.find(FakeBox::kEnum) != std::string::npos, "the cached enum rides byte-identical");
    // The cache stores the server's prose as the server wrote it; masking is applied where
    // the prose enters the prompt, so a served boot masks exactly like a live one (W5).
    chk(cache1.find(FakeBox::kKey) != std::string::npos, "the cached description holds the raw prose");
    chk(body2.find(FakeBox::kKey) == std::string::npos, "a served description never rides a raw key");
    chk(body2.find(FakeBox::kKeyMasked) != std::string::npos, "a served description is masked at prompt entry");

    // ── boot 3: the config moved, so the entry must miss ──
    write_config(home, config_for(box.port, "/mcp2"));   // the box answers any /mcp* path
    box.reset();
    const long t3 = fake::run_dd_tui(home, base, out_path, &status);
    const std::string cache3 = fake::slurp(cache);
    chk(box.count_tools_lists() == 1, "a changed url is a miss: the server is asked again");
    chk(fingerprint_in(cache3) != fingerprint_in(cache1), "the new entry is keyed by the new config");
    chk(cache3.find("\"fake_tool\"") != std::string::npos, "the miss persisted a fresh manifest");
    chk(t3 >= 350, "a miss pays the round trip again");
    chk(first_chat_body(box).find("\"fake_tool\"") != std::string::npos, "the fresh manifest registers the tool");

    // A fourth boot under the moved config is served again: the key is the config, not the boot.
    box.reset();
    fake::run_dd_tui(home, base, out_path, &status);
    chk(box.count_tools_lists() == 0, "the moved config is served on its next boot");

    box.halt();

    if (g_fail == 0) {
        std::printf("mcp schema cache at boot: all ok · boot1 %ldms · boot2 %ldms · boot3 %ldms\n",
                    t1, t2, t3);
        return 0;
    }
    std::printf("%d failures · dd_tui said:\n%s\n", g_fail, fake::slurp(out_path).substr(0, 1200).c_str());
    return 1;
}

// dd::core::Home — config and state follow the folder dd was started from; the install is where the binary sits. Nothing reaches $HOME.
#include "dd/core/home.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace dd::core;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static std::set<std::string> entries(const fs::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) out.insert(e.path().string());
    return out;
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / ("dd_home_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // ---- what a plain start picks. The test binary sits in build/, which is
    // make's output: dd never adopts it, so with nothing owning a config.yaml the
    // launch folder decides. Run the suite from the project root, not from build/.
    ::unsetenv("DD_HOME");
    ::unsetenv("DAEDALUS_HOME");
    set_daedalus_home_override("");
    const fs::path exe = exe_dir();
    const fs::path start = fs::current_path();
    chk(resolve_home().root == start, "make's output folder is not adopted: the launch folder decides");
    { std::string w; chk(!resolve_home("", (exe / "x" / "config.yaml").string()).root.empty(),
        "an explicit --config inside make's output still decides: only an implicit pick is refused"); }
    chk(resolve_home().root != platform_default_daedalus_home(), "never ~/.daedalus by default");

    // ---- a folder that owns a config.yaml governs, wherever dd sits
    const fs::path project = tmp / "project";
    fs::create_directories(project);
    { std::ofstream(project / "config.yaml") << "model:\n  base_url: http://p/v1\n"; }
    chk(::chdir(project.c_str()) == 0, "dd is started from the project folder");
    chk(resolve_home().root == project, "its config.yaml governs: the home is the project folder");
    chk(get_daedalus_home() == project, "the ported modules agree on it");
    fs::remove(project / "config.yaml");
    chk(resolve_home().root == project, "a launch folder that owns nothing is still where dd works");
    chk(::chdir(start.string().c_str()) == 0, "back to where the test was started");

    // ---- the flags and the environment still outrank every folder rule
    ::setenv("DAEDALUS_HOME", (tmp / "legacy").c_str(), 1);
    chk(resolve_home().root == tmp / "legacy", "DAEDALUS_HOME still names a home");
    ::setenv("DD_HOME", (tmp / "dd").c_str(), 1);
    chk(resolve_home().root == tmp / "dd", "DD_HOME beats DAEDALUS_HOME");
    chk(get_daedalus_home() == tmp / "dd", "get_daedalus_home follows DD_HOME");
    chk(resolve_home("", (tmp / "cfgdir" / "config.yaml").string()).root == tmp / "cfgdir",
        "the folder of --config is the home");
    chk(resolve_home((tmp / "flag").string(), (tmp / "cfgdir" / "config.yaml").string()).root == tmp / "flag",
        "--home beats --config");
    chk(resolve_home("rel/dir").root.is_absolute(), "a relative --home is made absolute");
    chk(resolve_home("a/../b").root.filename() == "b", "and normalised lexically, not through symlinks");
    set_daedalus_home_override((tmp / "override").string());
    chk(get_daedalus_home() == tmp / "override", "the override beats the env for the ported modules");
    set_daedalus_home_override("");
    ::unsetenv("DD_HOME");
    ::unsetenv("DAEDALUS_HOME");

    // ---- the mix that used to happen: asking for the Python install's home
    const fs::path pyhome = platform_default_daedalus_home();
    std::string nohome;
    ::setenv("DD_HOME", pyhome.string().c_str(), 1);
    chk(resolve_home("", "", &nohome).root.empty() && !nohome.empty(),
        "DD_HOME naming ~/.daedalus is refused, with a reason");
    chk(resolve_home(pyhome.string(), "", &nohome).root.empty() && nohome.find("--home") != std::string::npos,
        "--home ~/.daedalus is refused too, and says what asked");
    ::unsetenv("DD_HOME");
    chk(resolve_home().root != pyhome, "nothing names ~/.daedalus by default");

    const Home h = resolve_home((tmp / "box").string());
    chk(h.config() == tmp / "box" / "config.yaml" && h.skills() == tmp / "box" / "skills" &&
            h.rules() == tmp / "box" / "rules" && h.state() == tmp / "box" / "state" &&
            h.cache() == tmp / "box" / "cache" && h.logs() == tmp / "box" / "logs" &&
            h.skins() == tmp / "box" / "skins",
        "the layout");

    // ---- a fresh home: nothing reaches into $HOME
    const fs::path fake_home = tmp / "HOME";
    fs::create_directories(fake_home);
    ::setenv("HOME", fake_home.c_str(), 1);
    const auto before = entries(fake_home);
    std::string why;
    chk(ensure_home(h, &why), "ensure_home: " + why);
    chk(fs::is_directory(h.state()) && fs::is_directory(h.cache()) && fs::is_directory(h.logs()), "working folders exist");
    chk(!fs::exists(h.skills()) && !fs::exists(h.rules()), "skills/ and rules/ are the operator's: not made for them");
    chk(init_config(h, "", &why), "init_config writes a config: " + why);
    chk(fs::exists(h.config()), "config.yaml is beside everything else");
    chk(slurp(h.config()).find("base_url") != std::string::npos, "the default config names a model endpoint");
    chk(slurp(h.config()).find("knowledge:") != std::string::npos, "the default config carries the knowledge section");
    chk(!init_config(h, "", &why), "init never overwrites");
    chk(entries(fake_home) == before, "nothing was created under $HOME");

    // ---- --from copies an existing config once
    const Home h2 = resolve_home((tmp / "second").string());
    { std::ofstream(tmp / "mine.yaml") << "model:\n  base_url: http://x/v1\n"; }
    chk(init_config(h2, (tmp / "mine.yaml").string(), &why), "init --from: " + why);
    chk(slurp(h2.config()) == "model:\n  base_url: http://x/v1\n", "--from copied byte for byte");
    chk(!init_config(resolve_home((tmp / "third").string()), (tmp / "missing.yaml").string(), &why) && !why.empty(),
        "init --from a missing file says why");

    // ---- the example config in the repo is the embedded default (generated from it)
    for (const char* rel : {"examples/ariadne/config.yaml", "../examples/ariadne/config.yaml"})
        if (fs::exists(rel)) {
            chk(slurp(rel) == default_config(), "examples/ariadne/config.yaml is the embedded default");
            break;
        }

    fs::remove_all(tmp);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all home tests passed\n");
    return g_fail ? 1 : 0;
}

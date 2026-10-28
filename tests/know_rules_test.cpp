// dd::know tripwires — rules that cost nothing until a pattern fires.
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "dd/know/tripwire.hpp"

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::know;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", label.c_str()); }
}
static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

static core::ToolCall call(const std::string& tool, const std::string& args) {
    core::ToolCall c;
    c.id = "c";
    c.name = tool;
    c.arguments = args;
    return c;
}
static core::ToolCall cmd(const std::string& command) {
    core::Json a = core::json_obj({{"cmd", std::make_shared<core::Json>(core::json_str(command))}});
    return call("run_terminal", core::json_dump(a));
}

static Rule rule(const std::string& name, const std::string& condition, const std::string& tool = "", bool once = true, int gap = 0) {
    Rule r;
    r.name = name;
    r.condition = condition;
    r.tool = tool;
    r.once = once;
    r.repeat_gap = gap;
    r.text = "text of " + name;
    return r;
}

static void test_parse() {
    Rule r;
    std::string why;
    chk(Tripwires::parse("podman-not-docker.md",
                         "---\nname: podman-not-docker\ndescription: podman only\ncondition: '(^|[\\s;&|(])docker\\b'\nscope: tool:run_terminal\nonce: true\nrepeat_gap: 20\n---\n"
                         "This box runs podman.\nUse it.\n",
                         &r, &why),
        "parse: " + why);
    chk(r.name == "podman-not-docker" && r.tool == "run_terminal" && r.once && r.repeat_gap == 20 && r.description == "podman only", "parse: fields");
    chk(r.condition == "(^|[\\s;&|(])docker\\b", "parse: single quotes keep the backslashes");
    chk(r.text == "This box runs podman.\nUse it.", "parse: the body is the text");
    chk(Tripwires::parse("only-a-name.md", "---\ncondition: foo\n---\nbar\n", &r, &why) && r.name == "only-a-name", "parse: the file's name when there is none");
    chk(!Tripwires::parse("nocond.md", "---\nname: x\n---\ntext\n", &r, &why) && has(why, "no condition"), "parse: no condition, no rule");
    chk(!Tripwires::parse("notext.md", "---\nname: x\ncondition: foo\n---\n", &r, &why), "parse: nothing to say, no rule");
    chk(Tripwires::parse("d.md", "---\ncondition: foo\ndescription: the words\n---\n", &r, &why) && r.text == "the words", "parse: the description speaks when there is no body");
    chk(Tripwires::parse("o.md", "---\ncondition: foo\nonce: false\n---\nx\n", &r, &why) && !r.once, "parse: once: false");
    chk(Tripwires::parse("s.md", "---\ncondition: foo\nscope: tool\n---\nx\n", &r, &why) && r.tool.empty(), "parse: scope: tool is any tool");

    Tripwires t;
    chk(!t.add(rule("bad", "(unclosed"), &why) && has(why, "bad"), "add: a broken regex is refused, with the rule's name");
    chk(t.size() == 0, "add: and not kept");
    chk(t.add(rule("a", "foo")) && t.add(rule("a", "bar")) && t.size() == 1, "add: a rule of the same name replaces the earlier one");
}

static void test_check() {
    Tripwires t;
    int turn = 0;
    t.set_clock([&] { return turn; });
    t.add(rule("once", "docker", "run_terminal", true, 0));
    t.add(rule("gap", "git push", "run_terminal", true, 3));
    t.add(rule("always", "rm -rf /tmp/x", "run_terminal", false));
    t.add(rule("anytool", "SECRET-TOKEN", ""));
    t.add(rule("icase", "(?i)forbidden\\s+word", "run_terminal"));

    chk(t.check(cmd("ls -la")).empty(), "check: an innocent call trips nothing");
    auto f = t.check(cmd("docker ps"));
    chk(f.size() == 1 && f[0]->name == "once", "check: a matching call trips its rule");
    chk(t.check(cmd("docker ps")).empty(), "check: told once: the retry passes");
    chk(t.check(cmd("docker run x")).empty(), "check: ... for the whole session");

    chk(t.check(cmd("git push origin x")).size() == 1, "gap: speaks");
    turn = 2;
    chk(t.check(cmd("git push origin x")).empty(), "gap: not again within the gap");
    turn = 3;
    chk(t.check(cmd("git push origin x")).size() == 1, "gap: after the gap it speaks again");

    for (int i = 0; i < 3; ++i) chk(t.check(cmd("rm -rf /tmp/x")).size() == 1, "always: once: false refuses every time " + std::to_string(i));

    chk(t.check(call("write_file", "{\"path\":\"/a\",\"content\":\"docker\"}")).empty(), "scope: a rule for run_terminal ignores another tool");
    chk(t.check(call("write_file", "{\"path\":\"/a\",\"content\":\"has SECRET-TOKEN in it\"}")).size() == 1, "scope: a rule for any tool sees every tool's arguments");
    chk(t.check(call("run_terminal", "{\"cmd\":\"ls\",\"intent\":\"Not docker at all\"}")).empty() == true, "match: the model's own words (intent) are not what is matched");
    chk(t.check(cmd("echo FORBIDDEN   Word")).size() == 1, "icase: a leading (?i) means case-insensitive");
    chk(t.check(call("run_terminal", "{\"cmd\":\"echo broken")).empty(), "check: arguments cut off mid-string still match against what is there (nothing here)");
    core::Json nested = core::json_obj({{"args", std::make_shared<core::Json>(core::json_arr({std::make_shared<core::Json>(core::json_str("SECRET-TOKEN"))}))}});
    chk(Tripwires().check(call("x", core::json_dump(nested))).empty(), "check: no rules, nothing");
    Tripwires u;
    u.add(rule("deep", "SECRET-TOKEN"));
    chk(u.check(call("x", core::json_dump(nested))).size() == 1, "match: strings inside arrays and objects are matched too");
}

static void test_starters() {
    Tripwires t;
    int turn = 0;
    t.set_clock([&] { return turn; });
    for (const auto& [file, text] : Tripwires::starter_rules()) {
        Rule r;
        std::string why;
        chk(Tripwires::parse(file, text, &r, &why), "starter " + file + ": " + why);
        chk(t.add(r, &why), "starter " + file + " compiles: " + why);
    }
    chk(t.size() == 6, "starters: six rules");

    // each rule on what it is for, and on what it is not for (a fresh engine each time: told once)
    struct Case {
        const char* rule;
        const char* hits;
        const char* misses;
    };
    const Case cases[] = {
        {"podman-not-docker", "docker ps -a", "podman ps && cat Dockerfile"},
        {"podman-not-docker", "cd x && docker compose up", "echo podman-compose"},
        {"no-ai-trailers", "git commit -m \"fix\n\nCo-Authored-By: Claude <x@y>\"", "git commit -m \"fix the trailer parser\""},
        {"no-ai-trailers", "git commit -m 'x' -m 'Claude-Session: https://a/b'", "git log --oneline"},
        {"no-commit-all-no-verify", "git commit -a -m x", "git commit -m \"add all the things\""},
        {"no-commit-all-no-verify", "git commit -am \"x\"", "git commit --amend -m x"},
        {"no-commit-all-no-verify", "git commit --no-verify -m x", "git add a.cpp && git commit -m x"},
        {"no-force-push-main", "git push --force origin main", "git push origin feature"},
        {"no-force-push-main", "git push -f origin main", "git push --force origin feature-x"},
        {"never-touch-live-pods", "podman stop mazemaker-db", "podman ps && podman logs x"},
        {"never-touch-live-pods", "systemctl --user restart mazemaker-pod.service", "systemctl --user status mazemaker-pod.service"},
        {"ask-before-heavy-launches", "./llama-server -m x.gguf", "make build/ariadne"},
        {"ask-before-heavy-launches", "make test", "echo testing"},
    };
    for (const auto& c : cases) {
        Tripwires one;
        one.set_clock([&] { return turn; });
        for (const auto& [file, text] : Tripwires::starter_rules()) {
            Rule r;
            if (Tripwires::parse(file, text, &r, nullptr) && r.name == c.rule) one.add(r);
        }
        const auto hit = one.check(cmd(c.hits));
        chk(hit.size() == 1 && hit[0]->name == c.rule, std::string("starter ") + c.rule + " holds: " + c.hits);
        Tripwires two;
        for (const auto& [file, text] : Tripwires::starter_rules()) {
            Rule r;
            if (Tripwires::parse(file, text, &r, nullptr) && r.name == c.rule) two.add(r);
        }
        chk(two.check(cmd(c.misses)).empty(), std::string("starter ") + c.rule + " lets through: " + c.misses);
    }
    // the force-push rule is a hard one: it speaks every time
    Tripwires hard;
    for (const auto& [file, text] : Tripwires::starter_rules()) {
        Rule r;
        if (Tripwires::parse(file, text, &r, nullptr) && r.name == "no-force-push-main") hard.add(r);
    }
    chk(hard.check(cmd("git push --force origin main")).size() == 1 && hard.check(cmd("git push --force origin main")).size() == 1, "starter: force-push to main is refused every time");
}

static void test_files_and_gate(const fs::path& tmp) {
    const fs::path rules = tmp / "rules";
    fs::create_directories(rules);
    std::ofstream(rules / "b-second.md") << "---\ncondition: 'two'\n---\nsecond\n";
    std::ofstream(rules / "a-first.md") << "---\nname: first\ncondition: 'one'\nscope: tool:run_terminal\n---\nfirst\n";
    std::ofstream(rules / "broken.md") << "---\nname: broken\ncondition: '(unclosed'\n---\nx\n";
    std::ofstream(rules / "nocond.md") << "---\nname: nocond\n---\nx\n";
    std::ofstream(rules / "notes.txt") << "not a rule";
    Tripwires t;
    chk(t.load_dir(rules) == 2 && t.size() == 2, "load_dir: two good rules of five files");
    const auto names = t.names();
    chk(names.size() == 2 && names[0] == "first" && names[1] == "b-second", "load_dir: in file order, named by the file or the frontmatter");
    chk(t.load_dir(tmp / "nowhere") == 0, "load_dir: a missing folder is no rules");

    TripwireGate gate(t);
    std::string fired;
    gate.on_fire = [&](const std::string& n) { fired = n; };
    chk(!gate.veto(cmd("ls")), "gate: an innocent call passes");
    const auto v = gate.veto(cmd("echo one"));
    chk(v.has_value(), "gate: a matching call is refused");
    chk(v && has(*v, "\"error\"") && has(*v, "[rule first") && has(*v, "first") && has(*v, "This call was not run"), "gate: the refusal is an error result naming the rule and the way on: " + (v ? *v : ""));
    chk(has(fired, "first") && has(fired, "run_terminal"), "gate: the operator is told: " + fired);
    chk(!gate.veto(cmd("echo one")), "gate: the retry passes");
    const auto both = gate.veto(call("write_file", "{\"content\":\"two\"}"));
    chk(both && has(*both, "second"), "gate: a rule with no tool scope sees a write too");
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / ("dd_know_rules_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    test_parse();
    test_check();
    test_starters();
    test_files_and_gate(tmp);
    fs::remove_all(tmp);
    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("all know rules tests passed\n");
    return g_fail ? 1 : 0;
}

// Verifies dd::tools::apply_v4a_patch / fs_edit — the V4A application layer.
// The contract under test is not "does the edit land" but "does a patch that cannot be applied
// land nothing at all", and whether the failure names the hunk the model has to rewrite.
#include "dd/tools/patch_apply.hpp"
#include "dd/tools/skill_provenance.hpp"

#include "dd/core/json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using namespace dd;
using namespace dd::tools;
using namespace dd::core;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            std::cerr << "FAIL: " << msg << "\n";          \
            ++failures;                                    \
        }                                                  \
    } while (0)

static void put(const std::string& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

static std::string get(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static bool exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// The reply of fs_edit, parsed.
static Json reply(const std::string& s) { return json_parse(s); }
// Each helper binds the parsed reply to a named value: json_get returns a pointer *into* it, and a
// temporary dies before the pointer is read.
static bool failed(const std::string& s) {
    const Json j = reply(s);
    return json_get(j, "error") != nullptr;
}
static std::string err_text(const std::string& s) {
    const Json j = reply(s);
    const Json* e = json_get(j, "error");
    return e && e->type == Json::Str ? e->str : std::string();
}
static int applied(const std::string& s) {
    const Json j = reply(s);
    const Json* a = json_get(j, "applied");
    return a && a->type == Json::Num ? static_cast<int>(a->num) : -1;
}
static std::string notes_joined(const std::string& s) {
    const Json j = reply(s);
    const Json* n = json_get(j, "notes");
    std::string out;
    if (n && n->type == Json::Arr)
        for (const auto& e : n->arr) if (e->type == Json::Str) out += e->str + "; ";
    return out;
}

// Aggregate-init of a struct with defaulted members trips -Werror=missing-field-initializers, so the
// options are built by name.
static ApplyOptions opts(const std::string& root) {
    ApplyOptions o;
    o.root = root;
    return o;
}

static std::string run(const std::string& patch, const ApplyOptions& opt) {
    Json args = json_obj({{"patch", std::make_shared<Json>(json_str(patch))}});
    return fs_edit(args, opt);
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "dd_patch_apply_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    const std::string root = tmp.string();

    // --- Update, exact match ---
    put(root + "/a.txt", "one\ntwo\nthree\n");
    auto r = run("*** Begin Patch\n*** Update File: " + root + "/a.txt\n@@ two @@\n-two\n+TWO\n*** End Patch", opts(root));
    CHECK(!failed(r), "exact hunk applied");
    CHECK(get(root + "/a.txt") == "one\nTWO\nthree\n", "content after exact hunk");
    CHECK(notes_joined(r).find("matched 1 place(s)") != std::string::npos, "note reports the match count");
    CHECK(applied(r) == 1, "one file applied");

    // --- Update, whitespace drift absorbed by the ladder ---
    put(root + "/b.txt", "def greet(name):\n    print(name)\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/b.txt\n@@ greet @@\n-def greet(name):\n-    print(name)\n+def greet(name):\n+    return name\n*** End Patch", opts(root));
    CHECK(!failed(r), "indented hunk applied through the ladder");
    CHECK(get(root + "/b.txt") == "def greet(name):\n    return name\n", "content after ladder match");

    // --- Update that does not match: names the file and the hunk, writes nothing ---
    put(root + "/c.txt", "alpha\nbeta\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/c.txt\n@@ nowhere @@\n-gamma\n+GAMMA\n*** End Patch", opts(root));
    CHECK(failed(r), "no match is an error");
    CHECK(err_text(r).find("hunk 1") != std::string::npos, "error names the hunk");
    CHECK(err_text(r).find("hint: nowhere") != std::string::npos, "error echoes the hint");
    CHECK(get(root + "/c.txt") == "alpha\nbeta\n", "no match leaves the file untouched");
    CHECK(applied(r) == 0, "nothing applied");

    // --- Ambiguous match: refused, never resolved by picking a candidate ---
    put(root + "/d.txt", "x\nsame\nsame\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/d.txt\n@@ same @@\n-same\n+other\n*** End Patch", opts(root));
    CHECK(failed(r), "two matches is an error");
    CHECK(err_text(r).find("2 matches") != std::string::npos, "error counts the matches");
    CHECK(get(root + "/d.txt") == "x\nsame\nsame\n", "ambiguous patch writes nothing");

    // --- A patch of two files where the second hunk fails: the first file is NOT written ---
    put(root + "/e1.txt", "one\n");
    put(root + "/e2.txt", "two\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/e1.txt\n-one\n+ONE\n*** Update File: " + root + "/e2.txt\n-nope\n+NOPE\n*** End Patch", opts(root));
    CHECK(failed(r), "the failing hunk fails the patch");
    CHECK(get(root + "/e1.txt") == "one\n", "the good hunk in the same patch wrote nothing");
    CHECK(get(root + "/e2.txt") == "two\n", "the failing file untouched");
    CHECK(applied(r) == 0, "all-or-nothing across files");

    // --- Add ---
    r = run("*** Begin Patch\n*** Add File: " + root + "/new.txt\n+first\n+second\n*** End Patch", opts(root));
    CHECK(!failed(r), "add applied");
    CHECK(get(root + "/new.txt") == "first\nsecond\n", "added content");
    r = run("*** Begin Patch\n*** Add File: " + root + "/new.txt\n+third\n*** End Patch", opts(root));
    CHECK(failed(r), "add over an existing file is refused");
    CHECK(get(root + "/new.txt") == "first\nsecond\n", "refused add left the file alone");

    // --- Two operations on one file compose (the second sees the first) ---
    put(root + "/f.txt", "one\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/f.txt\n@@ one @@\n-one\n+ONE\n*** Update File: " + root + "/f.txt\n@@ ONE @@\n-ONE\n+FINAL\n*** End Patch", opts(root));
    CHECK(!failed(r), "two hunks on one file both apply");
    CHECK(get(root + "/f.txt") == "FINAL\n", "second hunk continued from the first");

    // --- Context-only hunk: no change, and the note says why ---
    put(root + "/g.txt", "keep\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/g.txt\n@@ keep @@\n keep\n*** End Patch", opts(root));
    CHECK(!failed(r), "context-only hunk is not an error");
    CHECK(get(root + "/g.txt") == "keep\n", "context-only hunk changed nothing");
    CHECK(notes_joined(r).find("context only") != std::string::npos, "note says context only");

    // --- Add-only hunk: appended, and said so ---
    put(root + "/h.txt", "head\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/h.txt\n@@ tail @@\n+tail\n*** End Patch", opts(root));
    CHECK(!failed(r), "add-only hunk applied");
    CHECK(get(root + "/h.txt") == "head\ntail\n", "add-only hunk appended at the end");
    CHECK(notes_joined(r).find("appended") != std::string::npos, "note says appended");

    // --- Context plus additions, no removals: the context is the anchor, not the end of file ---
    put(root + "/i.txt", "1. first\n2. second\n3. third\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/i.txt\n@@ 2. second @@\n 2. second\n 3. third\n+4. fourth\n*** End Patch", opts(root));
    CHECK(!failed(r), "insert-after-context hunk applied");
    CHECK(get(root + "/i.txt") == "1. first\n2. second\n3. third\n4. fourth\n", "additions landed after the context, not at EOF");
    CHECK(notes_joined(r).find("appended") == std::string::npos, "note does not call it an append");
    CHECK(notes_joined(r).find("inserted") != std::string::npos, "note says inserted");

    // --- Additions ordered before the context keep that order ---
    put(root + "/j.txt", "alpha\nbeta\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/j.txt\n@@ beta @@\n+zero\n beta\n*** End Patch", opts(root));
    CHECK(!failed(r), "insert-before-context hunk applied");
    CHECK(get(root + "/j.txt") == "alpha\nzero\nbeta\n", "additions landed before the context");

    // --- Unmatched context never silently appends ---
    put(root + "/k.txt", "only line\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/k.txt\n@@ missing @@\n missing\n+extra\n*** End Patch", opts(root));
    CHECK(failed(r), "unmatched context with additions is an error, not an append");
    CHECK(get(root + "/k.txt") == "only line\n", "no match left the bytes alone");
    CHECK(err_text(r).find("hunk 1") != std::string::npos, "failure names the hunk");
    CHECK(err_text(r).find("missing") != std::string::npos, "failure echoes the hint");

    // --- Delete and Move are refused without allow_delete ---
    put(root + "/del.txt", "gone\n");
    r = run("*** Begin Patch\n*** Delete File: " + root + "/del.txt\n*** End Patch", opts(root));
    CHECK(failed(r), "delete refused without allow_delete");
    CHECK(err_text(r).find("allow_delete") != std::string::npos, "error says what to set");
    CHECK(exists(root + "/del.txt"), "refused delete left the file");
    ApplyOptions allow = opts(root);
    allow.allow_delete = true;
    r = run("*** Begin Patch\n*** Delete File: " + root + "/del.txt\n*** End Patch", allow);
    CHECK(!failed(r), "delete with allow_delete applied");
    CHECK(!exists(root + "/del.txt"), "file removed");

    // --- A Move carrying hunks is refused: the format drops them, and success would be a lie ---
    put(root + "/mv.txt", "old\nkeep\n");
    r = run("*** Begin Patch\n*** Move File: " + root + "/mv.txt -> " + root + "/mv2.txt\n@@ keep @@\n-old\n+NEW\n*** End Patch", opts(root));
    CHECK(failed(r), "move without allow_delete is refused");
    CHECK(exists(root + "/mv.txt") && !exists(root + "/mv2.txt"), "refused move changed nothing");
    r = run("*** Begin Patch\n*** Move File: " + root + "/mv.txt -> " + root + "/mv2.txt\n@@ keep @@\n-old\n+NEW\n*** End Patch", allow);
    CHECK(failed(r), "a Move with hunks is refused, not applied");
    CHECK(err_text(r).find("takes no hunks") != std::string::npos, "error says why the hunks are dropped");
    CHECK(err_text(r).find("then Update") != std::string::npos, "error says what to do instead");
    CHECK(exists(root + "/mv.txt") && !exists(root + "/mv2.txt"), "refused move-with-hunks changed nothing");

    // --- Move, no hunks: body moved verbatim, destination written before the source is removed ---
    r = run("*** Begin Patch\n*** Move File: " + root + "/mv.txt -> " + root + "/mv2.txt\n*** End Patch", allow);
    CHECK(!failed(r), "plain move applied");
    CHECK(get(root + "/mv2.txt") == "old\nkeep\n", "moved body is verbatim");
    CHECK(!exists(root + "/mv.txt"), "source removed after the destination exists");
    put(root + "/mv3.txt", "occupied\n");
    r = run("*** Begin Patch\n*** Move File: " + root + "/mv2.txt -> " + root + "/mv3.txt\n*** End Patch", allow);
    CHECK(failed(r), "move to an existing destination is refused");
    CHECK(get(root + "/mv2.txt") == "old\nkeep\n" && get(root + "/mv3.txt") == "occupied\n", "refused move left both files");

    // --- Move out of the root is refused ---
    r = run("*** Begin Patch\n*** Move File: " + root + "/mv2.txt -> /tmp/dd_move_outside.txt\n*** End Patch", allow);
    CHECK(failed(r), "move outside the root is refused");
    CHECK(err_text(r).find("escapes allowed directory") != std::string::npos, "error names the escape");
    CHECK(exists(root + "/mv2.txt") && !exists("/tmp/dd_move_outside.txt"), "refused move changed nothing");

    // --- '..' in any path is refused ---
    put(root + "/../escape.txt", "nope\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/../escape.txt\n-nope\n+NOPE\n*** End Patch", opts(root));
    CHECK(failed(r), "traversal refused");
    CHECK(err_text(r).find("'..'") != std::string::npos, "error names the traversal");
    const std::string esc = get(root + "/../escape.txt");
    CHECK(esc == "nope\n", "traversal wrote nothing (got: [" + esc + "])");
    fs::remove(root + "/../escape.txt", ec);

    // --- A binary file is refused, not edited ---
    put(root + "/bin.dat", std::string("abc\0def", 7));
    r = run("*** Begin Patch\n*** Update File: " + root + "/bin.dat\n-abc\n+xyz\n*** End Patch", opts(root));
    CHECK(failed(r), "binary file refused");
    CHECK(err_text(r).find("binary") != std::string::npos, "error says binary");

    // --- Provenance: a learned skill the model edited records who edited it, body preserved ---
    fs::create_directories(tmp / "skills" / "learned", ec);
    const std::string skill = (tmp / "skills" / "learned" / "SKILL.md").string();
    const std::string body = "---\nname: flow-x\norigin: learned\nstate: active\n---\n\n# Steps\n1. do the thing\n";
    put(skill, body);
    ApplyOptions prov = opts(root);
    prov.skills_root = (tmp / "skills").string();
    prov.origin = "model_edit";
    prov.now = 1730000000;
    r = run("*** Begin Patch\n*** Update File: " + skill + "\n@@ do the thing @@\n-1. do the thing\n+1. do the thing, then measure\n*** End Patch", prov);
    CHECK(!failed(r), "learned skill edited");
    const std::string after = get(skill);
    CHECK(after.find("edited_by: model_edit") != std::string::npos, "edited_by recorded");
    CHECK(after.find("edited_at: 1730000000") != std::string::npos, "edited_at recorded");
    CHECK(after.find("origin: learned") != std::string::npos, "origin kept");
    CHECK(after.find("# Steps\n1. do the thing, then measure\n") != std::string::npos, "body edited");
    CHECK(after.find("state: active") != std::string::npos, "frontmatter kept");
    CHECK(after.find("1. do the thing\n") == std::string::npos, "old line gone");

    // --- A skill the learn loop authored is annotated; a hand-written one is not ---
    const std::string hand = (tmp / "skills" / "reference" / "SKILL.md").string();
    fs::create_directories(tmp / "skills" / "reference", ec);
    put(hand, "---\nname: ref-y\norigin: curated\n---\n\n# Notes\nbody text\n");
    r = run("*** Begin Patch\n*** Update File: " + hand + "\n@@ body text @@\n-body text\n+new body\n*** End Patch", prov);
    CHECK(!failed(r), "curated skill edited");
    CHECK(get(hand).find("edited_by") == std::string::npos, "non-learned skill not annotated");
    CHECK(get(hand) == "---\nname: ref-y\norigin: curated\n---\n\n# Notes\nnew body\n", "curated body edited verbatim");

    // --- Outside the skills root, nothing is annotated ---
    put(root + "/plain.txt", "---\nname: z\norigin: learned\n---\nline\n");
    r = run("*** Begin Patch\n*** Update File: " + root + "/plain.txt\n@@ line @@\n-line\n+newline\n*** End Patch", prov);
    CHECK(!failed(r), "plain file edited");
    CHECK(get(root + "/plain.txt") == "---\nname: z\norigin: learned\n---\nnewline\n", "plain file not annotated");

    // --- Garbage in: the parse error is the report ---
    r = run("not a patch at all", opts(root));
    CHECK(failed(r), "non-patch text is an error");
    CHECK(applied(r) == 0, "nothing applied");

    // --- Provenance is a stack: a tool call inside a learn-loop write attributes to the tool ---
    const int outer = dd::tools::set_current_write_origin("learn_loop");
    {
        dd::tools::WriteOriginScope inner("tool_call");
        CHECK(dd::tools::get_current_write_origin() == "tool_call", "origin nests");
    }
    CHECK(dd::tools::get_current_write_origin() == "learn_loop", "origin returns to the outer writer");
    dd::tools::reset_current_write_origin(outer);
    CHECK(dd::tools::get_current_write_origin() == "foreground", "reset clears back to the default writer");

    fs::remove_all(tmp, ec);
    std::cout << (failures ? "FAILURES: " + std::to_string(failures) : "ok") << "\n";
    return failures ? 1 : 0;
}

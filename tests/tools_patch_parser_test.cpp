// Tests for the pure V4A patch parser.
#include "dd/tools/patch_parser.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace dd;
using namespace dd::tools;

static int g_fail = 0;
static void chk(bool ok, const std::string& label) {
    if (!ok) { std::printf("FAIL %s\n", label.c_str()); ++g_fail; }
}

static std::string dump(const std::vector<PatchOperation>& ops) {
    std::string s;
    for (const auto& op : ops) {
        s += "op=";
        switch (op.operation) {
            case OperationType::ADD: s += "add"; break;
            case OperationType::UPDATE: s += "update"; break;
            case OperationType::DELETE: s += "delete"; break;
            case OperationType::MOVE: s += "move"; break;
        }
        s += " file=" + op.file_path;
        s += " new=" + (op.new_path ? *op.new_path : std::string("<null>"));
        s += " content=" + (op.content ? *op.content : std::string("<null>"));
        for (const auto& h : op.hunks) {
            s += " hunk[hint=" + (h.context_hint ? *h.context_hint : std::string("<null>")) + "]";
            for (const auto& hl : h.lines) s += " {" + hl.prefix + "|" + hl.content + "}";
        }
        s += "\n";
    }
    return s;
}

int main() {
    // P1: update with a hunk; @@ hint without closing @@ -> None
    std::string p1 =
        "*** Begin Patch\n"
        "*** Update File: a/b.py\n"
        "@@ def foo():\n"
        "+    new = 1\n"
        "-    old = 2\n"
        "     keep = 3\n"
        "*** End Patch";
    {
        auto [ops, err] = parse_v4a_patch(p1);
        chk(!err.has_value(), "P1.err");
        chk(ops.size() == 1, "P1.n");
        std::string exp = "op=update file=a/b.py new=<null> content=<null> hunk[hint=<null>] {+|    new = 1} {-|    old = 2} { |    keep = 3}\n";
        chk(dump(ops) == exp, "P1.dump");
    }

    // P2: add + delete + move
    std::string p2 =
        "*** Add File: new/x.txt\n"
        "+hello\n"
        "+world\n"
        "*** Delete File: gone/y.txt\n"
        "*** Move File: old/z.py -> new/z.py\n"
        "*** End Patch";
    {
        auto [ops, err] = parse_v4a_patch(p2);
        chk(!err.has_value(), "P2.err");
        chk(ops.size() == 3, "P2.n");
        std::string exp =
            "op=add file=new/x.txt new=<null> content=<null> hunk[hint=<null>] {+|hello} {+|world}\n"
            "op=delete file=gone/y.txt new=<null> content=<null>\n"
            "op=move file=old/z.py new=new/z.py content=<null>\n";
        chk(dump(ops) == exp, "P2.dump");
    }

    // P3: no begin/end markers
    std::string p3 = "*** Update File: f.py\n@@\n+1\n";
    {
        auto [ops, err] = parse_v4a_patch(p3);
        chk(!err.has_value(), "P3.err");
        chk(ops.size() == 1, "P3.n");
        std::string exp = "op=update file=f.py new=<null> content=<null> hunk[hint=<null>] {+|1}\n";
        chk(dump(ops) == exp, "P3.dump");
    }

    // P4: backslash no-newline line skipped; bare content -> ' ' prefix
    std::string p4 =
        "*** Begin Patch\n"
        "*** Update File: g.py\n"
        "@@\n"
        "\\ No newline at end of file\n"
        "+added\n"
        "weird line\n"
        "*** End Patch";
    {
        auto [ops, err] = parse_v4a_patch(p4);
        chk(!err.has_value(), "P4.err");
        chk(ops.size() == 1, "P4.n");
        std::string exp = "op=update file=g.py new=<null> content=<null> hunk[hint=<null>] {+|added} { |weird line}\n";
        chk(dump(ops) == exp, "P4.dump");
    }

    // P5: empty
    {
        auto [ops, err] = parse_v4a_patch("");
        chk(!err.has_value(), "P5.err");
        chk(ops.empty(), "P5.n");
    }

    // P6: no markers
    {
        auto [ops, err] = parse_v4a_patch("no markers here");
        chk(!err.has_value(), "P6.err");
        chk(ops.empty(), "P6.n");
    }

    if (g_fail == 0) { std::printf("all patch-parser tests passed\n"); return 0; }
    std::printf("%d failures\n", g_fail);
    return 1;
}

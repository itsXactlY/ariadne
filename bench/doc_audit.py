#!/usr/bin/env python3
"""Doc honesty, measured. Two checks over the tree, run after every wave.

1. WIRING. Expand the Makefile's product recipes (dd, dd_agent, dd_tui), take the
   transitive `#include "..."` closure from those TUs, and derive every `src/*.cpp`
   linked into no product binary and every `include/dd/**/*.hpp` reached by none.
   Compare that against what README lists under "Ported but not wired": a module the
   README calls unwired but a product binary reaches is a falsified claim, and an
   unwired module README does not name is an undeclared one.

2. CITATIONS. Every `path[:line[-line]]` inside backticks in README.md and docs/*.md
   must resolve to a file in the tree (exact path, or a basename unique in the tree),
   the range must sit inside the file, and the cited lines must share a token with the
   doc line that cites them. A range that resolves onto unrelated code is anchor drift
   — the same failure as the wiring list, one level down.

Not tree files, so not citable: runtime artifacts beside the binary (`config.yaml`,
`brief.md`, `report.md`, the `_test.cpp` suffix), URIs (`skill://…`), and provenance
paths that belong to the Python harness — those are written `py:` and skipped here.

  python3 bench/doc_audit.py            # drift report; exit 0 when clean
"""
import glob
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
RUNTIME = {"config.yaml", "brief.md", "report.md", "SKILL.md"}
SUFFIX = re.compile(r"^_?test\.cpp$|^.*_test\.cpp$")

CIT = re.compile(r"`([A-Za-z0-9_./-]+\.(?:cpp|hpp|md|yaml|py))(?::(\d+)(?:-(\d+))?)?`")
TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_.]{3,}|[0-9]{2,}")


def make_vars():
    text = pathlib.Path(ROOT / "Makefile").read_text()
    for f in sorted((ROOT / "mk").glob("*.mk")):
        text += "\n" + f.read_text()
    return {m.group(1): m.group(2) for m in re.finditer(r"([A-Z0-9_]+)\s*:?=\s*(.+)", text)}


def expand(s, vars_, depth=0):
    def wildcard(m):
        g = m.group(1)
        return " ".join(sorted(glob.glob(g)) or sorted(glob.glob(str(ROOT / g))))

    for _ in range(7):
        s = re.sub(r"\$\(([A-Z0-9_]+)\)", lambda m: expand(vars_.get(m.group(1), ""), vars_, depth + 1), s)
        s = re.sub(r"\$(?![{(])([A-Z0-9_]+)", lambda m: expand(vars_.get(m.group(1), ""), vars_, depth + 1), s)
        s = re.sub(r"\$\(wildcard\s+([^)]+)\)", wildcard, s)
        s = re.sub(r"\$\(sort\s+([^)]+)\)", lambda m: " ".join(sorted(set(m.group(1).split()))), s)
    return s


def rel(p):
    try:
        return str(pathlib.Path(p).resolve().relative_to(ROOT))
    except ValueError:
        return str(p)


def product_tus(vars_):
    text = pathlib.Path(ROOT / "Makefile").read_text()
    out = {}
    for m in re.finditer(r"\$\(CXX\)(.*?)-o\s+\$?\(?(?:BUILD|OUT)\)?/(\w+)", text):
        if m.group(2) not in ("dd", "dd_agent", "dd_tui"):
            continue
        out.setdefault(m.group(2), set()).update(rel(w) for w in expand(m.group(1), vars_).split() if w.endswith(".cpp"))
    return out


def resolve_include(from_file, path):
    for c in ([os.path.join("include", path)] if path.startswith("dd/") else []) + \
             [os.path.normpath(os.path.join(os.path.dirname(from_file), path)), os.path.normpath(path)]:
        if (ROOT / c).exists():
            return rel(c)
    return None


def closure(tus):
    inc = re.compile(r'#include\s+"([^"]+)"')
    seen, stack = set(), list(tus)
    while stack:
        f = stack.pop()
        if f in seen or not (ROOT / f).exists():
            continue
        seen.add(f)
        for m in inc.finditer((ROOT / f).read_text(errors="replace")):
            g = resolve_include(f, m.group(1))
            if g:
                stack.append(g)
    return seen


def wiring():
    vars_ = make_vars()
    prod = product_tus(vars_)
    reached = closure({f for s in prod.values() for f in s})
    src_tus = [rel(f) for f in glob.glob(str(ROOT / "src/**/*.cpp"), recursive=True)]
    hdrs = [rel(f) for f in glob.glob(str(ROOT / "include/dd/**/*.hpp"), recursive=True)]
    unwired = sorted(f for f in src_tus if f not in reached)
    unreached = sorted(h for h in hdrs if h not in reached)
    header_only = sorted(h for h in hdrs if not (ROOT / ("src/" + h[len("include/dd/"):-4] + ".cpp")).exists())
    return prod, src_tus, hdrs, reached, unwired, unreached, header_only


def readme_list():
    """The names README lists under 'Ported but not wired', keyed by directory."""
    line = next((l for l in (ROOT / "README.md").read_text().split("\n")
                 if l.startswith("**Ported but not wired.**")), "")
    groups = {}
    dirs = set()
    for m in re.finditer(r"`src/([a-z_]+)/`?\s*\(([^)]*)\)", line):
        groups.setdefault("src/" + m.group(1), set()).update(re.findall(r"[a-z0-9_]+", m.group(2)))
        dirs.add("src/" + m.group(1))
    hm = re.search(r"header-only ports under `include/dd/` \(([^)]*)\)", line)
    if hm:
        groups.setdefault("include/dd", set()).update(re.findall(r"[a-z0-9_]+", hm.group(1)))
    for m in re.finditer(r"`(acp|cli|gateway|cron)/`", line):
        groups.setdefault("src/" + m.group(1), set())
        groups.setdefault("include/dd/" + m.group(1), set())
        dirs.add("src/" + m.group(1))
        dirs.add("include/dd/" + m.group(1))
    return groups, dirs


def tokens(text):
    return [t.lower() for t in TOKEN.findall(text)]


def shares(doc_line, snippet):
    dt, st = tokens(doc_line), set(tokens(snippet))
    for t in dt:
        if t in st or any(t in s or s in t for s in st if len(s) > 3):
            return True
    return False


def citations():
    docs = ["README.md"] + [rel(f) for f in sorted(glob.glob(str(ROOT / "docs/*.md")))] + ["mk/README.md"]
    tree = {}
    for f in (glob.glob(str(ROOT / "src/**/*.cpp"), recursive=True)
              + glob.glob(str(ROOT / "include/dd/**/*.hpp"), recursive=True)
              # The installer is a first-class TU, not make's output: docs cite it by
              # path, so it has to be in the citable tree or the gate calls a real file
              # "NO SUCH FILE" and pushes the doc toward wording that hides a dependency.
              + glob.glob(str(ROOT / "installer/*.cpp"))
              + glob.glob(str(ROOT / "tests/*.cpp"))
              + glob.glob(str(ROOT / "docs/*.md"))
              + glob.glob(str(ROOT / "mk/*.md"))
              + ["README.md", "examples/daedalus/config.yaml", "bench/replay_corpus.py",
                 "bench/doc_audit.py", "src/core/default_config.cpp"]):
        if (ROOT / f).exists():
            tree[rel(f)] = (ROOT / f).read_text(errors="replace")
    bybase = {}
    for f in tree:
        bybase.setdefault(os.path.basename(f), []).append(f)

    out = []
    for d in docs:
        body = (ROOT / d).read_text(errors="replace")
        lines = body.split("\n")
        for m in CIT.finditer(body):
            raw, a, b = m.group(1), m.group(2), m.group(3)
            docln = body[:m.start()].count("\n") + 1
            # A claim and its evidence rarely sit on one line: judge against the paragraph.
            lo_i, hi_i = docln - 1, docln - 1
            while lo_i > 0 and lines[lo_i - 1].strip():
                lo_i -= 1
            while hi_i + 1 < len(lines) and lines[hi_i + 1].strip():
                hi_i += 1
            docline = "\n".join(lines[lo_i:hi_i + 1])
            if raw.startswith("py:") or "://" in raw:
                continue
            base = os.path.basename(raw)
            if base in RUNTIME or SUFFIX.match(base):
                continue
            where = raw if raw in tree else None
            if where is None:
                alt = bybase.get(base, [])
                if len(alt) == 1:
                    where = alt[0]
                elif alt:
                    out.append((d, docln, raw, "AMBIGUOUS", "matches " + ", ".join(alt)))
                    continue
                else:
                    out.append((d, docln, raw, "NO SUCH FILE",
                                "not in the tree; Python provenance must be written `py:`"))
                    continue
            if not a:
                continue
            fl = tree[where].split("\n")
            lo, hi = int(a), int(b) if b else int(a)
            if hi > len(fl):
                out.append((d, docln, raw, "RANGE PAST END", f"{where} has {len(fl)} lines"))
                continue
            snippet = "\n".join(fl[lo - 1:hi])
            if not shares(docline, snippet):
                out.append((d, docln, raw, "ANCHOR DRIFT",
                            f"{where}:{lo}-{hi} holds `{snippet.strip()[:66]}` — shares no token with the citing line"))
            out.append((d, docln, raw, "OK", f"{where}:{lo}-{hi}"))
    return out


def main():
    prod, src_tus, hdrs, reached, unwired, unreached, header_only = wiring()
    ureach = set(unreached)
    print("product binaries: " + ", ".join(f"{k} {len(v)} TUs" for k, v in sorted(prod.items())))
    print(f"closure reaches {len(reached)} files "
          f"({sum(1 for f in reached if f.endswith('.cpp'))} TUs, {sum(1 for f in reached if f.endswith('.hpp'))} headers)")
    print(f"src TUs linked into NO product binary: {len(unwired)} of {len(src_tus)}")
    print(f"headers reached by NO product binary:  {len(unreached)} of {len(hdrs)}")
    print(f"header-only ports (no .cpp counterpart): {len(header_only)}, unreached: "
          f"{len([h for h in header_only if h in ureach])}")

    groups, dirs = readme_list()
    problems = []
    for d, names in sorted(groups.items()):
        if d.startswith("src/"):
            # a TU the README says is unwired must really be unwired, and every really-unwired
            # TU in that directory must be named — by name, or by naming the directory itself.
            real = {u for u in unwired if u.startswith(d + "/")}
            listed = {f"{d}/{n}.cpp" for n in names} if names else real
            for f in sorted(listed):
                if f in reached:
                    problems.append(f"README calls {f} unwired; a product binary reaches it")
                elif f not in real:
                    problems.append(f"README lists {f}; no such TU is unwired (stale name)")
            for f in sorted(real - listed):
                problems.append(f"{f} is unwired but README does not name it")
        elif d.startswith("include/dd"):
            # a header whose TU is unwired is covered by the src/ claim above; only the
            # header-only ports need naming, and the README writes them by basename.
            ho_names = groups.get("include/dd", set())
            real = {h for h in header_only if h in ureach}
            listed = {h for h in real if os.path.basename(h).split(".")[0] in ho_names}
            for h in sorted(header_only):
                if h not in ureach and os.path.basename(h).split(".")[0] in ho_names:
                    problems.append(f"README calls {h} unreached; the closure reaches it")
            for h in sorted(real - listed):
                problems.append(f"{h} is unreached and README does not name it")

    cits = citations()
    bad = [c for c in cits if c[3] != "OK"]
    print(f"\ncitations checked: {len(cits)} | unresolved or drifting: {len(bad)}")
    for d, ln, raw, why, note in bad:
        print(f"  {d} docL{ln} `{raw}` — {why}: {note}")
    print("\nwiring vs README:")
    for p in problems:
        print(f"  {p}")
    print("clean" if not bad and not problems else f"DRIFT: {len(bad)} citations, {len(problems)} wiring claims")
    return 0 if not bad and not problems else 1


if __name__ == "__main__":
    sys.exit(main())

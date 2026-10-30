#!/usr/bin/env bash
# A/B token measurement for edit_file, measured on the tape.
#
# Same binary, same prompt, same starting file; only the deck differs (DD_DISABLE_TOOLS). A
# different build would measure the compiler, not the tool.
#
#   bench/ab-edit-file.sh off      # arm without edit_file (read_file + a full write_file rewrite)
#   bench/ab-edit-file.sh on       # arm with edit_file    (read_file + one V4A hunk)
#   bench/ab-edit-file.sh report   # the table, and the parity check
#
# DD_NO_SOAK=1 keeps the maze out of the run: recall only, never a write, so the tape the model
# pays for is the tape of this task. DD_MAX_TURNS bounds the loop so both arms stop on the same rule.
set -uo pipefail

BIN="$HOME/ariadne/ariadne"
HOME_DIR="$HOME/ariadne"
OUT="${AB_OUT:-/tmp/dd-ab-edit}"
TARGET="$OUT/probe.py"

# A file big enough that a rewrite costs real tokens, small enough that one hunk is obviously cheaper.
seed() {
  mkdir -p "$OUT"
  python3 - "$TARGET" <<'PY'
import sys
lines = ["import logging\n", "\n", "log = logging.getLogger(__name__)\n", "\n", "MAX_LINES = 40\n", "\n"]
for i in range(1, 41):
    lines.append(f"def row{i}():\n    return {i} * MAX_LINES\n\n")
lines.append("def main():\n    total = sum(row{i}() for i in range(1, 41))\n    return total\n")
# the target must exist before the model sees the task: a missing file measures flailing, not editing
with open(sys.argv[1], "w") as f:
    f.write("".join(lines))
PY
}

TASK='Edit probe.py: change MAX_LINES from 40 to 60, and make main() log the total it computed before returning it. Change only those two places — do not rewrite the file.'

arm() {
  local name=$1
  # only the target is reset; the other arm's tape and result must survive for the comparison
  rm -f "$TARGET"; seed
  cd "$OUT"   # a bare path in the task resolves here, not in dd's home
  local off=""
  [ "$name" = off ] && off="edit_file"
  DD_NO_SOAK=1 DD_MAX_TURNS=6 NO_COLOR=1 DD_TUI_MOUSE=0 DD_TUI_COLS=120 DD_TUI_ROWS=30 \
  DD_TAPE_LOG="$OUT/$name.jsonl" DD_DISABLE_TOOLS="$off" \
  "$BIN" --home "$HOME_DIR" -- "$TASK" > "$OUT/$name.out" 2> "$OUT/$name.err"
  cp "$TARGET" "$OUT/$name.result.py"
}

report() {
  python3 - "$OUT" <<'PY'
import json, sys, pathlib
out = pathlib.Path(sys.argv[1])
rows = []
for arm in ("off", "on"):
    log = out / f"{arm}.jsonl"
    rows.append((arm, [json.loads(l) for l in log.read_text().splitlines() if l.strip()] if log.exists() else None))

def tot(calls, k): return sum(c[k] for c in calls)

print(f"{'arm':6} {'calls':>5} {'prompt':>8} {'cached':>8} {'completion':>10} {'tape':>6} {'history':>7}")
for arm, calls in rows:
    if calls is None:
        print(f"{arm:6} {'-':>5}   (not run)"); continue
    last = calls[-1]
    print(f"{arm:6} {len(calls):>5} {tot(calls,'prompt'):>8} {tot(calls,'cached'):>8} "
          f"{tot(calls,'completion'):>10} {last['tape']:>6} {last['history']:>7}")

a = next((r for arm, r in rows if arm == "off"), None)
b = next((r for arm, r in rows if arm == "on"), None)
if a and b:
    p, q = tot(a, "prompt"), tot(b, "prompt")
    o, n = tot(a, "completion"), tot(b, "completion")
    # Call 1 is the only call whose prompt differs by exactly the tool array: same system text, same
    # task, same tape length. So its delta IS the schema's fixed cost, and everything past it is tape
    # churn the edit avoided. Adding them separately keeps the two costs from being conflated.
    fixed = b[0]["prompt"] - a[0]["prompt"]
    churn = (q - p) - fixed * len(b)
    print(f"\nfixed cost  edit_file in the tool array: {fixed:+d} prompt tokens/call"
          f"  ({fixed * len(b):+d} over {len(b)} calls)")
    print(f"tape churn  fewer tool round-trips:      {churn:+d} prompt tokens")
    print(f"paid output one hunk vs a shell edit:    {n - o:+d} completion tokens")
    print(f"net:                                    {(q + n) - (p + o):+d} tokens")
    print(f"cache reuse off {tot(a,'cached')/p:.2f} of prompt, on {tot(b,'cached')/q:.2f}")

    # Byte identity is too strict a parity rule: two model-authored edits can be equivalent and differ
    # by a format spec. The rule is "both applied the requested change", with the residual shown.
    x = (out / "off.result.py").read_text()
    y = (out / "on.result.py").read_text()
    print("\nboth arms applied the requested change:")
    for k in ("MAX_LINES = 60", "log.info"):
        print(f"  {k!r}: off {'yes' if k in x else 'no'}, on {'yes' if k in y else 'no'}")
    if x == y:
        print("  the two files are byte-identical")
    else:
        import difflib
        res = [l for l in difflib.unified_diff(x.splitlines(), y.splitlines(), "off", "on")
               if l.startswith(("+", "-")) and not l.startswith(("+++", "---"))]
        print(f"  residual difference ({len(res)} line(s)) — cost is compared on equivalent edits:"
              + "".join("\n    " + l.rstrip() for l in res))
PY
}

case "${1:-report}" in
  off|on) arm "$1"; report ;;
  report) report ;;
  *) echo "usage: ab-edit-file.sh off|on|report" >&2; exit 2 ;;
esac

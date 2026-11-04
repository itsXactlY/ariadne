#!/usr/bin/env python3
"""Regenerate <home>/config.openrouter-pinned.yaml from <home>/config.yaml.

The pinned lane points the operator's own home at one remote upstream. Keeping it as a second
hand-written file is how it came to be missing 73 settings -- the whole knowledge block, the
pulse and btquant servers, the retry policy, the system prompt -- so it is GENERATED instead:
only the model block is replaced, everything else is config.yaml byte for byte.

    tools/gen-pinned-config.py [HOME]     # default: $DD_HOME, else ~/ariadne

Prints what it replaced. Exit 1 when <home>/config.yaml is missing.
"""
import os
import re
import sys
import pathlib

home = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DD_HOME") or pathlib.Path.home() / "ariadne")
src = home / "config.yaml"
dst = home / "config.openrouter-pinned.yaml"
if not src.is_file():
    print(f"no {src}: nothing to derive the pinned lane from", file=sys.stderr)
    raise SystemExit(1)

lines = src.read_text().split("\n")
try:
    start = next(i for i, l in enumerate(lines) if l.startswith("model:"))
    end = next(i for i in range(start + 1, len(lines)) if re.match(r"^[a-z_]+:", lines[i]))
except StopIteration:
    print(f"{src}: no model: block", file=sys.stderr)
    raise SystemExit(1)

HEADER = '''# config.openrouter-pinned.yaml -- the SAME home as config.yaml, pointed at OpenRouter.
#
# Generated from config.yaml (model block swapped) so the two lanes cannot drift apart again.
# Run it beside your home, never instead of it:
#
#   DD_API_KEY=sk-or-... ariadne --config ~/ariadne/config.openrouter-pinned.yaml
#
# Regenerate after editing config.yaml:  tools/gen-pinned-config.py
#
# WHAT THIS LANE IS FOR: a remote upstream, one model, and the settings that belong to it. Every
# other key -- skills, tool_search, mcp_servers, knowledge, the ledger mirror, compression, the
# system prompt -- is config.yaml's, because a second hand-written file is exactly how the old one
# came to carry 13 of the 84 settings this home runs on.
'''

MODEL = '''model:
  base_url: https://openrouter.ai/api/v1   # the client appends "chat/completions"
  name: deepseek/deepseek-v4.1-flash-1017  # sent verbatim as "model"; see the pin below
  # THE KEY. There is no var expansion for model.api_key (only mcp_servers.*.headers expand ENV
  # per request), so a placeholder written here would be SENT as the credential and rejected.
  # Export DD_API_KEY instead -- it wins over this line -- or paste the key in the clear.
  api_key: ""

  # THE ROUTING. model.extra is carried to the upstream verbatim (HttpLLM::extra), which is the
  # only way a /v1 endpoint learns WHICH provider answers: /models/deepseek/deepseek-v4.1-flash-
  # 1017/endpoints listed 46 of them when this file was generated.
  #
  # order, not only. This is not a preference -- "only" plus one provider is a dead turn whenever
  # that provider throttles, which is exactly what happened: a 53 KB request came back
  #   429 · Retry-After: 180 · "deepseek/deepseek-v4.1-flash-1017 is temporarily rate-limited
  #   upstream ... or add your own provider key" · provider_name: Sail Research
  # while the SAME request unpinned answered in one send from a healthy provider. The model is
  # served by 46 endpoints; naming one as the ONLY one throws 45 away and converts its rolling
  # per-minute limit into ten minutes of the operator's evening.
  #
  # order = try these first (the pin, as a preference), allow_fallbacks = the rest of the pool
  # answers when the preferred one is throttled or down. For a real hard pin (measuring ONE
  # provider) write only: ["..."] instead -- and expect its 429s to be yours.
  extra:
    provider:
      order: ["sail-research/fp4/us"]
      allow_fallbacks: true
'''

# The original header (everything above model:) described the reference box and duplicates this
# file's own header -- drop it, but only if it is comments and blanks, so a real key above model:
# is never lost.
pre = lines[:start]
if any(l.strip() and not l.strip().startswith("#") for l in pre):
    print("note: config.yaml has a real key above model: -- keeping it", file=sys.stderr)
    keep = None
else:
    keep = []

# The key is the ONE thing this file owns that config.yaml does not: never overwrite a live one.
# A regeneration that silently blanks api_key logs you out of the upstream with a 401, which looks
# like a provider problem and is not.
model = MODEL
if dst.is_file():
    old = dst.read_text()
    m = re.search(r"^  api_key: *(\S*)?$", old, re.M)
    if m and m.group(1) and m.group(1) not in ('""', "''"):
        model = re.sub(r'^  api_key: *""$', '  api_key: "' + m.group(1).strip('"') + '"', MODEL, count=1, flags=re.M)
        print(f"kept the key already in {dst.name} ({len(m.group(1).strip(chr(34)))} chars)", file=sys.stderr)

out = HEADER + "\n" + "\n".join((keep if keep is not None else pre) + model.split("\n") + lines[end:])
dst.write_text(out)
print(f"wrote {dst} ({out.count(chr(10))} lines); model block replaced at config.yaml lines {start + 1}-{end}")

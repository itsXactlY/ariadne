# PLAN — continuous build to the end

Scope rule: this plan contains **only gaps an engineer can close without an
operator decision**. Anything that changes what the harness is allowed to do,
spends money, or picks a backend is listed under **Not in this plan** with the
reason. Ordering is by (1) correctness of what already ships, (2) measured cost,
(3) blast radius.

Build protocol, every wave, no exceptions:

1. read the seam (`read_file`, not from this document — line numbers drift)
2. change it
3. `make test` — green means the suite ran, not that it compiled
4. **run the thing that changed and look at its output**
5. record the number in `docs/PARITY.md` and the claim in `README.md`
6. commit. One wave = one commit. A wave is done at the commit, not at the build.

Anything in this file that a run contradicts gets deleted from this file, not
special-cased.

---

## W1 — the tool-result envelope (highest cost, one seam)

Today: caps are compile-time constants and truncation is announced with no way
back. `include/dd/tools/shell.hpp:25` `max_output = 8000`; `src/tools/fs_tools.cpp:85`
`offset = 1, limit = 2000`; no `tool_output` section exists in any config and
`config.cpp` has no such key; `src/tools/shell.cpp:1122-1137` appends
`...(truncated)` and reports `output_bytes` but never the offset to continue
from; `fs_tools.cpp:164-171` computes `next_offset` but never a per-line cap —
`line_length` has zero hits in `fs_tools.cpp`, so a single 100 KB line is handed
back whole. `PARITY.md` "Tool output budget" already names this.

| step | change | proof |
|---|---|---|
| 1a | `tool_output: {max_bytes, max_lines, max_line_length}` in config; `tools::get_tool_output_limits()` resolves it (defaults 50000 / 2000 / 2000, never raises). `ShellOptions::max_output` and `fs_read`'s `limit` read from it instead of header constants. | a config with `max_lines: 3` yields exactly 3 lines and a `next_offset`; the same binary with the section absent yields the defaults |
| 1b | `fs_read` applies `max_line_length`; a cut line is marked `line_cut` with the byte count it dropped | a fixture with one 40 KB line returns a bounded string, and the marker says how much went |
| 1c | truncation becomes resumable: both `run_terminal` and `read_file` report `bytes_dropped` / `lines_dropped` plus the resume offset (`fs_tools` already computes `next_offset`; `shell.cpp:1136` already reports `output_bytes`) | a `grep -R` over the tree returns bounded output whose reply names the offset that continues it |
| 1d | `terminal_hints::annotate_failure(cmd, exit, output)` on non-zero exit, plus two rules the port lacks: **empty output with exit 0**, and **output truncated, N dropped — narrow the command**. The port has 4 rules (`gh` JSON field, `command not found`, `ModuleNotFoundError`, import) and neither of these is among them | the two classes that cost real work this session: `cp -r` producing a 0-byte "before" with exit 0, and a hunk that appended at EOF. Each must now come back with a hint naming the cause |

Exit criterion (number, not prose): one binary, one prompt, `DD_TAPE_LOG`, config
differs only by `tool_output`. Report prompt tokens per turn for an unbounded
`grep -R` arm vs the bounded arm, and the turn count the model needs to finish
the same task once the reply names the resume offset. This is the same method as
`bench/ab-edit-file.sh` (`DD_DISABLE_TOOLS` gave +260 prompt tokens/call and
−22,277 net on a two-place edit of a 129-line file).

## W2 — verbose tells the truth about a result

`PARITY.md`: verbose "shows what the card kept, not what the tool produced".
W1's `bytes_dropped` is the missing datum; render the status row from it rather
than from the card cap. Proof: `Scene::row_text(y)` assertion (`tests/ui_test.cpp:169`)
showing produced bytes ≠ displayed bytes on a truncated result.

## W3 — fold duplicated inline logic into the ported modules (no new behavior)

- `binary_extensions::has_binary_extension` vs the inline detection at
  `fs_tools.cpp:133`, and `patch_apply`'s binary refusal which is currently only
  exercised by a test. One source of truth, extension list from the module.
- `tools::interrupt` vs the inline cancel path: `dd_tui.cpp:187-210`
  (`g_cancel`, SIGINT/SIGTERM/SIGHUP) and `ui.cpp:911-917` (fills
  `{"error":"cancelled"}` for skipped calls). Move the state into the module and
  keep the same semantics.

Invariant that must survive the move: **every tool call the loop skipped gets a
result message**. That is what keeps the tape legal. Proof: existing tests stay
green plus a UI test asserting a cancelled batch has a result for every call id.
Behavior must be byte-identical before and after — this wave is a refactor, and
if the reply JSON changes it is not a refactor.

## W4 — transient retry on the wire (reliability)

Today: one attempt, any failure throws — `http_llm.cpp:171-178`, `net/http.cpp:73-78`.
`agent::jittered_backoff(attempt, base=5, max=120, jitter=0.5)` is ported and wired
into nothing.

Wire it only around **transient** conditions: connect/timeout `CURLE_*`, 5xx, and a
closed connection. Rules:
- never retry a request cut by the cancel callback (`CURLE_ABORTED_BY_CALLBACK`) —
  an interrupted turn is the operator's decision, not a retry
- never retry a 4xx or a schema rejection — retrying those costs a whole turn for a
  deterministic answer
- bounded attempts, and the attempt count lands in the reply/error so a slow endpoint
  is visible instead of mysterious

Proof: a local fake endpoint that answers 500 then 200 — assert exactly two attempts,
the backoff between them, and that a cancelled request produces zero retries.

## W5 — redact untrusted server text at the point it enters the prompt

`dd_tui.cpp:646-651` puts a server's `description` and `inputSchema` into the deck
verbatim, and server `instructions` into notes (`:622-624`); `agent::mask_token` is
applied only inside `render.cpp:44-45`. Apply the existing masking to MCP description
and instruction **strings only** — not to schema nodes (names and enums must stay
exact, and the probes below show the schemas need no rewriting here). No capability
is added or removed; long key-like tokens from a server stop riding every subsequent
prompt. Proof: a server whose description carries a key-shaped token renders masked
in the composed prompt while tool names and enum values stay byte-identical.

## W6 — MCP schema cache at boot (the unported IO half)

`mcp_schema_cache` exposes only pure helpers; `_cache_path`, `_load_all`, `_save_all`,
`get_cached_entry`, `write_cache_entry`, `clear_cache_entry` are explicitly **not**
ported. `config_fingerprint()` (sha256 of the connection-defining config, 16 hex) is
ported and is the staleness key. Implement the disk half against the existing atomic
write (`fs_tools.cpp:211-231` — temp file + `rename`).

Invariant: a cache entry is served only when the fingerprint matches the current
config; a changed `tools_include`/`tools_exclude`/command/URL must miss, never serve a
stale tool list. Proof: boot a slow server twice — first boot lists, second boot is
served from cache and is measurably faster; then edit the config and assert a miss.
Priority is low because `list_tools()` runs at boot (`dd_tui.cpp:626`) and on a bare
`/server` (`:1172`), **not per turn**, so this is boot latency, not token cost.

## W7 — mid-turn steering

Verified in the tree, not from `PARITY.md`: input typed during a turn lands in
`queued` (`dd_tui.cpp:1080`, pushed at `:1401-1402`) and is drained only by the idle
loop (`:1499-1502`), gated on `!running` — so steering cannot act between tool rounds.
Drain at the tool-round boundary — before the next generation, not mid-stream, and never
while a tool call is executing. Steering text joins as a user turn; `/interrupt` keeps its
current meaning and stays the only mid-call action. Proof: a scripted turn where steering
arrives after tool round 1 must be present in the prompt of round 2, and the queued text
must never be sent twice.

## W8 — calibration from measurement, not guesses

`knowledge.confidence` and the offer thresholds are guesses. `dd replay FILE`
(`src/cli_main.cpp:256`) measures offers and misses per threshold and has never been
run against the live catalogue. Run it against the real catalogue, derive thresholds
from the observed offer/miss curve, and record the curve in `PARITY.md`. No operator
decision: the thresholds become data, and the data is reported rather than asserted.

## W9 — keep the docs honest as each wave lands

`README.md:26` is the "ported but not wired" list; it was already wrong once this
session (it claimed `patch_parser`/`fuzzy_match`/`skill_provenance` were linked into
no binary after `5c1d207` wired them). After every wave, re-derive the list by grep
(include lines + symbol use, excluding the module's own TU and its own test) and
rewrite the line. Waves that falsify a claim must delete the claim.

---

## Not in this plan

**Needs an operator decision** (capability, spend, or a security posture):

- sub-agents — none in C++; `cap_delegate_calls(...,3)` (`agent.cpp:343`) records
  delegated calls the harness cannot execute. Running them is a new capability.
- a fetch/browser tool — until one exists there is no call site for
  `website_policy`, `threat_patterns`, `browser_camofox_state`, `tool_backend_helpers`
- `ssl_guard` — wiring it would bypass the CA verification all three HTTP clients rely on
- `env_passthrough` — which variables cross the shell boundary is a policy choice
- `budget_config`, `usage_pricing`, `state_scrub` spend/pricing — spend is the operator's
- `gateway`, `cron` — whole subsystems, not modules
- `delegation_output_schema`, `osv_package_parse` — no consumer in the shipping harness
- remote-backend levers (`prompt_caching`, `prompt_cache_boundary`, `payload_tape`,
  `classification`, `backend_kind`, `retry` beyond transient) — no lever against this
  llama.cpp server; cache reuse here comes from slot reuse (measured 0.68 → 0.78)

**Proven unnecessary against this backend** — falsified live, do not wire:

- `schema_sanitizer` + `unrename_tool_args`: `key-with-space`, `anyOf` nullable,
  `type:["string","null"]`, `$ref`, `enum` with `/`, `pattern` each returned **HTTP 200**
  from `127.0.0.1:8080/v1/chat/completions` with the schema passed unmodified. Wiring it
  would rewrite schemas for a problem the server does not have.
- `message_sanitization::close_interrupted_tool_sequence`: a transcript ending on a raw
  `tool` result with no assistant closer returned **HTTP 200**. The closer fixes nothing here.

## Definition of done

The plan is finished when every wave is committed, `make test` reports
`0 test binary(ies) failed`, `README.md:26` matches a fresh grep, `PARITY.md` carries a
measured number for each wave, and the two falsified modules are documented as
**not wired on purpose** rather than left in an "unwired" list.

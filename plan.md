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

**Closed.** The caps are config-driven and a cut names itself. `tool_output`
ships in `src/core/default_config.cpp` and `examples/ariadne/config.yaml`;
`tools::resolve_tool_output_from(cfg)` resolves it against the config the
process loaded, and both mains hand the result to `ShellOptions::max_output`
and `FsLimits` (`max_lines`, `max_line_length`, `max_read_bytes`) instead of
header constants. `run_terminal` reports `bytes_dropped` with the marker
`[truncated: N of M bytes dropped]`; `read_file` reports `lines_dropped`,
`lines_cut` and `next_offset`, and counts lines before `offset` as asked-for,
not lost. `terminal_hints::annotate_failure` rides a non-zero exit, with the
two rules the port lacks.

| step | change | proof |
|---|---|---|
| 1a | `tool_output: {max_bytes, max_lines, max_line_length}` in config; `tools::get_tool_output_limits()` resolves it (defaults 50000 / 2000 / 2000, never raises). `ShellOptions::max_output` and `fs_read`'s `limit` read from it instead of header constants. | a config with `max_lines: 3` yields exactly 3 lines and a `next_offset`; the same binary with the section absent yields the defaults |
| 1b | `fs_read` applies `max_line_length`; a cut line is marked `line_cut` with the byte count it dropped | a fixture with one 40 KB line returns a bounded string, and the marker says how much went |
| 1c | truncation becomes resumable: both `run_terminal` and `read_file` report `bytes_dropped` / `lines_dropped` plus the resume offset (`fs_tools` already computes `next_offset`; `shell.cpp:1136` already reports `output_bytes`) | a `grep -R` over the tree returns bounded output whose reply names the offset that continues it |
| 1d | `terminal_hints::annotate_failure(cmd, exit, output)` on non-zero exit, plus two rules the port lacks: **empty output with exit 0**, and **output truncated, N dropped — narrow the command**. The port has 4 rules (`gh` JSON field, `command not found`, `ModuleNotFoundError`, import) and neither of these is among them | the two classes that cost real work this session: `cp -r` producing a 0-byte "before" with exit 0, and a hunk that appended at EOF. Each must now come back with a hint naming the cause |

Exit criterion (number, not prose): one binary, one prompt, `DD_TAPE_LOG`, config
differs only by `tool_output`. **Achieved** — two homes, config differing by the
single `max_bytes` line (8000 vs 2000000), `grep -R TODO` over 1200 files
producing 1,009,800 bytes:

| call | bounded | unbounded | delta |
|---|---|---|---|
| 1, no result yet | 4315 | 4315 | 0 |
| 2, carries the result | 9207 | 22151 | +12944 |
| 3, result still in tape | 9403 | 22308 | +12905 |

`level` stayed `ok` in both arms: the bound buys window cost, not a compaction.
Two premises in this wave were falsified on the way and fixed at the source
rather than worked around: `get_tool_output_limits()` re-reads the home
`config.yaml`, so a `--config` override was ignored, and its defaults (50000 /
2000 / 2000) would have widened an unset config sixfold; and hint detection
matches English wording while `/bin/sh` is bash under a German locale, so
`annotate_failure` never fired — the child is pinned to `LC_MESSAGES=C`.
Turn count was not a lever here: both arms finished in 3 calls, 2 tools.

## W2 — verbose tells the truth about a result

**Closed.** `preview()` became `result_facts()`: the reply's own accounting
(`output_bytes`/`bytes_dropped` on a shell reply, `total_lines`/`lines`/
`lines_dropped`/`lines_cut`/`next_offset` on a read) drives the card. The header
status carries the loss at every verbosity, verbose adds the reconciliation row
and the resume offset, and the card's own row cap is announced (`wrap_rows`
returns the rows its cap did not show). Two premises died in the run: the
header's fixed name budget (`w - 26`) clipped a long status onto the name, so a
tight header showed `✓ done · …` — the datum now degrades through a fit ladder
and, when nothing fits, moves to the wrapped accounting rows; and the account
was one clipped row, which reads as a finished sentence, so it wraps instead.

| step | change | proof |
|---|---|---|
| 2a | `ResultFacts` from the reply; header status carries the dropped count | `row_text` finds `✓ done · 1,001,800 bytes dropped` on a truncated result, at 150 cols |
| 2b | verbose reconciliation row + resume offset; the card's own cap names its hidden rows | `8,000 of 1,009,800 bytes reached the model · 1,001,800 dropped`, `300 of 1,312 lines · 1,012 dropped · continue at offset 301`, `… +N rows past the verbose cap` |
| 2c | narrow-width degradation, not clipping | at 44 cols inline the header stays `✓ done ┐` and the account wraps to `model · 1,001,800 dropped` |

Live: `DD_VERBOSE=verbose build/ariadne --home /tmp/dd_ab/a -- "grep -R TODO
/tmp/dd_ab/src"` printed both rows on the real card. Card height at normal
verbosity grows by one row (16 → 17) because the cut row is real.

## W3 — fold duplicated inline logic into the ported modules (no new behavior)

- `binary_extensions::has_binary_extension` vs the inline detection at
  `fs_tools.cpp:133`, and `patch_apply`'s binary refusal which is currently only
  exercised by a test. One source of truth, extension list from the module.
- `tools::interrupt` vs the inline cancel path: `src/dd_tui.cpp:187-210`
  (`g_cancel`, SIGINT/SIGTERM/SIGHUP) and `ui.cpp:911-917` (fills
  `{"error":"cancelled"}` for skipped calls). Move the state into the module and
  keep the same semantics.

Invariant that must survive the move: **every tool call the loop skipped gets a
result message**. That is what keeps the tape legal. Proof: existing tests stay
green plus a UI test asserting a cancelled batch has a result for every call id.
Behavior must be byte-identical before and after — this wave is a refactor, and
if the reply JSON changes it is not a refactor.

## W4 — transient retry on the wire (reliability)

**Closed.** One rule, in one place: `wire_failure(WireResult)` (`include/dd/agent/retry.hpp`)
answers `None / Answer / Cancelled / Transient`, and only `Transient` is sent again. A
fact about the socket can change on a second send; a fact about the server cannot, so a
4xx, the byte cap, the stall clock and an error chunk inside the stream are answers.
`HttpLLM::chat` (`src/agent/http_llm.cpp`) holds the send loop and `prime` takes the same
rule through `net::Response`. `jittered_backoff` has a call site at last: `wire_backoff_ms`
scales it to the wire — 250 ms base, 2 s cap — so a restarting server is answered by the
second send instead of ending the turn.

| rule | where | proof |
|---|---|---|
| a socket fact is re-sent: refused connect, a socket cut before the response line, a 5xx that handed nothing out | `wire_failure` | `build/dd_wire_retry_tests` — a fake loopback endpoint answers 500 then 200: exactly two connections, ≥245 ms between them, the answer lands |
| a stream that already spoke is never re-sent | `StreamSink::spoke` | same suite: a delta handed out, then the socket cut — one connection, the caller keeps the words it got, nothing is spoken twice |
| a 4xx, the byte cap, the stall clock and an error chunk are answers, not hiccups | `wire_failure` | one connection, no pause paid (<200 ms), the server's own words come back |
| a call cut by Esc is never re-sent | `cancelled` in `WireResult` | one connection at most, no backoff paid, the error says `interrupted` |
| the send count rides out, so a slow endpoint is visible | `CallStats.attempts` → `LlmStats.attempts`; `with_sends()` on the error | the suite asserts `attempts == 2` and that `latency_ms` counts the whole wait, not only the send that answered; the engine row renders it as `· ×2` and the verbose status row as `sent 2` (`dd_ui_tests`) |
| `DD_WIRE_ATTEMPTS=1` is the old one-shot wire | `HttpLLM::retry_attempts` | the suite: one connection, and the error names the server's reason without a send count |

## W5 — redact untrusted server text at the point it enters the prompt

`src/dd_tui.cpp:649-654` puts a server's `description` and `inputSchema` into the deck
verbatim, and server `instructions` into notes (`:625-627`); `agent::mask_token` is
applied only inside `render.cpp:44-45`. Apply the existing masking to MCP description
and instruction **strings only** — not to schema nodes (names and enums must stay
exact, and the probes below show the schemas need no rewriting here). No capability
is added or removed; long key-like tokens from a server stop riding every subsequent
prompt. Proof: a server whose description carries a key-shaped token renders masked
in the composed prompt while tool names and enum values stay byte-identical.

## W6 — MCP schema cache at boot (the IO half, now ported)

**Closed.** One rule, one file: a cached manifest may be served only while the config
still hashes to what was cached. `dd/tools/mcp_schema_cache.cpp` now holds the whole
half — `cache_path`, `load_all`, `save_all`, `get_cached_entry`, `has_cached_entry`,
`write_cache_entry`, `clear_cache_entry`, plus `server_fingerprint`, which folds ariadne's
per-name `tools: {name: {enabled}}` gate into the payload Python spells as an exclude
list, so disabling a tool changes the key. `config_fingerprint` is unchanged and keeps
its Python ground-truth cases.

The file is one object read and rewritten whole, so a write is a read-modify-write: a
`std::mutex` makes two threads writing two servers not lose one of them. It is a boot
and operator-command lock, not a hot one. Writes go through a temp file in the same
directory + `fsync` + `rename`, mode 0600, then a directory `fsync` — a rename that is
not durable would silently cost the next boot a live list it already paid for.

What the cache saves is the `tools/list` round trips, **not** the connect: boot still
handshakes, so an unreachable server still shows as unreachable rather than as a stale
tool list. Headers are not part of the key, as in Python — they do not change the
manifest. Priority was low for the same reason it is now closed cheaply: `list_tools()`
runs at boot and on a bare `/server`, **not per turn**, so this is boot latency, not
token cost.

| rule | where | proof |
|---|---|---|
| an entry is served only on a matching fingerprint | `get_cached_entry` | `build/dd_mcp_schema_cache_tests`: match served, changed key missed, absent server missed; a served entry is a copy, so mutating it cannot poison the file |
| ariadne's `tools: {name: {enabled}}` gate is part of the key | `server_fingerprint` | same suite: `enabled: true` equals absent, disabling a tool changes the key, moving the url changes it, rotating a header does not |
| the write is atomic and 0600 | `save_all` | same suite: byte-equal round trip, mode 0600, no `.dd-tmp-*` left behind, parent dir created on a fresh home |
| an unchanged manifest is not rewritten | `write_cache_entry` | same suite asserts the mtime is identical; the boot test asserts the served boot leaves the file byte-identical |
| a corrupt or partial cache costs a live list, not a boot failure | `load_all` | same suite: empty, garbage, a truncated document, a list and a scalar all read as empty |
| two writers do not lose a server | the lock | same suite: 8 threads write 8 servers, all 8 present and all keyed |
| boot reads disk on a match and lists live on a miss | `src/dd_tui.cpp` boot loop | `build/dd_mcp_cache_boot_tests`: a server whose `tools/list` costs 400 ms — boot 1 asks and writes, boot 2 asks nothing but handshakes and is served, a url edit asks again and re-keys, the moved config is served on its next boot |
| a cached manifest registers the same tools, masked the same way | the same registration path | boot test asserts the chat body carries the cached name, enum and `required` byte-identical, the raw key absent, the masked form present, and `schema cached`/`(cache)` in the readouts |

## W7 — mid-turn steering

**Closed.** Steering is a seam, not a widget. `Agent::steer_hook` (`src/agent/agent.cpp:375`)
runs at the round boundary — after the tool results are appended, before the next request,
never while a tool call is executing — and what it returns rides as user messages, which
`TapeEngine::on_steering` (`src/context/engine.cpp:639`) stamps `steered` so the fold is a fact
of the tape. The count is the turn's (`begin_turn` resets it at `:317`). `/interrupt` keeps its
meaning and stays the only mid-call action.

| step | change | proof |
|---|---|---|
| 7a | the hook at the boundary; folded text joins as a user turn | `test_steering` (`tests/agent_loop_test.cpp:419`): two prompts typed mid-turn reach the engine at the boundary, the model sees both, neither twice |
| 7b | the hand-off behind it: `818e37c` landed the seam, `7b570ab` replaced the mutex + `std::deque` with `dd::core::SlotRing<16`, drained by one `claim_all` per boundary, overflow held by the key handler alone | a prompt typed while an answer streams prints `◈ steered · 1 prompt entered this turn` in a tmux pane, and the answer carries it |

## W8 — calibration from measurement, not guesses

**Closed.** `ariadne-skills replay` now measures the offer rule against the catalogue a home actually
holds, and the thresholds it shipped are the numbers the measurement produced. Ground truth is
behavioural: the corpus is 1333 real operator asks harvested from Daedalus's own session log,
each labelled with the skill ids the harness *actually opened* after that ask — 169 opened a
skill, 1164 opened none. The shipped rule offered 296 times over those 1333 asks, 228 of them
on asks that opened nothing at all, and was right at rank 1 only 23 of 70 times it was judged
(prec 0.34). The fitted rule offers 217, false 171, prec 0.41.

| step | change | proof |
|---|---|---|
| the corpus is harvested, not written | `bench/replay_corpus.py` | injected harness text dropped by marker not by length (1312 injected, 496 short of 4275 user turns), asks deduplicated, labels read out of the assistant's own `skill_view` calls and filtered to ids the live catalogue holds — 502 of 502 entry lines parsed, so no stale name becomes a phantom "right" |
| replay measures the rule's own levers, not just a score sweep | `cmd_replay` (`src/cli_main.cpp`) | `--home DIR` opens the real catalogue (502 skills); the run prints score sweep, matched × mass grid, precision at rank 1, false offers over the unlabeled half, and the open-bar sweep |
| the term-count lever is not the lever | the grid | `min_matched` 1→4 at every mass keeps precision between .32 and .44; `matched>=4` is strictly worse (104 offers, 8 right, prec .32, top-3 recall 28 → 10). So `min_matched` stays 2 |
| the absolute evidence bar is | the grid | mass 6.4 → prec .34, 7.12 → .37, **9.5 → .41**, 11.9 → .36 at the same recall. `offer_mass` 6.5 → 9.5, `calibrated_at` 568 → 502 (measured, not guessed) |
| opening a head costs the model a head | the open-bar sweep | at 0.25 the rule opens 73 heads for 9 right ones; at 0.40 it opens 17 for 4. `open_score` 0.25 → 0.40 |
| the numbers reach the running harness | `Router::offers` passes `cat->size()` | `examples/ariadne/config.yaml` carries the same three numbers and the generated `default_config.cpp` shows them; `build/dd_know_router_tests` re-derives `needed()` from the defaults (9.5 at 502, 4.65 at 20, 12.6 at 3700, never under 1) |
| the curve is reported, not asserted | `docs/PARITY.md` "Calibration" | the two-row before/after table, the lever finding, and what the numbers are *not*: precision is a floor because labels are what the model did rather than what it should have done, and the corpus cannot measure the `find()` call a good offer saves |

`make test`: 0 test binaries failed.

## W9 — keep the docs honest as each wave lands

**Closed.** Re-derivation is a tool, not a habit: `make doc-audit` (`bench/doc_audit.py`)
expands the three product recipes from the `Makefile`, walks the transitive `#include`
closure from them, and checks every path-and-line citation in `README.md` and
`docs/*.md` against the tree. It fails on a claim the tree contradicts, so a wave that
moves code cannot leave a doc pointing somewhere else.

| what | where | proof |
|---|---|---|
| reachability is derived from the recipes, not remembered | `bench/doc_audit.py` | `ariadne` 59 TUs, `ariadne-agent` 18, `ariadne` 82; closure reaches **176 files** (83 TUs, 93 headers); **33 of 116** `src/*.cpp` linked into no product binary, **49 of 142** headers reached by none, 16 of 32 header-only ports unreached |
| the README's unwired list is now bidirectional | `README.md` "Ported but not wired" | names every unwired TU by name or directory *and* rejects a name the closure reaches; `src/tools/mcp_schema_cache.cpp` was on the list and is **wired** since W6 (`Makefile:139-140`), so it is off it |
| a citation must point at the thing it describes | `citations()` | 25 citations checked; an anchor whose lines share no token with the paragraph is `ANCHOR DRIFT`. Python provenance is written `py:` (12 rows in `docs/KNOWLEDGE.md`) so it is never mistaken for a repo path; runtime artifacts (`brief.md`, `report.md`, `tape.jsonl`) are excluded |
| anchors that waves moved | `docs/PARITY.md` | rules `807-815` → `934-944`; prompt voice `783-797` → `908-925`; contract wiring `824-833` → `911-917`; plan ledger `490,716-742,928-944,1220-1249` → `527`, `790-810`, `1019-1032`, `1177`, `1338-1344`; steering `1159` → `1195`; `README.md:95` → `:107`; `ui_test.cpp:169` → `170-173`; thresholds cite the checked-in `examples/ariadne/config.yaml:71-79`, not the generated `default_config.cpp` |
| findings the waves falsified are marked, not left standing | `docs/KNOWLEDGE.md` F1, F4, F8, F10 | F1: retrieval fires at the turn boundary, on a rebase, and now on a steering fold (`engine.cpp:343,404,659`); F4: catalogues are refreshed (`src/dd_tui.cpp:763-768,834-836,856`); F8: `build_skills` keys are read (`wiring.cpp:22-29`); F10: home is beside the binary (`home.cpp:106`, `dd_agent_main.cpp:145-150`) |

`make doc-audit`: clean — 25 citations, 0 unresolved or drifting, 0 wiring claims.

## W10 — no locks, no queues: every hand-off becomes atomic ownership

Last, deliberately: its blast radius is every cross-thread site in the tree, so a wave that
changes hand-off *shapes* has to land after the behaviour it carries is final. Inventory read
from the tree, not from `PARITY.md`. It changes no capability, spends nothing, picks no backend,
so it is engineer-decidable like every other wave:

| what | where | why it is in scope |
|---|---|---|
| 3 mutexes, 8 `lock_guard` sites | `src/agent/retry.cpp:15` (jitter counter), `src/agent/prompt_cache_boundary.cpp:16` (x3), `include/dd/gateway/pairing.hpp:55` + `src/gateway/pairing.cpp` (x4) | the only locks left anywhere. Each is reached only by its own test binary (`tests/agent_retry_test.cpp`, `tests/agent_boundary_test.cpp`, `tests/gateway_pairing_test.cpp`) — single-threaded, so the lock guards state no second thread reaches |
| 5 queues | `src/context/maze.cpp:239` (`Job`), `:240` (`LedgerEntry`), `include/dd/know/router.hpp:115` (`opened_q_`), `include/dd/learn/sink.hpp:35` (`q_`), `include/dd/ui.hpp:269` (`Scene q_`) | Vyukov MPSC allocates one node per push and grows without bound. `Scene::llm_content` (`src/ui.cpp:799`) posts an event per streamed delta, so a long answer is thousands of nodes and thousands of string copies on the hot path |
| 6 deques | `src/core/json.cpp:198`, `include/dd/know/working_set.hpp:80`, `include/dd/learn/episode.hpp:53`, `include/dd/ui.hpp:273/275/281` | not hand-offs — each has exactly one owning thread. They stay; each must name it |
| `core::park` / `unpark`, 10 sites | `src/know/catalogue.cpp`, `src/know/router.cpp`, `src/learn/sink.cpp` | a futex wait on an atomic word: no lock, nothing polls. Stays |
| `fsx::FileLock` x3 + `Ledger::FileLock` | `src/context/maze.cpp:257/280/299`, `src/learn/ledger.cpp:20` | cross-process durability on the spool and the ledger, held by the kernel, not by us. Out of scope |

**The shape.** `SlotRing<N>` gains a multi-producer side. One word per slot carries the stamp
*and* the state — `free`, `writing`, `published`, `taken` — so no thread reaches a state it did
not take. A producer stamps with one `fetch_add` on a shared counter, which makes publish order
global rather than per-thread, then CASes `free -> writing(seq)` on the slot it picked and writes
the payload only once it holds it. A consumer CASes `published(seq) -> taken` on the exact word it
read, and resets the slot after moving the payload out. Wait-free on both sides, fixed slots
reused, no node, no allocation in the steady state. The single-writer publish keeps its cheaper
path (`stamp == 0` means the sole writer owns the slot): the two shapes differ exactly where
ownership is taken, which is the whole difference between one producer and several.

| step | change | proof |
|---|---|---|
| 10a | multi-producer `publish`: shared stamp counter, `free -> writing(seq)` before the payload exists | `core_slot_ring_test` gains 8 producers against one consumer: nothing lost, nothing claimed twice, a drain in stamp order; clean under `make tsan` |
| 10b | `Scene` holds no queue: a streaming card's text lives in a fixed-capacity buffer its producing thread owns alone and only the **length** word is shared (bytes land before the length moves, release), so a delta costs no node and no copy; lifecycle events go through the ring | `MpscQueue` is gone from `ui.hpp`; the same answer streams in a tmux pane; an allocation counter in the test binary counts fewer allocations across a 200-delta turn than today, where every delta is a node plus a copy |
| 10c | `opened_q_`, `QueueSink::q_`, `Soaker::State::q` and `out` move to the ring; a refused publish is counted and named in the reply, never dropped — the maze spool stays the durable overflow | the refusal counter reads 0 on a live turn with tools firing; a deliberately full ring counts and the reply says so |
| 10d | the three mutexes go: the jitter counter becomes one `atomic<uint64_t>` (`fetch_add`); boundary and pairing are unwired, so their locks are **deleted** and their headers state the single owner instead of guarding state nobody else reaches | `src` and `include` contain no mutex, no `lock_guard`, no `condition_variable`; `make test` green |
| 10e | every remaining `std::deque` names its owning thread in the header comment | a fresh `grep -n "std::deque" include` shows an owner line above each one |
| 10f | `make tsan` grows past the ring to every site this wave touches | `make tsan` runs the ring, `Scene`, `QueueSink` and `Soaker` under ThreadSanitizer and reports no race |

**Landed** on `fix/audit-20261024`, each step built, run, and looked at. What the proofs read:

| step | the number the proof produced |
|---|---|
| 10a | 8 producers against one consumer: every payload claimed once **or** refused, per-producer order kept, stamps monotonic across the drain; clean under `make tsan` |
| 10b | `Scene` holds no queue. A counting allocator in `tests/ui_test.cpp`: **0 allocations** across a 200-delta streamed turn against **214** for the queue shape rebuilt in the same binary as the baseline, and **0** over 200 `SlotRing` publish/claim cycles |
| 10c | every refusal named: scene event ring **44 refused of 300** posted (`dropped_events()`), a stream buffer at its cap counts the bytes it refused (`dropped_live_bytes()`), router `opened_lost` **6 of 70** with each refused open still emitted to the sink, soak `unledgered` **44 of 300** with the 256 held slots still claimable through `take()`, learn `dropped()` asserted — and all three read **0** on a live turn with tools firing and a live drained soak |
| 10d | `src` and `include` carry no mutex, no `lock_guard`, no `condition_variable`: jitter is one `fetch_add`, the boundary registry is `thread_local`, pairing takes the kernel's file lock across its whole read-modify-write |
| 10e | every `std::deque` left in a header names the one thread that owns it |
| 10f | `make tsan` runs the ring, `Scene`, `QueueSink` and `Soaker`: no race reported |

No site needed the `SnapCell` fallback: the live-turn counters read 0, so every hand-off this wave touched is a ring, and the spool stayed the only durable overflow.

**Falsified, not asserted:** a ring is right only where the payload is owned by exactly one thread
on each side of the hand-off and the volume is bounded. If a live turn shows *any* refused publish
at a site, that site is not a ring: payload flow there becomes a `SnapCell` snapshot plus a park
word, and the refusal count goes in `PARITY.md` rather than being special-cased away. A streaming
delta is neither single-owner-per-side nor bounded, which is why 10b does not put it in a ring.
Done means the invariant is greppable: no lock, no queue, and every deque left says who owns it.

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

The hand-off invariant is part of done: `src` and `include` carry no mutex, no
`lock_guard` and no `condition_variable`, no cross-thread hand-off uses a queue, and
every `std::deque` left in a header names the one thread that owns it. `make tsan`
covers every site W10 touches, not only the ring.

# PARITY — daedalus-cpp against an omp-style harness

One ledger for three questions: what the harness does today, what it does not
do, and what is planned next. Every claim below was measured against the tree
on branch `fix/audit-20261024`, not read out of a README.

## Where the harness stands

| Area | Today | Evidence |
|---|---|---|
| Context tape | Append-only render, epoch per generation, rebase from retrieval at watermarks, prewarm between turns | `42ea640` is an ancestor of HEAD; `src/context/engine.cpp:232,337,383-403,420` |
| Retrieval | One `Router` at turn start, after tool batches, on failure; local BM25f catalogues + async pod leg | `src/know/trigger.cpp`, `src/know/router.cpp` |
| Skills as guidelines | Bodies read on demand: `read_file("skill://name")`, `#2`, `#all`; offer thresholds in config, not code | `src/know/prompt.cpp:30-34`, `examples/daedalus/config.yaml:71-79`, `src/know/policy.cpp:24-28` |
| Rules | Tripwires that cost nothing until a pattern fires, loaded from `./rules/*.md` and `rule:` rows, `knowledge.rules` on/off | `src/dd_tui.cpp:934-944` |
| Learning | `dd::learn` service: episodes, credit, priors, drafted skills, lifecycle | `docs/LEARN.md`, `src/learn/*` |
| One prompt voice | A single `PromptComposer`, the pod's protocol text is not pasted | `src/dd_tui.cpp:908-925` |
| Verbosity | off / normal / full: card caps lift, arguments unclipped, per-call token accounting and epoch/plan on the status row, reasoning card forced open, and a cut result states the tool's dropped count instead of the card's cap | `Verbosity` in `include/dd/ui.hpp`, `src/ui.cpp`, `/verbose` + `display.verbose` + env `DD_VERBOSE` |
| Workflow contract | One `PromptComposer::workflow_contract()` section in the composed prompt: plan before multi-step work, facts from running, done means run-and-looked, skills are guidance | `src/know/prompt.cpp:46-57`, wired at `src/dd_tui.cpp:911-917`, `agent.contract` toggles it |
| Plan ledger | `todo` registered in both binaries; the ledger rides `ephemeral_system_prompt` (never the tape), draws as a sidebar panel, `/plan` reads it back | `src/dd_tui.cpp:524`, `src/dd_tui.cpp:787-807`, `src/dd_tui.cpp:1016-1029`, `src/dd_tui.cpp:1335-1341`, `src/dd_agent_main.cpp:197-218` |

## What is missing — measured, not assumed

### Tool output budget — closed at the tool layer
The bound now sits where the result is produced, not in the card. `tool_output`
(`max_bytes`, `max_lines`, `max_line_length`, `read_bytes`) resolves from the
config the process actually loaded — `tools::resolve_tool_output_from(cfg)`,
`src/tools/tool_output_limits.cpp` — into `ShellOptions::max_output` and
`FsLimits`, so `run_terminal` and `read_file` cut what they hand back before it
enters the tape. Absent or unusable values keep the numbers the tools shipped
before the section existed (8000 bytes / 2000 lines / 4000 bytes per line /
1 MiB per read), NOT `DEFAULT_MAX_*` (50000), which mirror the Python module: a
config without the section must not widen the context cost of every result
sixfold. `get_tool_output_limits()` reads the home copy and stays the parity
path; the tool layer uses the loaded config so a `--config` override counts.
A cut now names itself and how to continue: `[truncated: N of M bytes dropped]`
plus `bytes_dropped` on a terminal result, `lines_dropped` / `lines_cut` /
`next_offset` on a read, and a window that stops inside a file reports
`truncated` rather than reading as whole. Proof:
`make build/dd_tools_output_envelope_tests`.
Measured on the local server with `DD_TAPE_LOG` — two homes whose config
differs by exactly one line, same prompt, `grep -R TODO` over 1200 files
producing 1,009,800 bytes: the call that carries the result costs **9207 prompt
tokens** bounded against **22151** unbounded, and the call after it **9403**
against **22308**. That is **12,944 prompt tokens per call** for bytes the
model never reads. `level` stayed `ok` in both arms, so this is window cost,
not compaction.
Still open: a spill read verb (the dropped tail cannot be recovered from the
reply, only re-fetched by narrowing).

### What a card says about a cut result

The card renders the reply's own accounting, not its own cap. `result_facts()`
(`src/ui.cpp`) reads `output_bytes` / `bytes_dropped` from a shell reply and
`total_lines` / `lines` / `lines_dropped` / `lines_cut` / `next_offset` from a
read reply. The header carries the loss so even a folded card says it was cut
(`✓ 95ms · 1,001,800 bytes dropped`); verbose adds the reconciliation row
(`8,000 of 1,009,800 bytes reached the model · 1,001,800 dropped`) and, for a
cut read, the offset a continuation starts at
(`300 of 1,312 lines · 1,012 dropped · continue at offset 301`). The card's own
verbosity cap reports itself (`… +46 rows past the verbose cap`), so a shortened
render cannot be read as a whole result. Where the header has no room for the
number it moves to the wrapped accounting rows instead of being clipped: a
clipped `✓ done · …` is not a degraded datum, it is a lie with an ellipsis.
Asserted in `tests/ui_test.cpp` through `Scene::row_text`; seen live with
`DD_VERBOSE=verbose dd_tui` on the same 1,009,800-byte grep.

### Mid-turn steering — closed
Input typed while a turn runs is folded into that turn instead of waiting for it to end. The
TUI hands the running worker's queue to `Agent::steer_hook` (`src/dd_tui.cpp:1192`), which the
loop calls at the tool-round boundary (`src/agent/agent.cpp:375`) — after the tool results are
appended, before the next request — and the prompts it returns ride as user messages.
`TapeEngine::on_steering` (`src/context/engine.cpp:639`) stamps each one with `steered`, so the
fold is a fact of the tape rather than a widget's guess: the prompt card carries
`·◈ steered · 1 prompt entered this turn` whether or not the sidebar has room for it; the
sidebar's history row counts it too, but the tape panel is a vertical budget — cards can eat the
rows that open it — so it is asserted offscreen (`tests/ui_test.cpp`) at 120x30 verbose and
120x46 normal, and the row degrades to the message count rather than clipping the fold to
"1 s…". `begin_turn` resets the count (`:317`) so the number belongs to the turn, never to the
session. A prompt
typed after the last round is left to the idle loop rather than folded mid-turn — a fold there
would silently swap the answer the operator asked for (`tests/agent_loop_test.cpp`) — and
`interrupt()` drops the fold with the tool batch. Order across the boundary is asserted
(`tests/agent_run_test.cpp`). Seen live on the same 1,009,800-byte grep at 120x30: typing
"stop. report only how many lines matched" while the tool card was up redirected the answer the
model was composing from the truncated dump — 12 lines, no further call — and the tape log
records the second call on the same epoch with its prefix cached
(`{"epoch":0,"tape":5,"history":5,"prompt":9187,"cached":4249}`).

### Sub-agents
None in C++. `cap_delegate_calls(...,3)` (`src/agent/agent.cpp:101`, applied at `:352`) caps
delegated calls the harness cannot execute: they are recorded, never run.

### Calibration — closed: the thresholds are measured, and they moved
`dd replay FILE [--home DIR]` (`src/cli_main.cpp`) now sweeps the offer rule's own levers
against the catalogue a home actually holds, and reports precision at rank 1 alongside
offers and misses. Run it against the live catalogue of **502 skills** with a corpus of
**1333 real operator asks** harvested from Daedalus's own session log (`bench/replay_corpus.py`):
injected harness text dropped by marker, deduplicated, and labelled *behaviourally* — the
skill ids the harness really opened after that ask. 169 asks opened a skill, 1164 opened none.

| rule | offers / 1333 | right@1 | wrong@1 | right in top-3 / 169 | false offers / 1164 | prec@1 | heads opened |
|---|---|---|---|---|---|---|---|
| shipped: matched>=2, mass>=6.5 (fitted at 568 → 6.37 here), score>=0.15, open>=0.25 | 296 | 23 | 45 | 28 | 228 | **0.34** | 101, of which 10 right |
| fitted: matched>=2, mass>=9.5 (fitted at 502), score>=0.15, open>=0.40 | 217 | 19 | 27 | 25 | 171 | **0.41** | 17, of which 4 right |

The lever table is the reason the bar moved and the term count did not. Holding score at
0.15 and sweeping `min_matched` 1→4 at every mass, precision at rank 1 stays between .32 and
.44 whatever the term count is — `matched>=4` is strictly worse (104 offers, 8 right, prec .32,
top-3 recall down from 28 to 10). Sweeping the absolute mass bar is what moves it: 6.4 → .34,
7.12 → .37, 9.5 → **.41**, 11.9 → .36. So `min_matched` stays 2, `offer_mass` goes 6.5 → 9.5,
`calibrated_at` 568 → 502 (the size it was fitted at, measured rather than guessed), and
`open_score` 0.25 → 0.40 — opening the head of a skill costs the model a whole head, and at
0.25 that bought 73 heads for 9 right ones against 17 for 4 at 0.40. `examples/daedalus/config.yaml`
carries the same three numbers, and `tests/know_router_test.cpp` re-derives `needed()` from them.

**What the numbers are not:** ground truth is behavioural, so precision is a floor, not a
fact. An ask whose right answer is `design-md` scored wrong at rank 1 when the top hit was
`daedalus-identity-update` — a plausibly adjacent card, counted against the rule. Labels also
come from the Python harness's own catalogue, which is not this one. The corpus is a snapshot
of one operator's asks, not a distribution; the curve is reproducible with the two commands in
`bench/replay_corpus.py`'s docstring, and re-running it after the catalogue changes is the
only way these numbers stay true. What the rule costs per offer is a line in the prompt; what
it saves is the `find()` call the operator would otherwise make, and the corpus cannot measure
that second half — a saved call leaves no trace in the log.

## Plan

P0 shipped on this branch (`fix/audit-20261024`, uncommitted), in this order —
each step built, tested, then looked at:

1. **Verbose mode.** `dd::ui::Verbosity {Minimal, Normal, Verbose}`, `/verbose
   off|on|full`, `display.verbose`, env `DD_VERBOSE`. Caps lift, arguments
   unclip, per-call token accounting and the epoch/plan segments join the status
   row, the reasoning card opens every line. Verbosity is one more cache key, so
   cards re-block only when it changes; render kernels and the MPSC feed stayed
   untouched.
2. **Workflow contract.** `PromptComposer::workflow_contract()`, toggled by
   `agent.contract`. `README.md:108` now names it; its old "built-in tool
   preamble" claim was drift — no such string ever existed.
3. **Plan lifecycle.** `todo` registered in both binaries; the ledger rides
   `ephemeral_system_prompt`, never the tape, so the cached prefix stays
   byte-identical and the plan survives a rebase by construction. Sidebar panel,
   `/plan`, and a count on the status row in verbose.
4. **This ledger**, plus the README line that drifted.

Next, in priority order, not promised: sub-agents. Calibration closed on this branch —
the thresholds are now measured numbers, not guesses (see above). The tool
output budget and mid-turn steering shipped on this branch — the first because
it is what makes verbose honest about a result, the second because a turn that
cannot be redirected is a turn the operator cannot steer.

## Verification

`make test` runs every binary in `$(TEST_BINS)` serially with
`DD_TEST_TMP=$(BUILD)/tmp` (`mk/README.md`). The Scene renders offscreen, so
visual checks are assertions on `Scene::row_text(y)` (`tests/ui_test.cpp:170-173`)
— a card's rows are read back as plain text, which is the same frame a tty
would blit.

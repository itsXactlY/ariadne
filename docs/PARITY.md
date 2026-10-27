# PARITY — daedalus-cpp against an omp-style harness

One ledger for three questions: what the harness does today, what it does not
do, and what is planned next. Every claim below was measured against the tree
on branch `fix/audit-20261024`, not read out of a README.

## Where the harness stands

| Area | Today | Evidence |
|---|---|---|
| Context tape | Append-only render, epoch per generation, rebase from retrieval at watermarks, prewarm between turns | `42ea640` is an ancestor of HEAD; `src/context/engine.cpp:232,337,383-403,420` |
| Retrieval | One `Router` at turn start, after tool batches, on failure; local BM25f catalogues + async pod leg | `src/know/trigger.cpp`, `src/know/router.cpp` |
| Skills as guidelines | Bodies read on demand: `read_file("skill://name")`, `#2`, `#all`; offer thresholds in config, not code | `src/know/prompt.cpp:30-34`, `src/core/default_config.cpp:57-75` |
| Rules | Tripwires that cost nothing until a pattern fires, loaded from `./rules/*.md` and `rule:` rows, `knowledge.rules` on/off | `src/dd_tui.cpp:807-815` |
| Learning | `dd::learn` service: episodes, credit, priors, drafted skills, lifecycle | `docs/LEARN.md`, `src/learn/*` |
| One prompt voice | A single `PromptComposer`, the pod's protocol text is not pasted | `src/dd_tui.cpp:783-797` |
| Verbosity | off / normal / full: card caps lift, arguments unclipped, per-call token accounting and epoch/plan on the status row, reasoning card forced open, and a cut result states the tool's dropped count instead of the card's cap | `Verbosity` in `include/dd/ui.hpp`, `src/ui.cpp`, `/verbose` + `display.verbose` + env `DD_VERBOSE` |
| Workflow contract | One `PromptComposer::workflow_contract()` section in the composed prompt: plan before multi-step work, facts from running, done means run-and-looked, skills are guidance | `src/know/prompt.cpp:46-57`, wired at `src/dd_tui.cpp:824-833`, `agent.contract` toggles it |
| Plan ledger | `todo` registered in both binaries; the ledger rides `ephemeral_system_prompt` (never the tape), draws as a sidebar panel, `/plan` reads it back | `src/dd_tui.cpp:490,716-742,928-944,1220-1249`, `src/dd_agent_main.cpp:183-205` |

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

### Mid-turn steering
Input typed while a turn runs lands in `queued` (`src/dd_tui.cpp:1080`, pushed at `:1401-1402`) and is
sent only by the idle loop (`:1499-1502`), gated on `!running`; only `/interrupt` acts during
it. The loop never drains the queue between tool rounds, so steering cannot
redirect work mid-turn.

### Sub-agents
None in C++. `cap_delegate_calls(...,3)` (`src/agent/agent.cpp:101`, applied at `:352`) caps
delegated calls the harness cannot execute: they are recorded, never run.

### Calibration
`knowledge.confidence` and the offer thresholds are guesses. `dd replay FILE`
(`src/cli_main.cpp:256`) exists and measures offers and misses per threshold;
it has never been run against the live catalogue.

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
   `agent.contract`. `README.md:95` now names it; its old "built-in tool
   preamble" claim was drift — no such string ever existed.
3. **Plan lifecycle.** `todo` registered in both binaries; the ledger rides
   `ephemeral_system_prompt`, never the tape, so the cached prefix stays
   byte-identical and the plan survives a rebase by construction. Sidebar panel,
   `/plan`, and a count on the status row in verbose.
4. **This ledger**, plus the README line that drifted.

Next, in priority order, not promised: the four gaps above — the tool output
budget first (it is what makes verbose honest about a result), then mid-turn
steering, sub-agents, calibration.

## Verification

`make test` runs every binary in `$(TEST_BINS)` serially with
`DD_TEST_TMP=$(BUILD)/tmp` (`mk/README.md`). The Scene renders offscreen, so
visual checks are assertions on `Scene::row_text(y)` (`tests/ui_test.cpp:169`)
— a card's rows are read back as plain text, which is the same frame a tty
would blit.

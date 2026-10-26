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
| Verbosity | off / normal / full: card caps lift, arguments unclipped, per-call token accounting and epoch/plan on the status row, reasoning card forced open | `Verbosity` in `include/dd/ui.hpp`, `src/ui.cpp`, `/verbose` + `display.verbose` + env `DD_VERBOSE` |
| Workflow contract | One `PromptComposer::workflow_contract()` section in the composed prompt: plan before multi-step work, facts from running, done means run-and-looked, skills are guidance | `src/know/prompt.cpp:46-57`, wired at `src/dd_tui.cpp:824-833`, `agent.contract` toggles it |
| Plan ledger | `todo` registered in both binaries; the ledger rides `ephemeral_system_prompt` (never the tape), draws as a sidebar panel, `/plan` reads it back | `src/dd_tui.cpp:490,716-742,928-944,1220-1249`, `src/dd_agent_main.cpp:183-205` |

## What is missing — measured, not assumed

### Tool output budget
A result gets no size bound anywhere: `tool_output_limits` is ported
(`include/dd/tools/tool_output_limits.hpp`) and wired into no binary, so the
only cap is the UI card — the whole string still enters the tape and costs
context. A per-tool budget plus a spill read verb would close this; verbose
currently shows what the card kept, not what the tool produced.

### Mid-turn steering
Input typed while a turn runs lands in `queued` (`src/dd_tui.cpp:1039`) and is
sent in order when the turn ends (`:1360-1361`); only `/interrupt` acts during
it. The loop never drains the queue between tool rounds, so steering cannot
redirect work mid-turn.

### Sub-agents
None in C++. `cap_delegate_calls(...,3)` (`src/agent/agent.cpp:343`) caps
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

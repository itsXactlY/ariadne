# PARITY — Ariadne against an omp-style harness

One ledger for three questions: what the harness does today, what it does not
do, and what is planned next. Every claim below was measured against the tree
on `main` (the audit branch `fix/audit-20261024` is merged into it), not read
out of a README.

## Where the harness stands

| Area | Today | Evidence |
|---|---|---|
| Context tape | Append-only render, epoch per generation, rebase from retrieval at watermarks, prewarm between turns | `42ea640` is an ancestor of HEAD; `src/context/engine.cpp:241,273,279,492,859` |
| Retrieval | One `Router` at turn start, after tool batches, on failure; local BM25f catalogues + async pod leg | `src/know/trigger.cpp`, `src/know/router.cpp` |
| Skills as guidelines | Bodies read on demand: `read_file("skill://name")`, `#2`, `#all`; offer thresholds in config, not code | `src/know/prompt.cpp:30-34`, `examples/ariadne/config.yaml:71-79`, `src/know/policy.cpp:24-28` |
| Rules | Tripwires that cost nothing until a pattern fires, loaded from `./rules/*.md` and `rule:` rows, `knowledge.rules` on/off | `src/dd_tui.cpp:1401-1407` |
| Learning | `dd::learn` service: episodes, credit, priors, drafted skills, lifecycle | `docs/LEARN.md`, `src/learn/*` |
| One prompt voice | A single `PromptComposer`, the pod's protocol text is not pasted | `src/dd_tui.cpp:1376-1385` |
| Verbosity | off / normal / full: card caps lift, arguments unclipped, reasoning card forced open, and a cut result states the tool's dropped count instead of the card's cap. `full` is the instrument, not a bigger card: every call leaves a `Kind::Wire` line — the status that answered and the one that started a re-send, the sends and the wall clock the pauses cost, the prompt and the share of it served from cache, the session totals, the epoch and the tape, the watermarks, the ledger's revision and whether it is anchored, the last rebase decision — plus a second line with the response headers that are not transport plumbing; the status row adds `in`, `cache N (M%)`, `sent N · <waited>`, `http`, `rev`, `rebases`; the engine panel adds `in`/`sends`/`http`/`hdrs` and the tape panel `marks`/`rebase`/`ledger` | `Verbosity` and `Kind::Wire` in `include/dd/ui.hpp:267`, `wire_line` at `src/ui.cpp:1800`, pushed at `src/ui.cpp:1760`, the engine's `in` row at `src/ui.cpp:2581`, the tape's `marks` row at `src/ui.cpp:2658`, `/verbose` + `display.verbose` + env `DD_VERBOSE` |
| Workflow contract | One `PromptComposer::workflow_contract()` section in the composed prompt: plan before multi-step work, facts from running, done means run-and-looked, skills are guidance | `src/know/prompt.cpp:46-57`, wired at `src/dd_tui.cpp:1378-1379`, `agent.contract` toggles it |
| Plan ledger | `todo` registered in both binaries; every state is also written to the maze as an append-only revision (`ariadne:plan:<session>:<rev>`, newest wins), adopted back by a fresh start, `/resume` and `/clear`, and carried into the turn's deep recall query; the ledger is handed over as the agent's ephemeral prompt and the tape engine commits it as numbered revisions on the tape — an anchor carrying the list, deltas after it, the list restated whenever no anchor survives a rebase, never in the request's tail — and an emptied ledger retires itself | `src/dd_tui.cpp:1238`, `src/dd_agent_main.cpp:135`, `src/dd_tui.cpp:1501`, `src/ui.cpp:2458`, `src/dd_tui.cpp:1831`, `src/context/engine.cpp:607` |
| Code colouring, from data | A fence names a language and a file answers it: `GrammarSet::load` walks every skill root for `highlight.yaml`, `code_color` orders the skin's own `colors.code_*` line over the language file over the palette default, a tag no file defines claims no keyword, and only a language the file declares `diff: true` colours a `-` as a deletion | `GrammarSet::load` and `code_color` in `src/ui_grammar.cpp:273-325`, `highlight_row` in `src/ui.cpp:1419-1515`, where patch colouring outside a fence is scoped to a hunk header's claim (`hunk_contract` at `src/ui.cpp:1344-1375`: `@@ -a,b +c,d @@` promises at most b + c rows and an inflated claim is capped), roots at `src/dd_tui.cpp:1201-1218`, defaults shipped as `skills/highlight.yaml` and copied by `installer/ariadne_install.cpp:179-186` |
| Palette contrast | Every foreground that carries glyphs is measured against the skin's ground by WCAG luminance and lifted toward the pole the ground is not, so a hue keeps its hue and only luminance gives way; a stripe whose ground is the ground it sits on drops the fill before the colour, an achromatic claim falls back to the palette's hue for that kind, and what moved is counted on the status row | `lift` and `Theme::readable` at `src/ui.cpp:746-762`, the load pass at `src/ui.cpp:856-897`, `GrammarSet::reconcile` at `src/ui_grammar.cpp:296-315`, the readout at `src/ui.cpp:2652`, asserted in `tests/ui_test.cpp:1160-1230` |

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
Still true: the dropped tail is not in the reply — a read resumes from the offset it names, a shell
result is re-fetched by narrowing.

### Measured substrate: the ceiling, not a scorecard

Session `dd-6ae2310c` (2026-10-28, from 06:01), as the harness counters reported them and cross-checked
against `/props`. They move every turn; the ratios are the point, not the totals.

| counter | value | what it constrains |
|---|---|---|
| requests | 1,147 | — |
| prompt tokens read | 908,307 at 146 tok/s | the *uncached* part. Every token a window or a note adds lands here |
| prompt tokens reused | 45,573,645 | 98% of all prompt tokens (45,573,645 / 46,481,952); 500 of 500 requests reused part of their prompt; last request 5,310 of 10,962 |
| tokens written | 571,178 at 46.7 tok/s | decode. The long-reply side is the slow side |
| context | `/props default_generation_settings.n_ctx` = 86,192; the harness panel showed 78,690 for the same server | props is what the server was launched with, the panel is the window actually usable; `total_slots: 1` — two concurrent runs cannot share the server |
| KV cache | 8-bit, streamed: 32,768 positions per layer in VRAM, the rest in RAM | past 32,768 positions a layer reads RAM, not VRAM |
| experts in VRAM | 8,651 (8.5 GB) | the rest of the MoE is fetched from RAM per call |
| speculation | MTP drafts up to 3 tokens, prompt lookup on | the only decode accelerator on this box |
| images | off | vision is not resident |
| model / engine | `qwen3.8-flash-next-gyro-gyro-s` / v0.1.51 | — |
| GPU | RTX 4060 Ti, 16 GB — 15,707 of 16,380 MiB used | 673 MiB of headroom |
| CPU / RAM | Ryzen 7 3800X, 16 threads; 27 Gi of 31 Gi used, 3.8 Gi available | the KV spill and the RAM experts share it |

Read these as a constraint ledger, not a result to beat. Per request the model re-reads ~39.7k cached
tokens and only ~790 fresh ones, so 98% reuse is what a warm prefix costs on a box with 673 MiB of VRAM
free and 3.8 GiB of RAM free — it is the boundary, not a win. The moment a prompt goes cold the price is
146 tok/s of prefill, not the ~500 tok/s this repo previously claimed; at 46.7 tok/s decode a
1,500-token turn budget is ~32 s of writing before it pays for itself. Any second GPU consumer — a pulse
deep leg, a second slot, images switched on — pushes the KV/expert layout past 16 GB and the spill lands
in the RAM that is left, which is why the pod invariant keeps the deep leg serialised.

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
`DD_VERBOSE=verbose ariadne` on the same 1,009,800-byte grep.

### Mid-turn steering — closed
Input typed while a turn runs is folded into that turn instead of waiting for it to end. What was
typed mid-turn lands in a `core::SlotRing<16>` (`src/dd_tui.cpp:1627`) and `Agent::steer_hook`
(`src/dd_tui.cpp:1673`) claims it with `claim_all()` — the payloads it pinned, oldest stamp first, so a
prompt typed earlier enters earlier however the slots wrapped. Overflow is held in a list the key handler
owns alone, and once anything is held everything typed after it waits too. The
loop calls at the tool-round boundary (`src/agent/agent.cpp:419`) — after the tool results are
appended, before the next request — and the prompts it returns ride as user messages.
`TapeEngine::on_steering` (`src/context/engine.cpp:807`) stamps each one with `steered`, so the
fold is a fact of the tape rather than a widget's guess: the prompt card carries
`·◈ steered · 1 prompt entered this turn` whether or not the sidebar has room for it; the
sidebar's history row counts it too, but the tape panel is a vertical budget — cards can eat the
rows that open it — so it is asserted offscreen (`tests/ui_test.cpp`) at 120x30 verbose and
120x46 normal, and the row degrades to the message count rather than clipping the fold to
"1 s…". `begin_turn` resets the count (`:387`) so the number belongs to the turn, never to the
session. A prompt
typed after the last round is left to the idle loop rather than folded mid-turn — a fold there
would silently swap the answer the operator asked for (`tests/agent_loop_test.cpp`) — and
`interrupt()` drops the fold with the tool batch. Order across the boundary is asserted
(`tests/agent_run_test.cpp`). Seen live on the same 1,009,800-byte grep at 120x30: typing
"stop. report only how many lines matched" while the tool card was up redirected the answer the
model was composing from the truncated dump — 12 lines, no further call — and the tape log
records the second call on the same epoch with its prefix cached
(`{"epoch":0,"tape":5,"history":5,"prompt":9187,"cached":4249}`).

### Hand-off without a queue — closed
The cross-thread hand-off carries no node and no lock. A lifecycle event publishes into the scene's
fixed-slot `core::SlotRing` (one `fetch_add` for the order stamp, one CAS for ownership), and a streamed
delta is not an event at all: its bytes go into a buffer its producing thread owns alone and only the
**length** word is shared, so bytes land before the length moves. Measured in the same binary as the
baseline (`tests/ui_test.cpp`, a counting allocator): across a **200-delta** streamed turn the hand-off
allocates **0** where the MPSC queue it replaced allocated **214** — one heap node per delta plus a copy —
and `SlotRing` publish/claim over 200 cycles allocates **0** too. A full ring refuses rather than growing
and every refusal is a number a test reads: **44 refused of 300** events posted (`dropped_events()`), the
router's **6 of 70** opens (`opened_lost`, each refused open still emitted to the sink), the soak's
**44 of 300** (`unledgered`, the 256 held slots still claimable), and **0** at all three on a live turn
with tools firing and a drained soak. `src` and `include` now hold no mutex, no `lock_guard`, no
`condition_variable`, and no queue. `make tsan` builds and runs ten instrumented binaries —
`slot_ring`, `mark_set`, `ui`, `learn_credit`, `context`, `serve_concurrency`, `devices_race`,
`device_gateway`, `relay_policy`, `limbo_race` — under `TSAN_OPTIONS=halt_on_error=1`, and exits non-zero if
any one reports a race or a leak. The gateway's replay and kill marks are atomic words in
`dd::core::MarkSet` (`include/dd/core/mark_set.hpp:76-82`), not a map behind a mutex: a
mutex had come back there when replay was added, and this seam deleted it again.

### Sub-agents
None in C++. `cap_delegate_calls(...,3)` (`src/agent/agent.cpp:105`, applied at `:363`) caps
delegated calls the harness cannot execute: they are recorded, never run.

### Calibration — closed: the thresholds are measured, and they moved
`ariadne-skills replay FILE [--home DIR]` (`src/cli_main.cpp`) now sweeps the offer rule's own levers
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
0.25 that bought 73 heads for 9 right ones against 17 for 4 at 0.40. `examples/ariadne/config.yaml`
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

## Seam graph

The three seams the ledger above claims, as shipped objects. The highlight seam is what the Rust grammar table
becomes: roots are read once at startup and the pointers travel through the paint chain, so a render pass parses
nothing (`src/dd_tui.cpp:1219`, `include/dd/ui.hpp:217`, `include/dd/ui.hpp:252`). The hand-off seam is the same
object at both ends — a lock-free ring whose capacity is its ceiling (`include/dd/core/slot_ring.hpp:75-108`).

```mermaid
classDiagram
    direction LR

    class Canvas {
        <<surface>>
        +put(x, y, ch, style) void
        +text(x, y, s, style, max_w) int
        +to_ansi(color, truecolor) string
    }
    class Frame {
        -prev_ : vector~Cell~
        +present(truecolor) string
    }
    class Scene {
        -th_ : const Theme*
        -grammars_ : const GrammarSet*
        -frame_ : unique_ptr~Frame~
        -cache_ : deque
        +reconcile() void
    }
    class Theme
    class GrammarSet {
        +load(roots) GrammarSet
        -table : Grammar map
    }
    class Grammar
    class CodeCtx {
        +set : const GrammarSet*
        +lang : const Grammar*
        +highlight_row(text, out, out_w) void
    }
    class SlotRing {
        <<ring>>
        +publish(payload) bool
        +claim_one(out) bool
        +refused() uint64
    }
    class LiveBuf {
        <<buf>>
    }
    class QueueSink {
        <<interface>>
        +publish(Event) void
    }
    class Router
    class Agent {
        +steer_hook : function~string()~
    }
    class TapeEngine {
        +on_steering : function~string()~
    }

    Frame --|> Canvas
    Scene o-- Frame : frame_
    Scene o-- Theme : th_, owned by the root
    Scene o-- GrammarSet : grammars_, owned by the root
    Scene o-- LiveBuf
    Scene ..> CodeCtx : paint_row carries it
    CodeCtx ..> GrammarSet : table lookup per line
    GrammarSet o-- Grammar
    Scene o-- SlotRing : q_, 256 slots of Event
    Router o-- SlotRing : opened_q_, 64 slots of Opened
    QueueSink o-- SlotRing : 8192-slot ceiling, runtime cap
    Agent ..> SlotRing : the typed 16-slot steering ring
    Agent ..> TapeEngine : steer_hook polled at segment boundaries
```

Anchors: `include/dd/ui.hpp:73` (Canvas), `:101` (Frame), `:206-220` (GrammarSet::load), `:234-243` (CodeCtx),
`:381` (Scene), `:485` (`SlotRing<kEventSlots, Event> q_`), `:502` (LiveBuf), `:521-522` (the paint cache),
`include/dd/agent/agent.hpp:99` (`steer_hook`), `include/dd/context/engine.hpp:86` (`on_steering`),
`include/dd/learn/sink.hpp:43`, `include/dd/know/router.hpp:120`, `src/context/maze.cpp:246`
(`SlotRing<256, Job>`).

## Plan

P0 shipped on `main` (`fix/audit-20261024` is merged into it), in this order —
each step built, tested, then looked at:

1. **Verbose mode.** `dd::ui::Verbosity {Minimal, Normal, Verbose}`, `/verbose
   off|on|full`, `display.verbose`, env `DD_VERBOSE`. Caps lift, arguments
   unclip, per-call token accounting and the epoch/plan segments join the status
   row, the reasoning card opens every line. Verbosity is one more cache key, so
   cards re-block only when it changes; render kernels and the scene feed stayed
   untouched.
2. **Workflow contract.** `PromptComposer::workflow_contract()`, toggled by
   `agent.contract`. `README.md:76-77` names it; its old "built-in tool
   preamble" claim was drift — no such string ever existed.
3. **Plan lifecycle.** `todo` registered in both binaries; the ledger rides the
   tape as numbered revisions — the first revision of an epoch is an ANCHOR and
   carries the whole list, each later revision carries only what moved, and every
   revision says a higher number wins. An item records which of the two it is
   (`include/dd/context/tape.hpp:36`), so a rebase asks whether the *list* is
   still on the wire rather than whether some revision is: when no anchor
   survives the cut, the list is restated as the epoch's own anchor and the
   deltas that rode with the dropped one go with it
   (`src/context/engine.cpp:132`, `src/context/engine.cpp:607`). An emptied
   ledger retires itself with a revision that says so. Sidebar panel, `/plan`,
   and a count on the status row in verbose.
   *Corrected 2026-10-28:* the premise was wrong on Strata's hybrid models. The
   plan behind the payload sat in front of the engine's turn checkpoint, so every
   call fell back to a periodic one (every 16,384 tokens read) and re-read up to
   that much. The tape engine now takes the plan (`take_ephemeral`).
   *Extended 2026-11-04:* the ledger is not a property of the run. Every state
   it reaches is written to the maze as a node of its own,
   `ariadne:plan:<session>:<rev>`, append-only and newest-wins like the tape's
   revisions, chained to the turn that was newest when it moved — so `think`
   from a plan reaches the work, and a turn is adjacent to the plan it worked
   under (`src/context/plan.cpp`, `Soaker::plan`). The write rides the soak
   queue and its spool, so a dead pod delays a plan instead of losing it. The
   read side is a label walk plus one primary-key read — no embedding, so a boot
   asks it before anything is warm. A fresh start adopts the newest OPEN plan
   (within 72h) instead of handing the model a window with no plan in it,
   `/resume` brings that session's plan back with its turns, `/clear` carries an
   open list into the new session, and an emptied ledger writes a closing
   revision instead of leaving a stale plan open in the graph. The turn signal
   carries the open items into the deep recall query (`src/know/trigger.cpp`),
   and a plan id is kept out of the window set so a recall hit that IS the plan
   node is not dropped as already-on-the-tape (`TapeEngine::in_window`). Proved
   live: two revisions written and the newest read back, and a headless boot
   printing `◈ plan adopted from the maze`.
   *Corrected 2026-11-04:* the first correction stopped at the carrier. The
   restatement still asked whether the newest revision — always a delta after a
   tick — had left the window, which it never had, so a new epoch opened with
   "what moved since revision N, the list itself is the anchor above" and no
   anchor above it: the plan was gone, and the model read changes against a list
   it could not see. Measured by putting that one condition back: `0 anchor,
   1 delta`, three assertions red (`e13e648`). The same pass stopped a merge
   entry with no `id` being dropped: the content it repeats names the item it
   means, anything else is a new item (`src/tools/todo_tool.cpp:130`).
4. **This ledger**, plus the README line that drifted.

Also shipped here, each with its number: the wire's transient retry (proved
against a loopback endpoint that answers 500 then 200 and never answers twice) — and, since
2026-11-04, a rate limit is no longer filed as a verdict: 429 with `Retry-After` is waited out for as
long as the server asked, and a wait past the 150 s retry budget is refused with the number in the
error instead of slept away. *Extended 2026-11-04 (later the same evening, after a first prompt died on
openrouter's `Retry-After: 180`):* the budget bounds the ladder — the client's own guess — and not the
server's answer to the same question, so an instructed wait is obeyed past the budget up to
`agent.retry.retry_after_cap_ms` (default 300 s, the number the operator can act on when it is too
long), the refusal names that ceiling and the knob, a spent budget is 0 rather than a negative an
unbounded check would read as "keep going", and a pause long enough to look like a dead program is
announced before it sleeps and again when it wakes (`HttpLLM::on_wait`) — Esc still stops the turn
while it sleeps. *Extended again the same night, after the screen showed nothing but
`⟳ 429 · asked for 180s` from turn zero:* a ceiling PER INSTRUCTION is not a bound on the call —
nine sends that each obey a 180 s instruction is twenty-seven minutes of a turn spent asleep on a
provider that is simply refusing, which is what `agent.retry.attempts: 9` produced. Instructed
waiting is now also capped in total per call (`agent.retry.retry_after_total_ms`, default 240 s,
0 = never obey one): the first 180 s window is honored, the second ends the call with the amount
already spent and the knob that would allow more. The pause announcement carries the server's own
sentence as well as its status, because "429" cannot tell a rate limit from an exhausted key or a
pin on a throttled provider — and in this case it was the last of the three: the body said
`deepseek/deepseek-v4.1-flash-1017 is temporarily rate-limited upstream ... or add your own provider
key`, provider_name Sail Research, while the same 53 KB request routed without the pin answered in
one send. A single-provider `only` turns one provider's rolling per-minute limit into a dead turn;
`order` + `allow_fallbacks` keeps the preference and lets the other 45 endpoints serve it.
The same day, a provider that died *before its first token* stopped being
an answer: OpenRouter reports it inside a 200 — `Upstream error from Sail Research: The response
stream was interrupted and could not be recovered. Please retry the request.` — so the turn ended with
nothing handed out, every time; and a 200 whose body is a JSON error rather than a stream was read as a
successful call with no words at all. Both are `Transient` now, capped at one more send, because a
verdict on the request comes back identically the second time (`src/agent/retry.cpp:66`,
`src/agent/http_llm.cpp:369`, the cap at `src/agent/http_llm.cpp:437`). And the same gate run against a
 keep-alive endpoint — no `Content-Length`, no `Connection: close`, socket held 4s past the last byte — which is how
 llama-server, vLLM and OpenAI actually answer: the turn has to end at `data: [DONE]`, not at EOF, so a whole answer
 arrives in under a second on exactly one send, a reader that stops mid-stream counts as `completed`
 (`include/dd/agent/chat_stream.hpp:38`, `src/agent/http_llm.cpp:135`, `include/dd/agent/retry.hpp:55`), and a
 `CURLE_WRITE_ERROR` that lands beside the terminator keeps the answer instead of re-sending it. Reading to EOF cost
 every turn the stall timeout and filed a finished answer as a stalled server; under `--serve` the held socket kept
 `busy_` set, so the first request never answered and every later one 409d behind it.
Then the later waves, each with its number: redaction of untrusted server text at the point it enters the prompt, the MCP
schema cache at boot, the duplicated inline logic folded into the ported
modules, and the doc audit that re-measures this file's citations instead of
trusting them (`make doc-audit`: 187 citations, 0 unresolved or drifting). Calibration closed
on `main` — the thresholds are now measured numbers, not guesses (see
above). The tool output budget and mid-turn steering shipped here — the first
because it is what makes verbose honest about a result, the second because a
turn that cannot be redirected is a turn the operator cannot steer. The last
wave closed the hand-off: no lock, no queue, no node per delta (see above).

Next, in priority order, not promised: sub-agents.

## Verification

`make test` runs every binary in `$(TEST_BINS)` serially with
`DD_TEST_TMP=$(BUILD)/tmp` (`mk/README.md`). The Scene renders offscreen, so
visual checks are assertions on `Scene::row_text(y)` (`tests/ui_test.cpp:332-334`)
— a card's rows are read back as plain text, which is the same frame a tty
would blit.

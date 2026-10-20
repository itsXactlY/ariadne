# dd_tui — a harness in pure C++ that got tired of arguing

**TL;DR:** the agent harness lives in `src/agent/agent.cpp`. Every feature anyone
thought was missing from it ended up as a paragraph of prose about how it works
somewhere else. That's boring. So it got fixed in C++ instead. Four prompts.
One binary, `-Werror`, 23k lines, zero mock-based checks.

---

## The whataboredome

Every "but what about X?" you get out of a Python/TS harness debate —
*but what about routing? compression? what about the tool loop just being
a Python dict?* — usually spawns a feature request. In this tree it spawned
something dumber: a README paragraph. The harness had already been right
about everything; it just couldn't *prove* it in a language that compiles.

So the whole evolution arc is four prompts wide. Nothing else asked for it.

## Prompt 1 — "port it. C++20. make it run."

Not a rewrite. The run loop, `core::Message`, the hand-rolled `core::Json`,
the tool registry — already ported and green behind **64 test suites**. The
only missing leaves were the HTTP transport (libcurl), three real filesystem
tools, and an entrypoint. One `int main()` later there is a `build/dd_agent`
that drives a live llama.cpp OpenAI endpoint and does real I/O.

The ground rules got written down before the fun started: plain `g++` +
Makefile (CMake 4.4.3 is broken for this tree — *it is, don't use it*),
`-std=c++20 -Wall -Wextra -Werror`, `std::regex` with no lookaround. The
PoC binary became the stable floor. Everything below it builds on top
without disturbing it.

## Prompt 2 — "make it a real harness, not a toy."

So it became a harness. `dd_tui` drives the **same** `Agent::run` loop —
one `step_hook` added, no fork. What it wires in is all real, not simulated:

- `read_file` / `write_file` / `list_dir` — real filesystem I/O.
- `run_terminal` — denylist-based (`sudo`, recursive `rm` on `/`·`~`·`*`,
  `mkfs`, `dd`, shutdown/reboot, curl-pipe-shell, force-push …). It's a
  guard, **not** a jail — the README says so, in bold.
- **Every enabled MCP tool, discovered live**: reads `mcp_servers.*` from
  config, speaks Streamable-HTTP MCP (JSON-RPC 2.0 over one POST, SSE
  `data:` responses), calls `tools/list`, registers each tool with its real
  `inputSchema`. The running mazemaker pod yields **34 tools**.
- The MCP server's `initialize` **`instructions`** — the recall-first /
  persist-per-turn memory protocol — get appended to
  `agent.system_prompt`. That is what makes recall-first *happen*, not
  get requested.

## Prompt 3 — "make it real cutting edge, fixed geometry, boxes. atomic lockfree. no mutexes if not 100% needed. AVX-512 and OMP are not toys to ignore."

This is the one that hurt. Two sentences of English became engineering
constraints, not adjectives:

- **Threads, no locks.** Worker and tool threads only `push` into a
  Vyukov MPSC queue — one atomic exchange, zero mutexes on the render
  path. The render thread owns every bit of scene state.
  `interrupt_requested` is one `std::atomic<bool>` doubling as the libcurl
  abort flag, so Esc cancels an LLM call *in flight*. Measured:
  **0.86 s from Esc to ready during a live generation**, and `/slots`
  confirmed the server slot went idle — the generation was actually
  dropped, not ignored.
- **SIMD + OpenMP, picked by CPUID once** (`dd/ui_simd.hpp`): cell diff,
  ASCII scan/width/widen as AVX-512 (F+BW+VL) / AVX2 / scalar kernels —
  one binary for every box. Rows encode in parallel on big frames, cards
  re-wrap in parallel after a resize; small frames stay serial so the
  TUI never wakes threads on cores the model is using.
- **Double-buffered cell-diff render**: `Frame::present()` emits only
  changed cells, inside synchronized output (`ESC[?2026H`), on the
  alternate screen. An unchanged frame writes nothing.
- Rounded panels with titles set into the border, a 34-col sidebar
  (`session` / `engine` / `tools`), fixed-height tool cards, `turn 3 ·
  4 calls · 2 tools · 41.2s`. Skins from `~/.daedalus/skins/*.yaml`,
  truecolor when `COLORTERM` says so. Raw `termios` input (restored on
  exit and on fatal signals), CSI/SS3 keys, bracketed paste, SGR mouse
  wheel, UTF-8 line editing, tab-completed `/help /tools /turns /clear`.

## Prompt 4 — "no fake shit lottery. verify against the live stack."

Every number in this file is a measurement from a real run, not a
prototype's self-report. pty-driven verification (pyte + PIL frames)
against the actual `~/.daedalus/config.yaml`, the actual `:8080`
(Qwen3.8-Flash-Next IQ1_M, 1 slot, `n_ctx` 86000), the actual mazemaker
pod:

- a real `list_dir` turn: 2 calls, **17.4 s** warm cache, engine panel
  at 21–36 t/s gen / ~50 t/s prefill, ctx 7.2k/86.0k — from the
  server's own `timings`, not an estimate.
- the memory round-trip: the model issues `mazemaker_recall` and gets
  live memory back at sim 0.54.
- `tests/ui_test.cpp` checks each kernel set the CPU has against scalar,
  and 1-thread vs 8-thread frames **byte for byte**.

It also earned one honest reprimand: a stale binary showed `┤─⟁` title
bugs that were already fixed in the source. Rebuild after every `src/ui.cpp`
edit *before* looking at screenshots. That kind of mistake only exists
because verification is real — a mock would have lied and moved on.

## The bug that was worth its own header

libcurl 8.16+ calls the write callback with **four** args —
`size_t fn(char*, size_t, size_t, void*)` — not the classic three. A
3-param sink reads `nitems` (a byte count) as the userptr →
**SIGSEGV**. The 4-param form that appends and returns `nitems` took the
whole port from crashing to `-Werror`-clean. That's what "bleeding edge"
means here: the language caught a dependency's ABI drift.

## What it doesn't do

Honest, because the README is 6k words of it and the honest line is one
of them: no smart model routing, no cron/gateway loop, no retrieval
compression with watermark tiers, no credential pooling. Those are
the other harness's territory. This one does the things it doesn't do
instead *in compiled code*: LSP/AST code intelligence, hashline edit
safety, typed tools (line selectors, archive/SQLite/JSON readers),
recoverable `artifact://` elision + a deterministic compaction order
(shake → snapcompact → handoff → remote) with async speculative
compaction, 32-concurrent subagents with `agent://` messaging,
persistent eval kernels, session tree rewind/fork.

## Layout

```
src/            port TUs (agent · core · tools · ui · ui_simd)
include/dd/     headers
tests/          64 _test.cpp suites — `make test`
build/          dd · dd_agent · dd_tui · dd_ui_tests
```

```sh
make build/dd_tui
build/dd_tui                                # interactive REPL
build/dd_tui -- "list files in /tmp using list_dir"   # single-shot, same cards
```

---

*No Python, no asyncio, no virtualenv, no "it's just that the harness is
written in C++" — the C++ harness is the artifact.*

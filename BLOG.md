# dd_tui — a harness in pure C++ that got tired of arguing

**TL;DR:** the agent harness lives in `src/agent/agent.cpp`. Every feature anyone
thought was missing from it ended up as a paragraph of prose about how it works
somewhere else. That's boring. So it got fixed in C++ instead. Four prompts.
One binary, `-Werror`, 55,835 lines across 356 files, 92 test binaries.

---

## The whataboredome

Every "but what about X?" you get out of a Python/TS harness debate —
*but what about routing? compression? what about the tool loop just being
a Python dict?* — usually spawns a feature request. In this tree it spawned
something dumber: a README paragraph. The harness had already been right
about everything; it just couldn't *prove* it in a language that compiles.

So the whole evolution arc is five prompts wide. Nothing else asked for it.

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

- **Threads, no locks.** Worker and tool threads handed the render thread
  a Vyukov MPSC queue — one atomic exchange per push, zero mutexes on the
  render path. The render thread owns every bit of scene state. Prompt 5
  deleted that queue: a node per hand-off and unbounded growth is not
  "no locks", it is a lock-free shape that still pays for memory.
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
  4 calls · 2 tools · 41.2s`. Skins from `skins/*.yaml` beside the binary,
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

## Prompt 5 — "no locks, no queues: every hand-off becomes atomic ownership"

Prompt 3 said *no mutexes if not 100% needed*. This made it literal: `src` and
`include` contain **zero** `std::mutex`, `lock_guard` and `condition_variable`,
and the MPSC header is deleted. The proof is a grep, not a memory.

- **`core::SlotRing<N,T>` is the only hand-off shape.** One word carries a payload's
  order stamp *and* its ownership, so no thread can reach a state it did not take.
  `publish()` is the sole writer's path — a slot at 0 means that thread owns it, so it
  writes the payload first and the release store makes the stamp arrive with it.
  `publish_any()` is the several-producer path — one `fetch_add` makes publish order
  global rather than per-thread, then a CAS takes `free -> writing(seq)` on the slot it
  picked, payload written only once held, so a half-written payload is never readable.
  The claimer resets the slot after moving, so slots are reused and a hand-off allocates
  nothing. Six typed sites: the scene's event ring (256 slots), the router's open ring
  (64), the soak's job and ledger rings (256 each), the learn sink (8192), the TUI's
  steering ring (16).
- **A streamed delta is not an event.** Its bytes go into a buffer its producing thread
  owns alone and only the **length** word is shared, so bytes land before the length
  moves. Measured with a counting allocator against the queue rebuilt in the same binary
  as the baseline: across a 200-delta turn the hand-off allocates **0** where the queue
  allocated **214** — one heap node per delta plus a copy.
- **Overflow is a number, never silence.** A full ring refuses instead of growing and
  every site reads its counter: **44 refused of 300** events posted, **6 of 70** router
  opens (a refused open still reaches the sink), **44 of 300** soak ledger entries (the
  256 held slots stay claimable). On a live turn with tools firing and a drained soak all
  three read **0** — that is the only way a ring is falsifiable: one nonzero refusal on a
  real turn would mean the shape is wrong at that site, and the site would go back to a
  snapshot cell.
- **The three locks that survived the first audit are gone**, each for a different
  reason: the retry jitter counter is one `fetch_add`, because the tick was the only
  value it ever held and there was no invariant a lock could protect; the prompt-cache
  registry is `thread_local`, because a prefix is registered while a prompt is built and
  matched while the next prompt on that same thread is built, so the state never crosses
  a thread; gateway pairing takes the kernel's file lock for its whole read-modify-write,
  because a userspace mutex cannot cover a second dd on the same home. Every `std::deque`
  left in a header names the one thread that owns it.
- `make tsan` runs the ring, `Scene`, `QueueSink` and `Soaker`: **no race reported**.
  `make test`: **1025 checks, 0 test binaries failed**.

## The bug that was worth its own header

libcurl 8.16+ calls the write callback with **four** args —
`size_t fn(char*, size_t, size_t, void*)` — not the classic three. A
3-param sink reads `nitems` (a byte count) as the userptr →
**SIGSEGV**. The 4-param form that appends and returns `nitems` took the
whole port from crashing to `-Werror`-clean. That's what "bleeding edge"
means here: the language caught a dependency's ABI drift.

## What it doesn't do

Honest, because the README is 6k words of it and the honest line is one
of them: no smart model routing, no cron/gateway loop wired in, no
credential pooling, no LSP/AST code intelligence, no hashline edits, no
subagents, no eval kernels, no session-tree rewind. Parts of the Python
harness are ported into this tree but linked into no binary — and that
list is measured by `make doc-audit`, which expands the three product
recipes and takes the transitive include closure from them: **176 files
reached**, **33 of 116** `src/*.cpp` linked into no product binary, **49 of
142** headers reached by none, 16 of them header-only. It is not the
"unwired" list from memory: `retry` is the wire's send policy, `url_safety`
gates every URL that reaches the wire, `patch_parser` + `fuzzy_match` +
`patch_apply` are the `edit_file` tool, and `tool_output_limits` +
`terminal_hints` bound a result where it is produced. What *is* wired is the run
loop, the context tape with retrieval-based rebases, the knowledge router,
the learn loop and a `run_terminal` that is a guard, not a sandbox.

## Layout

```
src/            port TUs (agent · core · tools · ui · ui_simd)
include/dd/     headers
tests/          92 test binaries — `make test`; 4 tsan binaries — `make tsan`
build/          dd · dd_agent · dd_tui · dd_ui_tests · dd_slot_ring_tsan
```

```sh
make build/dd_tui
build/dd_tui                                # interactive REPL
build/dd_tui -- "list files in /tmp using list_dir"   # single-shot, same cards
```

---

*No Python, no asyncio, no virtualenv, no "it's just that the harness is
written in C++" — the C++ harness is the artifact.*

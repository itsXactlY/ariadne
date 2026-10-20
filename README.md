# daedalus-cpp


<p align="center"><img src="Daedalus-CPP.png" alt="Daedalus-CPP" width="90%"/></p>

**TL;DR:** C++20 port of the Daedalus agent. Plain `g++` + `Makefile` (CMake 4.4.3 is broken for this tree — don't use it). One real agent binary (`build/dd_agent`) that drives the ported run loop over a live llama.cpp OpenAI endpoint via libcurl, with working `read_file`/`write_file`/`list_dir` tools doing real I/O.

```
make build/dd_agent          # agent binary (links -lcurl)
make                         # build/dd (CLI entry)
make test                    # 64 test suites
```

## Build

- **Compiler:** `g++`, `-std=c++20 -Wall -Wextra -Werror -O2 -Iinclude`.
- **System:** CMake is **4.4.3 and broken** here. The `Makefile` is the source of truth. A `CMakeLists.txt` exists but is not the working path.
- `-Werror` is strict, including `-Werror=unused-function`: every non-`inline` `static` helper must be referenced in its TU. Prefer `inline`/`constexpr`.
- `std::regex` here gets no lookaround; `\w`/`\d` are ASCII-only.

Layout: `src/` (port TUs) · `include/dd/` (headers) · `tests/` (64 `_test.cpp` suites) · `build/` (outputs).

## Run the agent

```
build/dd_agent                                  # msg on stdin, config from ~/.daedalus/config.yaml
build/dd_agent path/to/config.yaml -- hello     # config arg, then -- , then message words
echo "list files in /tmp/ using list_dir" | build/dd_agent
```

Config path resolves from `HOME` (→ `~/.daedalus/config.yaml`) unless given as the first argv. No Python Daedalus on the box? `mkdir -p ~/.daedalus/logs && cp examples/daedalus/config.yaml ~/.daedalus/` — that file is the whole config (model, turns, skin, MCP, denylist). The message is stdin, or the words after `--`.

| Env | Overrides |
|---|---|
| `DD_BASE_URL` | `model.base_url` (must end `/v1`; code POSTs `/chat/completions`) |
| `DD_MODEL` | `model.name` |
| `DD_API_KEY` | `model.api_key` (llama.cpp needs none) |
| `DD_MAX_TURNS` | `agent.max_turns` (config default `400` — **cap it**; a bad sample loops for minutes, ~17s/turn on the local model) |

Output: `[base=... model=... turns=...]` header, then `=== final (N LLM calls) ===` + the assistant's final answer. `read_file`/`write_file`/`list_dir` run real filesystem I/O.

## Full harness kit (`build/dd_tui`)

`build/dd_tui` is the real agentic harness: a next-gen OOP TUI (`dd::ui`: double-buffered cell-diff `Frame` + `Widget` subclasses + `Scene` row layout) that drives the **same** run loop (`Agent::run`) — no fork of the loop, one `step_hook` added to `agent.hpp`/`agent.cpp` for per-step streaming.

```
make build/dd_tui
build/dd_tui                                   # interactive REPL (you> ), config from ~/.daedalus
build/dd_tui path/to/config.yaml -- "msg"      # single-shot
```

### TUI internals (`dd::ui`)

- **Layout, fixed geometry:** transcript panel | 34-col sidebar (`session`, `engine`, `tools`) over a 3-row prompt box and a status row. Panels are rounded boxes with their titles set into the border. The sidebar drops under 100 cols; under 60×14 the screen says so. Cards are at most 112 cols wide.
- **Cards:** user = gold bar; answer = rounded `⟁ DAEDALUS ⟁` box with the call latency; tool = square card of **fixed height** (name, status `queued`/spinner/`✓ 1.2s`/`✗`, flattened args, two-line result preview); notes; errors; a rule closing each turn (`turn 3 · 4 calls · 2 tools · 41.2s`).
- **Skin:** colours, spinner wings, thinking verbs and branding come from `~/.daedalus/skins/<display.skin>.yaml` (neural defaults built in). Truecolor when `COLORTERM=truecolor|24bit`, else xterm-256.
- **Reasoning, live:** calls stream (`stream: true`, `dd/agent/chat_stream`); `delta.reasoning_content` fills a dashed `◈ thinking` card (fixed-height live tail, so nothing below it jumps), the answer streams into its card. `/reasoning show|full|hide` picks tail / every line / one row (`display.show_reasoning: false` starts hidden).
- **Effort:** `/effort` or `/reasoning` + `off low medium high xhigh max ultra`, sent as `reasoning_effort` (`off` → `"none"`, thinking disabled). Only levels the model's chat template names (read from `/props`) are offered — anything else makes the template raise, a 500 — so `xhigh/max/ultra` appear only on templates that go that deep. The start level is `agent.reasoning_effort`, rounded down to the template's ceiling.
- **Engine stats are real:** each call reports llama-server `timings` (gen/prefill t/s, `cache_n`) and `usage`; `n_ctx` comes from `/props` once at start (the ctx meter).
- **Render:** `Canvas` (16-byte padding-free `Cell`, wide-glyph aware) → `Frame::present()` diffs against the last frame and emits only changed cells, inside synchronized output (`?2026`), on the alternate screen. An unchanged frame writes nothing.
- **Threads, no locks:** worker and tool threads only `push` events into a Vyukov MPSC queue (one atomic exchange); the render thread owns every bit of scene state and drains the queue each frame. `Agent::interrupt_requested` is `std::atomic<bool>`, and doubles as the libcurl abort flag, so Esc cancels an LLM call in flight (measured: 0.86 s from Esc to ready during a live generation).
- **SIMD + OpenMP (`dd/ui_simd.hpp`):** cell diff, ASCII scan/width/widen as AVX-512 (F+BW+VL) / AVX2 / scalar kernels, picked once by CPUID — one binary for every box. Rows encode in parallel (OpenMP) on big frames, cards re-wrap in parallel after a resize; small frames stay serial so the TUI never wakes threads on cores the model uses. `tests/ui_test.cpp` checks each kernel set the CPU has against scalar, and 1-thread vs 8-thread frames byte for byte. The AVX-512 set has not run on hardware yet (Zen 2 here; QEMU TCG has no AVX-512).
- **Input:** raw `termios` (restored on exit and on fatal signals), CSI/SS3 keys, bracketed paste, SGR mouse wheel (`DD_TUI_MOUSE=0` leaves the mouse to the terminal), UTF-8 line editing, history. Enter sends (while busy: queued for the end of the turn), Esc/Ctrl-C interrupt, PgUp/PgDn or wheel scroll, Tab completes `/help /tools /turns N /effort L /reasoning L|show|full|hide /clear /interrupt /quit` and their arguments, Ctrl-L redraws. stderr goes to `~/.daedalus/logs/dd_tui.log` while the screen is up.
- **No tty** (`-- msg` or piped stdin): the same cards print inline, in order.
- `run_terminal` runs `{ cmd; } </dev/null 2>&1`: a child never reads the raw-mode tty.

### Context tape (`dd::context`, `src/context/`)

The agent loop no longer re-sends a growing transcript. `TapeEngine` (an `agent::ContextEngine`) keeps the session and decides what each request carries — the Daedalus tape, ported:

- **Render once, append only.** The first call of an epoch renders the whole view with every hygiene layer; after that a message is rendered once, when it enters the payload, and every later request reuses those bytes. The previous request is an exact prefix of the next, so the server re-reads only what is new (`context_test`: 0 prefix breaks over 20 tool-heavy calls, 18 without the tape). `context.payload_tape: false` renders every call from scratch.
- **Hygiene only at an epoch start** (`Renderer::epoch`): head window (`memory.soak.window_turns` × 2 messages), finished tool exchanges past `context.tool_group_ttl` (8) collapsed to one line naming each call, reasoning past `context.reasoning_window` (3) dropped — the maze has it. Tool results are capped to their share of the window as they enter (`[[RESULT CUT …]]` says how to continue; `read_file` pages by `offset`/`limit`).
- **Retrieval is the memory.** Every finished turn is soaked to mazemaker (`auto:turn:<session>:<ts>`), reasoning the first time it is sent (`auto:reasoning:…`); tool calls and output never are, turns without an answer neither. A lock-free queue feeds one writer thread; disk (`<cwd>/.daedalus/`) only while the pod is dead — a spool, replayed one write at a time when it answers. One recall per turn is rendered onto the question (`memory.turn_recall`).
- **Rebase from retrieval at the watermarks.** Pool tokens (as the server counts them, both slots) against `compression.fast_layer_tokens`: past *low* an idle sidekick slot is erased, past *high* the next epoch is prewarmed between turns (sidekick slot on a multi-slot llama-server, parked and cancellable on Strata), past *hard* main moves onto it — or rebases inline. Turn 0 of an epoch: the question, a `[rebased]` anchor naming `auto:turn:<session>:*`, `[this turn so far]` (one line per call already run), one recall, the protected tail (`compression.protect_last_n`). No summarizer.
- **A rebase must make room.** The new epoch is sized to land under half the rebase point: the tail gives way from its front, whole tool groups at a time, never the newest group (the live state the turn continues from), and every dropped call is named in `[this turn so far]`. A rebase that still could not get under the high watermark is skipped and said once — the window runs over the fast layer rather than dropping the working state every call. The size estimate counts chars/4 scaled by the server-measured tokens-per-char of the last call (tools array included).
- **Measured live** (Strata 0.1.37, Qwen3.8-Flash-Next IQ1_M, one slot, real config + mazemaker): 11 calls over 3 turns reused 88% of all prompt tokens — each call's cache is the previous prompt plus its completion, ±1 token; only a new question with its recall is read. With a forced 8.5k fast layer the next epoch was prewarmed while the operator read, adopted at the next turn, and that turn's first call reused 99.1%. Soaks landed as `auto:turn:dd-*` and `auto:reasoning:dd-*` rows. `DD_TAPE_LOG=<file>` writes one line per call (epoch, tape, prompt, cached, completion).
- **Tools are discovered, not carried.** Local tools and `tool_search.always_load` stay in the array; the rest is reached through `tool_search` / `tool_describe` / `tool_call`, so the array never changes and stays in the cached prefix. On the reference pod: 38 tools (~5.8k tokens) → 11 in the window (~2.0k), 30 deferred.
- **The endpoint is told how to behave** (`Endpoint::shape`): main pinned to slot 1 on a multi-slot llama-server (`id_slot`), `n_cache_reuse 256`, `reasoning_budget_tokens` (`agent.thinking_budget`, default 1024) with its message; `reasoning_effort` from `/effort`.
- TUI: a `tape` panel (epoch, window vs rebase point, prefix reuse of the last call, history vs tape, tools in/deferred, maze and soak counters), `reuse` in the status row, `/context`, `/rebase`, `/clear` (new session). `DD_FAST_LAYER=N` overrides the fast layer (forces rebases for testing); `DD_NO_SOAK=1` recalls but writes nothing to the maze (no soak, and the model's own mazemaker writes answer an error), so test turns never show up in later recalls.

### Bound to mazemaker (`dd/context/memory`, `maze`, `recall`)

The harness forgets on purpose; mazemaker is where everything it drops lives — and where the session itself is written. One interface (`dd::context::Memory`), the pod behind it over MCP, a fake graph in the tests.

- **The session is a chain in the graph.** Labels are unique per session; every soak hands its memory id back (the engine's *ledger*, lock-free from the soak worker); edges chain the nodes as they are written — turn → next turn (`session_next`), reasoning → the turn it served (`reasoned_in`), via `connections_import`. The dream cycles and later traversals see a session's shape, not loose rows.
- **One recall per turn, the graph around it.** Measured on the pod: a recall is ~7 s and the pod serialises them; `think` 0.9 s, `get` 0.08 s, `afe_facts` 0.08 s, `classify_intent` 0.04 s. So a turn spends exactly one recall and enriches it in parallel: spreading activation from the best hit read back by id, the facts extracted from the top hits, and — for a temporal question — this session's own out-of-window turns by id. Nothing already on the tape comes back (the ledger knows which ids were written this epoch), and the block fits a budget.
- **A rebase names ids.** Turn 0 of a new epoch lists this session's turn ids: `mazemaker_get(memory_ids=[…])` reads them back in ~0.1 s instead of a search.
- **Skills from the catalogue.** The 503 skills in the maze are indexed once in the background (3.2 s) and matched locally with BM25 — a fitting skill costs the turn no recall (cutoff measured: fitting skills score 14.5–35, unrelated text ≤ 7.8; paths are ignored). `skill_load(name)` reassembles the full SKILL.md from its `skillsrc` parts (0.2 s).
- **Resume from the maze.** `/resume` lists sessions, `/resume <id>` or `/resume last` rebuilds one from its `auto:turn` chain — the maze is the session store; a resumed session continues its chain. Any session soaked under `auto:turn:<id>` resumes, the Python harness's included.
- **The model knows the contract.** The system prompt says what the harness already does (recall first, soak every turn, ids after a rebase, skills by name), so the model recalls only for what is missing and saves only distilled facts.

What it wires in, all **real**:
- `read_file` / `write_file` / `list_dir` — real filesystem I/O.
- `run_terminal` — everything is allowed except a denylist (`sudo`, `su`, recursive `rm` on `/`·`~`·`*`, `mkfs`, `dd`, shutdown/reboot, curl-pipe-shell, force-push, …); extra words via config `command_denylist`. It is a guard, **not** a jail — do not treat it as sandboxing.
- **Every enabled MCP tool, discovered live**: `dd_tui` reads `mcp_servers.*` from config, speaks Streamable-HTTP MCP (JSON-RPC 2.0 over one POST, SSE `data:` responses, session-less), calls `tools/list`, and registers each enabled tool with its real `inputSchema`. The running mazemaker pod yields 34 tools.
- The MCP server's `initialize` **`instructions`** (the MAZEMAKER MEMORY PROTOCOL — recall-first, persist-after-turn) are appended to `agent.system_prompt`. That is what makes recall-first behavior real, not simulated.

Continuity across REPL turns rides on the external memory graph (the agent re-recalls each turn), which is the protocol's own design — not a persistent in-process transcript.

Verified smoke (`DD_MAX_TURNS=4`, local IQ1 model ~17s/turn + ~22s/recall → wall ≥300s): registers 34 mazemaker tools, the model answers "as required by the memory protocol", issues `mazemaker_recall`, and the round-trip returns live memory (top hit sim 0.54). See `src/tools/mcp.hpp` / `src/tools/mcp_client.cpp` for the transport.

## libcurl gotcha (bit me once)

libcurl **8.16+** calls the write callback with **four** args — `size_t fn(char* buffer, size_t size, size_t nitems, void* outstream)` (`curl.h:285`) — not the classic three. A 3-param sink reads `nitems` (a byte count) as the userptr → **SIGSEGV**. Correct form appends `nitems` and returns `nitems`:

```cpp
static std::size_t curl_sink(char* buffer, std::size_t size, std::size_t nitems, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(buffer, nitems);
    return nitems;                       // return != nitems => CURLE_WRITE_ERROR
}
```

## JSON / Message API (core)

`core::Json` is a hand-rolled value type: `Type{Null,Bool,Num,Str,Arr,Obj}`, `JsonSP = shared_ptr<Json>`, members `arr`/`obj`. Builders `json_parse`/`json_dump`/`json_obj`/`json_arr`/`json_str`/`json_num`; init-lists take `JsonSP` (wrap with `jsp(...)`); `arr_push`/`obj_set` take a bare `Json`; read size **before** a move. `message_to_json`/`message_from_json` marshal `core::Message` (wire `tool_calls` = `{id,type:"function",function:{name,arguments}}`; `finish_reason` is a **sibling** of `message` on `choices[0]`).

---

## The whole deal — context tape

TL;DR of how this tree got here, oldest → newest:

1. **Seed.** One plain goal: get a *running* `int main()` agent (`build/dd_agent`) that drives the already-ported run loop (`src/agent/agent.cpp`) against the real llama.cpp OpenAI endpoint — libcurl transport + real filesystem tools.
2. **Ported base, not rewritten.** The run loop, `core::Message`, `core::Json`, tool registry were already ported and green (64 suites, `make test`). Only the missing leaves got written: the HTTP transport + the FS tool handlers + the entrypoint.
3. **Scale.** Long evolution, heavy tool churn, one hard crash (libcurl 4-arg callback SIGSEGV) burned most of the tokens — then the 4-param sink fix, clean `-Werror` rebuild, live smoke.
4. **Proof of concept (this README).** A working binary + documented gotchas + verified smoke = the *context tape*: the ground rules of the port. Everything above this line is that PoC baseline.

**Where the real phun starts:** below this line is open territory — the PoC binary is the stable floor, and actual agent work (better tools, real gateway, cron, the `payload_tape`/`prompt_cache_boundary` islands, the rest of the ~200 TUs) now builds *on top* of it without disturbing it.

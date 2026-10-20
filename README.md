# daedalus-cpp

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

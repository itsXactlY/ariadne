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

Config path resolves from `HOME` (→ `~/.daedalus/config.yaml`) unless given as the first argv. The message is stdin, or the words after `--`.

| Env | Overrides |
|---|---|
| `DD_BASE_URL` | `model.base_url` (must end `/v1`; code POSTs `/chat/completions`) |
| `DD_MODEL` | `model.name` |
| `DD_API_KEY` | `model.api_key` (llama.cpp needs none) |
| `DD_MAX_TURNS` | `agent.max_turns` (config default `400` — **cap it**; a bad sample loops for minutes, ~17s/turn on the local model) |

Output: `[base=... model=... turns=...]` header, then `=== final (N LLM calls) ===` + the assistant's final answer. `read_file`/`write_file`/`list_dir` run real filesystem I/O.

## Full harness kit (`build/dd_tui`)

`build/dd_tui` is the real agentic harness: an ANSI terminal REPL that drives the **same** run loop (`Agent::run`) — no fork of the loop, one `step_hook` added to `agent.hpp`/`agent.cpp` for per-step streaming.

```
make build/dd_tui
build/dd_tui                                   # interactive REPL (you> ), config from ~/.daedalus
build/dd_tui path/to/config.yaml -- "msg"      # single-shot
```

What it wires in, all **real**:
- `read_file` / `write_file` / `list_dir` — real filesystem I/O.
- `run_terminal` — guarded by the config `command_allowlist` (first token must be listed) plus a small deny set (`sudo`, `rm -rf /`, …). It is a guard, **not** a jail — do not treat it as sandboxing.
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

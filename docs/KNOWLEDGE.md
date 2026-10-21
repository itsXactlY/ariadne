# Knowledge: one retrieval path for skills, memory and tools

Status: plan, 2026-10-22. Nothing here is implemented. Branch `feat/knowledge` (worktree
`~/projects/daedalus-cpp-knowledge`), cut from `main` 006a752.

## 1. Verdict

daedalus-cpp has retrieval parts and no retrieval policy. It looks something up at two moments
(a turn starts; the window is rebuilt), matches skills against the raw user message, and tells the
model the retrieval is "already done". While the model works (tool call after tool call, where
guessing happens) nothing looks anything up.

The plan: one `Router` decides **when**, **what** and **how** to retrieve; every body of knowledge
sits behind one `Catalogue` interface; the **harness retrieves, the model confirms by reading**.
The model is a 1-bit coder: it does not volunteer retrieval, so the harness cannot leave it to
the model's judgment (the Python Daedalus learned this: `agent/skill_router.py` docstring).

Orientation is oh-my-pi (omp 18.12.1, read from its docs and prompts, section 3): it keeps
retrieving first through a skills list the model MUST read from, `find` before any guessing,
rules that stay dormant until a pattern fires, and loop redirects. We take the mechanisms, not the
numbers: omp lists every discovered skill in its system prompt, which suits a large-window cloud
model; we have 498 skills and a 54.6k rebase mark.

## 2. What is wrong today (measured 2026-10-22)

| # | Finding | Evidence |
|---|---|---|
| F1 | Two retrieval moments only: turn start and rebase. After the first tool call nothing looks anything up. | `engine.cpp:263`, `engine.cpp:429` |
| F2 | The system prompt has three voices that contradict each other: the preamble ("memory recall is only for past decisions"), the pod's ~860-token `instructions` pasted verbatim ("RECALL FIRST ... NO EXCEPTIONS", "SAVE PER TURN", `session_search`, a tool this harness does not have), and the harness paragraph ("already done ... only for what the block lacks"). | `dd_tui.cpp:727`, `:750`, `:893`; `initialize` reply |
| F3 | Skill matching is blind and noisy. Query = the whole user message; absolute BM25 threshold 12.0 (a sum that grows with message length); ASCII-only tokenizer (ä/ö/ü split words), no stopwords, no German; tags unused; paths stripped, which also strips the strongest signal (`.../jrwl-messenger/...`). About 9 ms per match (document frequencies rebuilt per call). | `tool_search.cpp:161,294`, `recall.cpp:111,148` |
| F4 | The catalogue is read once: `if (maze->up()) skills.load_async()`. Pod down at launch means empty for the whole session while 467 SKILL.md lie on disk. `skills::Scanner/Registry` (disk) feed only the `dd` CLI; `dd_tui` uses only `SkillIndex` (maze). | `dd_tui.cpp:827`, `cli_main.cpp` |
| F5 | Catalogue quality. 611 `skill:` rows are 498 skills + 5 notes posing as skills (`fix-i3-services-not-starting` was *offered* as one) + 1 AFE fragment. 107 names exist in both `builtin` and `local` (95 identical sha, 4 differ, 8 unknown). 101 cards have no body slices (`load()` then returns the 650-char card as "the skill"). 96 legacy cards carry raw frontmatter in the text. Only 134 cards have tags. | maze census |
| F6 | A loaded skill is a tool result: it ages out (`tool_group_ttl`, rebase) and nothing re-attaches it, so the model forgets the procedure it was following. Bodies: median 6 KB (~1.7k tokens), p90 16.8 KB (~4.7k), max 55 KB (~15k), against a rebase mark of ~54.6k. | maze census |
| F7 | Nothing is measured: no offered-to-opened counter. | |
| F8 | Dead config: `skills.auto_route`, `skills.external_dirs`, `skills.disabled` are in the schema, nothing reads them. | `harness_config.hpp:289` |
| F9 | The deep recall blocks the turn start: 6-8 s scoped to `skill:*`, 12-17 s for the pod's `lean`/`semantic` modes. The scope leaks (`auto:reasoning`, `skillsrc` rows come back) and one card (`dlm-verschluesselungsschicht-diagnose`) is a hub: 6 of 8 queries. Unscoped, the top 8 held zero skills. | probes |
| F10 | Home: the binary reads `~/.daedalus/{config.yaml,skins,logs}` (shared with the Python Daedalus) and writes spill and the soak spool to `<cwd>/.daedalus`, so state depends on where it was launched. | `dd_tui.cpp:374,628,667,854`, `dd_agent_main.cpp:176`, `cli_main.cpp:16`, `core/home.cpp` |

### Replay: the current matcher on real prompts

77 real prompts (29 Daedalus turns soaked in the maze, 48 operator messages) through the repo's own
`SkillIndex` against the live catalogue (503 indexed):

- 23 offers. By my reading: about 8 right, 1 arguable, about 14 wrong. Wrong ones are the
  pattern you would expect: a smoke test "write one long markdown file in a single write_file call"
  gets `discord-outbox-file-write`; a TUI-animation complaint gets `mazemaker-dream-engine` while
  `daedalus-tui-verbose-animation` sits at 11.3, just under 12; continuation summaries and pasted
  terminals go to whichever long-description skill soaks up the most common words
  (`jack-in-a-box`, `jack-in-a-box-hermes-integration`, `daedalus-single-source-of-truth`).
- At least 11 prompts had a fitting skill and got none: "deploy the site" (`mazemaker-site-ops`
  9.9), the podman-mess message (`podman-deep-dive` 11.5, `never-touch-live-pods-or-postgres` 12.0),
  the BTQuant questions, "clean sample config" (`daedalus-clean-context-setup` 9.2). Short prompts
  cannot reach 12; long ones reach it for the wrong skill.
- Semantic recall scoped to `skill:*` finds what lexical misses (podman x3, BTQuant x4,
  `daedalus-tui-verbose-animation` first) but costs ~7 s per call and has the hub problem.
  Neither leg is enough alone; the local lexical leg has to become good, and the semantic leg has to
  be asynchronous.

The labels are mine; the operator vetoes them in the corpus file (section 12). The corpus holds raw
operator prompts, so it stays out of git.

## 3. What omp does, and what we take

| omp 18.12.1 | Source | daedalus-cpp |
|---|---|---|
| Skills as `name: description` in the system prompt; "Matching skill -> MUST read `skill://<name>` first" | `prompts/system/system-prompt.md` (Skills & Rules), `docs/skills.md` | 498 names would cost ~12.5k tokens, 23% of the window. Ship a **catalogue map** (46 groups, <= 250 tokens) plus top-k **cards per decision point**; keep the imperative. |
| `skill://`, `rule://`, `memory://`, `history://` all resolved by the one `read` verb | `docs/skills.md`, `docs/rulebook-matching-pipeline.md` s.8 | `Uri` + `Resolver`; `read_file("skill://x#sec")` works; `skill_load` stays as an alias for a weak model. |
| Rules in three buckets: always-apply (full text in the prompt), rulebook (listed, read on demand), TTSR (dormant until a pattern fires) | `docs/rulebook-matching-pipeline.md` s.5 | **Pins**, **Cards**, **Tripwires**. |
| TTSR: match on the model's own output, inject the rule, retry; `once` / `after-gap`; survives compaction | `docs/ttsr-injection-lifecycle.md` | `Tripwires` as a pre-call gate on tool arguments (no stream abort in v1): veto the call, return the rule as the result; once/after-gap; the ledger survives a rebase. |
| `find` FIRST: "NEVER guess grep/glob targets"; hits `path:range score`, below ~0.4 = weak, widen | `prompts/tools/find.md`, system prompt "Exploration" | `find(query, kinds)` over the catalogues with the same score semantics. Workspace code search later. |
| Loop redirects: identical call x N; repeated reasoning | `prompts/system/tool-call-loop-redirect.md`, `thinking-loop-redirect.md` | `StallTrigger`: the stop-poking-blind guard. |
| Memory: compact summary injected at session start; `recall`/`reflect`/`retain`/`learn`; "recalled rows changed" notice | `docs/memory.md`, `prompts/system/memory-recall-changes.md` | `OperatorBrief` pins in the prefix; the pod recall stays as the deep leg; `learn` becomes maze-native skills; staleness note after a rebase. |
| `intent` on every tool call ("Reading model role settings") | system prompt, Tool I/O | optional `intent` argument on the local tools: a query the model wrote itself, for free. |
| Managed skills never overwrite authored ones; lowest precedence; colliding names: winner keeps the bare name, the other is namespaced, identical copies collapse | `docs/skills.md` | Same rules; sha256 collapses 95 of the 107 duplicates. |
| Config in `~/.omp/agent`, `--profile`, `--config` overlay | `docs/config-usage.md` | We differ on purpose: beside the binary (s.10). `--config` overlay is worth keeping. |

omp does not solve our scale problem: it has no semantic skill search (its `find` is for code) and
lists every skill. That part is ours.

## 4. Principles

1. **One router decides, catalogues answer, the engine delivers.** Nothing else calls the pod to
   "recall something".
2. **Cards are local and cheap, bodies load on demand, the pod is the slow deep leg** and always
   has a deadline.
3. **Append-only.** Retrieval never edits the prefix. The system prompt changes at launch only;
   everything later is an appended item the tape renders once.
4. **The harness decides, the model confirms.** Offer ladder by confidence: silent, pointer,
   head-open, open. Measured by opened/offered.
5. **Triggers are pure.** `plan(signal, working_set) -> Plan` does no I/O: unit-testable.
6. **Lock-free reads.** Catalogues publish an immutable snapshot (`shared_ptr<const Index>`, atomic
   swap, as `SkillIndex::Slot` does now); one single-slot mailbox carries the deep leg back.

## 5. Architecture

### 5.1 Layers

```
app      dd_tui.cpp                 composition root only: Home -> config -> catalogues -> Router -> engine
context  TapeEngine, Tape, Renderer consume Router::on(Signal); no retrieval logic left in the engine
know     Router . Trigger* . Distiller . WorkingSet . Deliverer . Tripwires
         Catalogue <- SkillCatalogue <- { DiskSkills, MazeSkills }
                   <- MemoryCatalogue   (cards cached locally; deep() is the pod)
                   <- ToolCatalogue     (adapter over ToolDeck)
                   <- SessionCatalogue  (the soak ledger)
mem      Memory (interface, header-only) <- Maze (pod over MCP) ; Soaker (writes)
core     Home, json, yaml, path
```

Dependency rule: `context -> know -> mem -> core`. `context/memory.hpp` is the seam and moves to
`mem/` untouched.

### 5.2 Value types

```cpp
namespace dd::know {
enum class Kind { Skill, Memory, Tool, Session, Rule };
struct Uri  { Kind kind; std::string id, path; };        // skill://name[/rel][#sec]  mem://1290233  rule://name
struct Card {                                             // what a catalogue knows without opening anything
    Uri uri; std::string title, gist, group, source;     // gist: one line, when to use it (<= 200 chars)
    std::vector<std::string> tags; size_t body_bytes = 0; double prior = 1.0;
};
struct Query { std::vector<std::pair<std::string, float>> terms; std::string text; std::vector<Kind> kinds; };
struct Hit   { Card card; double score = 0; double margin = 0; std::string why; };   // why: matched terms (TUI, tests)
struct Section { size_t offset = 0, limit = 0; };        // progressive disclosure: head first, then #2, #3
struct Body  { Uri uri; std::string text; size_t total = 0; std::string more; };
}
```

### 5.3 The catalogue contract

```cpp
class Catalogue {
public:
    virtual ~Catalogue() = default;
    virtual Kind kind() const = 0;
    virtual bool ready() const = 0;
    virtual uint64_t generation() const = 0;                          // bumps when the card set changes
    virtual size_t size() const = 0;
    virtual std::vector<Hit> search(const Query&, int k) const = 0;   // local, lock-free, never blocks, <= 1 ms
    virtual std::optional<Body> open(const Uri&, Section) = 0;        // may block; owns its deadline
    virtual std::string map() const = 0;                              // the awareness block, one line per group
    virtual void refresh() = 0;                                       // background; never throws; bumps generation
};
```

| Class | Responsibility | Replaces / reuses |
|---|---|---|
| `SkillCatalogue` | merges `SkillSource`s (precedence, sha-collapse, namespacing), owns the `Bm25f` index, `open` with sections | `SkillIndex` |
| `DiskSkills` (a `SkillSource`) | `<home>/skills`, then `skills.external_dirs` read-only; `skill://name/rel` reaches `references/` | `skills::Scanner/Registry` become its engine |
| `MazeSkills` (a `SkillSource`) | cards from `skill:*`, bodies from `skillsrc:*`; filters notes, AFE fragments, legacy cards (parses their inner frontmatter) | `SkillIndex::publish/load` |
| `MemoryCatalogue` | curated rows (`decision: bug: invariant: ops: fact: status: signal: feedback: user: reference:` ~3.7k) as cards cached in `<home>/cache`, delta-synced by id; `search` is local lexical; `deep()` is the pod recall | `Recaller::run` becomes `deep()` |
| `ToolCatalogue` | adapter over `ToolDeck`'s deferred catalog | `ToolDeck::search` |
| `SessionCatalogue` | this session's soaked turns by id | `ledger_`, `turn_ids()` |
| `Bm25f` | inverted index, field weights, normalized score | `tools::rank_documents` for skills |
| `Distiller` | `Signal -> Query` (section 7) | the raw-text query |
| `Trigger` (x6) | pure `plan(signal, working_set)` | scattered calls in `engine.cpp` |
| `WorkingSet` | what is offered / opened / pinned in which epoch; repeat policy; `reattach()` | `in_window()` |
| `Deliverer` | renders a `Delivery` for a `Placement` within a token budget | the string building in `Recaller::run` |
| `Router` | facade: `Delivery on(Signal)`; owns the deep-leg worker and telemetry | `recaller_` + `skills_` in the engine |
| `Tripwires` | rules with a pattern; `check(ToolCall) -> vector<const Rule*>` | new |
| `PromptComposer` | one voice for the system prompt (section 9) | string concatenation in `main()` |
| `Home` | beside-the-binary paths (section 10) | five hard-coded `~/.daedalus` sites |

Patterns, because they were chosen and not sprinkled: Strategy (`Ranker`, `Trigger`, `SkillSource`),
Facade (`Router`), Composite (`SkillCatalogue` over sources), Adapter (`ToolCatalogue`), Null Object
(`NoMemory` when the pod is not configured), Value Objects (`Uri`, `Card`, `Hit`).

### 5.4 Signals and the engine seam

```cpp
using Signal = std::variant<SessionStarted, TurnStarted /*text, anchor*/, CallsFinished /*CallRecord[]*/,
                            Rebased /*epoch*/, Asked /*query, kinds*/>;
struct CallRecord { std::string tool, args_digest, intent; std::vector<std::string> paths, binaries;
                    int exit = 0; std::string error_sig; size_t bytes = 0; };
```

| Engine hook (exists today) | Router call |
|---|---|
| `begin_turn` | `on(TurnStarted)` -> text for the question note |
| `on_assistant` + `on_tool_results` | collect `CallRecord`s (the engine already sees both) |
| `after_tools` | `on(CallsFinished)` -> a hidden item appended to the session |
| `rebase` / `retrieval_base` | `on(Rebased)` -> anchor blocks (the same code path as every other delivery) |
| `end_turn` | telemetry flush; `WorkingSet` turn bump |

A mid-turn note is one appended item: `Session::append(msg, hidden=true)` exists, `Renderer::one`
passes a hidden user item through unchanged (rendered once, byte-stable afterwards),
`Session::current_user()` ignores hidden items so the question never moves, and `sanitize` keeps
it. The rebase anchor already proves such an item survives the chat template. One gap: the
epoch-start collapse pass (`render.cpp`, aged tool groups) knows groups, not notes, so P3 adds
`Item::note` and collapses a note together with the group it follows.

## 6. When, what, how

| Signal | Fires when | What | How | Where / budget | Guard |
|---|---|---|---|---|---|
| Session start | process start / `/new` | pins (`user:`+`feedback:`, flagged `invariant:`: <= 800 tokens), catalogue map (<= 250) | one `get` batch, 3 s deadline, cached copy in `<home>/cache` if the pod is down | system prefix (stable, KV-cached) | built once per launch |
| Turn start | every non-trivial turn | skills + curated memory + tools | local lexical sync (< 5 ms); deep pod recall async, 1.5 s deadline; late result becomes an after-tools note | question note <= 600 tokens | skip `trivial_prompt`; ladder by confidence |
| After a tool batch (**shift**) | >= 2 new content terms (tool args, `intent`, path project/extension, binaries) not covered by this epoch's offers | skills, `invariant:` / `rule:` cards | local only | after-tools note <= 250 | not within 2 rounds of the last note; cards once per epoch |
| Failure | exit != 0 / error head, and (second failure of that tool family this turn, or the error signature matches a card) | `bug:` `invariant:` `ops:` cards, debugging skills | local, plus deep async | after-tools <= 300 | one per signature |
| Stall | identical call x2, or 3 blind explorations (`list_dir`/`read_file` misses, `ls`/`find`/`grep` with nothing new) | redirect notice + retrieval on the *question* | local + deep | after-tools <= 350 | once per turn |
| Rebase | epoch start | active skills as pointers (+ head for the one in use), pins unchanged | `WorkingSet::reattach` | anchor | tokens counted with the measured ratio |
| Model asks | `find`, `read skill://`, `mazemaker_*` | whatever it asks | always served | tool result | recorded as *opened* |
| Tripwire | a rule pattern matches a tool call's arguments | the rule text | `Tripwires::check` before execution | the vetoed call's result | `once` / `after-gap` |
| Idle | between turns | refresh catalogues (generation), warm caches | background | none | never while a turn runs |

**Budgets.** Turn-start note <= 600 tokens, whole turn <= 1,500 (2.7% of the 54.6k mark), <= 2 opened
bodies, <= 6k tokens of bodies. A skill opens head-first (<= 1.2k tokens plus a section list);
`#2`, `#3` page the rest. Strata prefills at ~500 tok/s, so 300 tokens of note cost 0.6 s: every
note has to earn it.

**Offer ladder** (calibrated thresholds, section 7): below `offer` nothing; `offer..open` a pointer
line with `read_file("skill://x")`; at or above `open` and `body_bytes <= auto_max` the head is
opened for the model in the same note. The ladder exists because the model will not follow pointers
reliably; `opened/offered` tells us when to move the thresholds.

What the model sees:

```
<retrieval why="turn">
skill  podman-deep-dive 0.71 - rootless podman / quadlet debugging   -> read_file("skill://podman-deep-dive")
skill  never-touch-live-pods-or-postgres 0.64 - rule                 -> read_file("skill://never-touch-live-pods-or-postgres")
memory #1290233 invariant:podman-symlinks - ...                      -> read_file("mem://1290233")
</retrieval>

<retrieval why="shift: quadlet, systemctl">skill://podman-quadlet-ops 0.68 - read it before the next edit.</retrieval>

<system-notice reason="blind_exploration">3 calls found nothing (list_dir x2, read_file miss). STOP guessing paths:
find("...") or read a skill, then act.</system-notice>

[rule podman-not-docker, once] This box runs podman/quadlet, never docker. Retry with podman. (skill://podman-quadlet-ops)
```

## 7. Ranking, distillation, calibration

- **Tokenizer**: Unicode-aware lowercase; fold ä/ö/ü/ß to ae/oe/ue/ss and strip diacritics; split
  identifiers on `-_./:#` and camelCase; keep version- and error-shaped tokens whole (`0.1.45`,
  `ENOENT`, `E0433`, `HTTP 400`); EN and DE stopword lists.
- **`Bm25f`**: fields name x5, tags x3, group x1.5, gist x1; inverted index built once per
  generation; `search` touches only postings of the query terms (the current code rebuilds document
  frequencies for the whole catalogue on every call, ~9 ms).
- **Normalized score**: `score / ideal(query)` in [0,1], so a long message cannot buy a high score
  with common words. Offer when `norm >= offer` **and** `margin >= m` against the runner-up, or
  `norm >= open`. Starting values `offer 0.45, margin 0.12, open 0.70`, then **fitted on the labeled
  corpus** (grid search; the numbers ship with the test that pins them).
- **`Distiller`** turns a signal into `Query`: drops pasted blocks (> N lines of terminal output,
  continuation summaries), keeps identifiers, error codes, versions, binaries; keeps project names
  *inside paths* (`jrwl-messenger`, `llama.cpp-strata`) and drops only generic path words (`src`,
  `include`, `home`); uses the model's own `intent` when present; <= 12 weighted terms.
- **Hub penalty**: `prior = 1 / (1 + log(1 + wins/expected))` from the corpus replay, so
  `jack-in-a-box` stops winning on length. Fixes the symptom; the cause is cards that say too much,
  addressed in section 8.
- **Semantic leg (optional, last)**: one pod recall scoped to `skill:*`, async, results filtered
  client-side on the label prefix (the scope leaks), fused with the lexical ranking by RRF (k=60),
  hub cards demoted. Needs a measured win over lexical-only before it ships; a local embedder is
  a separate decision.

## 8. Catalogue quality (the catalogue has to deserve the router)

Harness side (`SkillCatalogue`): collapse by sha256; keep the 4 differing duplicates as `name` and
`source/name`; never present a note or an AFE fragment as a skill; parse the frontmatter inside a
legacy card; fall back to disk for a card with no body; every skill gets a derived `gist`.
Bridge side (separate repo, not touched here): enrich each card at index time with a `Triggers:`
line (5-12 phrases in the operator's vocabulary, EN+DE) and `Tags:`; give the 101 bodyless cards
bodies or retire them; write maze-native skills with `source: maze` and their `references/`.
Both together are what lifts routing without a model call at query time (omp keeps a
`skill-descriptions.db` for the same reason: descriptions are made once, matched cheaply).

`dd skills check` prints the census above (bodyless, duplicates, legacy, notes, no-gist) and exits
non-zero past thresholds; it runs in CI against a card snapshot.

Refresh: `generation()` plus `refresh()` on pod recovery and every 10 min while idle. "Empty for
the whole session" cannot happen again.

## 9. Prompt: one voice

`PromptComposer` assembles ordered sections, each owned by a module: Role and tool preamble;
**Retrieval contract** (from the router); Operator pins; Catalogue map; MCP notes. The pod's
`instructions` is not pasted: `mcp_servers.<name>.instructions: harness|verbatim|off`, default
`harness` for the maze server, which substitutes the contract below (the tool list stays; the
protocol steps that contradict the harness, and `session_search`, go).

Contract (RFC 2119 style as in omp, ~110 words):

```
Retrieval. The harness attaches <retrieval> notes: pointers (skills, memories, rules), never answers.
- A pointer that matches the task MUST be opened before you act on it: read_file("skill://name"), read_file("mem://id").
- NEVER guess a path, a command or a skill name. Unknown -> find(query) first.
- A <retrieval> note after a tool result means the task shifted or a call failed: read it before the next call.
- Live state (GPU, RAM, files, ports): run the command; memory is history.
- The same call twice with the same result: stop, find(), change approach.
```

The "live state first" rule from the current preamble stays; the "recall is already done" sentence
goes.

## 10. Home beside the binary

```
<dir>/dd_tui          the binary
<dir>/config.yaml     config
<dir>/skills/ rules/ skins/      own content
<dir>/state/          soak-spool.jsonl, spill/<session>/
<dir>/cache/          card caches, brief
<dir>/logs/
```

`Home::resolve`: `--home DIR` > `$DD_HOME` > the directory of `/proc/self/exe` (symlinks resolved).
**Never** `~/.daedalus` implicitly. One definition: `core/home.cpp` uses it too, `DAEDALUS_HOME` stays
as an override for the ported Python modules. The five hard-coded sites go.

- `dd_tui --init` writes `config.yaml` from `examples/daedalus/config.yaml` and creates the dirs
  (`--from FILE` copies an existing config once).
- `dd import-daedalus [--skills] [--skins]` copies from `~/.daedalus` explicitly, once. Nothing is
  shared at runtime.
- The 467-skill tree is referenced read-only through `skills.external_dirs`, or copied by the import.
- Not writable (`/usr/bin`) and no `--home`: say so in one line and exit.
- `skills.enabled`, `skills.external_dirs`, `skills.disabled`, `skills.auto_route` get wired (F8);
  new knobs live under `knowledge:` (budgets, thresholds, deadlines). `memory.prefetch_timeout_s`'s
  "turn waits on the pod" assumption is replaced by `knowledge.route.deep_ms`.

Test: run with `HOME=$(mktemp -d)` beside a copied binary in an empty directory; assert config and
dirs appear beside the binary and nothing under HOME.

## 11. Telemetry

Per signal and catalogue: offered, opened (read `skill://`, `skill_load`, auto-open), tokens,
latency, deadline misses; tripwire and stall fires. One JSON line per turn to
`<home>/logs/knowledge.jsonl`, a `know 3>1` chip (offered>opened) in the TUI. This is what moves the
ladder thresholds and what tells the curator which skills are dead.

## 12. Phases and acceptance

| Phase | Scope | Acceptance |
|---|---|---|
| **P0 Home** | `Home`, `--init`, `--home`, `import-daedalus`, state under `<home>/state`; dead knobs wired | HOME-clean test; existing suites green; the live `~/.daedalus/config.yaml` copied once with `--init --from` |
| **P1 Catalogue + ranking** | `Catalogue`, `SkillCatalogue` (Disk + Maze), `Bm25f`, `Distiller`, sha collapse, refresh; replaces `SkillIndex`; `dd find`, `dd skills check` | On the labeled corpus: top-3 recall >= 85% on prompts that have a skill; false offers <= 10% on prompts that have none; `search` p99 <= 1 ms; umlaut and hub cases pass; started with the pod down it still serves >= 460 skills from disk; a pod that comes up later bumps `generation` |
| **P2 Router + working set** | `Router`, `WorkingSet`, `Deliverer`, `PromptComposer`, turn-start path with deadline, rebase `reattach`, `read_file("skill://...")`, `find` | `DD_TAPE_LOG` cached/prompt ratio unchanged (>= 95%); first model call <= local + 1.5 s even with the pod at 12 s; notes <= 1,500 tokens/turn; `opened/offered` visible |
| **P3 Mid-turn** | shift / failure / stall triggers, `intent` argument | fake-Strata scenarios (the server built for the window fix, moved under `tests/`): failure -> bug card after the second failure; identical call x2 -> redirect once; a shift -> one note, none for the next 2 rounds; byte-identical prompt prefix with and without notes |
| **P4 Tripwires** | `Tripwires`, `ToolGate` hook in `Agent::execute_tool_calls`, rules from `<home>/rules` and maze `rule:` rows | `docker run` vetoed once and retried with podman; `once` honored; zero tokens added when nothing matches (prompt bytes identical) |
| **P5 Learn** | maze-native skills from solved work; usage telemetry to curation | authored skill appears in `find` on the next refresh without a restart |
| **P6 optional** | semantic leg; workspace `find` | only with a measured win over P1 |

Minimal useful cut: **P0 + P1 + the turn-start half of P2 + the prompt contract** fixes F2-F5, F8, F10
and the measured misses. P3 is the "stop poking blind" part.

Test assets: a scrubbed, labeled corpus (`tests/data/know_prompts.jsonl`, neutral prompts only) and a
card snapshot (name, group, tags, gist, sha; no bodies). The raw replay inputs stay local.

## 13. Risks and decisions

- **The model ignores pointers.** The ladder auto-opens heads at high confidence; `opened/offered`
  decides the thresholds. Calibrating on the real model needs a live Strata run: ask first.
- **Hidden user item after tool results** must behave on the Qwen template: the rebase anchor already
  does this; P3 adds a template test with `/props` chat_template.
- **Pod contention.** Soak writes, the deep leg and the prewarm all hit one serialised pod. One deep
  call in flight; none while a prewarm runs; measure before raising.
- **Strata prefill** (~500 tok/s) prices every token injected: budgets are hard caps, not hints.
- **Non-goals**: an LLM planner in the hot path (the Python `LlmPlanner`); the whole catalogue in
  the prompt; mid-stream abort (omp TTSR) in v1; pod changes; an embedding model in the harness
  before the lexical leg is fixed.
- **Decisions needed**: flat home layout (recommended) vs `<dir>/data/`; which invariants become the
  first tripwires (proposal: podman-not-docker, no Claude commit trailers, never touch live pods or
  postgres, no heavy test launches on the workstation without asking, no `commit -a` / `--no-verify`);
  who implements P0-P1 (a single agent, on `feat/knowledge`).

## 14. Reproducing the measurements

1. Cards: `mazemaker_list_by_label_prefix("skill:")` paged by 200, then `mazemaker_get` by 25 (the
   rows `SkillIndex::load_async` reads).
2. Prompts: Daedalus turns `auto:turn:dd-*` plus operator messages; first 700 chars each.
3. Replay: a 60-line driver linking `recall.cpp`, `tool_search.cpp`, `core/*` (`SkillIndex::load_rows`,
   `match`, `search`), 11 s to build, ~9 ms per search.

All read-only; no engine or harness was started for any of it.

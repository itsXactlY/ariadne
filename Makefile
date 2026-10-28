BUILD ?= build
.DEFAULT_GOAL := $(BUILD)/ariadne
.PHONY: all test clean install install-all doc-audit tsan
all: $(BUILD)/ariadne
# The install is the folder the operator runs ariadne from, the one that owns
# config.yaml; build/ is make's output and never a home (ariadne refuses it).
INSTALL_DIR ?= $(HOME)/projects/daedalus

# Single-invocation, whole-target builds: intentionally no per-TU object files.
CXX ?= g++
# -O2 is a strong runtime/compile-time balance. -pipe avoids temporary assembly
# files; -mtune=native tunes scheduling without baking host-only ISA into the binary.
CXXFLAGS := -std=c++20 -Wall -Wextra -Werror -O2 -pipe -mtune=native -Iinclude
# Use mold when installed; fall back automatically to the system linker otherwise.
MOLD_FLAG := $(shell command -v mold >/dev/null 2>&1 && printf '%s' '-fuse-ld=mold')
CXXFLAGS += $(MOLD_FLAG)

# Every executable rule writes into build/. Create it while parsing the Makefile,
# so even direct goals such as `make $(BUILD)/ariadne` work from a clean checkout.
$(shell mkdir -p $(BUILD))
CORE := src/core/path.cpp src/core/yaml.cpp src/core/json.cpp src/core/message.cpp
SKILLS := src/skills/scanner.cpp src/skills/registry.cpp
TOOLS := src/tools/registry.cpp src/tools/todo_tool.cpp
TOOLRUN := src/tools/shell.cpp src/tools/fs_tools.cpp src/tools/path_security.cpp
AGENT := src/agent/agent.cpp src/agent/chat_stream.cpp
CONFIG := src/core/config.cpp
GATEWAY := src/gateway/platform_registry.cpp
RETRY := src/agent/retry.cpp
CLASSIFICATION := src/agent/classification.cpp
PROMPT_CACHE := src/agent/prompt_caching.cpp
BOUNDARY := src/agent/prompt_cache_boundary.cpp
TAPE := src/agent/payload_tape.cpp
CRON := src/cron/schedule.cpp
COREHOME := src/core/home.cpp
CRON_JOBS := src/cron/jobs.cpp
STICKER := src/gateway/sticker_cache.cpp
PAIRING := src/gateway/pairing.cpp
PATH_SEC := src/tools/path_security.cpp
BIN_EXT := src/tools/binary_extensions.cpp
INTERRUPT := src/tools/interrupt.cpp
PROVENANCE := src/tools/skill_provenance.cpp
DEBUG := src/tools/debug_helpers.cpp
BUDGET := src/tools/budget_config.cpp
TOOLLIMITS := src/tools/tool_output_limits.cpp
ENVPT := src/tools/env_passthrough.cpp
ENV := src/core/env.cpp
BACKENDHELPERS := src/tools/tool_backend_helpers.cpp
AUDIOCONT := src/tools/audio_container.cpp
SHA := src/core/sha1.cpp
SHA256 := src/core/sha256.cpp
CAMOFOX := src/tools/browser_camofox_state.cpp
THINTS := src/tools/terminal_hints.cpp
THREAT := src/tools/threat_patterns.cpp
SCHEMA := src/tools/schema_sanitizer.cpp
URLSAFE := src/tools/url_safety.cpp
OSVPARSE := src/tools/osv_package_parse.cpp
ANSI := src/tools/ansi_strip.cpp
UI := src/ui.cpp src/ui_simd.cpp src/ui_grammar.cpp
NET := src/net/http.cpp $(URLSAFE)
HTTPLLM := src/agent/http_llm.cpp $(RETRY) $(URLSAFE)   # the wire's send policy lives in retry.cpp
CONTEXT := src/context/tape.cpp src/context/render.cpp src/context/endpoint.cpp src/context/maze.cpp src/context/deck.cpp src/context/engine.cpp src/context/recall.cpp
MCPSCHEMA := src/tools/mcp_schema_cache.cpp $(SHA256)   # the staleness key is a sha256 of the config
MCP := src/tools/mcp_client.cpp $(URLSAFE)
DELEG := src/tools/delegation_output_schema.cpp
PATCH := src/tools/patch_parser.cpp
FUZZY := src/tools/fuzzy_match.cpp
APPLY := src/tools/patch_apply.cpp
FS := src/tools/fs_tools.cpp $(PATH_SEC)
TODO := src/tools/todo_tool.cpp
TSEARCH := src/tools/tool_search.cpp
WP := src/tools/website_policy.cpp
AZ := src/gateway/authz_mixin.cpp
CH := src/gateway/channel_directory.cpp
HK := src/gateway/hooks.cpp
MI := src/gateway/mirror.cpp
BK := src/agent/backend_kind.cpp
SG := src/agent/ssl_guard.cpp
RU := src/agent/retry_utils.cpp
KVW := src/agent/kv_watermarks.cpp
MS := src/agent/message_sanitization.cpp
HOMEINIT := src/core/home_init.cpp src/core/default_config.cpp
KNOW := src/know/types.cpp src/know/text.cpp src/know/bm25.cpp src/know/catalogue.cpp src/know/skills.cpp src/know/wiring.cpp src/know/signal.cpp src/know/working_set.cpp src/know/trigger.cpp src/know/deliver.cpp src/know/prompt.cpp src/know/memory_catalogue.cpp src/know/tool_catalogue.cpp src/know/router.cpp src/know/policy.cpp src/know/resolver.cpp src/know/tripwire.cpp src/know/scan.cpp
# what the know module links against: the skill scanner, sha256, config, the home; and the pod's side (recall, maze, deck)
LEARN := $(wildcard src/learn/*.cpp) src/core/crc32c.cpp $(PROVENANCE)   # the sink names its writer
KNOWDEP := $(KNOW) $(SKILLS) $(SHA256) $(CONFIG) $(COREHOME)
KNOWPOD := src/context/recall.cpp src/context/maze.cpp src/context/deck.cpp $(NET) $(MCP) $(TSEARCH) $(TOOLS)

# the embedded default config is examples/ariadne/config.yaml, byte for byte
src/core/default_config.cpp: examples/ariadne/config.yaml
	{ printf 'namespace dd {\nnamespace core {\nconst char* default_config() {\n    return R"DDCFG('; cat $<; printf ')DDCFG";\n}\n}  // namespace core\n}  // namespace dd\n'; } > $@.tmp.$$$$ && mv $@.tmp.$$$$ $@

$(BUILD)/dd_home_tests: $(CORE) $(COREHOME) $(HOMEINIT) include/dd/core/home.hpp tests/core_home_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(HOMEINIT) tests/core_home_test.cpp -o $(BUILD)/dd_home_tests

$(BUILD)/dd_snap_cell_tests: include/dd/core/snap_cell.hpp tests/core_snap_cell_test.cpp
	$(CXX) $(CXXFLAGS) tests/core_snap_cell_test.cpp -pthread -o $(BUILD)/dd_snap_cell_tests

$(BUILD)/dd_park_tests: include/dd/core/park.hpp tests/core_park_test.cpp
	$(CXX) $(CXXFLAGS) tests/core_park_test.cpp -pthread -o $(BUILD)/dd_park_tests

# The ring's whole claim is that racing threads hand a payload over once each. That is a
# data race to prove, so the target the suite runs is not the only one: the TSan build
# below is what falsifies it. `make tsan` builds and runs it.
$(BUILD)/dd_slot_ring_tests: include/dd/core/slot_ring.hpp tests/core_slot_ring_test.cpp
	$(CXX) $(CXXFLAGS) tests/core_slot_ring_test.cpp -pthread -o $(BUILD)/dd_slot_ring_tests

$(BUILD)/dd_slot_ring_tsan: include/dd/core/slot_ring.hpp tests/core_slot_ring_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=thread tests/core_slot_ring_test.cpp -pthread -o $(BUILD)/dd_slot_ring_tsan

$(BUILD)/dd_know_scan_tests: src/know/scan.cpp include/dd/know/scan.hpp tests/know_scan_test.cpp
	$(CXX) $(CXXFLAGS) src/know/scan.cpp tests/know_scan_test.cpp -o $(BUILD)/dd_know_scan_tests

$(BUILD)/dd_know_bm25_tests: src/know/bm25.cpp src/know/text.cpp src/know/scan.cpp src/know/types.cpp include/dd/know/bm25.hpp include/dd/core/par.hpp tests/know_bm25_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp src/know/bm25.cpp src/know/text.cpp src/know/scan.cpp src/know/types.cpp tests/know_bm25_test.cpp -o $(BUILD)/dd_know_bm25_tests

$(BUILD)/dd_know_tokenizer_tests: src/know/text.cpp src/know/scan.cpp include/dd/know/text.hpp include/dd/know/scan.hpp tests/know_tokenizer_test.cpp
	$(CXX) $(CXXFLAGS) src/know/text.cpp src/know/scan.cpp tests/know_tokenizer_test.cpp -o $(BUILD)/dd_know_tokenizer_tests

$(BUILD)/dd_know_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(wildcard include/dd/know/*.hpp) tests/know_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(CORE) $(KNOWDEP) $(KNOWPOD)) tests/know_test.cpp -pthread -lcurl -o $(BUILD)/dd_know_tests

$(BUILD)/dd_know_router_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(wildcard include/dd/know/*.hpp) tests/know_router_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(CORE) $(KNOWDEP) $(KNOWPOD)) tests/know_router_test.cpp -pthread -lcurl -o $(BUILD)/dd_know_router_tests

$(BUILD)/dd_know_rules_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(AGENT) $(wildcard include/dd/know/*.hpp) tests/know_rules_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(AGENT)) tests/know_rules_test.cpp -pthread -lcurl -o $(BUILD)/dd_know_rules_tests

$(BUILD)/ariadne-skills: $(LEARN) $(PROVENANCE) $(CORE) $(KNOWDEP) $(KNOWPOD) $(wildcard include/dd/know/*.hpp) src/cli_main.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(LEARN) $(PROVENANCE) $(CORE) $(KNOWDEP) $(KNOWPOD)) src/cli_main.cpp -pthread -lcurl -o $(BUILD)/ariadne-skills

$(BUILD)/ariadne-agent: $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(TOOLRUN) $(TOOLLIMITS) $(THINTS) $(AGENT) $(CONFIG) src/dd_agent_main.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(TOOLRUN) $(TOOLLIMITS) $(THINTS) $(AGENT) $(CONFIG) src/dd_agent_main.cpp -lcurl -o $(BUILD)/ariadne-agent

# The sidegag: built first and built alone — one translation unit, bare $(CXX), no other
# target in the tree — then it compiles the harness and installs it. `make install`
# proves the toolchain by exercising it, not by asserting it. No suite gate and no probes:
# a missing libcurl or OpenMP is `make`'s own failure, in the language that matters.
$(BUILD)/ariadne-install: installer/ariadne_install.cpp
	@$(CXX) $(CXXFLAGS) -Wshadow -o $@ $<

# The harness only: ariadne, plus the config its home owns. ariadne is always the target
# because it is the only binary that can write the reference config, and --init seeds
# from its own copy rather than one an installer invented.
install: $(BUILD)/ariadne-install
	@$< --booted --root . --home $(INSTALL_DIR)

# The same sidegag, wider: also ariadne-agent (the running agent) and ariadne-skills (the skill catalogue).
install-all: $(BUILD)/ariadne-install
	@$< --booted --root . --home $(INSTALL_DIR) --agent --cli

$(BUILD)/ariadne: $(CORE) $(COREHOME) $(HOMEINIT) $(KNOWDEP) $(TOOLS) $(TOOLRUN) $(TOOLLIMITS) $(THINTS) $(APPLY) $(PROVENANCE) $(ANSI) $(AGENT) $(CONFIG) $(MCP) $(MCPSCHEMA) $(TSEARCH) $(NET) $(HTTPLLM) $(CONTEXT) $(UI) $(wildcard include/dd/*.hpp include/dd/*/*.hpp) src/dd_tui.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(LEARN) $(CORE) $(COREHOME) $(HOMEINIT) $(KNOWDEP) $(TOOLS) $(TOOLRUN) $(TOOLLIMITS) $(THINTS) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(ANSI) $(AGENT) $(CONFIG) $(MCP) $(MCPSCHEMA) $(TSEARCH) $(NET) $(HTTPLLM) $(CONTEXT) $(UI)) src/dd_tui.cpp -pthread -lcurl -o $(BUILD)/ariadne

$(BUILD)/dd_maze_tests: $(CORE) $(TOOLS) $(TSEARCH) $(MCP) $(AGENT) $(NET) $(HTTPLLM) $(CONTEXT) $(KNOWDEP) $(wildcard include/dd/context/*.hpp include/dd/know/*.hpp) tests/maze_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(CORE) $(TOOLS) $(TSEARCH) $(MCP) $(AGENT) $(NET) $(HTTPLLM) $(CONTEXT) $(KNOWDEP)) tests/maze_test.cpp -pthread -lcurl -o $(BUILD)/dd_maze_tests

$(BUILD)/dd_context_tests: $(CORE) $(TOOLS) $(TSEARCH) $(MCP) $(AGENT) $(NET) $(HTTPLLM) $(CONTEXT) $(KNOWDEP) $(wildcard include/dd/context/*.hpp include/dd/know/*.hpp) tests/context_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(CORE) $(TOOLS) $(TSEARCH) $(MCP) $(AGENT) $(NET) $(HTTPLLM) $(CONTEXT) $(KNOWDEP)) tests/context_test.cpp -pthread -lcurl -o $(BUILD)/dd_context_tests

# The Soaker under a race detector: the turn thread publishes into its ring, the spool worker
# claims from it, and the ledger counters are read while the worker is still writing them.
$(BUILD)/dd_context_tsan: $(CORE) $(TOOLS) $(TSEARCH) $(MCP) $(AGENT) $(NET) $(HTTPLLM) $(CONTEXT) $(KNOWDEP) $(wildcard include/dd/context/*.hpp include/dd/know/*.hpp) tests/context_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-but-set-variable -Wno-maybe-uninitialized -fsanitize=thread $(sort $(CORE) $(TOOLS) $(TSEARCH) $(MCP) $(AGENT) $(NET) $(HTTPLLM) $(CONTEXT) $(KNOWDEP)) tests/context_test.cpp -pthread -lcurl -o $(BUILD)/dd_context_tsan

$(BUILD)/dd_chat_stream_tests: $(CORE) src/agent/chat_stream.cpp include/dd/agent/chat_stream.hpp tests/chat_stream_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) src/agent/chat_stream.cpp tests/chat_stream_test.cpp -o $(BUILD)/dd_chat_stream_tests

$(BUILD)/dd_ui_tests: $(CORE) $(ANSI) $(CONFIG) $(UI) include/dd/ui.hpp include/dd/ui_simd.hpp tests/ui_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(CORE) $(ANSI) $(CONFIG) $(UI) tests/ui_test.cpp -pthread -o $(BUILD)/dd_ui_tests

# Scene under a race detector: the agent thread publishes into q_, the render thread claims from
# it, and the scene's own deques are touched by exactly one of the two. Without TSan the claim is
# an argument; with it the claim is a result. -fopenmp is off here because TSan cannot model
# libgomp, so the pragmas compile to sequential loops: the parallel reduction's accumulator and
# the thread-count helper then look unused, which is an artifact of this build, not a defect in
# the shipped -fopenmp one, so those two warnings are silenced here and nowhere else.
$(BUILD)/dd_ui_tsan: $(CORE) $(ANSI) $(CONFIG) $(UI) include/dd/ui.hpp include/dd/ui_simd.hpp tests/ui_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-but-set-variable -fsanitize=thread $(CORE) $(ANSI) $(CONFIG) $(UI) tests/ui_test.cpp -pthread -o $(BUILD)/dd_ui_tsan

$(BUILD)/dd_tests: $(CORE) $(SKILLS) tests/skills_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) tests/skills_test.cpp -o $(BUILD)/dd_tests

$(BUILD)/dd_tools_tests: $(CORE) $(SKILLS) $(TOOLS) tests/tools_registry_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) tests/tools_registry_test.cpp -o $(BUILD)/dd_tools_tests

$(BUILD)/dd_agent_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) tests/agent_loop_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) tests/agent_loop_test.cpp -o $(BUILD)/dd_agent_tests

$(BUILD)/dd_config_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) tests/config_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) tests/config_test.cpp -o $(BUILD)/dd_config_tests

$(BUILD)/dd_gateway_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) tests/gateway_registry_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) tests/gateway_registry_test.cpp -o $(BUILD)/dd_gateway_tests

$(BUILD)/dd_agent_retry_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) tests/agent_retry_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) tests/agent_retry_test.cpp -o $(BUILD)/dd_agent_retry_tests

$(BUILD)/dd_agent_misc_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) tests/agent_misc_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) tests/agent_misc_test.cpp -o $(BUILD)/dd_agent_misc_tests

$(BUILD)/dd_agent_boundary_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) tests/agent_boundary_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) tests/agent_boundary_test.cpp -o $(BUILD)/dd_agent_boundary_tests

$(BUILD)/dd_agent_tape_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) tests/agent_tape_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) tests/agent_tape_test.cpp -o $(BUILD)/dd_agent_tape_tests

$(BUILD)/dd_cron_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) tests/cron_schedule_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) tests/cron_schedule_test.cpp -o $(BUILD)/dd_cron_tests

$(BUILD)/dd_cron_jobs_tests: $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) tests/cron_jobs_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) tests/cron_jobs_test.cpp -o $(BUILD)/dd_cron_jobs_tests

$(BUILD)/dd_sticker_tests: $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) $(STICKER) tests/gateway_sticker_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) $(STICKER) tests/gateway_sticker_test.cpp -o $(BUILD)/dd_sticker_tests

$(BUILD)/dd_pairing_tests: $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) $(STICKER) $(PAIRING) tests/gateway_pairing_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) $(STICKER) $(PAIRING) tests/gateway_pairing_test.cpp -o $(BUILD)/dd_pairing_tests

$(BUILD)/dd_tools_output_envelope_tests: $(CORE) $(TOOLS) $(TOOLRUN) $(CONFIG) $(COREHOME) $(TOOLLIMITS) $(THINTS) tests/tools_output_envelope_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TOOLS) $(TOOLRUN) $(CONFIG) $(COREHOME) $(TOOLLIMITS) $(THINTS) tests/tools_output_envelope_test.cpp -o $(BUILD)/dd_tools_output_envelope_tests

$(BUILD)/dd_tools_security_tests: $(CORE) $(TOOLS) $(PATH_SEC) $(BIN_EXT) tests/tools_security_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TOOLS) $(PATH_SEC) $(BIN_EXT) tests/tools_security_test.cpp -o $(BUILD)/dd_tools_security_tests

$(BUILD)/dd_tools_interrupt_tests: $(CORE) $(TOOLS) $(INTERRUPT) $(PROVENANCE) tests/tools_interrupt_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TOOLS) $(INTERRUPT) $(PROVENANCE) tests/tools_interrupt_test.cpp -o $(BUILD)/dd_tools_interrupt_tests

$(BUILD)/dd_debug_tests: $(CORE) $(COREHOME) $(DEBUG) tests/tools_debug_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(DEBUG) tests/tools_debug_test.cpp -o $(BUILD)/dd_debug_tests

$(BUILD)/dd_budget_tests: $(CORE) $(TOOLS) $(BUDGET) tests/tools_budget_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TOOLS) $(BUDGET) tests/tools_budget_test.cpp -o $(BUILD)/dd_budget_tests

$(BUILD)/dd_tool_limits_tests: $(CORE) $(COREHOME) $(CONFIG) $(TOOLLIMITS) tests/tools_output_limits_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(CONFIG) $(TOOLLIMITS) tests/tools_output_limits_test.cpp -o $(BUILD)/dd_tool_limits_tests

$(BUILD)/dd_env_passthrough_tests: $(CORE) $(COREHOME) $(CONFIG) $(ENVPT) tests/tools_env_passthrough_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(CONFIG) $(ENVPT) tests/tools_env_passthrough_test.cpp -o $(BUILD)/dd_env_passthrough_tests

$(BUILD)/dd_backend_helpers_tests: $(CORE) $(COREHOME) $(CONFIG) $(ENV) $(BACKENDHELPERS) tests/tools_backend_helpers_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(CONFIG) $(ENV) $(BACKENDHELPERS) tests/tools_backend_helpers_test.cpp -o $(BUILD)/dd_backend_helpers_tests

$(BUILD)/dd_audio_container_tests: $(CORE) $(AUDIOCONT) tests/tools_audio_container_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(AUDIOCONT) tests/tools_audio_container_test.cpp -o $(BUILD)/dd_audio_container_tests

$(BUILD)/dd_sha1_tests: $(CORE) $(SHA) tests/core_sha1_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SHA) tests/core_sha1_test.cpp -o $(BUILD)/dd_sha1_tests

$(BUILD)/dd_sha256_tests: $(CORE) $(SHA256) tests/core_sha256_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SHA256) tests/core_sha256_test.cpp -o $(BUILD)/dd_sha256_tests

$(BUILD)/dd_camofox_tests: $(CORE) $(COREHOME) $(SHA) $(CAMOFOX) tests/tools_camofox_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(SHA) $(CAMOFOX) tests/tools_camofox_test.cpp -o $(BUILD)/dd_camofox_tests

$(BUILD)/dd_terminal_hints_tests: $(CORE) $(THINTS) tests/tools_terminal_hints_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(THINTS) tests/tools_terminal_hints_test.cpp -o $(BUILD)/dd_terminal_hints_tests

$(BUILD)/dd_threat_patterns_tests: $(CORE) $(THREAT) tests/tools_threat_patterns_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(THREAT) tests/tools_threat_patterns_test.cpp -o $(BUILD)/dd_threat_patterns_tests -licuuc -licudata

$(BUILD)/dd_schema_sanitizer_tests: $(CORE) $(SCHEMA) tests/tools_schema_sanitizer_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SCHEMA) tests/tools_schema_sanitizer_test.cpp -o $(BUILD)/dd_schema_sanitizer_tests

$(BUILD)/dd_url_safety_tests: $(CORE) $(URLSAFE) tests/tools_url_safety_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(URLSAFE) tests/tools_url_safety_test.cpp -o $(BUILD)/dd_url_safety_tests

$(BUILD)/dd_osv_parse_tests: $(CORE) $(OSVPARSE) tests/tools_osv_parse_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(OSVPARSE) tests/tools_osv_parse_test.cpp -o $(BUILD)/dd_osv_parse_tests

$(BUILD)/dd_ansi_strip_tests: $(CORE) $(ANSI) tests/tools_ansi_strip_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(ANSI) tests/tools_ansi_strip_test.cpp -o $(BUILD)/dd_ansi_strip_tests

$(BUILD)/dd_mcp_schema_cache_tests: $(CORE) $(MCPSCHEMA) tests/tools_mcp_schema_cache_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(MCPSCHEMA) tests/tools_mcp_schema_cache_test.cpp -o $(BUILD)/dd_mcp_schema_cache_tests

$(BUILD)/dd_delegation_schema_tests: $(CORE) $(DELEG) tests/tools_delegation_schema_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(DELEG) tests/tools_delegation_schema_test.cpp -o $(BUILD)/dd_delegation_schema_tests

$(BUILD)/dd_patch_parser_tests: $(CORE) $(PATCH) tests/tools_patch_parser_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(PATCH) tests/tools_patch_parser_test.cpp -o $(BUILD)/dd_patch_parser_tests

$(BUILD)/dd_fuzzy_match_tests: $(CORE) $(FUZZY) tests/tools_fuzzy_match_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(FUZZY) tests/tools_fuzzy_match_test.cpp -o $(BUILD)/dd_fuzzy_match_tests

# The V4A application layer: the patch parser's operations applied to real files. fs_edit reads the
# provenance stack, so that TU is in the link.
# bench tool, not a test: it needs a skills root on disk, so it is run against a live home rather
# than a fixture. The sink's handback rule is asserted in dd_learn_author_tests; this is the live
# proof that a skill the model edited through edit_file is handed back and its bytes do not move.
$(BUILD)/dd_sink_handback_probe: $(LEARN_AUTHOR) $(LEARN_AUTHOR_DEPS) $(PROVENANCE) bench/sink-handback-probe.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(LEARN_AUTHOR) $(LEARN_AUTHOR_DEPS) $(PROVENANCE)) bench/sink-handback-probe.cpp -pthread -o $(BUILD)/dd_sink_handback_probe

$(BUILD)/dd_patch_apply_tests: $(CORE) $(FS) $(PROVENANCE) $(PATCH) $(FUZZY) $(APPLY) tests/tools_patch_apply_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(FS) $(PROVENANCE) $(PATCH) $(FUZZY) $(APPLY) tests/tools_patch_apply_test.cpp -o $(BUILD)/dd_patch_apply_tests

$(BUILD)/dd_patch_apply_asan: $(CORE) $(FS) $(PROVENANCE) $(PATCH) $(FUZZY) $(APPLY) tests/tools_patch_apply_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -Wno-maybe-uninitialized -fsanitize=address,undefined $(CORE) $(FS) $(PROVENANCE) $(PATCH) $(FUZZY) $(APPLY) tests/tools_patch_apply_test.cpp -o $(BUILD)/dd_patch_apply_asan
$(BUILD)/dd_todo_tests: $(CORE) $(TODO) tests/tools_todo_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TODO) tests/tools_todo_test.cpp -o $(BUILD)/dd_todo_tests
$(BUILD)/dd_tool_search_tests: $(CORE) $(TSEARCH) tests/tools_tool_search_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TSEARCH) tests/tools_tool_search_test.cpp -o $(BUILD)/dd_tool_search_tests
$(BUILD)/dd_website_policy_tests: $(CORE) $(WP) tests/tools_website_policy_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(WP) tests/tools_website_policy_test.cpp -o $(BUILD)/dd_website_policy_tests

$(BUILD)/dd_gateway_authz_tests: $(CORE) $(AZ) tests/gateway_authz_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(AZ) tests/gateway_authz_test.cpp -o $(BUILD)/dd_gateway_authz_tests

$(BUILD)/dd_channel_directory_tests: $(CORE) $(CH) tests/gateway_channel_directory_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(CH) tests/gateway_channel_directory_test.cpp -o $(BUILD)/dd_channel_directory_tests

$(BUILD)/dd_hooks_tests: $(CORE) $(HK) tests/gateway_hooks_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(HK) tests/gateway_hooks_test.cpp -o $(BUILD)/dd_hooks_tests

$(BUILD)/dd_mirror_tests: $(CORE) $(MI) tests/gateway_mirror_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(MI) tests/gateway_mirror_test.cpp -o $(BUILD)/dd_mirror_tests

$(BUILD)/dd_backend_kind_tests: $(BK) tests/agent_backend_kind_test.cpp
	$(CXX) $(CXXFLAGS) $(BK) tests/agent_backend_kind_test.cpp -o $(BUILD)/dd_backend_kind_tests

$(BUILD)/dd_errors_tests: tests/agent_errors_test.cpp
	$(CXX) $(CXXFLAGS) tests/agent_errors_test.cpp -o $(BUILD)/dd_errors_tests

$(BUILD)/dd_ssl_guard_tests: $(SG) tests/agent_ssl_guard_test.cpp
	$(CXX) $(CXXFLAGS) $(SG) tests/agent_ssl_guard_test.cpp -o $(BUILD)/dd_ssl_guard_tests

$(BUILD)/dd_retry_tests: $(RU) tests/agent_retry_utils_test.cpp
	$(CXX) $(CXXFLAGS) $(RU) tests/agent_retry_utils_test.cpp -o $(BUILD)/dd_retry_tests

# The retry policy's proof is a connection count, so it needs a real socket: a fake
# endpoint on loopback answers 500 once and 200 on the second send, and nothing twice.
$(BUILD)/dd_wire_retry_tests: $(CORE) src/agent/chat_stream.cpp $(RETRY) $(NET) $(HTTPLLM) include/dd/agent/retry.hpp include/dd/agent/http_llm.hpp tests/agent_wire_retry_test.cpp
	$(CXX) $(CXXFLAGS) $(sort $(CORE) src/agent/chat_stream.cpp $(RETRY) $(NET) $(HTTPLLM)) tests/agent_wire_retry_test.cpp -pthread -lcurl -o $(BUILD)/dd_wire_retry_tests

# W5's proof is the bytes ariadne sends, so it boots ariadne against a fake MCP server
# and a fake endpoint on one loopback box. The binary itself is the thing under test.
$(BUILD)/dd_mcp_prose_mask_tests: include/dd/agent/redact.hpp tests/mcp_prose_mask_test.cpp tests/fake_mcp_box.hpp $(BUILD)/ariadne
	$(CXX) $(CXXFLAGS) tests/mcp_prose_mask_test.cpp -pthread -o $(BUILD)/dd_mcp_prose_mask_tests

# W6's boot proof: three boots of the same home against a server whose tools/list costs
# real milliseconds. It runs the shipped binary, so it depends on it.
$(BUILD)/dd_mcp_cache_boot_tests: tests/mcp_cache_boot_test.cpp tests/fake_mcp_box.hpp $(BUILD)/ariadne
	$(CXX) $(CXXFLAGS) tests/mcp_cache_boot_test.cpp -pthread -o $(BUILD)/dd_mcp_cache_boot_tests


$(BUILD)/dd_kv_watermarks_tests: $(KVW) src/core/json.cpp tests/agent_kv_watermarks_test.cpp
	$(CXX) $(CXXFLAGS) $(KVW) src/core/json.cpp tests/agent_kv_watermarks_test.cpp -o $(BUILD)/dd_kv_watermarks_tests

$(BUILD)/dd_message_sanitization_tests: $(MS) src/core/json.cpp tests/agent_message_sanitization_test.cpp
	$(CXX) $(CXXFLAGS) $(MS) src/core/json.cpp tests/agent_message_sanitization_test.cpp -o $(BUILD)/dd_message_sanitization_tests

$(BUILD)/dd_redact_tests: tests/agent_redact_test.cpp
	$(CXX) $(CXXFLAGS) tests/agent_redact_test.cpp -o $(BUILD)/dd_redact_tests

$(BUILD)/dd_usage_pricing_tests: tests/agent_usage_pricing_test.cpp
	$(CXX) $(CXXFLAGS) tests/agent_usage_pricing_test.cpp -o $(BUILD)/dd_usage_pricing_tests

$(BUILD)/dd_turn_context_tests: tests/agent_turn_context_test.cpp
	$(CXX) $(CXXFLAGS) tests/agent_turn_context_test.cpp -o $(BUILD)/dd_turn_context_tests

$(BUILD)/dd_context_references_tests: tests/agent_context_references_test.cpp
	$(CXX) $(CXXFLAGS) tests/agent_context_references_test.cpp -o $(BUILD)/dd_context_references_tests

$(BUILD)/dd_acp_tools_tests: src/core/json.cpp tests/acp_tools_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/acp_tools_test.cpp -o $(BUILD)/dd_acp_tools_tests

$(BUILD)/dd_root_heuristics_tests: tests/root_heuristics_test.cpp
	$(CXX) $(CXXFLAGS) tests/root_heuristics_test.cpp -o $(BUILD)/dd_root_heuristics_tests

$(BUILD)/dd_state_sql_tests: src/core/json.cpp tests/state_sql_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/state_sql_test.cpp -o $(BUILD)/dd_state_sql_tests

$(BUILD)/dd_cli_tests: tests/cli_heuristics_test.cpp
	$(CXX) $(CXXFLAGS) tests/cli_heuristics_test.cpp -o $(BUILD)/dd_cli_tests

$(BUILD)/dd_state_scrub_tests: src/core/json.cpp tests/state_scrub_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/state_scrub_test.cpp -o $(BUILD)/dd_state_scrub_tests

$(BUILD)/dd_run_helpers_tests: src/core/json.cpp tests/run_helpers_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/run_helpers_test.cpp -o $(BUILD)/dd_run_helpers_tests

$(BUILD)/dd_state_errors_tests: tests/state_errors_test.cpp
	$(CXX) $(CXXFLAGS) tests/state_errors_test.cpp -o $(BUILD)/dd_state_errors_tests

$(BUILD)/dd_constants_tests: src/core/json.cpp tests/constants_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/constants_test.cpp -o $(BUILD)/dd_constants_tests

$(BUILD)/dd_task_intensity_tests: src/core/json.cpp tests/task_intensity_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/task_intensity_test.cpp -o $(BUILD)/dd_task_intensity_tests

$(BUILD)/dd_json_tests: src/core/json.cpp include/dd/core/json.hpp tests/json_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/json_test.cpp -o $(BUILD)/dd_json_tests

$(BUILD)/dd_harness_config_tests: src/core/json.cpp tests/harness_config_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/harness_config_test.cpp -o $(BUILD)/dd_harness_config_tests

$(BUILD)/dd_model_metadata_tests: src/core/json.cpp tests/model_metadata_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/model_metadata_test.cpp -o $(BUILD)/dd_model_metadata_tests

$(BUILD)/dd_trajectory_compressor_tests: src/core/json.cpp tests/trajectory_compressor_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/trajectory_compressor_test.cpp -o $(BUILD)/dd_trajectory_compressor_tests

$(BUILD)/dd_cc_tests: src/core/json.cpp tests/context_compressor_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/context_compressor_test.cpp -o $(BUILD)/dd_cc_tests

# Test binaries run one after another, every one even if an earlier one fails; the target fails at the end if any did.
# mk/*.mk fragments append their own binaries with  TEST_BINS += $(BUILD)/dd_x_tests
TEST_BINS := $(BUILD)/dd_tests \
  $(BUILD)/dd_tools_tests \
  $(BUILD)/dd_agent_tests \
  $(BUILD)/dd_config_tests \
  $(BUILD)/dd_gateway_tests \
  $(BUILD)/dd_agent_retry_tests \
  $(BUILD)/dd_agent_misc_tests \
  $(BUILD)/dd_agent_boundary_tests \
  $(BUILD)/dd_agent_tape_tests \
  $(BUILD)/dd_cron_tests \
  $(BUILD)/dd_cron_jobs_tests \
  $(BUILD)/dd_sticker_tests \
  $(BUILD)/dd_pairing_tests \
  $(BUILD)/dd_tools_output_envelope_tests \
  $(BUILD)/dd_tools_security_tests \
  $(BUILD)/dd_tools_interrupt_tests \
  $(BUILD)/dd_debug_tests \
  $(BUILD)/dd_budget_tests \
  $(BUILD)/dd_tool_limits_tests \
  $(BUILD)/dd_env_passthrough_tests \
  $(BUILD)/dd_backend_helpers_tests \
  $(BUILD)/dd_audio_container_tests \
  $(BUILD)/dd_sha1_tests \
  $(BUILD)/dd_sha256_tests \
  $(BUILD)/dd_camofox_tests \
  $(BUILD)/dd_terminal_hints_tests \
  $(BUILD)/dd_threat_patterns_tests \
  $(BUILD)/dd_schema_sanitizer_tests \
  $(BUILD)/dd_url_safety_tests \
  $(BUILD)/dd_osv_parse_tests \
  $(BUILD)/dd_ansi_strip_tests \
  $(BUILD)/dd_mcp_schema_cache_tests \
  $(BUILD)/dd_delegation_schema_tests \
  $(BUILD)/dd_patch_parser_tests \
  $(BUILD)/dd_fuzzy_match_tests \
  $(BUILD)/dd_patch_apply_tests \
  $(BUILD)/dd_todo_tests \
  $(BUILD)/dd_tool_search_tests \
  $(BUILD)/dd_website_policy_tests \
  $(BUILD)/dd_gateway_authz_tests \
  $(BUILD)/dd_channel_directory_tests \
  $(BUILD)/dd_hooks_tests \
  $(BUILD)/dd_mirror_tests \
  $(BUILD)/dd_backend_kind_tests \
  $(BUILD)/dd_errors_tests \
  $(BUILD)/dd_ssl_guard_tests \
  $(BUILD)/dd_retry_tests \
  $(BUILD)/dd_wire_retry_tests \
  $(BUILD)/dd_mcp_prose_mask_tests \
  $(BUILD)/dd_mcp_cache_boot_tests \
  $(BUILD)/dd_kv_watermarks_tests \
  $(BUILD)/dd_state_errors_tests \
  $(BUILD)/dd_constants_tests \
  $(BUILD)/dd_task_intensity_tests \
  $(BUILD)/dd_harness_config_tests \
  $(BUILD)/dd_model_metadata_tests \
  $(BUILD)/dd_trajectory_compressor_tests \
  $(BUILD)/dd_cc_tests \
  $(BUILD)/dd_ui_tests \
  $(BUILD)/dd_chat_stream_tests \
  $(BUILD)/dd_context_tests \
  $(BUILD)/dd_maze_tests \
  $(BUILD)/dd_json_tests \
  $(BUILD)/dd_home_tests \
  $(BUILD)/dd_snap_cell_tests \
  $(BUILD)/dd_park_tests \
  $(BUILD)/dd_slot_ring_tests \
  $(BUILD)/dd_know_scan_tests \
  $(BUILD)/dd_know_tokenizer_tests \
  $(BUILD)/dd_know_bm25_tests \
  $(BUILD)/dd_know_tests \
  $(BUILD)/dd_know_router_tests \
  $(BUILD)/dd_know_rules_tests \
  $(BUILD)/dd_acp_tools_tests \
  $(BUILD)/dd_cli_tests \
  $(BUILD)/dd_context_references_tests \
  $(BUILD)/dd_learn_author_tests \
  $(BUILD)/dd_learn_credit_tests \
  $(BUILD)/dd_learn_inspect_tests \
  $(BUILD)/dd_learn_ledger_tests \
  $(BUILD)/dd_learn_lifecycle_tests \
  $(BUILD)/dd_learn_overlay_tests \
  $(BUILD)/dd_learn_service_tests \
  $(BUILD)/dd_learn_soak_tests \
  $(BUILD)/dd_message_sanitization_tests \
  $(BUILD)/dd_redact_tests \
  $(BUILD)/dd_root_heuristics_tests \
  $(BUILD)/dd_run_helpers_tests \
  $(BUILD)/dd_state_scrub_tests \
  $(BUILD)/dd_state_sql_tests \
  $(BUILD)/dd_turn_context_tests \
  $(BUILD)/dd_usage_pricing_tests

-include $(wildcard mk/*.mk)

# Every hand-off in the tree is a claim about racing threads, and the tree has no lock left to
# hide a race behind — so a race detector is the only thing that can falsify it. These four are
# the owners the audit names: the ring itself, the Scene, the sink, the Soaker. Built with
# -fsanitize=thread and run, not folded into `make test` (TSan costs the suite more than the
# claim is worth on every run). halt_on_error=1 makes a race a failed target rather than a
# warning that scrolls past.
tsan: $(BUILD)/dd_slot_ring_tsan $(BUILD)/dd_ui_tsan $(BUILD)/dd_learn_credit_tsan $(BUILD)/dd_context_tsan
	@fail=0; for t in $(BUILD)/dd_slot_ring_tsan $(BUILD)/dd_ui_tsan $(BUILD)/dd_learn_credit_tsan $(BUILD)/dd_context_tsan; do \
		echo "== $$t"; DD_TEST_TMP=$(BUILD)/tmp TSAN_OPTIONS=halt_on_error=1 ./$$t || { echo "TSAN FAILED: $$t"; fail=$$((fail+1)); }; done; \
		echo "$$fail tsan binary(ies) failed"; [ $$fail -eq 0 ]

test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do echo "== $$t"; DD_TEST_TMP=$(BUILD)/tmp ./$$t || { echo "FAILED: $$t"; fail=$$((fail+1)); }; done; \
	echo "$$fail test binary(ies) failed"; [ $$fail -eq 0 ]

doc-audit:
	@python3 bench/doc_audit.py

clean:
	-rm -rf $(BUILD)

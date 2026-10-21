CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -Werror -O2 -Iinclude
CORE := src/core/path.cpp src/core/yaml.cpp src/core/json.cpp src/core/message.cpp
SKILLS := src/skills/scanner.cpp src/skills/registry.cpp
TOOLS := src/tools/registry.cpp
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
UI := src/ui.cpp src/ui_simd.cpp
NET := src/net/http.cpp
HTTPLLM := src/agent/http_llm.cpp
CONTEXT := src/context/tape.cpp src/context/render.cpp src/context/endpoint.cpp src/context/maze.cpp src/context/deck.cpp src/context/engine.cpp src/context/recall.cpp
MCPSCHEMA := src/tools/mcp_schema_cache.cpp
MCP := src/tools/mcp_client.cpp
DELEG := src/tools/delegation_output_schema.cpp
PATCH := src/tools/patch_parser.cpp
FUZZY := src/tools/fuzzy_match.cpp
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

# the embedded default config is examples/daedalus/config.yaml, byte for byte
src/core/default_config.cpp: examples/daedalus/config.yaml
	{ printf 'namespace dd {\nnamespace core {\nconst char* default_config() {\n    return R"DDCFG('; cat $<; printf ')DDCFG";\n}\n}  // namespace core\n}  // namespace dd\n'; } > $@

build/dd_home_tests: $(CORE) $(COREHOME) $(HOMEINIT) include/dd/core/home.hpp tests/core_home_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(HOMEINIT) tests/core_home_test.cpp -o build/dd_home_tests

build/dd: $(CORE) $(SKILLS) $(TOOLS) src/cli_main.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) src/cli_main.cpp -o build/dd

build/dd_agent: $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) src/dd_agent_main.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) src/dd_agent_main.cpp -lcurl -o build/dd_agent


build/dd_tui: $(CORE) $(COREHOME) $(HOMEINIT) $(TOOLS) $(ANSI) $(AGENT) $(CONFIG) $(MCP) $(TSEARCH) $(NET) $(HTTPLLM) $(CONTEXT) $(UI) $(wildcard include/dd/*.hpp include/dd/*/*.hpp) src/dd_tui.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(CORE) $(COREHOME) $(HOMEINIT) $(TOOLS) $(ANSI) $(AGENT) $(CONFIG) $(MCP) $(TSEARCH) $(NET) $(HTTPLLM) $(CONTEXT) $(UI) src/dd_tui.cpp -pthread -lcurl -o build/dd_tui

build/dd_maze_tests: $(CORE) $(TOOLS) $(TSEARCH) $(MCP) $(AGENT) $(NET) $(HTTPLLM) $(CONTEXT) $(wildcard include/dd/context/*.hpp) tests/maze_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TOOLS) $(TSEARCH) $(MCP) $(AGENT) $(NET) $(HTTPLLM) $(CONTEXT) tests/maze_test.cpp -pthread -lcurl -o build/dd_maze_tests

build/dd_context_tests: $(CORE) $(TOOLS) $(TSEARCH) $(MCP) $(AGENT) $(NET) $(HTTPLLM) $(CONTEXT) $(wildcard include/dd/context/*.hpp) tests/context_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TOOLS) $(TSEARCH) $(MCP) $(AGENT) $(NET) $(HTTPLLM) $(CONTEXT) tests/context_test.cpp -pthread -lcurl -o build/dd_context_tests

build/dd_chat_stream_tests: $(CORE) src/agent/chat_stream.cpp include/dd/agent/chat_stream.hpp tests/chat_stream_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) src/agent/chat_stream.cpp tests/chat_stream_test.cpp -o build/dd_chat_stream_tests

build/dd_ui_tests: $(CORE) $(ANSI) $(CONFIG) $(UI) include/dd/ui.hpp include/dd/ui_simd.hpp tests/ui_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(CORE) $(ANSI) $(CONFIG) $(UI) tests/ui_test.cpp -pthread -o build/dd_ui_tests

build/dd_tests: $(CORE) $(SKILLS) tests/skills_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) tests/skills_test.cpp -o build/dd_tests

build/dd_tools_tests: $(CORE) $(SKILLS) $(TOOLS) tests/tools_registry_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) tests/tools_registry_test.cpp -o build/dd_tools_tests

build/dd_agent_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) tests/agent_loop_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) tests/agent_loop_test.cpp -o build/dd_agent_tests

build/dd_config_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) tests/config_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) tests/config_test.cpp -o build/dd_config_tests

build/dd_gateway_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) tests/gateway_registry_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) tests/gateway_registry_test.cpp -o build/dd_gateway_tests

build/dd_agent_retry_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) tests/agent_retry_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) tests/agent_retry_test.cpp -o build/dd_agent_retry_tests

build/dd_agent_misc_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) tests/agent_misc_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) tests/agent_misc_test.cpp -o build/dd_agent_misc_tests

build/dd_agent_boundary_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) tests/agent_boundary_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) tests/agent_boundary_test.cpp -o build/dd_agent_boundary_tests

build/dd_agent_tape_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) tests/agent_tape_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) tests/agent_tape_test.cpp -o build/dd_agent_tape_tests

build/dd_cron_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) tests/cron_schedule_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) tests/cron_schedule_test.cpp -o build/dd_cron_tests

build/dd_cron_jobs_tests: $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) tests/cron_jobs_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) tests/cron_jobs_test.cpp -o build/dd_cron_jobs_tests

build/dd_sticker_tests: $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) $(STICKER) tests/gateway_sticker_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) $(STICKER) tests/gateway_sticker_test.cpp -o build/dd_sticker_tests

build/dd_pairing_tests: $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) $(STICKER) $(PAIRING) tests/gateway_pairing_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(SKILLS) $(TOOLS) $(AGENT) $(CONFIG) $(GATEWAY) $(RETRY) $(CLASSIFICATION) $(PROMPT_CACHE) $(BOUNDARY) $(TAPE) $(CRON) $(CRON_JOBS) $(STICKER) $(PAIRING) tests/gateway_pairing_test.cpp -o build/dd_pairing_tests

build/dd_tools_security_tests: $(CORE) $(TOOLS) $(PATH_SEC) $(BIN_EXT) tests/tools_security_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TOOLS) $(PATH_SEC) $(BIN_EXT) tests/tools_security_test.cpp -o build/dd_tools_security_tests

build/dd_tools_interrupt_tests: $(CORE) $(TOOLS) $(INTERRUPT) $(PROVENANCE) tests/tools_interrupt_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TOOLS) $(INTERRUPT) $(PROVENANCE) tests/tools_interrupt_test.cpp -o build/dd_tools_interrupt_tests

build/dd_debug_tests: $(CORE) $(COREHOME) $(DEBUG) tests/tools_debug_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(DEBUG) tests/tools_debug_test.cpp -o build/dd_debug_tests

build/dd_budget_tests: $(CORE) $(TOOLS) $(BUDGET) tests/tools_budget_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TOOLS) $(BUDGET) tests/tools_budget_test.cpp -o build/dd_budget_tests

build/dd_tool_limits_tests: $(CORE) $(COREHOME) $(CONFIG) $(TOOLLIMITS) tests/tools_output_limits_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(CONFIG) $(TOOLLIMITS) tests/tools_output_limits_test.cpp -o build/dd_tool_limits_tests

build/dd_env_passthrough_tests: $(CORE) $(COREHOME) $(CONFIG) $(ENVPT) tests/tools_env_passthrough_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(CONFIG) $(ENVPT) tests/tools_env_passthrough_test.cpp -o build/dd_env_passthrough_tests


build/dd_backend_helpers_tests: $(CORE) $(COREHOME) $(CONFIG) $(ENV) $(BACKENDHELPERS) tests/tools_backend_helpers_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(CONFIG) $(ENV) $(BACKENDHELPERS) tests/tools_backend_helpers_test.cpp -o build/dd_backend_helpers_tests

build/dd_audio_container_tests: $(CORE) $(AUDIOCONT) tests/tools_audio_container_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(AUDIOCONT) tests/tools_audio_container_test.cpp -o build/dd_audio_container_tests

build/dd_sha1_tests: $(CORE) $(SHA) tests/core_sha1_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SHA) tests/core_sha1_test.cpp -o build/dd_sha1_tests

build/dd_sha256_tests: $(CORE) $(SHA256) tests/core_sha256_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SHA256) tests/core_sha256_test.cpp -o build/dd_sha256_tests

build/dd_camofox_tests: $(CORE) $(COREHOME) $(SHA) $(CAMOFOX) tests/tools_camofox_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(COREHOME) $(SHA) $(CAMOFOX) tests/tools_camofox_test.cpp -o build/dd_camofox_tests

build/dd_terminal_hints_tests: $(CORE) $(THINTS) tests/tools_terminal_hints_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(THINTS) tests/tools_terminal_hints_test.cpp -o build/dd_terminal_hints_tests

build/dd_threat_patterns_tests: $(CORE) $(THREAT) tests/tools_threat_patterns_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(THREAT) tests/tools_threat_patterns_test.cpp -o build/dd_threat_patterns_tests -licuuc -licudata

build/dd_schema_sanitizer_tests: $(CORE) $(SCHEMA) tests/tools_schema_sanitizer_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SCHEMA) tests/tools_schema_sanitizer_test.cpp -o build/dd_schema_sanitizer_tests

build/dd_url_safety_tests: $(CORE) $(URLSAFE) tests/tools_url_safety_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(URLSAFE) tests/tools_url_safety_test.cpp -o build/dd_url_safety_tests

build/dd_osv_parse_tests: $(CORE) $(OSVPARSE) tests/tools_osv_parse_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(OSVPARSE) tests/tools_osv_parse_test.cpp -o build/dd_osv_parse_tests

build/dd_ansi_strip_tests: $(CORE) $(ANSI) tests/tools_ansi_strip_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(ANSI) tests/tools_ansi_strip_test.cpp -o build/dd_ansi_strip_tests

build/dd_mcp_schema_cache_tests: $(CORE) $(SHA256) $(MCPSCHEMA) tests/tools_mcp_schema_cache_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SHA256) $(MCPSCHEMA) tests/tools_mcp_schema_cache_test.cpp -o build/dd_mcp_schema_cache_tests

build/dd_delegation_schema_tests: $(CORE) $(DELEG) tests/tools_delegation_schema_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(DELEG) tests/tools_delegation_schema_test.cpp -o build/dd_delegation_schema_tests

build/dd_patch_parser_tests: $(CORE) $(PATCH) tests/tools_patch_parser_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(PATCH) tests/tools_patch_parser_test.cpp -o build/dd_patch_parser_tests

build/dd_fuzzy_match_tests: $(CORE) $(FUZZY) tests/tools_fuzzy_match_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(FUZZY) tests/tools_fuzzy_match_test.cpp -o build/dd_fuzzy_match_tests
build/dd_todo_tests: $(CORE) $(TODO) tests/tools_todo_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TODO) tests/tools_todo_test.cpp -o build/dd_todo_tests
build/dd_tool_search_tests: $(CORE) $(TSEARCH) tests/tools_tool_search_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(TSEARCH) tests/tools_tool_search_test.cpp -o build/dd_tool_search_tests
build/dd_website_policy_tests: $(CORE) $(WP) tests/tools_website_policy_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(WP) tests/tools_website_policy_test.cpp -o build/dd_website_policy_tests

build/dd_gateway_authz_tests: $(CORE) $(AZ) tests/gateway_authz_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(AZ) tests/gateway_authz_test.cpp -o build/dd_gateway_authz_tests

build/dd_channel_directory_tests: $(CORE) $(CH) tests/gateway_channel_directory_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(CH) tests/gateway_channel_directory_test.cpp -o build/dd_channel_directory_tests

build/dd_hooks_tests: $(CORE) $(HK) tests/gateway_hooks_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(HK) tests/gateway_hooks_test.cpp -o build/dd_hooks_tests

build/dd_mirror_tests: $(CORE) $(MI) tests/gateway_mirror_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(MI) tests/gateway_mirror_test.cpp -o build/dd_mirror_tests

build/dd_backend_kind_tests: $(BK) tests/agent_backend_kind_test.cpp
	$(CXX) $(CXXFLAGS) $(BK) tests/agent_backend_kind_test.cpp -o build/dd_backend_kind_tests

build/dd_errors_tests: tests/agent_errors_test.cpp
	$(CXX) $(CXXFLAGS) tests/agent_errors_test.cpp -o build/dd_errors_tests

build/dd_ssl_guard_tests: $(SG) tests/agent_ssl_guard_test.cpp
	$(CXX) $(CXXFLAGS) $(SG) tests/agent_ssl_guard_test.cpp -o build/dd_ssl_guard_tests

build/dd_retry_tests: $(RU) tests/agent_retry_utils_test.cpp
	$(CXX) $(CXXFLAGS) $(RU) tests/agent_retry_utils_test.cpp -o build/dd_retry_tests

build/dd_kv_watermarks_tests: $(KVW) src/core/json.cpp tests/agent_kv_watermarks_test.cpp
	$(CXX) $(CXXFLAGS) $(KVW) src/core/json.cpp tests/agent_kv_watermarks_test.cpp -o build/dd_kv_watermarks_tests

build/dd_message_sanitization_tests: $(MS) src/core/json.cpp tests/agent_message_sanitization_test.cpp
	$(CXX) $(CXXFLAGS) $(MS) src/core/json.cpp tests/agent_message_sanitization_test.cpp -o build/dd_message_sanitization_tests

build/dd_redact_tests: tests/agent_redact_test.cpp
	$(CXX) $(CXXFLAGS) tests/agent_redact_test.cpp -o build/dd_redact_tests

build/dd_usage_pricing_tests: tests/agent_usage_pricing_test.cpp
	$(CXX) $(CXXFLAGS) tests/agent_usage_pricing_test.cpp -o build/dd_usage_pricing_tests

build/dd_turn_context_tests: tests/agent_turn_context_test.cpp
	$(CXX) $(CXXFLAGS) tests/agent_turn_context_test.cpp -o build/dd_turn_context_tests

build/dd_context_references_tests: tests/agent_context_references_test.cpp
	$(CXX) $(CXXFLAGS) tests/agent_context_references_test.cpp -o build/dd_context_references_tests

build/dd_acp_tools_tests: src/core/json.cpp tests/acp_tools_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/acp_tools_test.cpp -o build/dd_acp_tools_tests

build/dd_root_heuristics_tests: tests/root_heuristics_test.cpp
	$(CXX) $(CXXFLAGS) tests/root_heuristics_test.cpp -o build/dd_root_heuristics_tests

build/dd_state_sql_tests: src/core/json.cpp tests/state_sql_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/state_sql_test.cpp -o build/dd_state_sql_tests

build/dd_cli_tests: tests/cli_heuristics_test.cpp
	$(CXX) $(CXXFLAGS) tests/cli_heuristics_test.cpp -o build/dd_cli_tests

build/dd_state_scrub_tests: src/core/json.cpp tests/state_scrub_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/state_scrub_test.cpp -o build/dd_state_scrub_tests

build/dd_run_helpers_tests: src/core/json.cpp tests/run_helpers_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/run_helpers_test.cpp -o build/dd_run_helpers_tests

build/dd_state_errors_tests: tests/state_errors_test.cpp
	$(CXX) $(CXXFLAGS) tests/state_errors_test.cpp -o build/dd_state_errors_tests

build/dd_constants_tests: src/core/json.cpp tests/constants_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/constants_test.cpp -o build/dd_constants_tests

build/dd_task_intensity_tests: src/core/json.cpp tests/task_intensity_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/task_intensity_test.cpp -o build/dd_task_intensity_tests

build/dd_json_tests: src/core/json.cpp include/dd/core/json.hpp tests/json_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/json_test.cpp -o build/dd_json_tests

build/dd_harness_config_tests: src/core/json.cpp tests/harness_config_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/harness_config_test.cpp -o build/dd_harness_config_tests

build/dd_model_metadata_tests: src/core/json.cpp tests/model_metadata_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/model_metadata_test.cpp -o build/dd_model_metadata_tests


build/dd_trajectory_compressor_tests: src/core/json.cpp tests/trajectory_compressor_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/trajectory_compressor_test.cpp -o build/dd_trajectory_compressor_tests

build/dd_cc_tests: src/core/json.cpp tests/context_compressor_test.cpp
	$(CXX) $(CXXFLAGS) src/core/json.cpp tests/context_compressor_test.cpp -o build/dd_cc_tests

test: build/dd_context_references_tests

test: build/dd_turn_context_tests

test: build/dd_usage_pricing_tests

test: build/dd_redact_tests

test: build/dd_message_sanitization_tests

test: build/dd_kv_watermarks_tests
test: build/dd_acp_tools_tests
test: build/dd_root_heuristics_tests
test: build/dd_state_sql_tests
test: build/dd_cli_tests
test: build/dd_state_scrub_tests
test: build/dd_run_helpers_tests
test: build/dd_state_errors_tests
test: build/dd_constants_tests
test: build/dd_task_intensity_tests
test: build/dd_harness_config_tests
test: build/dd_model_metadata_tests
test: build/dd_trajectory_compressor_tests
test: build/dd_cc_tests
test: build/dd_ui_tests
test: build/dd_chat_stream_tests
test: build/dd_context_tests
test: build/dd_maze_tests
test: build/dd_json_tests
test: build/dd_home_tests

test: build/dd_tests build/dd_tools_tests build/dd_agent_tests build/dd_config_tests build/dd_gateway_tests build/dd_agent_retry_tests build/dd_agent_misc_tests build/dd_agent_boundary_tests build/dd_agent_tape_tests build/dd_cron_tests build/dd_cron_jobs_tests build/dd_sticker_tests build/dd_pairing_tests build/dd_tools_security_tests build/dd_tools_interrupt_tests build/dd_debug_tests build/dd_budget_tests build/dd_tool_limits_tests build/dd_env_passthrough_tests build/dd_backend_helpers_tests build/dd_audio_container_tests build/dd_sha1_tests build/dd_sha256_tests build/dd_camofox_tests build/dd_terminal_hints_tests build/dd_threat_patterns_tests build/dd_schema_sanitizer_tests build/dd_url_safety_tests build/dd_osv_parse_tests build/dd_ansi_strip_tests build/dd_mcp_schema_cache_tests build/dd_delegation_schema_tests build/dd_patch_parser_tests build/dd_fuzzy_match_tests build/dd_todo_tests build/dd_tool_search_tests build/dd_website_policy_tests build/dd_gateway_authz_tests build/dd_channel_directory_tests build/dd_hooks_tests build/dd_mirror_tests build/dd_backend_kind_tests build/dd_errors_tests build/dd_ssl_guard_tests build/dd_retry_tests
	./build/dd_tests
	./build/dd_tools_tests
	./build/dd_agent_tests
	./build/dd_config_tests
	./build/dd_gateway_tests
	./build/dd_agent_retry_tests
	./build/dd_agent_misc_tests
	./build/dd_agent_boundary_tests
	./build/dd_agent_tape_tests
	./build/dd_cron_tests
	./build/dd_cron_jobs_tests
	./build/dd_sticker_tests
	./build/dd_pairing_tests
	./build/dd_tools_security_tests
	./build/dd_tools_interrupt_tests
	./build/dd_debug_tests
	./build/dd_budget_tests
	./build/dd_tool_limits_tests
	./build/dd_env_passthrough_tests
	./build/dd_backend_helpers_tests
	./build/dd_audio_container_tests
	./build/dd_sha1_tests
	./build/dd_sha256_tests
	./build/dd_camofox_tests
	./build/dd_terminal_hints_tests
	./build/dd_threat_patterns_tests
	./build/dd_schema_sanitizer_tests
	./build/dd_url_safety_tests
	./build/dd_osv_parse_tests
	./build/dd_ansi_strip_tests
	./build/dd_mcp_schema_cache_tests
	./build/dd_delegation_schema_tests
	./build/dd_patch_parser_tests
	./build/dd_fuzzy_match_tests
	./build/dd_todo_tests
	./build/dd_tool_search_tests
	./build/dd_website_policy_tests
	./build/dd_gateway_authz_tests
	./build/dd_channel_directory_tests
	./build/dd_hooks_tests
	./build/dd_mirror_tests
	./build/dd_backend_kind_tests
	./build/dd_errors_tests
	./build/dd_ssl_guard_tests
	./build/dd_retry_tests
	./build/dd_kv_watermarks_tests
		./build/dd_message_sanitization_tests
		./build/dd_redact_tests
		./build/dd_usage_pricing_tests
		./build/dd_turn_context_tests
		./build/dd_context_references_tests
		./build/dd_acp_tools_tests
		./build/dd_root_heuristics_tests
		./build/dd_state_sql_tests
		./build/dd_cli_tests
		./build/dd_state_scrub_tests
		./build/dd_run_helpers_tests
	./build/dd_state_errors_tests
	./build/dd_constants_tests
	./build/dd_task_intensity_tests
	./build/dd_harness_config_tests
	./build/dd_model_metadata_tests
	./build/dd_trajectory_compressor_tests
	./build/dd_cc_tests
	./build/dd_ui_tests
	./build/dd_chat_stream_tests
	./build/dd_context_tests
	./build/dd_maze_tests
	./build/dd_json_tests
	./build/dd_home_tests

clean:
	-rm -rf build

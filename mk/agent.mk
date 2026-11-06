$(BUILD)/dd_agent_run_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) include/dd/agent/agent.hpp include/dd/agent/iteration_budget.hpp tests/agent_run_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) tests/agent_run_test.cpp -pthread -o $(BUILD)/dd_agent_run_tests

TEST_BINS += $(BUILD)/dd_agent_run_tests

$(BUILD)/dd_delegate_tests: $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS) tests/delegate_tool_test.cpp
	$(CXX) $(CXXFLAGS) $(sort $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS)) tests/delegate_tool_test.cpp -pthread -lcurl -o $(BUILD)/dd_delegate_tests

TEST_BINS += $(BUILD)/dd_delegate_tests

$(BUILD)/dd_delegate_tsan: $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS) tests/delegate_tool_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=thread $(sort $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS)) tests/delegate_tool_test.cpp -pthread -lcurl -o $(BUILD)/dd_delegate_tsan

TSAN_BINS += $(BUILD)/dd_delegate_tsan

# The parallel-batch segv of 2026-11-06, pinned: a batch of three children on the
# transport path (the path production runs; the factory path never copied on_wait,
# which is why the batch tests above could not see it), each child's first request
# answered 429 so its HttpLLM waits. The parent's on_wait captures the TUI's scene
# by reference; the fix keeps children from ever reaching it. The tsan twin runs
# the same thing under -fsanitize=thread, where a child reaching the parent's
# unsynchronized recorder is a race report.
$(BUILD)/dd_delegate_batch_race_tests: $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS) tests/delegate_batch_race_test.cpp
	$(CXX) $(CXXFLAGS) $(sort $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS)) tests/delegate_batch_race_test.cpp -pthread -lcurl -o $(BUILD)/dd_delegate_batch_race_tests

TEST_BINS += $(BUILD)/dd_delegate_batch_race_tests

$(BUILD)/dd_delegate_batch_race_tsan: $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS) tests/delegate_batch_race_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=thread $(sort $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS)) tests/delegate_batch_race_test.cpp -pthread -lcurl -o $(BUILD)/dd_delegate_batch_race_tsan

TSAN_BINS += $(BUILD)/dd_delegate_batch_race_tsan

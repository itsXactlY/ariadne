$(BUILD)/dd_agent_run_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) include/dd/agent/agent.hpp include/dd/agent/iteration_budget.hpp tests/agent_run_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) tests/agent_run_test.cpp -pthread -o $(BUILD)/dd_agent_run_tests

TEST_BINS += $(BUILD)/dd_agent_run_tests

$(BUILD)/dd_delegate_tests: $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS) tests/delegate_tool_test.cpp
	$(CXX) $(CXXFLAGS) $(sort $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS)) tests/delegate_tool_test.cpp -pthread -lcurl -o $(BUILD)/dd_delegate_tests

TEST_BINS += $(BUILD)/dd_delegate_tests

$(BUILD)/dd_delegate_tsan: $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS) tests/delegate_tool_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=thread $(sort $(CORE) $(TOOLS) $(TOOLRUN) $(PATCH) $(FUZZY) $(APPLY) $(PROVENANCE) $(INTERRUPT) $(DELEGATE) $(AGENT) $(CONFIG) $(HTTPLLM) $(NET) $(THINTS)) tests/delegate_tool_test.cpp -pthread -lcurl -o $(BUILD)/dd_delegate_tsan

TSAN_BINS += $(BUILD)/dd_delegate_tsan

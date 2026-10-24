$(BUILD)/dd_agent_run_tests: $(CORE) $(SKILLS) $(TOOLS) $(AGENT) include/dd/agent/agent.hpp include/dd/agent/iteration_budget.hpp tests/agent_run_test.cpp
	$(CXX) $(CXXFLAGS) $(CORE) $(SKILLS) $(TOOLS) $(AGENT) tests/agent_run_test.cpp -pthread -o $(BUILD)/dd_agent_run_tests

TEST_BINS += $(BUILD)/dd_agent_run_tests

# MCP client, one-shot HTTP and the LLM transport, against a throwaway loopback server.
MCP_TEST := src/tools/mcp_client.cpp src/net/http.cpp src/agent/http_llm.cpp src/agent/retry.cpp src/agent/chat_stream.cpp src/core/json.cpp src/core/message.cpp src/tools/url_safety.cpp   # the LLM transport re-sends through retry.cpp
MCP_TEST_H := $(wildcard include/dd/core/*.hpp include/dd/tools/*.hpp include/dd/net/*.hpp include/dd/agent/*.hpp)

$(BUILD)/dd_mcp_tests: $(MCP_TEST) $(MCP_TEST_H) tests/tools_mcp_test.cpp
	$(CXX) $(CXXFLAGS) $(MCP_TEST) tests/tools_mcp_test.cpp -pthread -lcurl -o $(BUILD)/dd_mcp_tests

$(BUILD)/dd_mcp_asan: $(MCP_TEST) $(MCP_TEST_H) tests/tools_mcp_test.cpp
	$(CXX) $(CXXFLAGS) -g -fsanitize=address,undefined $(MCP_TEST) tests/tools_mcp_test.cpp -pthread -lcurl -o $(BUILD)/dd_mcp_asan

$(BUILD)/dd_mcp_tsan: $(MCP_TEST) $(MCP_TEST_H) tests/tools_mcp_test.cpp
	$(CXX) $(CXXFLAGS) -g -fsanitize=thread $(MCP_TEST) tests/tools_mcp_test.cpp -pthread -lcurl -o $(BUILD)/dd_mcp_tsan

TEST_BINS += $(BUILD)/dd_mcp_tests

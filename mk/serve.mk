# `ariadne serve`: the HTTP server and the OpenAI-compatible chat API.

$(BUILD)/dd_serve_tests: $(SERVE) src/core/json.cpp include/dd/net/server.hpp include/dd/serve/chat_api.hpp tests/serve_test.cpp
	$(CXX) $(CXXFLAGS) $(SERVE) src/core/json.cpp tests/serve_test.cpp -pthread -o $(BUILD)/dd_serve_tests

TEST_BINS += $(BUILD)/dd_serve_tests

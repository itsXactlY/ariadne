# `ariadne serve`: the HTTP server and the OpenAI-compatible chat API.

$(BUILD)/dd_serve_tests: $(SERVE) src/core/json.cpp include/dd/net/server.hpp include/dd/serve/chat_api.hpp tests/serve_test.cpp
	$(CXX) $(CXXFLAGS) $(SERVE) src/core/json.cpp tests/serve_test.cpp -pthread $(TLS_LIBS) -o $(BUILD)/dd_serve_tests

TEST_BINS += $(BUILD)/dd_serve_tests

# Who may run at once: ChatApi hands agent events to one stream pointer, so a second
# concurrent claim steals the first turn's output and runs a second thread on one Agent.
# The race is what TSan sees; the stolen stream is what the operator sees.
$(BUILD)/dd_serve_concurrency_tests: $(SERVE) src/core/json.cpp include/dd/net/server.hpp include/dd/serve/chat_api.hpp tests/serve_concurrency_test.cpp
	$(CXX) $(CXXFLAGS) $(SERVE) src/core/json.cpp tests/serve_concurrency_test.cpp -pthread $(TLS_LIBS) -o $(BUILD)/dd_serve_concurrency_tests

TEST_BINS += $(BUILD)/dd_serve_concurrency_tests

# The transport fact the server computes (`Request::tls`, `Request::peer`) has to decide the
# answer: a bearer token arriving in the clear from off-loopback is refused, not compared.
$(BUILD)/dd_serve_transport_tests: $(SERVE) src/core/json.cpp include/dd/net/server.hpp include/dd/serve/chat_api.hpp tests/serve_transport_test.cpp
	$(CXX) $(CXXFLAGS) $(SERVE) src/core/json.cpp tests/serve_transport_test.cpp -pthread $(TLS_LIBS) -o $(BUILD)/dd_serve_transport_tests

TEST_BINS += $(BUILD)/dd_serve_transport_tests

$(BUILD)/dd_serve_concurrency_tsan: $(SERVE) src/core/json.cpp include/dd/net/server.hpp include/dd/serve/chat_api.hpp tests/serve_concurrency_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=thread $(SERVE) src/core/json.cpp tests/serve_concurrency_test.cpp -pthread $(TLS_LIBS) -o $(BUILD)/dd_serve_concurrency_tsan

# serve() must exit on `stop` even while a connection is still answering a turn: an
# unbounded drain meant Ctrl+C never ended the process.
$(BUILD)/dd_net_shutdown_tests: src/net/server.cpp include/dd/net/server.hpp tests/net_shutdown_test.cpp
	$(CXX) $(CXXFLAGS) src/net/server.cpp tests/net_shutdown_test.cpp -pthread $(TLS_LIBS) -o $(BUILD)/dd_net_shutdown_tests

TEST_BINS += $(BUILD)/dd_net_shutdown_tests

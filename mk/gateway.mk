# The device gateway (G4 LAN + G5 relay): crypto, identity, devices, /command, TLS.

$(BUILD)/dd_device_gateway_tests: $(GWDEV) $(SERVE) $(NET) src/core/json.cpp include/dd/gateway/*.hpp include/dd/net/server.hpp tests/device_gateway_test.cpp
	$(CXX) $(CXXFLAGS) $(GWDEV) src/net/server.cpp $(NET) src/core/json.cpp src/core/config.cpp src/core/yaml.cpp src/core/path.cpp tests/device_gateway_test.cpp -pthread $(CURL_LIBS) $(TLS_LIBS) -o $(BUILD)/dd_device_gateway_tests

TEST_BINS += $(BUILD)/dd_device_gateway_tests

# The relay's two wait policies: a peer that stops mid-frame, and a session that ends while
# a worker is still answering. Neither is reachable through a live socket, so the decision
# itself is what runs -- and both used to wait forever.
$(BUILD)/dd_relay_policy_tests: $(GWDEV) $(SERVE) $(NET) include/dd/gateway/*.hpp include/dd/net/server.hpp tests/relay_policy_test.cpp
	# cli.cpp reads the config, so core/config+yaml+path must link; without them the
	# Config symbols are undefined and the test binary is never produced.
	$(CXX) $(CXXFLAGS) $(GWDEV) $(SERVE) $(NET) src/core/json.cpp src/core/config.cpp src/core/yaml.cpp src/core/path.cpp tests/relay_policy_test.cpp -pthread $(CURL_LIBS) $(TLS_LIBS) -o $(BUILD)/dd_relay_policy_tests

TEST_BINS += $(BUILD)/dd_relay_policy_tests

$(BUILD)/dd_relay_policy_tsan: $(GWDEV) $(SERVE) $(NET) include/dd/gateway/*.hpp include/dd/net/server.hpp tests/relay_policy_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=thread $(GWDEV) $(SERVE) $(NET) src/core/json.cpp src/core/config.cpp src/core/yaml.cpp src/core/path.cpp tests/relay_policy_test.cpp -pthread $(CURL_LIBS) $(TLS_LIBS) -o $(BUILD)/dd_relay_policy_tsan

# devices.json is read whole, edited, written whole: two writers used to overwrite each
# other's rows, and both shared one temp name.
$(BUILD)/dd_devices_race_tests: src/gateway/crypto.cpp src/gateway/devices.cpp src/core/json.cpp include/dd/gateway/devices.hpp include/dd/gateway/crypto.hpp tests/devices_race_test.cpp
	$(CXX) $(CXXFLAGS) src/gateway/crypto.cpp src/gateway/devices.cpp src/core/json.cpp tests/devices_race_test.cpp -pthread -lssl -lcrypto -o $(BUILD)/dd_devices_race_tests

TEST_BINS += $(BUILD)/dd_devices_race_tests

$(BUILD)/dd_devices_race_tsan: src/gateway/crypto.cpp src/gateway/devices.cpp src/core/json.cpp include/dd/gateway/devices.hpp include/dd/gateway/crypto.hpp tests/devices_race_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=thread src/gateway/crypto.cpp src/gateway/devices.cpp src/core/json.cpp tests/devices_race_test.cpp -pthread -lssl -lcrypto -o $(BUILD)/dd_devices_race_tsan

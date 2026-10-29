# The device gateway (G4 LAN + G5 relay): crypto, identity, devices, /command, TLS.

$(BUILD)/dd_device_gateway_tests: $(GWDEV) $(SERVE) $(NET) src/core/json.cpp include/dd/gateway/*.hpp include/dd/net/server.hpp tests/device_gateway_test.cpp
	$(CXX) $(CXXFLAGS) $(GWDEV) src/net/server.cpp $(NET) src/core/json.cpp src/core/config.cpp src/core/yaml.cpp src/core/path.cpp tests/device_gateway_test.cpp -pthread $(CURL_LIBS) $(TLS_LIBS) -o $(BUILD)/dd_device_gateway_tests

TEST_BINS += $(BUILD)/dd_device_gateway_tests

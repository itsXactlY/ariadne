# dd::learn as a whole: the service and its integration test
LEARN_ALL := $(wildcard src/learn/*.cpp) src/core/crc32c.cpp

$(BUILD)/dd_learn_service_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_ALL) $(wildcard include/dd/know/*.hpp include/dd/learn/*.hpp) tests/learn_service_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -fopenmp -pipe $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_ALL)) tests/learn_service_test.cpp -pthread -lcurl -o $(BUILD)/dd_learn_service_tests

learn-service-test: $(BUILD)/dd_learn_service_tests
	./$(BUILD)/dd_learn_service_tests

$(BUILD)/dd_learn_service_asan: $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_ALL) $(wildcard include/dd/know/*.hpp include/dd/learn/*.hpp) tests/learn_service_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -fopenmp -pipe -g -Wno-maybe-uninitialized -fsanitize=address,undefined $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_ALL)) tests/learn_service_test.cpp -pthread -lcurl -o $(BUILD)/dd_learn_service_asan

$(BUILD)/dd_learn_service_tsan: $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_ALL) $(wildcard include/dd/know/*.hpp include/dd/learn/*.hpp) tests/learn_service_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -pipe -g -Wno-maybe-uninitialized -Wno-unknown-pragmas -fsanitize=thread $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_ALL)) tests/learn_service_test.cpp -pthread -lcurl -o $(BUILD)/dd_learn_service_tsan

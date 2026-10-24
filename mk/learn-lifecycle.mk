# dd::learn lifecycle: probation, active, dormant, retired
LEARN_LIFE := $(wildcard src/learn/*.cpp) src/core/crc32c.cpp

$(BUILD)/dd_learn_lifecycle_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_LIFE) $(wildcard include/dd/know/*.hpp include/dd/learn/*.hpp) tests/learn_lifecycle_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -fopenmp -pipe $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_LIFE)) tests/learn_lifecycle_test.cpp -pthread -lcurl -o $(BUILD)/dd_learn_lifecycle_tests

learn-lifecycle-test: $(BUILD)/dd_learn_lifecycle_tests
	./$(BUILD)/dd_learn_lifecycle_tests

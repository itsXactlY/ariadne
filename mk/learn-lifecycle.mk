# dd::learn lifecycle: probation, active, dormant, retired
LEARN_LIFE := $(wildcard src/learn/*.cpp) src/core/crc32c.cpp

build/dd_learn_lifecycle_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_LIFE) $(wildcard include/dd/know/*.hpp include/dd/learn/*.hpp) tests/learn_lifecycle_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp -pipe $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_LIFE)) tests/learn_lifecycle_test.cpp -pthread -lcurl -o build/dd_learn_lifecycle_tests

learn-lifecycle-test: build/dd_learn_lifecycle_tests
	./build/dd_learn_lifecycle_tests

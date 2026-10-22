# dd::learn as a whole: the service and its integration test
LEARN_ALL := $(wildcard src/learn/*.cpp) src/core/crc32c.cpp

build/dd_learn_service_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_ALL) $(wildcard include/dd/know/*.hpp include/dd/learn/*.hpp) tests/learn_service_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp -pipe $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_ALL)) tests/learn_service_test.cpp -pthread -lcurl -o build/dd_learn_service_tests

learn-service-test: build/dd_learn_service_tests
	./build/dd_learn_service_tests

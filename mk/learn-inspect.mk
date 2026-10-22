# dd learn status|why|export|forget: the read-only views and forget
build/dd_learn_inspect_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN) $(wildcard include/dd/know/*.hpp include/dd/learn/*.hpp) tests/learn_inspect_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp -pipe $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN)) tests/learn_inspect_test.cpp -pthread -lcurl -o build/dd_learn_inspect_tests

learn-inspect-test: build/dd_learn_inspect_tests
	./build/dd_learn_inspect_tests

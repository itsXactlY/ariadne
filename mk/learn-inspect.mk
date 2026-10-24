# dd learn status|why|export|forget: the read-only views and forget
$(BUILD)/dd_learn_inspect_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN) $(wildcard include/dd/know/*.hpp include/dd/learn/*.hpp) tests/learn_inspect_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -fopenmp -pipe $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN)) tests/learn_inspect_test.cpp -pthread -lcurl -o $(BUILD)/dd_learn_inspect_tests

learn-inspect-test: $(BUILD)/dd_learn_inspect_tests
	./$(BUILD)/dd_learn_inspect_tests

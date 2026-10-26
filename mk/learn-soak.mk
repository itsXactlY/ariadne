# dd::learn soak (tsan builds without -fopenmp: ThreadSanitizer does not understand libgomp): a time-compressed half year with injected faults (the sanitizer runs are shorter)
LEARN_SOAK_SRC := $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(PROVENANCE) $(wildcard src/learn/*.cpp) src/core/crc32c.cpp)
LEARN_SOAK_H := $(wildcard include/dd/know/*.hpp include/dd/learn/*.hpp)

$(BUILD)/dd_learn_soak_tests: $(LEARN_SOAK_SRC) $(LEARN_SOAK_H) tests/learn_soak_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -fopenmp -pipe $(LEARN_SOAK_SRC) tests/learn_soak_test.cpp -pthread -lcurl -o $(BUILD)/dd_learn_soak_tests

$(BUILD)/dd_learn_soak_asan: $(LEARN_SOAK_SRC) $(LEARN_SOAK_H) tests/learn_soak_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -fopenmp -pipe -g -Wno-maybe-uninitialized -fsanitize=address,undefined $(LEARN_SOAK_SRC) tests/learn_soak_test.cpp -pthread -lcurl -o $(BUILD)/dd_learn_soak_asan

$(BUILD)/dd_learn_soak_tsan: $(LEARN_SOAK_SRC) $(LEARN_SOAK_H) tests/learn_soak_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -pipe -g -Wno-maybe-uninitialized -Wno-unknown-pragmas -fsanitize=thread $(LEARN_SOAK_SRC) tests/learn_soak_test.cpp -pthread -lcurl -o $(BUILD)/dd_learn_soak_tsan

learn-soak-test: $(BUILD)/dd_learn_soak_tests
	./$(BUILD)/dd_learn_soak_tests

learn-soak-asan: $(BUILD)/dd_learn_soak_asan
	DD_SOAK_DAYS=40 DD_SOAK_TURNS=120 DD_SOAK_CRASHES=8 ./$(BUILD)/dd_learn_soak_asan

learn-soak-tsan: $(BUILD)/dd_learn_soak_tsan
	DD_SOAK_DAYS=30 DD_SOAK_TURNS=80 DD_SOAK_CRASHES=6 ./$(BUILD)/dd_learn_soak_tsan

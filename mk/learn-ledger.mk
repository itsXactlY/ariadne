LEARN_LEDGER := src/know/types.cpp src/core/crc32c.cpp src/learn/codec.cpp src/learn/journal.cpp src/learn/compactor.cpp src/learn/ledger.cpp
LEARN_LEDGER_H := $(wildcard include/dd/learn/*.hpp) include/dd/core/crc32c.hpp

$(BUILD)/dd_learn_ledger_tests: $(LEARN_LEDGER) $(LEARN_LEDGER_H) tests/learn_ledger_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -pipe $(LEARN_LEDGER) tests/learn_ledger_test.cpp -pthread -o $(BUILD)/dd_learn_ledger_tests

$(BUILD)/dd_learn_ledger_asan: $(LEARN_LEDGER) $(LEARN_LEDGER_H) tests/learn_ledger_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=address,undefined $(LEARN_LEDGER) tests/learn_ledger_test.cpp -pthread -o $(BUILD)/dd_learn_ledger_asan

$(BUILD)/dd_learn_ledger_tsan: $(LEARN_LEDGER) $(LEARN_LEDGER_H) tests/learn_ledger_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=thread $(LEARN_LEDGER) tests/learn_ledger_test.cpp -pthread -o $(BUILD)/dd_learn_ledger_tsan

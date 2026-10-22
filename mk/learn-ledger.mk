LEARN_LEDGER := src/know/types.cpp src/core/crc32c.cpp src/learn/codec.cpp src/learn/journal.cpp src/learn/compactor.cpp src/learn/ledger.cpp
LEARN_LEDGER_H := $(wildcard include/dd/learn/*.hpp) include/dd/core/crc32c.hpp

build/dd_learn_ledger_tests: $(LEARN_LEDGER) $(LEARN_LEDGER_H) tests/learn_ledger_test.cpp
	$(CXX) $(CXXFLAGS) -pipe $(LEARN_LEDGER) tests/learn_ledger_test.cpp -pthread -o build/dd_learn_ledger_tests

build/dd_learn_ledger_asan: $(LEARN_LEDGER) $(LEARN_LEDGER_H) tests/learn_ledger_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=address,undefined $(LEARN_LEDGER) tests/learn_ledger_test.cpp -pthread -o build/dd_learn_ledger_asan

build/dd_learn_ledger_tsan: $(LEARN_LEDGER) $(LEARN_LEDGER_H) tests/learn_ledger_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -fsanitize=thread $(LEARN_LEDGER) tests/learn_ledger_test.cpp -pthread -o build/dd_learn_ledger_tsan

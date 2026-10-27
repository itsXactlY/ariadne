LEARN_CREDIT := src/learn/stats.cpp src/learn/record.cpp src/learn/credit.cpp src/learn/observer.cpp src/learn/sink.cpp src/know/types.cpp
LEARN_CREDIT_H := $(wildcard include/dd/learn/*.hpp) include/dd/know/event.hpp include/dd/core/slot_ring.hpp include/dd/core/park.hpp

$(BUILD)/dd_learn_credit_tests: $(LEARN_CREDIT) $(LEARN_CREDIT_H) tests/learn_credit_test.cpp
	$(CXX) $(CXXFLAGS) $(LEARN_CREDIT) tests/learn_credit_test.cpp -pthread -o $(BUILD)/dd_learn_credit_tests

$(BUILD)/dd_learn_credit_tsan: $(LEARN_CREDIT) $(LEARN_CREDIT_H) tests/learn_credit_test.cpp
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread $(LEARN_CREDIT) tests/learn_credit_test.cpp -pthread -o $(BUILD)/dd_learn_credit_tsan

$(BUILD)/dd_learn_credit_asan: $(LEARN_CREDIT) $(LEARN_CREDIT_H) tests/learn_credit_test.cpp
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined $(LEARN_CREDIT) tests/learn_credit_test.cpp -pthread -o $(BUILD)/dd_learn_credit_asan

learn-credit-test: $(BUILD)/dd_learn_credit_tests
	./$(BUILD)/dd_learn_credit_tests

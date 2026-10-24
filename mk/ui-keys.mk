$(BUILD)/dd_ui_keys_tests: $(CORE) $(ANSI) $(CONFIG) $(UI) include/dd/ui.hpp include/dd/ui_keys.hpp tests/ui_keys_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(CORE) $(ANSI) $(CONFIG) $(UI) tests/ui_keys_test.cpp -pthread -o $(BUILD)/dd_ui_keys_tests

TEST_BINS += $(BUILD)/dd_ui_keys_tests

# dd::know stopword languages: the table in force is swapped while readers run, so the sanitizer builds matter too
$(BUILD)/dd_know_stopwords_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(wildcard include/dd/know/*.hpp) tests/know_stopwords_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(CORE) $(KNOWDEP) $(KNOWPOD)) tests/know_stopwords_test.cpp -pthread -lcurl -o $(BUILD)/dd_know_stopwords_tests
TEST_BINS += $(BUILD)/dd_know_stopwords_tests

$(BUILD)/dd_know_stopwords_tsan: $(CORE) $(KNOWDEP) $(KNOWPOD) $(wildcard include/dd/know/*.hpp) tests/know_stopwords_test.cpp
	$(CXX) $(CXXFLAGS) -g -fsanitize=thread $(sort $(CORE) $(KNOWDEP) $(KNOWPOD)) tests/know_stopwords_test.cpp -pthread -lcurl -o $(BUILD)/dd_know_stopwords_tsan

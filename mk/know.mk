# dd::know stopword languages: the table in force is swapped while readers run, so the sanitizer builds matter too
$(BUILD)/dd_know_stopwords_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(wildcard include/dd/know/*.hpp) tests/know_stopwords_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(CORE) $(KNOWDEP) $(KNOWPOD)) tests/know_stopwords_test.cpp -pthread -lcurl -o $(BUILD)/dd_know_stopwords_tests
TEST_BINS += $(BUILD)/dd_know_stopwords_tests

$(BUILD)/dd_know_stopwords_tsan: $(CORE) $(KNOWDEP) $(KNOWPOD) $(wildcard include/dd/know/*.hpp) tests/know_stopwords_test.cpp
	# TSan builds drop -fopenmp (libgomp's own threads are not the race worth finding), so the
	# omp pragmas in bm25/memory_catalogue/skills are unknown: without this they are -Werror errors
	# and this binary is never produced -- the run below then reports a missing file, not a race.
	$(CXX) $(CXXFLAGS) -g -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-but-set-variable -fsanitize=thread $(sort $(CORE) $(KNOWDEP) $(KNOWPOD)) tests/know_stopwords_test.cpp -pthread -lcurl -o $(BUILD)/dd_know_stopwords_tsan

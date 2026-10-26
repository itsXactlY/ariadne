# dd::learn authoring: episode recorder, detectors, template author, reviewer, skill sink
LEARN_AUTHOR := src/learn/episode.cpp src/learn/detector.cpp src/learn/author.cpp src/learn/reviewer.cpp src/learn/skill_sink.cpp
LEARN_AUTHOR_DEPS := src/know/types.cpp src/know/text.cpp src/know/bm25.cpp src/know/scan.cpp src/know/skills.cpp src/know/catalogue.cpp $(SKILLS) $(SHA256) $(CORE) $(COREHOME)

$(BUILD)/dd_learn_author_tests: $(LEARN_AUTHOR) $(PROVENANCE) $(wildcard include/dd/learn/*.hpp include/dd/know/*.hpp include/dd/tools/*.hpp) tests/learn_author_test.cpp tests/learn_tmp.hpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(LEARN_AUTHOR) $(LEARN_AUTHOR_DEPS) $(PROVENANCE)) tests/learn_author_test.cpp -pthread -lcurl -o $(BUILD)/dd_learn_author_tests

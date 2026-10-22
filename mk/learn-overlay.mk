# dd::learn read side: Affinity, Curator, LearnOverlay (stats.cpp and record.cpp are the ledger's)
LEARN_OVERLAY := src/learn/affinity.cpp src/learn/curator.cpp src/learn/overlay.cpp src/learn/stats.cpp src/learn/record.cpp

build/dd_learn_overlay_tests: $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_OVERLAY) $(wildcard include/dd/know/*.hpp include/dd/learn/*.hpp) tests/learn_overlay_test.cpp
	$(CXX) $(CXXFLAGS) -fopenmp $(sort $(CORE) $(KNOWDEP) $(KNOWPOD) $(LEARN_OVERLAY)) tests/learn_overlay_test.cpp -pthread -lcurl -o build/dd_learn_overlay_tests

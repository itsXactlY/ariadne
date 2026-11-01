# mk/limbo.mk — the limbo level's own suites. The level is a process spawner, a pipe pump, a
# router thread and an admission latch, so its tests are the only place the racing threads of
# that design get run against each other.
#
# The crossing is tested against tests/fake_engine.sh, a dweller that speaks the strata protocol
# line for line. A real engine would make the assertions about a load that takes minutes; the
# fake makes them about the protocol, which is what Charon is.
$(BUILD)/dd_limbo_charon_tests: src/limbo/charon.cpp src/limbo/spawn.cpp src/limbo/shade.cpp src/core/json.cpp \
                                include/dd/limbo/charon.hpp include/dd/limbo/shade.hpp tests/fake_engine.sh \
                                tests/limbo_charon_test.cpp
	$(CXX) $(CXXFLAGS) src/limbo/charon.cpp src/limbo/spawn.cpp src/limbo/shade.cpp src/core/json.cpp tests/limbo_charon_test.cpp \
		-pthread -o $(BUILD)/dd_limbo_charon_tests

$(BUILD)/dd_limbo_minos_tests: src/limbo/minos.cpp src/limbo/shade.cpp src/limbo/spawn.cpp src/core/json.cpp \
                               include/dd/limbo/minos.hpp include/dd/limbo/shade.hpp include/dd/limbo/spawn.hpp \
                               tests/fake_shade.hpp tests/limbo_minos_test.cpp
	$(CXX) $(CXXFLAGS) src/limbo/minos.cpp src/limbo/shade.cpp src/limbo/spawn.cpp src/core/json.cpp \
		tests/limbo_minos_test.cpp -pthread -o $(BUILD)/dd_limbo_minos_tests

$(BUILD)/dd_limbo_level_tests: $(LIMBO) include/dd/limbo/level.hpp tests/fake_shade.hpp tests/limbo_level_test.cpp
	$(CXX) $(CXXFLAGS) $(sort $(LIMBO)) tests/limbo_level_test.cpp -pthread $(CURL_LIBS) $(TLS_LIBS) -o $(BUILD)/dd_limbo_level_tests

# The pump thread, the router thread, the admission CAS and the respawn latch all touch the same
# slots; TSan is the only thing that can say they do it honestly. halt_on_error=1 makes a race a
# failed target, not a line that scrolls past.
$(BUILD)/dd_limbo_race_tsan: $(LIMBO) include/dd/limbo/level.hpp tests/fake_shade.hpp tests/fake_engine.sh \
                             tests/limbo_race_test.cpp
	$(CXX) $(CXXFLAGS) -pipe -g -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-but-set-variable \
		-fsanitize=thread $(sort $(LIMBO)) tests/limbo_race_test.cpp -pthread $(CURL_LIBS) $(TLS_LIBS) -o $(BUILD)/dd_limbo_race_tsan

TEST_BINS += $(BUILD)/dd_limbo_charon_tests $(BUILD)/dd_limbo_minos_tests $(BUILD)/dd_limbo_level_tests
TSAN_BINS += $(BUILD)/dd_limbo_race_tsan

# mk/vision.mk — native vision: view_image hands an image file to the wire as an
# image content part. The handler sniffs the container (it never decodes pixels),
# so the test can carry the contract with a headed body; the lift and the wire
# shape are exercised against the real message layer.
$(BUILD)/dd_vision_tests: $(CORE) $(TOOLS) $(VISION) tests/tool_vision_image_parts_test.cpp
	$(CXX) $(CXXFLAGS) $(sort $(CORE) $(TOOLS) $(VISION)) tests/tool_vision_image_parts_test.cpp -pthread -o $(BUILD)/dd_vision_tests

TEST_BINS += $(BUILD)/dd_vision_tests

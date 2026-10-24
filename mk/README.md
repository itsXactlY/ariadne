mk/*.mk: one fragment per part of the tree (learn-*.mk for dd::learn, tools.mk, mcp.mk, ...). The Makefile includes them all.

A fragment defines its build rules with `$(BUILD)/...` outputs (never a literal build/) and appends each test binary to the list `make test` runs:

    TEST_BINS += $(BUILD)/dd_x_tests

`make test` runs the whole list one binary after another (so tests that share a scratch folder never overlap), keeps going after a failure and exits non-zero at the end. It exports DD_TEST_TMP=$(BUILD)/tmp for tests that need scratch space: use it, never /tmp.

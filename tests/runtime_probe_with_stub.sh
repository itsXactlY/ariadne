#!/bin/sh
# tests/runtime_probe.sh with a stub backend: starts the stub, runs the probe against
# whichever binary BIN names, then stops the stub by pid. Kill is by pid, never by pattern:
# `pkill -f` matches the harness's own `sh -c` wrapper, whose cmdline contains the pattern,
# and takes the shell down with it (exit 137).
set -u
BIN=${BIN:-build/ariadne}
PROBE_HOME=${PROBE_HOME:-/tmp/ariadne-runtime-home}
PORT=${PROBE_STUB_PORT:-18759}
python3 tests/stub_llm.py $PORT >/tmp/probe_stub.log 2>&1 &
stub=$!
sleep 1
BIN=$BIN BASE_URL=http://127.0.0.1:$PORT/v1 PROBE_HOME=$PROBE_HOME sh tests/runtime_probe.sh
rc=$?
kill -9 $stub 2>/dev/null
wait $stub 2>/dev/null
exit $rc

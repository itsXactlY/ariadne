#!/bin/sh
# The release binary, walked the way an operator walks it — the paths no test binary reaches.
#
# Module tests cover functions. These product paths have no function to call: which home a
# start picks, what a start with no config does (and must NOT do: create one), what --init
# writes, whether a bind lets go, and whether a bind answers the right transport. This probe
# runs `build/ariadne` through those and fails on the behaviour, not the exit code alone.
#
#   sh tests/runtime_probe.sh                    # no turns: startup, home, binds, transport
#   BASE_URL=http://127.0.0.1:18784 sh tests/runtime_probe.sh   # with a stub backend, turns too
#
# Cleanup is by recorded pid. `pkill -f` is never used here: the harness wraps every command
# in `sh -c {…}` whose cmdline contains the pattern, so pkill -f kills the probe's own shell.
set -u

BIN=${BIN:-build/ariadne}
HOME_DIR=${PROBE_HOME:-/tmp/ariadne-runtime-home}
# Probe output lives OUTSIDE the home. A log written into the home is indistinguishable from
# state the product created there, which is exactly what "a refused start created nothing"
# has to be able to see.
LOG_DIR=/tmp/ariadne-runtime-probe
BASE_URL=${BASE_URL:-}
PORT=${PROBE_PORT:-18998}
PUB_PORT=${PROBE_PUB_PORT:-18997}
# The probe never leans on whatever the operator happens to be running. The pod an ariadne
# recalls from is named in the home's config.yaml (mcp_servers.mazemaker.url), not in the
# environment: DD_MAZE_URL is read by nothing in the binary, so exporting it neutralizes
# nothing and a probe turn spent its whole knowledge.deep_ms budget recalling from the live
# pod — slow, and measuring the operator's system while claiming to measure the stub's. The
# probe rewrites the home it made instead; see the init gate below.
STUB_PORT=${PROBE_STUB_PORT:-18784}
GW_PORT=${PROBE_GW_PORT:-18887}
fails=0
pids=""

note() { printf '  %s\n' "$1"; }
fail() { printf 'FAILED: %s\n' "$1"; fails=$((fails + 1)); }

stop_all() {
  for p in $pids; do kill -TERM "$p" 2>/dev/null; done
  for p in $pids; do
    i=0
    while kill -0 "$p" 2>/dev/null; do
      i=$((i + 1))
      if [ $i -gt 20 ]; then kill -9 "$p" 2>/dev/null; break; fi
      sleep 0.25
    done
    wait "$p" 2>/dev/null
  done
  pids=""
}
trap stop_all EXIT INT TERM

# Expects a specific exit code, and a specific line in the output. A start that exits 2 with
# the wrong sentence is as useless to an operator as one that exits 0.
expect() { # expect <label> <want_rc> <needle> <args...>
  label=$1; want=$2; needle=$3; shift 3
  log=$LOG_DIR/$label.log
  # DD_BASE_URL is exported only when it carries something. Exporting it empty used to be
  # harmless-looking and was not: presence alone made the start treat an empty value as an
  # endpoint, so an unconfigured home ran the TUI on defaults instead of refusing. The probe
  # bounds every start as well — a start that hangs is a failure, not a slow pass.
  if [ -n "$BASE_URL" ]; then
    timeout 60 env DD_HOME=$HOME_DIR DD_BASE_URL=$BASE_URL "$BIN" "$@" > "$log" 2>&1
  else
    timeout 60 env DD_HOME=$HOME_DIR "$BIN" "$@" > "$log" 2>&1
  fi
  rc=$?
  if [ $rc -ne "$want" ]; then fail "$label exited $rc, wanted $want"; head -6 "$log" | sed 's/^/    /'
  elif ! grep -q -- "$needle" "$log"; then
    fail "$label did not say \"$needle\""; head -6 "$log" | sed 's/^/    /'
  else note "$label ok"
  fi
}

ready() { # ready <url> <pid>
  i=0
  while [ $i -lt 120 ]; do
    curl -sk -m 2 "$1" >/dev/null 2>&1 && return 0
    kill -0 "$2" 2>/dev/null || return 1
    i=$((i + 1)); sleep 0.5
  done
  return 1
}

term_exit() { # term_exit <pid> -> ms until it is gone; 1 = never
  t0=$(date +%s%N)
  kill -TERM "$1" 2>/dev/null
  i=0
  while kill -0 "$1" 2>/dev/null; do
    i=$((i + 1))
    if [ $i -gt 20 ]; then return 1
    fi
    sleep 0.25
  done
  echo $(( ($(date +%s%N) - t0) / 1000000 ))
}

# -k is required here, not forgivable: the gateway serves TLS with a self-signed cert (a real
# device pins the fingerprint instead of a CA), so a strict curl gets 000 and reads an open
# refusal as a closed one. --serve is plain HTTP, where -k changes nothing.
code() { curl -sk -o /dev/null -w '%{http_code}' -m 8 "$@"; }

# ---------------------------------------------------------------- home and config
rm -rf "$HOME_DIR"; mkdir -p "$HOME_DIR"
rm -rf "$LOG_DIR"; mkdir -p "$LOG_DIR"

# A home with no config must refuse, explain, and create nothing. Creating one silently is
# how a copied binary ends up running an install nobody configured.
expect no-config 2 "no config at" --home "$HOME_DIR" -- --say hello
if find "$HOME_DIR" -mindepth 1 | grep -q .; then
  fail "a refused start created files in $HOME_DIR"; find "$HOME_DIR" -mindepth 1 | head -5 | sed 's/^/    /'
else
  note "nothing created by a refused start ok"
fi

# An exported empty DD_BASE_URL is a var set to nothing, not an endpoint. Presence alone used to
# read it as one, so this start ran the TUI on defaults from a home ariadne never configured and
# built state in it — the exact hazard the refusal exists to stop.
log=$LOG_DIR/empty-base.log
timeout 60 env DD_HOME=$HOME_DIR DD_BASE_URL= "$BIN" --home "$HOME_DIR" -- --say hello > "$log" 2>&1
if [ $? -ne 2 ]; then fail "DD_BASE_URL= ran instead of refusing (exit $? of an unconfigured home)"; head -6 "$log" | sed 's/^/    /'
elif ! grep -q 'no config at' "$log"; then fail "DD_BASE_URL= did not say no config at"; head -6 "$log" | sed 's/^/    /'
else note "empty DD_BASE_URL still refuses ok"
fi
if find "$HOME_DIR" -mindepth 1 | grep -q .; then
  fail "the DD_BASE_URL= refusal built state in $HOME_DIR"; find "$HOME_DIR" -mindepth 1 | head -5 | sed 's/^/    /'
else note "refusal with DD_BASE_URL= created nothing ok"
fi

# --init is the only path that may make a home.
expect init 0 "config.yaml" --home "$HOME_DIR" --init
if [ -f "$HOME_DIR/config.yaml" ]; then note "config.yaml written ok"
else fail "--init wrote no config.yaml"
fi

# Point the home's maze at a dead port. Left as shipped it names the live pod, so a probe turn
# waits knowledge.deep_ms for a recall that has nothing to do with the stub, and its remembers
# land in the operator's maze. A dead port also walks the degraded path — the tape must still
# work when the pod is not answering — and it makes the timing gates below measure the wire.
if [ -f "$HOME_DIR/config.yaml" ]; then
  sed -i 's#url: http://127.0.0.1:8765/mcp#url: http://127.0.0.1:9/mcp#' "$HOME_DIR/config.yaml"
  if grep -q 'url: http://127.0.0.1:9/mcp' "$HOME_DIR/config.yaml"
  then note "the home's maze points at a dead port: the probe measures the stub, not the pod"
  else fail "the probe's home still points at a live pod, so its turns lean on the operator's maze"
  fi
fi

# A named config outside the home is honoured without the agent running (--devices reads the
# device store, so it needs no endpoint), and the home it names is not rewritten by it.
mkdir -p /tmp/ariadne-runtime-other
printf 'agent:\n  model: stub-model\n' > /tmp/ariadne-runtime-other/config.yaml
expect config-flag 0 "" --config /tmp/ariadne-runtime-other/config.yaml --home "$HOME_DIR" --devices

# ---------------------------------------------------------------- serve, on loopback
if [ -n "$BASE_URL" ]; then
  python3 tests/stub_llm.py "$STUB_PORT" > "$LOG_DIR/stub.log" 2>&1 &
  stub_pid=$!; pids="$pids $stub_pid"
  i=0
  while [ $i -lt 40 ]; do
    curl -s -m 2 "http://127.0.0.1:$STUB_PORT/v1/models" >/dev/null 2>&1 && break
    i=$((i + 1)); sleep 0.25
  done
  if [ $i -ge 40 ]; then fail "the stub backend never answered"; stop_all; exit 1
  fi
  BASE_URL="http://127.0.0.1:$STUB_PORT"
  note "stub backend on :$STUB_PORT"

  DD_HOME=$HOME_DIR DD_BASE_URL=$BASE_URL "$BIN" --home "$HOME_DIR" --serve 127.0.0.1:$PORT > "$LOG_DIR/serve.log" 2>&1 &
  pid=$!; pids="$pids $pid"
  if ! ready "http://127.0.0.1:$PORT/health" "$pid"; then
    fail "serve never answered /health"; kill -9 "$pid" 2>/dev/null; stop_all; exit 1
  fi
  # /health answers with no token: it is the probe, not the surface.
  if [ "$(code http://127.0.0.1:$PORT/health)" = 200 ]; then note "loopback serve open on /health ok"
  else fail "/health did not answer without a token"
  fi
  # A loopback listener with no key is an open loop by design; a turn must actually produce
  # content, since a 200 with an empty choices array is a broken turn a status-only probe misses.
  curl -s -m 30 "http://127.0.0.1:$PORT/v1/chat/completions" -X POST -H 'Content-Type: application/json' \
    -d '{"model":"probe","messages":[{"role":"user","content":"hello"}]}' > "$LOG_DIR/serve.turn" 2>&1
  if grep -q '"content"' "$LOG_DIR/serve.turn"; then note "loopback turn produced content ok"
  else fail "loopback turn returned no content"; head -3 "$LOG_DIR/serve.turn" | sed 's/^/    /'
  fi
  curl -s -N -m 30 "http://127.0.0.1:$PORT/v1/chat/completions" -X POST -H 'Content-Type: application/json' \
    -d '{"model":"probe","stream":true,"messages":[{"role":"user","content":"hello"}]}' > "$LOG_DIR/serve.sse" 2>&1
  if grep -q 'data:' "$LOG_DIR/serve.sse"; then note "loopback SSE streamed chunks ok"
  else fail "loopback SSE produced no data: lines"; head -3 "$LOG_DIR/serve.sse" | sed 's/^/    /'
  fi
  # The stub speaks HTTP/1.1 and holds the socket after `data: [DONE]`, exactly like llama-server
  # waiting for the next request. So a turn that ends at its terminator comes back in milliseconds,
  # and one that ends at the stall clock comes back in seconds — status alone cannot tell them
  # apart, and a busy_ held by the stalled socket answers the next request with 409. Both halves
  # are gated: wall time for the stream, status for the turn after it.
  t0=$(date +%s%N)
  curl -s -N -m 30 "http://127.0.0.1:$PORT/v1/chat/completions" -X POST -H 'Content-Type: application/json' \
    -d '{"model":"probe","stream":true,"messages":[{"role":"user","content":"hello again"}]}' > "$LOG_DIR/serve.sse2" 2>&1
  sse_ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  if [ "$sse_ms" -lt 500 ] && grep -q '\[DONE\]' "$LOG_DIR/serve.sse2"; then
    note "streamed turn ended at its terminator in ${sse_ms}ms ok"
  elif grep -q '\[DONE\]' "$LOG_DIR/serve.sse2"; then
    fail "streamed turn took ${sse_ms}ms after [DONE]: the reader held the socket past the terminator"
  else fail "second streamed turn never reached [DONE]"
  fi
  second=$(curl -s -m 30 -o "$LOG_DIR/serve.turn2" -w '%{http_code}' \
    "http://127.0.0.1:$PORT/v1/chat/completions" -X POST -H 'Content-Type: application/json' \
    -d '{"model":"probe","messages":[{"role":"user","content":"second turn"}]}')
  if [ "$second" = 200 ] && grep -q '"content"' "$LOG_DIR/serve.turn2"; then note "turn after a streamed one accepted ok"
  elif [ "$second" = 409 ]; then fail "turn after a streamed one refused 409: busy_ never released"
  else fail "turn after a streamed one returned $second"
  fi
  # The CLI turn is the path an empty DD_BASE_URL used to hijack: with an endpoint it must run
  # and produce content, with none it must refuse (checked above, in the home section).
  # The endpoint has to be named here too: with only DD_HOME set the CLI reads the home's
  # config.yaml, whose base_url is the operator's live server, and the phase then measures
  # that system while claiming to measure the stub.
  DD_BASE_URL=$BASE_URL timeout 60 "$BIN" --home "$HOME_DIR" -- --say hello > "$LOG_DIR/turn.log" 2>&1
  if [ $? -eq 0 ] && grep -q 'stub answer' "$LOG_DIR/turn.log"; then note "cli turn produced content ok"
  else fail "cli turn did not produce a stub answer"; head -6 "$LOG_DIR/turn.log" | sed 's/^/    /'
  fi
  expect devices 0 "" --home "$HOME_DIR" --devices
  if ms=$(term_exit "$pid"); then note "loopback serve let go in ${ms}ms ok"
  else fail "serve ignored SIGTERM for >5s"; kill -9 "$pid" 2>/dev/null
  fi
  pids=""
fi

# ---------------------------------------------------------------- serve, past loopback
# A bind past loopback runs a shell for whoever connects, so it needs a key; and because the
# listener is plain HTTP, a request arriving from off-loopback is refused rather than answered
# with a bearer token in the clear. Loopback still answers — that is how a TLS-terminating
# proxy reaches it.
# --serve takes no message: a start that would run a turn is refused before the listener opens,
# so the key refusal is what this checks.
expect public-no-key 2 "serve.api_key or DD_SERVE_KEY" --home "$HOME_DIR" --serve 0.0.0.0:$PUB_PORT

if [ -n "$BASE_URL" ]; then
  DD_HOME=$HOME_DIR DD_BASE_URL=$BASE_URL DD_SERVE_KEY=probe-key "$BIN" --home "$HOME_DIR" \
    --serve 0.0.0.0:$PUB_PORT > "$LOG_DIR/public.log" 2>&1 &
  pid=$!; pids="$pids $pid"
  if ! ready "http://127.0.0.1:$PUB_PORT/health" "$pid"; then
    fail "public serve never answered /health"; kill -9 "$pid" 2>/dev/null; stop_all; exit 1
  fi
  # The startup must say the listener is plain HTTP and name the way out, before anyone connects.
  if grep -q 'plain HTTP' "$LOG_DIR/public.log" && grep -q '127.0.0.1' "$LOG_DIR/public.log"; then
    note "public bind names its transport ok"
  else fail "public bind did not say the listener is plain HTTP"; head -4 "$LOG_DIR/public.log" | sed 's/^/    /'
  fi
  if [ "$(code -H 'Authorization: Bearer probe-key' http://127.0.0.1:$PUB_PORT/v1/models)" = 200 ]; then
    note "loopback request to a keyed public bind answered ok"
  else fail "a keyed public bind refused its own loopback"
  fi
  if [ "$(code http://127.0.0.1:$PUB_PORT/v1/models)" = 401 ]; then note "no token on a keyed bind is 401 ok"
  else fail "a keyed public bind answered without a token"
  fi
  if ms=$(term_exit "$pid"); then note "public serve let go in ${ms}ms ok"
  else fail "public serve ignored SIGTERM for >5s"; kill -9 "$pid" 2>/dev/null
  fi
  pids=""
fi

# ---------------------------------------------------------------- gateway, over TLS
DD_HOME=$HOME_DIR DD_BASE_URL=$BASE_URL "$BIN" --home "$HOME_DIR" --gateway 127.0.0.1:$GW_PORT > "$LOG_DIR/gw.log" 2>&1 &
pid=$!; pids="$pids $pid"
if ! ready "https://127.0.0.1:$GW_PORT/pair.json" "$pid"; then
  fail "gateway never answered /pair.json over TLS"; kill -9 "$pid" 2>/dev/null; stop_all; exit 1
fi
if grep -q 'https://' "$LOG_DIR/gw.log"; then note "gateway names itself as https ok"
else fail "gateway banner does not name https"; head -4 "$LOG_DIR/gw.log" | sed 's/^/    /'
fi
# The gateway's surface is exactly three shapes: /pair.json open (it is what a device reads to
# pair), /health open (it is the probe), and everything that reaches the agent closed — it
# needs a paired device's token, and the refusal must happen before the agent is ever touched.
if [ "$(code https://127.0.0.1:$GW_PORT/health)" = 200 ]; then note "gateway /health answers without a token ok"
else fail "gateway /health did not answer without a token"
fi
if [ "$(code https://127.0.0.1:$GW_PORT/pair.json)" = 200 ]; then note "gateway /pair.json open for pairing ok"
else fail "gateway /pair.json did not answer"
fi
if [ "$(code https://127.0.0.1:$GW_PORT/v1/models)" = 401 ]; then note "gateway refuses an unpaired device ok"
else fail "gateway answered a request with no device token"
fi
if [ "$(code https://127.0.0.1:$GW_PORT/command -X POST -H 'Content-Type: application/json' -d '{"cmd":"models","args":"{}"}')" = 401 ]; then
  note "gateway refuses an unpaired /command ok"
else fail "gateway answered /command with no device token"
fi
# Identity before shape. A body with no cmd at all used to be answered as a malformed request
# (400 "No 'cmd' field"), which told anyone sitting on the port what the protocol looks like
# before they had proven who they are. The refusal must be about the caller, not the body.
if [ "$(code https://127.0.0.1:$GW_PORT/command -X POST -H 'Content-Type: application/json' -d '{"tool":"read_file","args":{}}')" = 401 ]; then
  note "a tokenless /command with no cmd is refused as unauthorized ok"
else fail "a tokenless /command answered a shape question -- the protocol reads from outside the trust boundary"
fi
# The identity-first rule must not close what is deliberately open: /command status is the
# hermes-crypto liveness call and answers anyone.
if [ "$(code https://127.0.0.1:$GW_PORT/command -X POST -H 'Content-Type: application/json' -d '{"cmd":"status"}')" = 200 ]; then
  note "tokenless /command status stays open ok"
else fail "the identity-first gate closed the open status command"
fi
if ms=$(term_exit "$pid"); then note "gateway let go in ${ms}ms ok"
else fail "gateway ignored SIGTERM for >5s"; kill -9 "$pid" 2>/dev/null
fi
pids=""

printf '%s\n' "runtime probe: $fails failed"
[ $fails -eq 0 ] || exit 1

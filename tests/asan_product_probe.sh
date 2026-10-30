#!/bin/sh
# The shipped binary under ASan+UBSan, walked the way an operator walks it.
#
# Module tests never run ariadne itself: startup, home resolution, --init, argument
# parsing, the serve bind and the gateway bind are product paths no test binary
# reaches. This runs the sanitizer-built binary through those paths and fails on a
# sanitizer report, not only on a non-zero exit -- ASan prints its report and the
# process can still exit 0 if the bad free happens after the turn.
#
#   make asan-product ASAN_BASE_URL=http://127.0.0.1:18777
# Without ASAN_BASE_URL the LLM-dependent probes are skipped, since a turn needs a
# backend and the script must not invent one.
set -u

BIN=${BIN:-build/ariadne_asan}
HOME_DIR=${ASAN_HOME:-/tmp/ariadne-asan-home}
BASE_URL=${ASAN_BASE_URL:-}
PORT=${ASAN_PORT:-18999}
GW_PORT=${ASAN_GW_PORT:-18888}
STUB_PORT=${ASAN_STUB_PORT:-18777}
fails=0

note() { printf '  %s\n' "$1"; }
fail() { printf 'FAILED: %s\n' "$1"; fails=$((fails + 1)); }

# A response body's shape, not just its status: a 200 with an empty choices array is a
# broken turn that every status-only probe waves through.
has() { # has <file> <needle> <what>
  if grep -q -- "$2" "$1" 2>/dev/null; then note "$3 ok"
  else fail "$3: expected \"$2\" in the response"; head -3 "$1" | sed 's/^/    /'
  fi
}

# A sanitizer report is the failure, whatever the exit status says.
no_report() {
  if grep -qE 'AddressSanitizer|LeakSanitizer|runtime error|UBSAN' "$1" 2>/dev/null; then
    fail "$(basename "$1"): sanitizer report"
    grep -m3 -E 'AddressSanitizer|LeakSanitizer|runtime error|UBSAN' "$1" | sed 's/^/    /'
  fi
}

run() { # run <label> <logfile> <args...>
  label=$1; log=$2; shift 2
  DD_HOME=$HOME_DIR DD_BASE_URL=$BASE_URL "$BIN" "$@" > "$log" 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then fail "$label exited $rc"; head -6 "$log" | sed 's/^/    /'
  else no_report "$log"; note "$label ok"
  fi
}

# A server probe: start, talk to it, ask it to stop, and check it stopped.
# Latency is the point -- a bind that will not let go while a turn is in flight is the
# bug this whole file exists to catch, so a turn is put in flight BEFORE the signal.
#
# Readiness and shutdown get two different ceilings, on purpose. A sanitizer-built
# binary under memory pressure takes tens of seconds to scan the catalogue and bind;
# failing on that makes the probe a flake, so readiness waits up to 60s and reports a
# dead process as dead. What is a real bug is a bind that ignores SIGTERM, so that
# ceiling stays at 5s -- measured in milliseconds, because a whole-second clock can
# hide a hang of up to 5s entirely.
ready() { # ready <url> <pid>
  i=0
  while [ $i -lt 120 ]; do
    curl -sk -m 2 "$1" >/dev/null 2>&1 && return 0
    kill -0 "$2" 2>/dev/null || return 1
    i=$((i + 1)); sleep 0.5
  done
  return 1
}

# What a shutdown bug is here: the BIND keeps accepting after SIGTERM. Leaving the process is
# a different thing -- LeakSanitizer scans the sanitizer's shadow memory at exit, and under
# host memory pressure that scan takes seconds -- so the two are measured apart, in
# milliseconds, and only the bind has the tight ceiling. Measuring the process as the bind
# reported a wedged accept loop that was really a slow sanitizer teardown.
term_shutdown() { # term_shutdown <pid> <url> -> "bind_ms process_ms"; rc 1 bind never let go, 2 process never left
  t0=$(date +%s%N)
  kill -TERM "$1" 2>/dev/null
  i=0
  while [ $i -lt 20 ]; do
    curl -sk -m 1 "$2" >/dev/null 2>&1 || break
    i=$((i + 1)); sleep 0.25
  done
  if [ $i -ge 20 ]; then return 1; fi
  bind_ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  j=0
  while kill -0 "$1" 2>/dev/null; do
    j=$((j + 1))
    if [ $j -gt 80 ]; then echo "$bind_ms never"; return 2
    fi
    sleep 0.25
  done
  echo "$bind_ms $(( ($(date +%s%N) - t0) / 1000000 ))"
  return 0
}

server_probe() { # server_probe <label> <flag> <port> <path> [turn-url]
  label=$1; flag=$2; port=$3; path=$4; turn=${5:-}
  # The gateway speaks TLS with a self-signed cert, so -k over https; --serve is plain http.
  scheme=http
  if [ "$flag" = gateway ]; then scheme=https; fi
  log=$HOME_DIR/$label.log
  # `--serve` / `--gateway`, not the bare word: as a positional argument the text becomes the
  # prompt, the binary answers a headless turn and exits, and the probe reports on a bind it
  # never started.
  DD_HOME=$HOME_DIR DD_BASE_URL=$BASE_URL "$BIN" --home "$HOME_DIR" "--$flag" 127.0.0.1:"$port" > "$log" 2>&1 &
  pid=$!
  url="$scheme://127.0.0.1:$port$path"
  if ! ready "$url" "$pid"; then
    if kill -0 "$pid" 2>/dev/null; then fail "$label never answered $path -- still alive after the 60s ceiling"
    else fail "$label died before answering $path"
    fi
    kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null; no_report "$log"; return
  fi
  turn_pid=""
  if [ -n "$turn" ]; then
    curl -sk -m 30 "$turn" -X POST -H 'Content-Type: application/json' \
      -d '{"model":"probe","stream":true,"messages":[{"role":"user","content":"slow"}]}' > "$HOME_DIR/$label.stream" 2>&1 &
    turn_pid=$!
    sleep 0.4
  fi
  out=$(term_shutdown $pid "$url"); rc=$?
  if [ $rc -eq 1 ]; then
    fail "$label kept the bind accepting after SIGTERM (>5s)"
    kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
    if [ -n "$turn_pid" ]; then wait $turn_pid 2>/dev/null; fi
    no_report "$log"; return
  elif [ $rc -eq 2 ]; then
    fail "$label never left after SIGTERM (>20s)"
    kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
    if [ -n "$turn_pid" ]; then wait $turn_pid 2>/dev/null; fi
    no_report "$log"; return
  fi
  bind=$(echo "$out" | cut -d' ' -f1); proc=$(echo "$out" | cut -d' ' -f2)
  if [ "$bind" -gt 5000 ]; then fail "$label took ${bind}ms to stop accepting after SIGTERM"
  elif [ "$proc" -gt 20000 ]; then fail "$label took ${proc}ms to leave after SIGTERM"
  else note "$label ok (bind refused ${bind}ms, process gone ${proc}ms after SIGTERM)"
  fi
  wait $pid 2>/dev/null
  if [ -n "$turn_pid" ]; then wait $turn_pid 2>/dev/null; fi
  no_report "$log"
}

# The HTTP surface a unit test can only fake: the bearer gate, both response shapes, a
# stream that terminates, and the pairing page's loopback rule — on the shipped binary,
# under the sanitizer.
serve_surface() { # serve_surface <port>
  port=$1
  log=$HOME_DIR/serve-surface.log
  DD_HOME=$HOME_DIR DD_BASE_URL=$BASE_URL DD_SERVE_KEY=probe-key "$BIN" --home "$HOME_DIR" --serve 127.0.0.1:"$port" > "$log" 2>&1 &
  pid=$!
  if ! ready "http://127.0.0.1:$port/health" "$pid"; then
    fail "serve-surface never answered"; kill -9 $pid 2>/dev/null; no_report "$log"; return
  fi
  url="http://127.0.0.1:$port/health"
  body='{"model":"probe","messages":[{"role":"user","content":"hello"}]}'
  code=$(curl -s -o /dev/null -w '%{http_code}' -m 5 "http://127.0.0.1:$port/v1/chat/completions" -X POST -H 'Content-Type: application/json' -d "$body")
  if [ "$code" = 401 ]; then note "no key rejected (401)"
  else fail "no key accepted ($code) — a keyless serve answers anyone on the port"
  fi
  code=$(curl -s -o /dev/null -w '%{http_code}' -m 5 -H 'Authorization: Bearer wrong' "http://127.0.0.1:$port/v1/chat/completions" -X POST -H 'Content-Type: application/json' -d "$body")
  if [ "$code" = 401 ]; then note "wrong key rejected (401)"
  else fail "wrong key accepted ($code)"
  fi
  curl -s -m 30 -H 'Authorization: Bearer probe-key' "http://127.0.0.1:$port/v1/chat/completions" -X POST \
    -H 'Content-Type: application/json' -d "{\"model\":\"probe\",\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}" > "$HOME_DIR/serve.plain" 2>&1
  has "$HOME_DIR/serve.plain" '"finish_reason":"stop"' "non-stream turn"
  has "$HOME_DIR/serve.plain" '"role":"assistant"' "non-stream message shape"
  curl -s -N -m 30 -H 'Authorization: Bearer probe-key' "http://127.0.0.1:$port/v1/chat/completions" -X POST \
    -H 'Content-Type: application/json' -d "{\"model\":\"probe\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}" > "$HOME_DIR/serve.stream" 2>&1
  has "$HOME_DIR/serve.stream" '"delta"' "streamed deltas"
  has "$HOME_DIR/serve.stream" '[DONE]' "stream terminator"
  # A client that hangs up mid-stream: the server must still be serving afterwards.
  curl -s -N -m 1 -H 'Authorization: Bearer probe-key' "http://127.0.0.1:$port/v1/chat/completions" -X POST \
    -H 'Content-Type: application/json' -d '{"model":"probe","stream":true,"messages":[{"role":"user","content":"slow"}]}' >/dev/null 2>&1
  sleep 1
  if curl -s -m 3 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then note "hangup left the bind responsive"
  else fail "hangup mid-stream killed or wedged the bind"
  fi
  out=$(term_shutdown $pid "$url"); rc=$?
  if [ $rc -eq 1 ]; then fail "serve-surface kept the bind accepting after SIGTERM (>5s)"; kill -9 $pid 2>/dev/null
  elif [ $rc -eq 2 ]; then fail "serve-surface never left after SIGTERM (>20s)"; kill -9 $pid 2>/dev/null
  else
    bind=$(echo "$out" | cut -d' ' -f1); proc=$(echo "$out" | cut -d' ' -f2)
    if [ "$bind" -gt 5000 ]; then fail "serve-surface took ${bind}ms to stop accepting after SIGTERM"
    else note "serve-surface ok (bind refused ${bind}ms, process gone ${proc}ms after SIGTERM)"
    fi
  fi
  wait $pid 2>/dev/null
  no_report "$log"
}

gateway_surface() { # gateway_surface <port>
  port=$1
  log=$HOME_DIR/gw-surface.log
  DD_HOME=$HOME_DIR DD_BASE_URL=$BASE_URL "$BIN" --home "$HOME_DIR" --gateway 127.0.0.1:"$port" > "$log" 2>&1 &
  pid=$!
  if ! ready "https://127.0.0.1:$port/health" "$pid"; then
    fail "gw-surface never answered"; kill -9 $pid 2>/dev/null; no_report "$log"; return
  fi
  url="https://127.0.0.1:$port/health"
  curl -sk -m 5 "https://127.0.0.1:$port/pair.json" > "$HOME_DIR/gw.pair" 2>&1
  has "$HOME_DIR/gw.pair" '"code"' "pairing page"
  has "$HOME_DIR/gw.pair" '"fp"' "fingerprint on the pairing page"
  code=$(curl -sk -o /dev/null -w '%{http_code}' -m 5 "https://127.0.0.1:$port/command" -X POST -H 'Content-Type: application/json' -d '{"tool":"read_file","args":{"path":"config.yaml"}}')
  if [ "$code" = 401 ]; then note "command without a device token rejected (401)"
  else fail "command without a device token accepted ($code) — the gateway runs tools for anyone"
  fi
  code=$(curl -sk -o /dev/null -w '%{http_code}' -m 5 "https://127.0.0.1:$port/bridge/stream" -X POST -H 'Content-Type: application/json' -d '{}')
  if [ "$code" = 401 ]; then note "bridge without a device token rejected (401)"
  else fail "bridge without a device token accepted ($code)"
  fi
  out=$(term_shutdown $pid "$url"); rc=$?
  if [ $rc -eq 1 ]; then fail "gw-surface kept the bind accepting after SIGTERM (>5s)"; kill -9 $pid 2>/dev/null
  elif [ $rc -eq 2 ]; then fail "gw-surface never left after SIGTERM (>20s)"; kill -9 $pid 2>/dev/null
  else
    bind=$(echo "$out" | cut -d' ' -f1); proc=$(echo "$out" | cut -d' ' -f2)
    if [ "$bind" -gt 5000 ]; then fail "gw-surface took ${bind}ms to stop accepting after SIGTERM"
    else note "gw-surface ok (bind refused ${bind}ms, process gone ${proc}ms after SIGTERM)"
    fi
  fi
  wait $pid 2>/dev/null
  no_report "$log"
}

rm -rf "$HOME_DIR"
mkdir -p "$HOME_DIR" || { echo "cannot make $HOME_DIR"; exit 1; }

# A turn needs a backend. If the operator named none, run the local stub: instant answers, so a
# bind that will not let go while a turn is in flight stays visible instead of hiding behind a
# slow transport. With no python3 the two backend probes skip, and say so.
stub_pid=""
if [ -z "$BASE_URL" ]; then
  if command -v python3 >/dev/null 2>&1; then
    python3 tests/stub_llm.py "$STUB_PORT" > "$HOME_DIR/stub.log" 2>&1 &
    stub_pid=$!
    i=0
    while [ $i -lt 40 ]; do
      curl -s -m 2 "http://127.0.0.1:$STUB_PORT/v1/models" >/dev/null 2>&1 && break
      i=$((i + 1)); sleep 0.25
    done
    if [ $i -ge 40 ]; then fail "the stub backend never answered"; kill -9 "$stub_pid" 2>/dev/null; exit 1; fi
    BASE_URL="http://127.0.0.1:$STUB_PORT"
    note "stub backend on :$STUB_PORT"
  else
    note "no python3: skipped the turn and the serve bind (a turn needs a backend)"
  fi
fi

run version      "$HOME_DIR/version.log"      --home "$HOME_DIR" --version
run init         "$HOME_DIR/init.log"         --home "$HOME_DIR" --init --rules
run devices      "$HOME_DIR/devices.log"      --home "$HOME_DIR" --devices
run pair         "$HOME_DIR/pair.log"         --home "$HOME_DIR" --pair --name asan-probe --host 127.0.0.1:$GW_PORT

# The config --init wrote points wherever the reference says; aim it at the backend
# this probe is allowed to dial, so a turn goes out and comes back through the
# sanitizer-built HTTP client.
if [ -n "$BASE_URL" ]; then
  sed -e "s|^\( *\)base_url:.*|\1base_url: $BASE_URL|" "$HOME_DIR/config.yaml" > "$HOME_DIR/config.probe.yaml"
  DD_HOME=$HOME_DIR DD_BASE_URL=$BASE_URL "$BIN" --config "$HOME_DIR/config.probe.yaml" -- --say hello > "$HOME_DIR/message.log" 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then fail "headless message exited $rc"; head -6 "$HOME_DIR/message.log" | sed 's/^/    /'
  else no_report "$HOME_DIR/message.log"; note "headless message ok"
  fi
  server_probe serve serve "$PORT" /health "http://127.0.0.1:$PORT/v1/chat/completions"
  serve_surface "$((PORT + 1))"
  # The interactive prompt: the slash commands only a live TUI reaches, fed from a pipe.
  DD_HOME=$HOME_DIR DD_BASE_URL=$BASE_URL DD_TUI=1 "$BIN" --home "$HOME_DIR" > "$HOME_DIR/tui.log" 2>&1 <<'EOF'
/tools
/learn
/home
/devices
/models
/skills
/mcp
/steer hold on
/goal finish this
/quit
EOF
  rc=$?
  if [ $rc -ne 0 ]; then fail "tui exited $rc"; head -6 "$HOME_DIR/tui.log" | sed 's/^/    /'
  else no_report "$HOME_DIR/tui.log"; note "tui slash paths ok"
  fi
else
  note "no backend: skipped the turn, the serve bind, the HTTP surface and the tui (a turn needs a backend)"
fi
server_probe gateway gateway "$GW_PORT" /health
gateway_surface "$((GW_PORT + 1))"

 paths=$(ls "$HOME_DIR"/*.log 2>/dev/null | wc -l)   # count before the home is removed
 rm -rf "$HOME_DIR"
if [ -n "$stub_pid" ]; then kill -9 "$stub_pid" 2>/dev/null; fi
if [ $fails -ne 0 ]; then echo "asan-product: $fails FAILED"; exit 1; fi
echo "asan-product: all probes passed (runtime sanitizers clean: 0 reports across $paths walked paths)"

#!/usr/bin/env bash
# tests/fake_engine.sh — a dweller that speaks the strata protocol, for the crossing's tests.
#
# It answers exactly as src/limbo/stream.cpp answers, so a change in what the crossing reads
# shows up here first. Markers ride inside the ids:
#   ...CRASH...  the process ends with code 7 mid-turn, no DONE
#   ...REFUSE... the engine refuses the request with ERR and stays idle
#   ...SLOW...   the turn takes a second, so a timeout has something to time out on
#   FAKE_QUIET=1   the process never answers READY: the shape a load that hangs has
#   FAKE_CTX=""    READY with no window
stops=0
# FAKE_QUIET: a dweller that reads commands and says nothing, forever. The crossing must bound a
# load like this rather than wait on it.
if [ -n "${FAKE_QUIET-}" ]; then
    while read -r _; do :; done
    exit 0
fi
# `-4096`, not `:-4096`: an empty FAKE_CTX is a dweller that says READY with no window, which is
# a shape the crossing has to refuse. The `:-` form would substitute 4096 and hide that case.
printf 'READY %s stop\n' "${FAKE_CTX-4096}"
printf 'INFO version=0.1.39 kv_mib=1 slots=1\n'
printf 'a note the log must keep\n' >&2
while read -r line; do
    case "$line" in
        QUIT) exit 0 ;;
        STOP)
            stops=$((stops + 1))
            printf 'INFO stops=%d\n' "$stops"
            ;;
        GEN*FLOOD*)
            # More lines than the ring holds: the pump must backpressure the pipe, not drop a token.
            for i in $(seq 1 5000); do printf 'T %d\n' "$((i % 100))"; done
            printf 'DONE 5000 10 1.0 50.0 stop\n'
            ;;
        GEN*CRASH*) exit 7 ;;
        GEN*REFUSE*) printf 'ERR context too long\n' ;;
        GEN*SLOW*)
            sleep 1
            printf 'DONE 1 10 5.0 1000.0 length\n'
            ;;
        GEN*)
            printf 'PP 10 10 5.0 2000\n'
            printf 'RESUME 3\n'
            printf 'T 1\nLP -0.2\n'
            printf 'T 2\nLP -0.1\n'
            printf 'T 0\n'
            printf 'DONE 3 10 5.0 12.0 stop 2 4 5\n'
            ;;
        VRAM*) printf 'VRAM 1 3 2\n' ;;
        *) printf 'INFO note=%s\n' "$line" ;;
    esac
done

#!/usr/bin/env python3
"""Esc, pressed mid-call, against the real binary on a real pty.

The module test can prove the hand-off holds a flag and not a bool. Only a running process
can prove the hand-off is actually WIRED: that Esc reaches the shell command that is in
flight, that a call the turn skipped still comes back as a result on the wire, and that the
Esc of one turn does not cancel the next one.

  BIN=build/ariadne python3 tests/interrupt_live_probe.py

Runs its own stub backend (tests/stub_llm.py in tool mode) and its own home, so it leans on
nothing the operator happens to be running.
"""
import fcntl
import os
import pty
import re
import select
import termios
import signal
import struct
import subprocess
import sys
import time

BIN = os.environ.get("BIN", "build/ariadne")
HOME = os.environ.get("PROBE_HOME", "/tmp/ariadne-esc-home")
STUB_PORT = int(os.environ.get("PROBE_STUB_PORT", "18791"))
ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07]*\x07|\x1b[@[X\\]")

fails = []
screen = ""


def note(msg):
    print(f"  {msg}", flush=True)


def fail(msg):
    print(f"FAILED: {msg}", flush=True)
    fails.append(msg)


def strip(text):
    out = ANSI.sub("", text)
    return out.replace("\x1b", "")


def clean_home():
    subprocess.run(["rm", "-rf", HOME], check=False)
    os.makedirs(HOME, exist_ok=True)


def init_home():
    r = subprocess.run([BIN, "--home", HOME, "--init"], capture_output=True, text=True, timeout=60)
    if r.returncode != 0 or not os.path.isfile(f"{HOME}/config.yaml"):
        fail(f"--init wrote no config (rc {r.returncode}): {r.stderr[:200]}")
        return False
    # The pod a turn recalls from is named in the home's config.yaml, not in the environment.
    # Point it at a port nothing listens on, so the probe measures the product, not the pod.
    cfg = open(f"{HOME}/config.yaml").read()
    cfg = re.sub(r"url: http://127\.0\.0\.1:\d+/mcp", "url: http://127.0.0.1:9/mcp", cfg)
    open(f"{HOME}/config.yaml", "w").write(cfg)
    return True


def start_stub():
    env = dict(os.environ, DD_STUB_TOOL="1", DD_STUB_TOOL_COMMAND="sleep 25",
               DD_STUB_TOOL_COMMAND2="echo the queued one")
    p = subprocess.Popen([sys.executable, "tests/stub_llm.py", str(STUB_PORT)], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    time.sleep(0.6)
    return p


def main():
    if not os.path.isfile(BIN):
        fail(f"no binary at {BIN}")
        return 1
    clean_home()
    if not init_home():
        return 1
    stub = start_stub()
    master, slave = pty.openpty()
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 120, 0, 0))
    env = dict(os.environ, DD_HOME=HOME, DD_BASE_URL=f"http://127.0.0.1:{STUB_PORT}",
               DD_NO_SOAK="1", DD_MAX_TURNS="4", DD_FAST_LAYER="4000", TERM="xterm-256color",
               DD_VERBOSE="full")
    proc = subprocess.Popen([BIN, "--home", HOME], env=env, stdin=slave, stdout=slave, stderr=slave,
                            start_new_session=True)
    os.close(slave)

    def pump(seconds):
        """Read the pty for `seconds`; return True if the screen changed as asked."""
        global screen
        deadline = time.time() + seconds
        while time.time() < deadline:
            r, _, _ = select.select([master], [], [], 0.2)
            if not r:
                continue
            try:
                screen += strip(os.read(master, 4096).decode("utf-8", "replace"))
            except OSError:
                return False
        return True

    def wait_for(needle, seconds, label):
        global screen
        deadline = time.time() + seconds
        while time.time() < deadline:
            if needle in screen:
                return True
            r, _, _ = select.select([master], [], [], 0.2)
            if r:
                try:
                    screen += strip(os.read(master, 4096).decode("utf-8", "replace"))
                except OSError:
                    break
        fail(f"{label}: never saw {needle!r} · screen tail: {screen[-260:]!r}")
        return False

    try:
        pump(2.0)
        os.write(master, b"run the long command\r")
        # The call has to be in flight before Esc means anything.
        wait_for("sleep 25", 12, "the tool call started")
        os.write(master, b"\x1b")   # Esc, the operator's cancel key
        mark = len(screen)          # what this turn's Esc is allowed to say
        wait_for("interrupting…", 6, "Esc reached the running turn")
        wait_for("⏹ interrupted", 12, "the turn stopped interrupted")
        # The scene repaints, so counting frames is not counting notes: presence is the claim.
        if "interrupting…" not in screen[mark:]:
            fail("this turn's Esc never said interrupting…")
        # The queued call never ran, and that is not silence: its id comes back as a result.
        wait_for("was skipped due to user interrupt", 12, "the queued call came back as a result")
        note("Esc cancelled the in-flight call; the queued call returned a cancelled result")

        # The next turn must not inherit the cancel. This is the whole reason the flag is per call.
        os.write(master, b"say it again\r")
        # Turn two asks a short command. A flag still set from turn one would skip it, so seeing
        # its output is proof the hand-off was cleared — not proof that a note stopped being drawn.
        wait_for("turn two ran", 30, "the next turn ran its own call after the Esc")
        note("the next turn ran clean — the Esc of turn one did not cancel turn two")
        open("/tmp/ariadne-esc-probe.log", "w").write(screen)   # what the operator saw, for a failed run to read
    finally:
        if fails:
            open("/tmp/ariadne-esc-probe.log", "w").write(screen)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            fail("the TUI did not leave on SIGTERM")
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        stub.terminate()
        try:
            stub.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(stub.pid), signal.SIGKILL)
        os.close(master)

    if fails:
        print(f"{len(fails)} check(s) failed")
        return 1
    print("interrupt live probe: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

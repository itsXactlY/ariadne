#!/usr/bin/env python3
"""An OpenAI-compatible backend that answers nothing.

Product probes (asan-product, the serve walk) need a backend to send a turn through the
real HTTP client and the real SSE reader. A stub that answers instantly is the only way
to exercise those paths without dialing a live model, so it speaks exactly the two
endpoints the client uses and nothing else:

  GET  /v1/models              -> one model id
  POST /v1/chat/completions    -> a short answer, streamed or not as the request asks

Deliberately slow by nothing: an instant answer is what makes a latency bug (a bind that
will not let go while a turn is in flight) visible rather than hidden behind transport.
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL = "stub-model"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):  # noqa: N802 - the method name is the HTTP verb
        if self.path.split("?")[0] == "/v1/models":
            body = json.dumps({"object": "list", "data": [{"id": MODEL, "object": "model"}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path.split("?")[0] == "/health":
            body = b'{"ok":true}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_POST(self):  # noqa: N802 - the method name is the HTTP verb
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            req = json.loads(raw)
        except ValueError:
            self.send_response(400)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        # A tool mode, for the probes that need a call actually running: the first request of a
        # turn asks for one command, the request after it (which carries the tool result) answers.
        # Without a real call there is nothing to interrupt, so an Esc probe would prove nothing.
        tool_mode = os.environ.get("DD_STUB_TOOL") == "1"
        msgs = req.get("messages", [])
        users = [m for m in msgs if m.get("role") == "user"]
        # A turn that opens with the operator's words asks for a call; the request carrying the
        # results answers. Turn two asks a SHORT call: if the previous turn's Esc were still set,
        # that call would never run and its output would never appear — which is the whole point.
        wants_tool = tool_mode and bool(msgs) and msgs[-1].get("role") == "user"
        first_turn = len(users) <= 1
        answer = "stub answer"
        tool_call = None
        if wants_tool and not first_turn:
            tool_call = [{
                "index": 0,
                "id": os.environ.get("DD_STUB_TOOL_ID", "call-1"),
                "type": "function",
                "function": {
                    "name": os.environ.get("DD_STUB_TOOL_NAME", "run_terminal"),
                    "arguments": json.dumps({"cmd": os.environ.get("DD_STUB_TOOL_COMMAND3", "echo turn two ran")}),
                },
            }]
        elif wants_tool:
            # Two shell calls, in one batch. Shell calls never run in parallel, so the second is
            # still queued when Esc lands: it is the call that must come back as a result even
            # though it never runs. One call alone would prove nothing about skipped ids.
            tool_call = [{
                "index": 0,
                "id": os.environ.get("DD_STUB_TOOL_ID", "call-1"),
                "type": "function",
                "function": {
                    "name": os.environ.get("DD_STUB_TOOL_NAME", "run_terminal"),
                    "arguments": json.dumps({"cmd": os.environ.get("DD_STUB_TOOL_COMMAND", "sleep 25")}),
                },
            }]
            if os.environ.get("DD_STUB_TOOL_COMMAND2"):
                tool_call.append({
                    "index": 1,
                    "id": os.environ.get("DD_STUB_TOOL_ID2", "call-2"),
                    "type": "function",
                    "function": {
                        "name": os.environ.get("DD_STUB_TOOL_NAME", "run_terminal"),
                        "arguments": json.dumps({"cmd": os.environ["DD_STUB_TOOL_COMMAND2"]}),
                    },
                })
        if req.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            if tool_call:
                chunk = {"choices": [{"index": 0, "delta": {"role": "assistant", "tool_calls": tool_call}}]}
                self.wfile.write(b"data: " + json.dumps(chunk).encode() + b"\n\n")
                self.wfile.flush()
                done = {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]}
                self.wfile.write(b"data: " + json.dumps(done).encode() + b"\n\n")
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
                return
            for piece in ["stub", " answer"]:
                chunk = {"choices": [{"index": 0, "delta": {"content": piece}}]}
                self.wfile.write(b"data: " + json.dumps(chunk).encode() + b"\n\n")
                self.wfile.flush()
            done = {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}
            self.wfile.write(b"data: " + json.dumps(done).encode() + b"\n\n")
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        else:
            body = json.dumps({"choices": [{"index": 0, "message": {"role": "assistant", "content": answer}}],
                               "usage": {"prompt_tokens": 8, "completion_tokens": 2, "total_tokens": 10}}).encode()
            if tool_call:
                body = json.dumps({"choices": [{"index": 0, "message": {"role": "assistant", "content": "",
                               "tool_calls": tool_call}, "finish_reason": "tool_calls"}],
                               "usage": {"prompt_tokens": 8, "completion_tokens": 6, "total_tokens": 14}}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def log_message(self, *a):   # silent: a probe's log must hold only the product's output
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18777
    # Threading, not single-threaded: protocol_version HTTP/1.1 means a streamed response ends at
    # [DONE] while the handler stays in the socket waiting for the next request, exactly as a real
    # llama-server does. A single-threaded stub would then refuse the turn after it and the probe
    # would measure the stub's queue instead of the product's busy_ release.
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()

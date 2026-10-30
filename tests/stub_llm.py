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
        answer = "stub answer"
        if req.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
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

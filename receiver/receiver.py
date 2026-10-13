#!/usr/bin/env python3
"""Minimal example receiver for raven_hug. Python 3 standard library only.

Accepts the bird's POST, checks the shared key, and appends one JSON line per
hug to a file. Use it as a starting point for your own memory system.

    RAVEN_KEY=change-me python3 receiver.py [--port 8080] [--out hugs.jsonl]

The firmware sends either one JSON object, or a JSON list of objects when it
is replaying hugs it queued while offline. Both are handled here.

Auth: the key is accepted either as the header X-Touch-Key or as a ?token=
query parameter, matching the two styles secrets.h.example can produce.
"""
import argparse
import hmac
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

KEY = os.environ.get("RAVEN_KEY", "")
OUT = "hugs.jsonl"
ALLOWED_GESTURES = {"hug"}   # add more here if you extend the firmware


class Handler(BaseHTTPRequestHandler):
    def _reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorised(self, query):
        got = self.headers.get("X-Touch-Key") or (query.get("token") or [""])[0]
        return bool(KEY) and hmac.compare_digest(got.encode(), KEY.encode())

    def do_POST(self):
        url = urlparse(self.path)
        if url.path != "/touch":
            return self._reply(404, {"error": "not found"})
        if not self._authorised(parse_qs(url.query)):
            return self._reply(401, {"error": "unauthorized"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 100_000:
                return self._reply(413, {"error": "bad length"})
            body = json.loads(self.rfile.read(length))
        except Exception:
            return self._reply(400, {"error": "bad json"})

        items = body if isinstance(body, list) else [body]
        if not items or len(items) > 200:
            return self._reply(400, {"error": "nothing to record or too many"})
        received = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        lines = []
        for t in items:
            if not isinstance(t, dict) or t.get("gesture") not in ALLOWED_GESTURES:
                return self._reply(400, {"error": "unknown gesture"})
            t["received_at"] = received
            t["queued_batch"] = isinstance(body, list)
            lines.append(json.dumps(t, separators=(",", ":")))
        with open(OUT, "a", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        self._reply(200, {"ok": True, "recorded": len(lines)})

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--out", default="hugs.jsonl")
    a = ap.parse_args()
    OUT = a.out
    if not KEY:
        sys.exit("Set RAVEN_KEY in the environment first.")
    print(f"listening on 0.0.0.0:{a.port}, writing to {OUT}")
    HTTPServer(("0.0.0.0", a.port), Handler).serve_forever()

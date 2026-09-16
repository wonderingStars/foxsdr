#!/usr/bin/env python3
"""A TLS front for end-to-end testing of the Android build's two uploads.

WHY THIS EXISTS. The Android build sends its usage report and its crash report
through Java's HttpsURLConnection (see src/core/net_post.hpp), and the app's
manifest network_security_config permits cleartext to loopback only - so the
only way to watch either upload against a REAL server is over TLS. An emulator
reaches the host at 10.0.2.2, which no public certificate can name, and adb
reverse is unavailable on some development machines, so the host has to present
a certificate for that literal address from a CA the device has been told to
trust (a user CA plus the debug-overrides trust-anchors block that only a
debuggable build honours).

This is that front: one HTTPS listener that logs every request field by field
and then either forwards it to a real server or answers it itself.

  --forward <base-url>   where a request that is not sunk goes. The crash
                         endpoint is proved against the REAL site binary, so
                         this points at it (http://127.0.0.1:8090).
  --sink <path-prefix>   answered here, 204, never forwarded. Usage reporting
                         does not go to the site at all in production - it goes
                         to a Cloudflare Worker - so there is no real server to
                         point at, and a listener that logs the request is the
                         honest stand-in. Repeatable.
  --log <file>           one block per request: the request line, the headers
                         this program actually sends (content type, user agent),
                         the body length and the body.

The log is the evidence, so it is written with flush() on every line: a run
killed at the end of a test must not lose its last request the way a buffered
redirect would.

NOTHING HERE IS FOR PRODUCTION. It terminates TLS with a certificate generated
for a test, it does not verify anything about the client, and it prints request
bodies to a file. It lives in tools/ because the next person to test an upload
from a device needs exactly this and should not have to write it again.

Usage:
  python3 tools/android_tls_front.py --listen 127.0.0.1:8443 \
      --cert front.crt --key front.key \
      --forward http://127.0.0.1:8090 --sink /u --log front.log

SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""

import argparse
import datetime
import http.server
import socketserver
import ssl
import sys
import urllib.error
import urllib.request

LOG = None
ARGS = None


def note(text):
    stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    line = stamp + " " + text
    print(line, file=sys.stderr, flush=True)
    if LOG is not None:
        LOG.write(line + "\n")
        LOG.flush()


class Front(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "AndroidTlsFront/1.0"

    # The default goes to stderr in apache format and duplicates every line
    # the handler already logs with more detail.
    def log_message(self, fmt, *args):
        pass

    def handle_one(self, method):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length > 0 else b""
        note("---- %s %s (%d bytes) ----" % (method, self.path, len(body)))
        note("  content-type: %s" % self.headers.get("Content-Type"))
        note("  user-agent:   %s" % self.headers.get("User-Agent"))
        note("  tls:          %s %s" % (self.connection.version(),
                                        self.connection.cipher()[0]
                                        if self.connection.cipher() else "?"))
        text = body.decode("utf-8", "replace")
        note("  body: " + (text if len(text) <= 4000 else text[:4000] + "...[clipped]"))

        for prefix in ARGS.sink:
            if self.path.startswith(prefix):
                note("  -> SUNK here (204), not forwarded")
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

        target = ARGS.forward.rstrip("/") + self.path
        req = urllib.request.Request(target, data=body, method=method)
        ct = self.headers.get("Content-Type")
        if ct:
            req.add_header("Content-Type", ct)
        ua = self.headers.get("User-Agent")
        if ua:
            req.add_header("User-Agent", ua)
        # The site is run with -trust-proxy=true, which is how the Pi runs it,
        # so it reads the forwarded address rather than the proxy's own.
        req.add_header("X-Forwarded-For", self.client_address[0])
        try:
            with urllib.request.urlopen(req, timeout=15) as resp:
                status = resp.status
                payload = resp.read()
                headers = dict(resp.headers)
        except urllib.error.HTTPError as e:
            status = e.code
            payload = e.read()
            headers = dict(e.headers)
        except Exception as e:  # connection refused, timeout, ...
            note("  -> FORWARD FAILED: %r" % (e,))
            self.send_response(502)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        note("  -> forwarded to %s, answered %d %s" %
             (target, status, payload[:200].decode("utf-8", "replace").strip()))
        self.send_response(status)
        for name in ("Content-Type", "Retry-After"):
            if name in headers:
                self.send_header(name, headers[name])
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if payload:
            self.wfile.write(payload)

    def do_POST(self):
        self.handle_one("POST")

    def do_GET(self):
        self.handle_one("GET")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    global ARGS, LOG
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.1:8443")
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--forward", required=True)
    ap.add_argument("--sink", action="append", default=[])
    ap.add_argument("--log")
    ARGS = ap.parse_args()

    if ARGS.log:
        LOG = open(ARGS.log, "a", encoding="utf-8")

    host, port = ARGS.listen.rsplit(":", 1)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(ARGS.cert, ARGS.key)

    srv = Server((host, int(port)), Front)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    note("front: https://%s -> %s (sinking %s)" %
         (ARGS.listen, ARGS.forward, ", ".join(ARGS.sink) or "nothing"))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

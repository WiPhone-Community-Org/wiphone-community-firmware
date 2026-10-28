#!/usr/bin/env python3
"""tile_relay.py - a LAN-only, read-only tile relay: the Mac's tile pool to a phone over Wi-Fi.

Serves <master>/<src>/<z>/<x>/<y>.png (default ~/tiles-master, docs/maps.md "Pooling the tiles of
several devices") and nothing else, for the phone's plain-HTTP custom source (0.9.82):

    python3 tools/tile_relay.py [--master DIR] [--port 8765]     # on the Mac; prints its address
    maps dlurl http://<mac>:8765/otm/{z}/{x}/{y}.png otm             # on the phone's console
    maps dl 3 <lat> <lon> <km> 17 elev 0                             # into /maps/otm, at LAN speed

Only GET /<otm|usgs-img|usgs-topo>/<z>/<x>/<y>.png is answered - everything else is a 404, so no
other file in the folder is reachable - with the pool's bytes as they are (USGS = JPEG under the
.png name; the phone sniffs the magic, never the URL). A tile the pool lacks (or holds as 0 bytes)
is a 404, which the phone counts as "no tile": run the job again once the pool has it (a re-run
skips every tile already on the card). It binds the Wi-Fi address only (en0), never 0.0.0.0, and
there is no TLS: home network only.
"""
import http.server
import os
import re
import socketserver
import subprocess
import sys

MASTER = os.path.expanduser("~/tiles-master")   # --master overrides
PATH = re.compile(r"^/(otm|usgs-img|usgs-topo)/(\d{1,2})/(\d{1,7})/(\d{1,7})\.png$")


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"           # keep-alive, as the phone's HTTPClient reuses the link

    def do_GET(self):
        m = PATH.match(self.path)
        data = None
        if m:
            p = os.path.join(MASTER, m.group(1), m.group(2), m.group(3), m.group(4) + ".png")
            try:
                with open(p, "rb") as f:
                    data = f.read()
            except OSError:
                data = None
            if data is not None and len(data) < 100:
                data = None
        if data is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg" if data[:3] == b"\xff\xd8\xff" else "image/png")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    global MASTER
    import argparse
    ap = argparse.ArgumentParser(description="LAN-only read-only tile relay for the phones")
    ap.add_argument("--master", default=MASTER)
    ap.add_argument("--port", type=int, default=8765)
    a = ap.parse_args()
    MASTER = os.path.expanduser(a.master)
    port = a.port
    ip = subprocess.run(["ipconfig", "getifaddr", "en0"], capture_output=True, text=True).stdout.strip()
    if not ip:
        sys.exit("no Wi-Fi address on en0")
    print("relay on http://%s:%d/<src>/{z}/{x}/{y}.png" % (ip, port), flush=True)
    Server((ip, port), Handler).serve_forever()


if __name__ == "__main__":
    main()

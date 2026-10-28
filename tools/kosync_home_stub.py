#!/usr/bin/env python3
"""kosync_home_stub.py — a throwaway KOSync `home=` server for the bench, with SLOW replies.

    python3 tools/kosync_home_stub.py                   # :8089, phone GETs held 3 s
    python3 tools/kosync_home_stub.py --hold 0          # an ordinary fast server
    python3 tools/kosync_home_stub.py --port 8090 --hold 3.5

Point a phone at it with `home=<this Mac's LAN IP>:8089` in /books/kosync.txt (any user=/key=:
the stub checks no password). It keeps the records in memory and serves what the phone's home
client uses: GET /syncs/progress/<doc> (a record, or `{}` for an unknown document, as the
reference server answers), PUT /syncs/progress (stamped with this Mac's clock, like a real
server), GET /users/auth, GET /healthcheck, plus GET /bench/records (every record, as JSON).

WHY THE HOLD. The KOSync review's races (KS-1: a page turned while the pull on open is on its
way; KS-2: a place parked while nobody has answered it; the close's push queued behind that
pull) live inside the pull's own duration, which on a LAN with home= as an IP address is well
under a second — shorter than the serial bridge can deliver two commands (it loses a command
sent < 1.3 s after the last). Holding each progress GET from the PHONE for ~3 s stretches the
pull on open to ~6 s, so `key down` / `key back` 1.5 s apart land inside it every time.
⚠ Keep --hold under the phone's 5 s "silence once connected" limit (KS_STALL_MS in
kosync_sync.cpp) or every GET times out and is retried.
Only GETs from OTHER hosts are held: curl on this Mac (seeding, reading back) is answered at
once. PUTs are never held.

Seed "the X4" from this Mac (the record is stamped NOW — seed just before the step):

    curl -s -X PUT http://127.0.0.1:8089/syncs/progress -H 'Content-Type: application/json' \\
      -d '{"document":"<partial md5>","percentage":0.70,"progress":"","device":"CrossPoint","device_id":"benchx4"}'

The phone's two document ids are printed here on its first pull of the book (partial MD5
first, then the file-name id). No password, no secrets: the auth headers are never printed.
"""
import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RECORDS = {}
LOCK = threading.Lock()
HOLD = 3.0


def stamp():
    return time.strftime("%H:%M:%S") + ".%03d" % (int(time.time() * 1000) % 1000)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):   # our own lines only (and never a header)
        pass

    def _reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def _local(self):
        return self.client_address[0] in ("127.0.0.1", "::1")

    def do_GET(self):
        who = self.client_address[0]
        if self.path == "/healthcheck":
            return self._reply(200, {"state": "OK"})
        if self.path == "/users/auth":
            return self._reply(200, {"authorized": "OK"})
        if self.path == "/bench/records":
            with LOCK:
                snap = dict(RECORDS)
            return self._reply(200, snap)
        if self.path.startswith("/syncs/progress/"):
            doc = self.path[len("/syncs/progress/"):]
            with LOCK:
                rec = dict(RECORDS.get(doc, {}))
            held = 0.0 if self._local() else HOLD
            what = ("%s %.4f %s" % (rec.get("device", "?"), rec.get("percentage", 0), rec.get("timestamp"))
                    if rec else "{} (unknown)")
            print("%s GET  %-15s %s -> %s%s" % (stamp(), who, doc, what,
                                               "  (held %.1f s)" % held if held else ""), flush=True)
            if held:
                time.sleep(held)
            return self._reply(200, rec)
        return self._reply(404, {"message": "not here"})

    def do_PUT(self):
        who = self.client_address[0]
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        if self.path != "/syncs/progress":
            return self._reply(404, {"message": "not here"})
        try:
            body = json.loads(raw.decode() or "{}")
            doc = str(body["document"])
            pct = float(body["percentage"])
        except (ValueError, KeyError, TypeError):
            return self._reply(400, {"message": "bad body"})
        ts = int(time.time())
        rec = {"document": doc, "percentage": pct, "progress": str(body.get("progress", "")),
               "device": str(body.get("device", "")), "device_id": str(body.get("device_id", "")),
               "timestamp": ts}
        with LOCK:
            RECORDS[doc] = rec
        print("%s PUT  %-15s %s <- %s %.4f (ts %d)" % (stamp(), who, doc, rec["device"], pct, ts),
              flush=True)
        return self._reply(200, {"document": doc, "timestamp": ts})


def main():
    global HOLD
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8089)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--hold", type=float, default=3.0,
                    help="seconds to hold each progress GET from another host (default 3; keep < 5)")
    a = ap.parse_args()
    HOLD = max(0.0, a.hold)
    if HOLD >= 5.0:
        print("WARNING: --hold %.1f >= the phone's 5 s stall limit - its GETs will time out" % HOLD)
    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    print("%s kosync stub on %s:%d, phone GETs held %.1f s (Ctrl-C to stop)" % (stamp(), a.bind, a.port, HOLD),
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

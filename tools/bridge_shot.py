#!/usr/bin/env python3
"""Take a WiPhone screenshot THROUGH the panicwatch bridge (no port open, no reset risk):
write `shot` to the cmd file, wait for SHOT BEGIN/END to land in the log, decode the frame.
Usage: bridge_shot.py <out.png> [cmdfile] [logfile]
"""
import base64
import sys
import time

from PIL import Image

out = sys.argv[1]
cmdf = sys.argv[2] if len(sys.argv) > 2 else "/tmp/wiphone.cmd"
logf = sys.argv[3] if len(sys.argv) > 3 else "/tmp/wiphone-serial.log"

start = len(open(logf, "rb").read())
open(cmdf, "w").write("shot\n")
t0 = time.time()
text = ""
while time.time() - t0 < 40:
    time.sleep(1)
    text = open(logf, "rb").read()[start:].decode("utf-8", "replace")
    if "SHOT END" in text:
        break
if "SHOT BEGIN" not in text or "SHOT END" not in text:
    sys.exit("no frame in the log after 40s (%d new bytes)" % len(text))
body = text.split("SHOT BEGIN", 1)[1]
hdr, rest = body.split("\n", 1)
w, h = int(hdr.split()[0]), int(hdr.split()[1])
ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")
b64 = "".join(l.strip() for l in rest.split("SHOT END", 1)[0].splitlines()
              if l.strip() and all(c in ok for c in l.strip()))
raw = base64.b64decode(b64 + "=" * (-len(b64) % 4))
if len(raw) < w * h * 2:
    sys.exit("short frame: %d of %d" % (len(raw), w * h * 2))
img = Image.new("RGB", (w, h))
px = img.load()
for i in range(w * h):
    v = (raw[2 * i] << 8) | raw[2 * i + 1]
    px[i % w, i // w] = (((v >> 11) & 0x1F) * 255 // 31,
                         ((v >> 5) & 0x3F) * 255 // 63,
                         (v & 0x1F) * 255 // 31)
img.save(out)
print("wrote %s (%dx%d)" % (out, w, h))

#!/usr/bin/env python3
"""shot.py — take a screenshot of a running WiPhone over the USB cable.

    tools/shot.py <port> [out.png] [--wait SECONDS] [--cmd "menu"]

Sends `shot` on the serial console and turns the base64 RGB565 frame that comes
back into a PNG. This is the only way to see what a WiPhone screen actually looks
like without holding the phone, which is why it exists: every screen in the Photos
app needs a key press, and a cable cannot press keys.

⚠ Opening the port RESETS the phone (DTR/RTS are wired to EN/IO0), so the script
waits for the boot to finish before typing. --wait tunes that.
"""
import sys, time, base64, argparse, serial
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("port")
ap.add_argument("out", nargs="?", default="shot.png")
ap.add_argument("--wait", type=float, default=15.0, help="seconds to let the phone boot")
ap.add_argument("--cmd", action="append", default=[], help="command(s) to run before the shot")
ap.add_argument("--baud", type=int, default=500000)
a = ap.parse_args()

s = serial.Serial()
s.port = a.port; s.baudrate = a.baud; s.timeout = 0.3
s.dtr = False; s.rts = False
s.open()
t0 = time.time()
while time.time() - t0 < a.wait:
    s.read(8192)
s.reset_input_buffer()

for c in a.cmd:
    s.write((c + "\n").encode()); s.flush()
    time.sleep(1.5)
    sys.stderr.write(s.read(65536).decode("utf-8", "replace"))
    s.reset_input_buffer()

s.write(b"shot\n"); s.flush()
buf = b""
t0 = time.time()
while time.time() - t0 < 40:
    b = s.read(65536)
    if b:
        buf += b
        if b"SHOT END" in buf:
            break
s.close()

text = buf.decode("utf-8", "replace")
if "SHOT BEGIN" not in text or "SHOT END" not in text:
    sys.exit("no frame in the reply (got %d bytes). Is the firmware new enough for `shot`?" % len(buf))
body = text.split("SHOT BEGIN", 1)[1]
hdr, rest = body.split("\n", 1)
w, h, fmt = int(hdr.split()[0]), int(hdr.split()[1]), hdr.split()[2]
# Only the base64 alphabet survives; log lines from other tasks can interleave and are dropped.
ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")
b64 = "".join(l.strip() for l in rest.split("SHOT END", 1)[0].splitlines()
              if l.strip() and all(c in ok for c in l.strip()))
raw = base64.b64decode(b64 + "=" * (-len(b64) % 4))
need = w * h * 2
if len(raw) < need:
    sys.exit("short frame: %d of %d bytes — a log line probably landed mid-dump" % (len(raw), need))

img = Image.new("RGB", (w, h))
px = img.load()
for i in range(w * h):
    v = (raw[2 * i] << 8) | raw[2 * i + 1]          # rgb565be
    px[i % w, i // w] = (((v >> 11) & 0x1F) * 255 // 31,
                         ((v >> 5) & 0x3F) * 255 // 63,
                         (v & 0x1F) * 255 // 31)
img.save(a.out)
print("wrote %s (%dx%d, %s)" % (a.out, w, h, fmt))

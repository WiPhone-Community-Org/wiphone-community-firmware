#!/usr/bin/env bash
# Build the fixtures for tests/test_jpeg.cpp from an EPUB.
#
#   tools/gen_jpeg_fixtures.sh /path/to/book.epub
#
# Pulls two greyscale JPEGs and one colour one out of the book, and renders greyscale
# references with macOS `sips` — a completely independent decoder shipped with the OS, which
# is the point: the test compares our pixels against someone else's.
#
# ⚠ The output is NOT committed. The book these came from is bought and this repo is public.
# Without the fixtures the JPEG suite skips itself and says so.
set -euo pipefail

EPUB="${1:-}"
if [ -z "$EPUB" ] || [ ! -f "$EPUB" ]; then
  echo "usage: $0 <book.epub>" >&2
  exit 2
fi
cd "$(dirname "$0")/.."
OUT="tests/fixtures/jpeg"
mkdir -p "$OUT"

# Pick images out of the zip: the first two 1-component JPEGs, and the first 3-component one.
python3 - "$EPUB" "$OUT" <<'PY'
import sys, zipfile
epub, out = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(epub)
grey, colour = [], []
for n in z.namelist():
    if not n.lower().endswith(('.jpg', '.jpeg')):
        continue
    d = z.read(n)
    if d[:2] != b'\xff\xd8':
        continue
    i, comps = 2, None
    while i + 3 < len(d):
        if d[i] != 0xFF:
            i += 1; continue
        m = d[i+1]
        if m == 0xFF: i += 1; continue
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7: i += 2; continue
        if m in (0xDA, 0xD9): break
        seg = (d[i+2] << 8) | d[i+3]
        if m in (0xC0, 0xC1):
            comps = d[i+9]; break
        if seg < 2: break
        i += 2 + seg
    if comps == 1 and len(grey) < 2:
        grey.append((n, d))
    elif comps == 3 and not colour:
        colour.append((n, d))
for k, (n, d) in enumerate(grey, 1):
    open("%s/grey%d.jpg" % (out, k), "wb").write(d)
    print("grey%d  <- %s (%d bytes)" % (k, n, len(d)))
for n, d in colour:
    open("%s/colour.jpg" % out, "wb").write(d)
    print("colour <- %s (%d bytes)" % (n, len(d)))
PY

# Reference pixels, via sips -> PNG -> PGM (P5). Nothing here needs an image library.
for f in "$OUT"/grey*.jpg; do
  base="${f%.jpg}"
  sips -s format png "$f" --out "$base.png" >/dev/null 2>&1
  python3 - "$base.png" "$base.pgm" <<'PY'
import sys, zlib, struct
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
pos, idat, w, h, ct = 8, b'', 0, 0, 0
while pos < len(d):
    ln, typ = struct.unpack('>I4s', d[pos:pos+8]); pos += 8
    data = d[pos:pos+ln]; pos += ln + 4
    if typ == b'IHDR':
        w, h, bd, ct = struct.unpack('>IIBB', data[:10])
    elif typ == b'IDAT':
        idat += data
raw = zlib.decompress(idat)
bpp = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
stride = w * bpp
out = bytearray(); prev = bytearray(stride); p = 0
for y in range(h):
    f = raw[p]; p += 1
    line = bytearray(raw[p:p+stride]); p += stride
    for i in range(stride):
        a = line[i-bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i-bpp] if i >= bpp else 0
        if f == 1: line[i] = (line[i] + a) & 255
        elif f == 2: line[i] = (line[i] + b) & 255
        elif f == 3: line[i] = (line[i] + (a+b)//2) & 255
        elif f == 4:
            pp = a + b - c
            pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
            pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            line[i] = (line[i] + pr) & 255
    out += bytes(line[i] for i in range(0, stride, bpp))   # first channel = luma
    prev = line
open(dst, 'wb').write(b"P5\n%d %d\n255\n" % (w, h) + bytes(out))
print("  reference %s: %dx%d" % (dst, w, h))
PY
  rm -f "$base.png"
done
# A GROUND-TRUTH pair: pixels we chose, encoded at quality 100 (near lossless). This is the
# only fixture where the right answer is actually known, which is what makes it the primary
# correctness check — the book images can only be compared against another decoder's opinion.
python3 - "$OUT" <<'PY2'
import struct, zlib, sys
out = sys.argv[1]
W = H = 128
raw, truth = bytearray(), bytearray()
for y in range(H):
    raw.append(0)                       # PNG filter: none
    for x in range(W):
        v = 255 if ((x // 8 + y // 8) % 2 == 0) else 0      # hard edges: maximum ringing
        if y > 80:
            v = int(255 * x / (W - 1))                       # a gradient
        if 40 <= y < 56 and 40 <= x < 88:
            v = 128                                          # a flat mid patch
        raw.append(v); truth.append(v)
def chunk(t, d):
    c = struct.pack('>I', len(d)) + t + d
    return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
open(out + "/truth.png", "wb").write(png)
open(out + "/truth.pgm", "wb").write(b"P5\n%d %d\n255\n" % (W, H) + bytes(truth))
print("  ground truth: 128x128 checkerboard + gradient + flat patch")
PY2
sips -s format jpeg -s formatOptions 100 "$OUT/truth.png" --out "$OUT/truth.jpg" >/dev/null 2>&1
rm -f "$OUT/truth.png"

echo "fixtures in $OUT (not committed - see the note at the top of this script)"

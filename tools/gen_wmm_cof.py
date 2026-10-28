#!/usr/bin/env python3
"""Generate WiPhone/wmm2025_cof.h - the World Magnetic Model coefficients the phone ships.

Input: NOAA's WMM.COF for WMM2025 (public domain), kept in the repo verbatim as
tools/data/WMM2025.COF. Its first line is the header

    2025.0            WMM-2025     11/13/2024

(epoch, model name, release date), then one row per Gauss coefficient

    n  m  g_nm  h_nm  gdot_nm  hdot_nm        (nT, nT, nT/year, nT/year)

for n = 1..12, m = 0..n (90 rows), then two lines of 9s.

Output: a C++ header with the epoch, the model name and date, and a static const table
{n, m, g, h, gdot, hdot} in the file's order (n ascending, m ascending), so row
(n, m) sits at index n*(n+1)/2 - 1 + m. The values are stored as float: every coefficient
in the file has one decimal and |value| < 32768, so float keeps each to ~0.002 nT - far
under the 1 nT the tests hold the field to (tests/test_wmm.cpp proves it on all 100 NOAA
test rows). The numbers are copied as the file spells them, not re-printed through Python.

    python3 tools/gen_wmm_cof.py                     # tools/data/WMM2025.COF -> WiPhone/wmm2025_cof.h
    python3 tools/gen_wmm_cof.py --cof X --out Y

When NOAA publishes WMM2030: drop its WMM.COF in tools/data/, run this with --cof, move
the validity window in WiPhone/wmm.cpp (epoch .. epoch + 5) and re-run tests/test_wmm.cpp
against the new test values.
"""
import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
NMAX = 12


def fail(msg):
    sys.exit("gen_wmm_cof: " + msg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cof", default=os.path.join(HERE, "data", "WMM2025.COF"))
    ap.add_argument("--out", default=os.path.join(REPO, "WiPhone", "wmm2025_cof.h"))
    a = ap.parse_args()

    raw = open(a.cof, "rb").read()
    sha = hashlib.sha256(raw).hexdigest()
    lines = raw.decode("ascii").splitlines()
    if not lines:
        fail("empty coefficient file")

    head = lines[0].split()
    if len(head) != 3:
        fail("header line should be 'epoch model date', got %r" % lines[0])
    epoch_txt, model, date = head
    epoch = float(epoch_txt)

    rows = []
    ended = False
    for ln, line in enumerate(lines[1:], start=2):
        s = line.strip()
        if not s:
            continue
        if s.startswith("9999"):
            ended = True
            break
        f = s.split()
        if len(f) != 6:
            fail("line %d: expected 'n m g h gdot hdot', got %r" % (ln, line))
        n, m = int(f[0]), int(f[1])
        for t in f[2:]:
            float(t)                      # must parse; the text itself is what we emit
        rows.append((n, m, f[2], f[3], f[4], f[5]))
    if not ended:
        fail("no 9999... terminator: truncated file?")

    want = [(n, m) for n in range(1, NMAX + 1) for m in range(0, n + 1)]
    got = [(r[0], r[1]) for r in rows]
    if got != want:
        fail("rows are not n=1..%d, m=0..n in order (%d rows)" % (NMAX, len(rows)))
    for r in rows:
        if r[1] == 0 and (float(r[3]) != 0.0 or float(r[5]) != 0.0):
            fail("h/hdot must be 0 for m = 0 (n=%d)" % r[0])
        for t in r[2:]:
            if abs(float(t)) >= 32768.0:
                fail("coefficient %s out of the float-exactness range this header assumes" % t)

    rel = os.path.relpath(os.path.abspath(a.cof), REPO)
    out = []
    out.append("/*")
    out.append(" * GENERATED - do not edit. Made by tools/gen_wmm_cof.py from %s" % rel)
    out.append(" * (NOAA %s coefficients, released %s, public domain), sha256" % (model, date))
    out.append(" * %s." % sha)
    out.append(" * Row (n, m) is at index n*(n+1)/2 - 1 + m. g, h in nT at the epoch;")
    out.append(" * gdot, hdot in nT/year. Included ONLY by WiPhone/wmm.cpp.")
    out.append(" */")
    out.append("#ifndef WMM2025_COF_H")
    out.append("#define WMM2025_COF_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define WMM_COF_EPOCH %s" % epoch_txt)
    out.append("#define WMM_COF_MODEL \"%s\"" % model)
    out.append("#define WMM_COF_DATE \"%s\"" % date)
    out.append("#define WMM_COF_NMAX %d" % NMAX)
    out.append("#define WMM_COF_ROWS %d" % len(rows))
    out.append("")
    out.append("struct WmmCoef { uint8_t n, m; float g, h, gdot, hdot; };")
    out.append("")
    out.append("static const WmmCoef WMM_COF[WMM_COF_ROWS] = {")
    for n, m, g, h, gd, hd in rows:
        out.append("  { %2d, %2d, %9sf, %9sf, %6sf, %6sf }," % (n, m, g, h, gd, hd))
    out.append("};")
    out.append("")
    out.append("#endif  // WMM2025_COF_H")

    with open(a.out, "w") as fh:
        fh.write("\n".join(out) + "\n")
    print("%s: epoch %s, %s (%s), %d rows -> %s" % (rel, epoch_txt, model, date, len(rows),
                                                    os.path.relpath(a.out, REPO)))
    if epoch != 2025.0:
        print("NOTE: epoch is not 2025.0 - move the validity window in WiPhone/wmm.cpp")


if __name__ == "__main__":
    main()

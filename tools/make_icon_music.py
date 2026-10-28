#!/usr/bin/env python3
"""Generate the Music menu icon in the phone's own RLE3 format.

    python3 tools/make_icon_music.py --preview     # ASCII, check it before committing
    python3 tools/make_icon_music.py --emit        # C arrays for src/assets/icons.h

The format, read off TFT_eSPI::drawImageRle3():

    "RLE3" | w | h | c
    c: low 7 bits = number of palette entries, HIGH BIT SET = palette carries alpha
    palette: c entries, RGBA (4 bytes each) when the alpha bit is set
    runs: one byte  cccLnnnn
          ccc  = palette index (0-7)
          L    = long flag; if set the low nibble is the TOP 4 bits of a 12-bit
                 count and the NEXT byte holds the low 8
          nnnn = count when L is clear

⚠ The decoder returns false unless the run lengths sum to EXACTLY w*h. A short or
long encoding does not draw a wrong picture — it draws nothing at all, silently.

The existing icons are 44x44 anti-aliased silhouettes over four alpha levels
(0, 85, 170, 255), black for the selected row and white for the unselected one.
This matches that exactly so Music sits beside Books without looking pasted in.
"""

import argparse

W = H = 44
SS = 4                      # supersampling factor; 4x4 per pixel gives 17 coverage
                            # levels, which is plenty to quantise into 4
LEVELS = (0, 85, 170, 255)


# Nudge to centre the drawn glyph in the 44x44 box. Derived from the rendered
# bounding box, not guessed: without it the notes sat high and left, leaving eleven
# blank rows along the bottom.
OFF_X, OFF_Y = -0.5, -1.5


def inside(x, y):
    """True if the supersample point (in 44-space, float) is inside the glyph.

    A beamed pair of eighth notes: two slanted oval heads, stems rising from the
    RIGHT of each head, and a beam across the tops slanting up to the right.
    Drawn as maths rather than a bitmap so it can be retuned without a paint program.
    """
    # --- note heads: ellipses rotated -20 degrees, the usual engraved slant
    x, y = x + OFF_X, y + OFF_Y
    for cx, cy in ((13.0, 32.0), (29.5, 27.5)):
        dx, dy = x - cx, y - cy
        # rotate by +20 deg into the ellipse's frame
        c, s = 0.9397, 0.3420
        rx, ry = dx * c + dy * s, -dx * s + dy * c
        if (rx / 6.6) ** 2 + (ry / 4.9) ** 2 <= 1.0:
            return True

    # --- stems: from the right edge of each head up to the beam
    for sx, top in ((13.0 + 5.9, 9.0), (29.5 + 5.9, 4.0)):
        if sx - 1.1 <= x <= sx + 1.1 and top <= y <= (32.0 if sx < 20 else 27.5):
            return True

    # --- beam: a thick slanted bar joining the stem tops
    x0, y0 = 13.0 + 5.9 - 1.1, 9.0
    x1, y1 = 29.5 + 5.9 + 1.1, 4.0
    if x0 <= x <= x1:
        t = (x - x0) / (x1 - x0)
        ytop = y0 + (y1 - y0) * t
        if ytop <= y <= ytop + 5.0:
            return True

    return False


def coverage():
    """Per-pixel coverage 0..1, supersampled."""
    grid = []
    for py in range(H):
        row = []
        for px in range(W):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS
                    y = py + (sy + 0.5) / SS
                    if inside(x, y):
                        hits += 1
            row.append(hits / float(SS * SS))
        grid.append(row)
    return grid


def quantise(grid):
    """Coverage -> palette index 0..3."""
    out = []
    for row in grid:
        out.append([min(3, int(v * 4.0 + 0.5)) if v > 0 else 0 for v in row])
    return out


def rle(idx):
    """Flatten to runs. Counts are capped at 4095 (the 12-bit long form)."""
    flat = [v for row in idx for v in row]
    runs, cur, n = [], flat[0], 1
    for v in flat[1:]:
        if v == cur and n < 4095:
            n += 1
        else:
            runs.append((cur, n))
            cur, n = v, 1
    runs.append((cur, n))
    return runs


def encode(runs, rgb):
    out = bytearray(b'RLE3')
    out += bytes([W, H, 0x80 | len(LEVELS)])
    for a in LEVELS:
        out += bytes([rgb[0], rgb[1], rgb[2], a])
    for c, n in runs:
        if n <= 15:
            out.append((c << 5) | n)
        else:
            out.append((c << 5) | 0x10 | ((n >> 8) & 0x0F))
            out.append(n & 0xFF)
    return bytes(out)


def carray(name, data):
    lines = [f"const unsigned char {name}[{len(data)}] PROGMEM = {{"]
    for i in range(0, len(data), 12):
        chunk = ', '.join('0x%02x' % b for b in data[i:i + 12])
        lines.append('  ' + chunk + ('' if i + 12 >= len(data) else ','))
    lines.append('};')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--preview', action='store_true')
    ap.add_argument('--emit', action='store_true')
    args = ap.parse_args()

    idx = quantise(coverage())

    if args.preview:
        shade = ' .:#'
        for row in idx:
            print(''.join(shade[v] for v in row))
        runs = rle(idx)
        print(f"\n{len(runs)} runs, total {sum(n for _, n in runs)} px (must be {W*H})")

    if args.emit:
        runs = rle(idx)
        assert sum(n for _, n in runs) == W * H, "run total must equal w*h or it draws nothing"
        print(carray('icon_Music_b', encode(runs, (0, 0, 0))))
        print()
        print(carray('icon_Music_w', encode(runs, (255, 255, 255))))


if __name__ == '__main__':
    main()

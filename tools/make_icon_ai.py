#!/usr/bin/env python3
"""Generate the AI main-menu icon in the phone's own RLE3 format.

    python3 tools/make_icon_ai.py --preview     # ASCII, check it before committing
    python3 tools/make_icon_ai.py --emit        # C arrays for src/assets/icons.h

Same format, size, alpha levels and encoder as tools/make_icon_almanac.py (and make_icon_music.py,
whose header has the RLE3 layout and the "run total must equal w*h or it draws nothing" trap).
Drawn as maths: a speech bubble with a four-pointed sparkle cut out of it - a question asked and
answered. Silhouette only (one colour, four alpha levels), like every other icon in the row.
"""

import argparse
import math

W = H = 44
SS = 4
LEVELS = (0, 85, 170, 255)


BUBBLE = (3.0, 4.0, 41.0, 32.0)        # x0, y0, x1, y1 of the bubble's body
RADIUS = 8.0                           # its rounded corners
TAIL = ((11.0, 30.0), (21.0, 30.0), (8.0, 41.5))   # the tail, down and to the left
STAR_C, STAR_R = (22.0, 18.0), 11.0    # the sparkle cut out of the middle
SMALL_C, SMALL_R = (33.5, 10.0), 5.0   # ...and a small one up to its right


def in_rounded_rect(x, y, r):
    x0, y0, x1, y1 = r
    if not (x0 <= x <= x1 and y0 <= y <= y1):
        return False
    cx = min(max(x, x0 + RADIUS), x1 - RADIUS)
    cy = min(max(y, y0 + RADIUS), y1 - RADIUS)
    return math.hypot(x - cx, y - cy) <= RADIUS


def in_triangle(x, y, t):
    (ax, ay), (bx, by), (cx, cy) = t
    d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by)
    d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy)
    d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)


def in_sparkle(x, y, c, r):
    """A four-pointed star with concave sides: |dx|^p + |dy|^p <= r^p, p < 1 (0.62: arms thick
    enough to survive the 4-level alpha at 44 px)."""
    p = 0.62
    dx, dy = abs(x - c[0]), abs(y - c[1])
    return dx ** p + dy ** p <= r ** p


def inside(x, y):
    if in_sparkle(x, y, STAR_C, STAR_R) or in_sparkle(x, y, SMALL_C, SMALL_R):
        return False
    return in_rounded_rect(x, y, BUBBLE) or in_triangle(x, y, TAIL)


def coverage():
    grid = []
    for py in range(H):
        row = []
        for px in range(W):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    if inside(px + (sx + 0.5) / SS, py + (sy + 0.5) / SS):
                        hits += 1
            row.append(hits / float(SS * SS))
        grid.append(row)
    return grid


def quantise(grid):
    return [[min(3, int(v * 4.0 + 0.5)) if v > 0 else 0 for v in row] for row in grid]


def rle(idx):
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
        print(carray('icon_AI_b', encode(runs, (0, 0, 0))))
        print()
        print(carray('icon_AI_w', encode(runs, (255, 255, 255))))


if __name__ == '__main__':
    main()

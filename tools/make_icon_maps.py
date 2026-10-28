#!/usr/bin/env python3
"""Generate the Maps main-menu icon in the phone's own RLE3 format.

    python3 tools/make_icon_maps.py --preview     # ASCII, check it before committing
    python3 tools/make_icon_maps.py --emit        # C arrays for src/assets/icons.h

Same format, size, alpha levels and encoder as tools/make_icon_music.py (read that file's
header for the RLE3 layout and the "run total must equal w*h or it draws nothing" trap).
Drawn as maths, not a bitmap: a folded paper map — three panels, the outer two tilted so the
creases read — with a dashed route wandering across it and a location pin standing on the
right-hand panel. Silhouette only (one colour, four alpha levels), like every other icon in
the row, so it sits beside Books and Music without looking pasted in.
"""

import argparse
import math

W = H = 44
SS = 4
LEVELS = (0, 85, 170, 255)


def in_quad(x, y, pts):
    """Point in a convex quad given counter-clockwise (or clockwise) corners."""
    sign = 0
    n = len(pts)
    for i in range(n):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        cross = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0)
        if cross == 0:
            continue
        s = 1 if cross > 0 else -1
        if sign == 0:
            sign = s
        elif s != sign:
            return False
    return True


def near_segment(x, y, ax, ay, bx, by, half):
    """Within `half` of the segment a-b."""
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return math.hypot(x - ax, y - ay) <= half
    t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / L2))
    return math.hypot(x - (ax + t * dx), y - (ay + t * dy)) <= half


# The map: three panels across the middle band of the box. The outer panels lean the
# other way so the two creases show as gaps in the silhouette.
PANELS = (
    ((3.0, 15.0), (15.0, 12.0), (15.0, 37.0), (3.0, 40.0)),
    ((16.5, 12.0), (28.5, 15.0), (28.5, 40.0), (16.5, 37.0)),
    ((30.0, 15.0), (42.0, 12.0), (42.0, 37.0), (30.0, 40.0)),
)

# The route: a dashed polyline knocked OUT of the paper (drawn as holes), so it reads as a
# trail on the map rather than a line floating on it.
ROUTE = ((5.0, 33.0), (11.0, 26.0), (17.0, 29.0), (23.0, 21.0), (27.0, 24.0))

# The pin: a drop shape standing on the right panel, knocked out around it so it separates
# from the paper, and its own hole in the head.
PIN_CX, PIN_CY, PIN_R = 36.0, 9.5, 5.0     # head centre and radius (top of the box)
PIN_TIP = (36.0, 21.5)


def in_paper(x, y):
    return any(in_quad(x, y, q) for q in PANELS)


def on_route(x, y):
    # dashes: 60 % of each segment, from its start
    for i in range(len(ROUTE) - 1):
        ax, ay = ROUTE[i]
        bx, by = ROUTE[i + 1]
        dx, dy = bx - ax, by - ay
        for d0, d1 in ((0.05, 0.40), (0.55, 0.90)):
            if near_segment(x, y, ax + dx * d0, ay + dy * d0, ax + dx * d1, ay + dy * d1, 1.05):
                return True
    return False


def in_pin(x, y, grow=0.0):
    r = PIN_R + grow
    if math.hypot(x - PIN_CX, y - PIN_CY) <= r:
        return True
    # the point: a triangle from the head's tangents down to the tip
    tx, ty = PIN_TIP
    ty += grow * 1.2
    ang = math.asin(min(1.0, r / max(1e-6, math.hypot(tx - PIN_CX, ty - PIN_CY))))
    base = math.atan2(ty - PIN_CY, tx - PIN_CX)
    a1, a2 = base - (math.pi / 2 - ang), base + (math.pi / 2 - ang)
    p1 = (PIN_CX + r * math.cos(a1), PIN_CY + r * math.sin(a1))
    p2 = (PIN_CX + r * math.cos(a2), PIN_CY + r * math.sin(a2))
    return in_quad(x, y, (p1, (tx, ty), p2))


def inside(x, y):
    if in_pin(x, y):
        # the hole in the pin's head
        return math.hypot(x - PIN_CX, y - PIN_CY) > 2.0
    if in_pin(x, y, grow=1.6):
        return False                       # the gap that separates the pin from the paper
    if in_paper(x, y):
        return not on_route(x, y)
    return False


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
        print(carray('icon_Maps_b', encode(runs, (0, 0, 0))))
        print()
        print(carray('icon_Maps_w', encode(runs, (255, 255, 255))))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Generate the Almanac main-menu icon in the phone's own RLE3 format.

    python3 tools/make_icon_almanac.py --preview     # ASCII, check it before committing
    python3 tools/make_icon_almanac.py --emit        # C arrays for src/assets/icons.h

Same format, size, alpha levels and encoder as tools/make_icon_music.py (read that file's
header for the RLE3 layout and the "run total must equal w*h or it draws nothing" trap).
Drawn as maths: a crescent moon and a sun rising over a horizon. Silhouette only (one
colour, four alpha levels), like every other icon in the row.
"""

import argparse
import math

W = H = 44
SS = 4
LEVELS = (0, 85, 170, 255)


def near_segment(x, y, ax, ay, bx, by, half):
    """Within `half` of the segment a-b."""
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return math.hypot(x - ax, y - ay) <= half
    t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / L2))
    return math.hypot(x - (ax + t * dx), y - (ay + t * dy)) <= half


# A crescent moon high on the left and a sun rising over a horizon on the right: the two
# things the Almanac is about, and the question it is asked most (is it light yet?).
MOON_C, MOON_R = (14.5, 16.0), 11.0          # the lit disc
BITE_C, BITE_R = (20.0, 12.0), 9.6          # the dark disc that bites it into a crescent
SUN_C, SUN_R = (30.0, 37.5), 7.0             # centre on the horizon: only the top half shows
HORIZON = (39.0, 41.5)                       # y band of the ground line
RAYS = (180.0, 135.0, 90.0, 45.0, 0.0)       # degrees, 0 = east, counter-clockwise (up)


def in_moon(x, y):
    return (math.hypot(x - MOON_C[0], y - MOON_C[1]) <= MOON_R and
            math.hypot(x - BITE_C[0], y - BITE_C[1]) > BITE_R)


def in_sun(x, y):
    if y > SUN_C[1]:
        return False
    d = math.hypot(x - SUN_C[0], y - SUN_C[1])
    if d <= SUN_R:
        return True
    for a in RAYS:
        r = math.radians(a)
        ax, ay = SUN_C[0] + math.cos(r) * (SUN_R + 2.6), SUN_C[1] - math.sin(r) * (SUN_R + 2.6)
        bx, by = SUN_C[0] + math.cos(r) * (SUN_R + 6.2), SUN_C[1] - math.sin(r) * (SUN_R + 6.2)
        if near_segment(x, y, ax, ay, bx, by, 1.15):
            return True
    return False


def inside(x, y):
    if HORIZON[0] <= y <= HORIZON[1] and 3.0 <= x <= 41.0:
        return True
    return in_moon(x, y) or in_sun(x, y)


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
        print(carray('icon_Almanac_b', encode(runs, (0, 0, 0))))
        print()
        print(carray('icon_Almanac_w', encode(runs, (255, 255, 255))))


if __name__ == '__main__':
    main()

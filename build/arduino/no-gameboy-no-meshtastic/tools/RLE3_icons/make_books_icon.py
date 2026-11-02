#!/usr/bin/env python3
"""Draw the Books main-menu icon and encode it, with no image libraries.

The repo's RLE3.py needs cv2 + numpy, which are not installed here and are a heavy thing to
require for one 44x44 glyph. This draws the shape directly, super-samples it for the same
alpha-edged look the built-in icons have, and writes:

    Books_w.png / Books_b.png     source art, so the icon can be re-edited like the others
    Books_w.rle3 / Books_b.rle3   the format the firmware draws
    Books_w.h / Books_b.h         C arrays to paste into icons.h

⚠ Do NOT run MAKE_ICONS.sh to pick these up: it regenerates icons.h from its own list, and
that list is missing both these icons AND the Meshtastic pair, so it would silently delete
them. Append the generated .h files instead (both names are in the list now, for the day
someone does run it).

RLE3, read off its decoder in TFT_eSPI.cpp (drawImageRle3):
    "RLE3", varint w, varint h, (nColors & 0x7F) | 0x80 if alpha,
    palette of RGB or RGBA, then runs:
      1 byte : iii0cccc            colour index, count 0-15
      2 bytes: iii1cccc cccccccc   colour index, count 0-4095
"""
import struct, zlib

W = H = 44
SS = 4                                  # super-sampling factor per axis


def inside_quad(px, py, quad):
    """Point in convex quad, given clockwise in icon coordinates."""
    sign = None
    for i in range(4):
        ax, ay = quad[i]
        bx, by = quad[(i + 1) % 4]
        cross = (bx - ax) * (py - ay) - (by - ay) * (px - ax)
        if cross == 0:
            continue
        s = cross > 0
        if sign is None:
            sign = s
        elif s != sign:
            return False
    return True


def coverage():
    """Alpha 0-255 per pixel, from a super-sampled draw of the glyph."""
    # An open book: two pages rising towards the middle, a solid spine joining them, and
    # lines of text knocked back out of each page.
    # The gap down the middle is left EMPTY on purpose: it is what makes two pages read as an
    # open book rather than as one slab. The top edges rising towards it do the rest.
    left = [(4, 15.5), (19, 10.5), (19, 31), (4, 34.5)]
    right = [(25, 10.5), (40, 15.5), (40, 34.5), (25, 31)]
    spine = None
    text_rows = [18.5, 23.0, 27.5]      # y centres; 2.6px tall
    cov = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    px = x + (sx + 0.5) / SS
                    py = y + (sy + 0.5) / SS
                    on = (inside_quad(px, py, left) or inside_quad(px, py, right)
                          or (spine is not None and inside_quad(px, py, spine)))
                    if on:
                        for ty in text_rows:
                            if abs(py - ty) <= 1.3 and (6.5 <= px <= 16.5 or
                                                        27.5 <= px <= 37.5):
                                on = False
                                break
                    if on:
                        hits += 1
            cov[y][x] = round(255 * hits / (SS * SS))
    return cov


def write_png(path, cov, ink):
    raw = b''
    for y in range(H):
        raw += b'\x00'                  # filter: none
        for x in range(W):
            raw += bytes((ink, ink, ink, cov[y][x]))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)


def varint(n):
    """7 bits per byte, most significant first, high bit = 'another byte follows'."""
    parts = [n & 0x7F]
    n >>= 7
    while n:
        parts.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(parts))


def write_rle3(path, cov, ink):
    # Four alpha levels: enough for a smooth edge, well inside the format's eight.
    levels = [0, 85, 170, 255]
    idx = []
    for y in range(H):
        for x in range(W):
            a = cov[y][x]
            idx.append(min(range(4), key=lambda i: abs(levels[i] - a)))

    out = bytearray(b'RLE3')
    out += varint(W)
    out += varint(H)
    out.append(len(levels) | 0x80)          # alpha palette
    for a in levels:
        out += bytes((ink, ink, ink, a))

    i = 0
    total = 0
    while i < len(idx):
        c = idx[i]
        n = 1
        while i + n < len(idx) and idx[i + n] == c and n < 4095:
            n += 1
        if n <= 15:
            out.append((c << 5) | n)
        else:
            out.append((c << 5) | 0x10 | (n >> 8))
            out.append(n & 0xFF)
        total += n
        i += n
    assert total == W * H, "run lengths must cover every pixel exactly"
    open(path, 'wb').write(bytes(out))
    return len(out)


def write_c(path, name, blob):
    b = open(blob, 'rb').read()
    lines = ["const unsigned char icon_%s[%d] PROGMEM = {" % (name, len(b))]
    for i in range(0, len(b), 12):
        lines.append("  " + ", ".join("0x%02x" % c for c in b[i:i + 12]) + ",")
    lines.append("};")
    open(path, 'w').write("\n".join(lines) + "\n")


if __name__ == "__main__":
    cov = coverage()
    for y in range(0, H, 2):             # preview, two rows per line
        print("".join(' ' if cov[y][x] < 40 else ('#' if cov[y][x] > 170 else '+')
                      for x in range(W)))
    for name, ink in (("Books_w", 255), ("Books_b", 0)):
        write_png(name + ".png", cov, ink)
        n = write_rle3(name + ".rle3", cov, ink)
        write_c(name + ".h", name, name + ".rle3")
        print("%s: %d bytes rle3" % (name, n))

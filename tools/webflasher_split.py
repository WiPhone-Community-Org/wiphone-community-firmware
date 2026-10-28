#!/usr/bin/env python3
"""webflasher_split.py <merged.bin> <outdir> - cut a merged image into the web flasher's two parts.

The merged image is what esptool's merge_bin writes (offset 0 based, headers patched for the
16 MB part). Its own partition table sits at 0x8000; that is where the nvs and otadata extents
come from - never assumed.

  wiphone-boot.bin  0x1000 .. nvs start      bootloader + partition table
  wiphone-app.bin   otadata start .. end     boot_app0 + the application

🛑 nvs (the mesh identity key, the booksync passcode and name, the KOSync memo, the WiFi
calibration) is in NEITHER part. merge_bin pads it with 0xFF, and written from offset 0 the one
merged image erased every user's NVS on every web-flasher install through 0.9.78.

The offsets a manifest needs are printed last as JSON on one line, so a caller can pick them up.
"""
import json
import struct
import sys

TABLE_OFFSET = 0x8000
TABLE_MAX = 0xC00
BOOT_OFFSET = 0x1000
BOOT_NAME, APP_NAME = "wiphone-boot.bin", "wiphone-app.bin"


class SplitError(Exception):
    pass


def partition_table(img):
    """(type, subtype, offset, size) for each entry, from the table inside the image."""
    out = []
    end = min(len(img), TABLE_OFFSET + TABLE_MAX)
    for i in range(TABLE_OFFSET, end - 31, 32):
        magic, ptype, sub, off, size = struct.unpack_from("<HBBII", img, i)
        if magic != 0x50AA:
            break
        out.append((ptype, sub, off, size))
    return out


def plan(img):
    """[(name, start, end)] for the two parts, and the nvs extent they step around."""
    nvs = otadata = None
    for ptype, sub, off, size in partition_table(img):
        if ptype == 1 and sub == 0x02 and nvs is None:
            nvs = (off, off + size)
        if ptype == 1 and sub == 0x00 and otadata is None:
            otadata = (off, off + size)
    if not nvs or not otadata:
        raise SplitError("partition table has no nvs/otadata entry - refusing to guess the offsets")
    if not (TABLE_OFFSET + TABLE_MAX <= nvs[0] and nvs[1] <= otadata[0]):
        raise SplitError("nvs 0x%x-0x%x is not between the table and otadata - this split no longer fits" % nvs)
    if len(img) <= otadata[0]:
        raise SplitError("image ends at 0x%x, before otadata 0x%x - not a merged image" % (len(img), otadata[0]))
    parts = [(BOOT_NAME, BOOT_OFFSET, nvs[0]), (APP_NAME, otadata[0], len(img))]
    for name, a, b in parts:
        if a < nvs[1] and b > nvs[0]:
            raise SplitError("%s would cover nvs - refusing to stage it" % name)
    return parts, nvs


def split(merged_path, out_dir):
    img = open(merged_path, "rb").read()
    parts, nvs = plan(img)
    for name, a, b in parts:
        open("%s/%s" % (out_dir, name), "wb").write(img[a:b])
        print("  %-18s 0x%06x..0x%06x  %d bytes" % (name, a, b, b - a))
    print("  nvs 0x%x..0x%x is in neither part" % nvs)
    print(json.dumps([{"path": name, "offset": a} for name, a, _ in parts]))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        split(sys.argv[1], sys.argv[2])
    except SplitError as e:
        sys.exit(str(e))

#!/usr/bin/env python3
"""check_webflasher_split.py - the web flasher's two parts never cover nvs (0.9.79).

Through 0.9.78 the installer wrote ONE merged image from offset 0. merge_bin pads the gap between the
partition table (0x8000) and boot_app0 (0xe000) with 0xFF, and that gap IS the nvs partition - so every
web-flasher install erased the mesh identity key (other nodes then flag a MISMATCH and DMs fail until
they forget the node), the booksync passcode and device name, the KOSync memo, and the WiFi
calibration. Seen on phone 1 on 2026-09-25. tools/webflasher_split.py now cuts the image into a part
below nvs and a part above it, reading the extents from the table INSIDE the image. This pins:

  1. on a synthetic 16 MB-layout image the parts are 0x1000..nvs and otadata..end, byte-equal to the
     image's own slices, and rejoined with 0xFF over nvs they give the image back;
  2. the parts come from the TABLE, not from constants: move nvs and otadata and the cuts follow;
  3. a table without nvs or otadata, a table whose nvs is not between it and otadata, and an image
     that ends before otadata are all REFUSED (the caller never stages a part that covers nvs);
  4. the committed installer page reads every part of the manifest and refuses one that would cover
     nvs - the page's own guard, matched by text, since no JS runs here;
  5. make_webflasher.sh stages through webflasher_split.py and publish_webflasher.sh copies the two
     parts and not a merged image;
  6. the page imports esptool-js from THIS site (a vendored bundle, webflasher/THIRD_PARTY.md), never a
     CDN, publish copies that bundle, and EVERY `loader.X(` / `transport.X(` the page calls is defined in
     the bundle it serves. The 0.9.79-NH page called `loader.after()` - 0.4.5 has `hardReset()` - so a
     finished install reported "Failed" and left the phone dark in the bootloader ({user}'s first browser
     install, 2026-09-26). A mock could not catch it: the mock copied the page's assumption. Only the
     served bytes can. A self-test plants `loader.after(` and shows it trips.
"""
import os
import re
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import webflasher_split as ws  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("  FAIL:", what)


def table(entries):
    t = b""
    for ptype, sub, off, size in entries:
        t += struct.pack("<HBBII", 0x50AA, ptype, sub, off, size) + b"x" * 16 + b"\0" * 4
    return t


def image(entries, total=0x14000):
    img = bytearray(os.urandom(total))
    img[0:0x1000] = b"\xff" * 0x1000
    for ptype, sub, off, size in entries:            # merge_bin pads what it is not given with 0xFF
        if ptype == 1 and sub == 0x02 and off + size <= total:
            img[off:off + size] = b"\xff" * size
    t = table(entries)
    img[0x8000:0x8000 + len(t)] = t
    img[0x8000 + len(t):0x8000 + len(t) + 32] = b"\xff" * 32
    return bytes(img)


STD = [(1, 0x02, 0x9000, 0x5000), (1, 0x00, 0xE000, 0x2000), (0, 0x10, 0x10000, 0x4000)]

# 1. the standard layout
img = image(STD)
parts, nvs = ws.plan(img)
check(nvs == (0x9000, 0xE000), "nvs extent %r" % (nvs,))
check(parts == [("wiphone-boot.bin", 0x1000, 0x9000), ("wiphone-app.bin", 0xE000, len(img))],
      "standard cuts %r" % (parts,))
with tempfile.TemporaryDirectory() as d:
    import io, contextlib
    mp = os.path.join(d, "m.bin")
    open(mp, "wb").write(img)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        ws.split(mp, d)
    boot = open(os.path.join(d, "wiphone-boot.bin"), "rb").read()
    app = open(os.path.join(d, "wiphone-app.bin"), "rb").read()
    check(boot == img[0x1000:0x9000] and app == img[0xE000:], "parts are the image's own slices")
    rejoined = b"\xff" * 0x1000 + boot + b"\xff" * (0xE000 - 0x9000) + app
    check(rejoined == img, "rejoined with 0xFF over nvs gives the image back")
    last = out.getvalue().strip().splitlines()[-1]
    check(last == '[{"path": "wiphone-boot.bin", "offset": 4096}, {"path": "wiphone-app.bin", "offset": 57344}]',
          "manifest line %r" % last)

# 2. the cuts follow the table
moved = [(1, 0x02, 0x9000, 0x6000), (1, 0x00, 0xF000, 0x2000), (0, 0x10, 0x11000, 0x3000)]
parts, nvs = ws.plan(image(moved))
check(nvs == (0x9000, 0xF000) and parts[0][2] == 0x9000 and parts[1][1] == 0xF000,
      "a bigger nvs moves both cuts %r %r" % (nvs, parts))
narrow = [(1, 0x02, 0xA000, 0x2000), (1, 0x00, 0xE000, 0x2000), (0, 0x10, 0x10000, 0x4000)]
parts, nvs = ws.plan(image(narrow))
check(parts[0][2] == 0xA000 and parts[1][1] == 0xE000, "a later nvs start moves the boot cut %r" % (parts,))


def refuses(entries, total=0x14000, why=""):
    try:
        ws.plan(image(entries, total))
    except ws.SplitError:
        return True
    print("  FAIL: not refused:", why)
    return False


# 3. refusals
check(refuses([(1, 0x00, 0xE000, 0x2000), (0, 0x10, 0x10000, 0x4000)], why="no nvs"), "no nvs")
check(refuses([(1, 0x02, 0x9000, 0x5000), (0, 0x10, 0x10000, 0x4000)], why="no otadata"), "no otadata")
check(refuses([(1, 0x02, 0x10000, 0x1000), (1, 0x00, 0xE000, 0x2000)], why="nvs after otadata"),
      "nvs after otadata")
check(refuses([(1, 0x02, 0x8800, 0x1000), (1, 0x00, 0xE000, 0x2000)], why="nvs inside the table"),
      "nvs inside the table")
check(refuses(STD, total=0xC000, why="image ends before otadata"), "image ends before otadata")
check(refuses([], why="empty table"), "empty table")

# 4. the page's guard
page = open(os.path.join(ROOT, "webflasher", "index.html")).read()
check("for (const part of manifest.builds[0].parts)" in page, "the page loops over every part")
check("builds[0].parts[0]" not in page, "the page does not flash parts[0] alone")
check(re.search(r"f\.address < nvs\[1\] && f\.address \+ f\.data\.length > nvs\[0\]", page),
      "the page refuses a part that covers nvs")
check("fileArray: files" in page, "the page hands every part to writeFlash")
check('return [0x9000, 0xe000];' in page, "the page falls back to the standard nvs extent")

# 5. the scripts
mk = open(os.path.join(ROOT, "tools", "make_webflasher.sh")).read()
check("tools/webflasher_split.py" in mk, "make_webflasher.sh stages through webflasher_split.py")
check("wiphone-merged.bin\"" not in mk.split("PY")[0].split("merge_bin")[1].split("\n")[0] if "merge_bin" in mk else True,
      "merge_bin writes a temp file")
pub = open(os.path.join(ROOT, "tools", "publish_webflasher.sh")).read()
check("wiphone-boot.bin" in pub and "wiphone-app.bin" in pub, "publish copies both parts")
check("cp " not in "".join(l for l in pub.splitlines() if "wiphone-merged.bin" in l),
      "publish never copies a merged image")

# 6. the page's library: served from here, and every method it calls exists in it
WEB = os.path.join(ROOT, "webflasher")


def library_problems(page_text):
    """The page's esptool-js import and calls, checked against the vendored bundle."""
    out = []
    m = re.search(r'import\s*\{[^}]*\}\s*from\s*"([^"]+)"', page_text)
    if not m:
        return ["the page has no `import {...} from \"...\"`"]
    src = m.group(1)
    if not src.startswith("./") or "://" in src:
        out.append("the page imports esptool-js from %r, not from this site" % src)
        return out
    path = os.path.join(WEB, src[2:])
    if not os.path.isfile(path):
        out.append("the page imports %r, which is not in webflasher/" % src)
        return out
    bundle = open(path, encoding="utf-8", errors="replace").read()
    # calls in CODE: the page's comments explain the bug by naming `loader.after()`
    code = re.sub(r"/\*.*?\*/", " ", page_text, flags=re.S)
    code = re.sub(r"(?<![:\"'])//[^\n]*", " ", code)          # line comments, not the // of a URL
    for obj, name in sorted(set(re.findall(r"\b(loader|transport)\.([A-Za-z_]\w*)\s*\(", code))):
        if not re.search(r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", bundle):
            out.append("the page calls %s.%s(), which the served bundle does not define" % (obj, name))
    return out


probs = library_problems(page)
for p in probs:
    print("  FAIL:", p)
fails += len(probs)
check("esptool-js-" in "".join(os.listdir(WEB)), "a vendored esptool-js bundle sits in webflasher/")
check("esptool-js-*.bundle.js" in pub, "publish copies the vendored bundle")
check("unpkg.com" not in page and "cdn." not in page, "no CDN in the page")
# the self-test: the 0.9.79-NH shape, and a method the bundle really has
bad = page.replace("loader.hardReset(", "loader.after(", 1)
check(bad != page and any("loader.after()" in p for p in library_problems(bad)),
      "self-test: a page calling loader.after() trips")
check(not library_problems(page.replace("loader.hardReset(", "loader.eraseFlash(", 1)),
      "self-test: a method the bundle defines passes")
check(any("not from this site" in p for p in library_problems(
    page.replace('from "./esptool-js', 'from "https://unpkg.com/esptool-js', 1))),
      "self-test: a CDN import trips")

if fails:
    print("check_webflasher_split: %d FAILED" % fails)
    sys.exit(1)
print("check_webflasher_split: ok")

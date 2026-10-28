#!/usr/bin/env python3
"""Test vectors for reading-position sync over KOSync (KOReader's sync protocol).

ONE generator, three consumers: the WiPhone host suite (tests/vectors_kosync.h), COVEY's suite
(tests/vectors_kosync.json, copied into covey-ui/tests/fixtures) and the CrossPoint fork's native
tests (the same header). Every device that computes a KOSync document id or a KOSync percentage
must agree to the byte with these numbers, or a position lands in the wrong book or the wrong
chapter with nothing logged anywhere.

What is pinned, and where each rule comes from:

  * The document id is KOReader's partial MD5 (frontend/util.lua util.partialMD5; CrossPoint
    lib/KOReaderSync/KOReaderDocumentId.cpp): 1024-byte chunks read at offset 0 and then at
    1024 << (2*i) for i = 0..10 (1 KiB, 4 KiB, 16 KiB, 64 KiB, 256 KiB, 1 MiB, 4 MiB, 16 MiB,
    64 MiB, 256 MiB, 1 GiB); a chunk that starts at or past the end is skipped, a short last one
    is hashed as it is; the id is the MD5 hex, lower case. CrossPoint's other method ("filename",
    its DEFAULT) is the MD5 of the file's base name. Both are emitted.

  * The percentage is CrossPoint's, because it is the device that cannot be taught another one
    without a fork, and a stock KOReader/CrossPoint client lands by it (ProgressMapper's fallback
    when `progress` is not an XPath). It is BYTE-weighted over CrossPoint's spine, which is NOT
    the reading order the WiPhone and COVEY use:
      - every <itemref> whose idref names a manifest <item> is in it, in document order —
        linear="no" items and non-HTML items included (ContentOpfParser.cpp reads only `idref`);
      - an idref that names no manifest item is dropped;
      - an item's path is normalisePath(decodeUriEscapes(opfDir + href)) (FsHelpers.cpp): %XX
        decoded, split on '/', empty components dropped, '..' pops, '.' is KEPT as a component;
      - its size is the zip central directory's UNCOMPRESSED size for that exact name, or 0 when
        the name is not in the zip (BookMetadataCache.cpp buildBookBin);
      - cum[i] = sizes[0] + ... + sizes[i]; total = cum[-1];
      - percentage = (cum[c-1] + within * size[c]) / total for CrossPoint spine item c.
    A reading-order chapter maps to the FIRST CrossPoint item with the same path. Inverse: target =
    p * total; c = first i with cum[i] >= target (so an exact boundary picks the earlier item);
    within = (target - cum[c-1]) / size[c] (0 for a zero-size item); an item that is not in the
    reading order resolves to the next reading-order chapter at within 0 (or the last one at
    within 1 when none follows).
    `within` itself is the device's own "how far through this chapter": CrossPoint uses its page
    fraction, the WiPhone byte offset / extracted length, COVEY character offset / extracted
    length. The chapter part is exact on every device; the within part is approximate by nature.

  * Percentages go on the wire as JSON numbers with at most 6 decimals, trailing zeros stripped
    ("0", "1", "0.5", "0.231"); a reader must accept any JSON number, exponent form included.

Usage:
    python3 tools/gen_kosync_vectors.py --fixtures tests/fixtures \
        --out-h tests/vectors_kosync.h --out-json tests/vectors_kosync.json
"""
import argparse
import hashlib
import json
import os
import re
import zipfile
import xml.etree.ElementTree as ET

FIXED_DATE = (2026, 9, 24, 12, 0, 0)


def partial_md5(path):
    m = hashlib.md5()
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        for i in range(-1, 11):
            off = 0 if i < 0 else 1024 << (2 * i)
            if off >= size:
                break
            f.seek(off)
            m.update(f.read(1024))
    return m.hexdigest()


def filename_md5(path):
    return hashlib.md5(os.path.basename(path).encode("utf-8")).hexdigest()


def decode_uri_escapes(s):
    """FsHelpers::decodeUriEscapes: %XX (either case) to the byte; anything else literally."""
    b = s.encode("utf-8")
    out = bytearray()
    i = 0
    hexd = b"0123456789abcdefABCDEF"
    while i < len(b):
        if b[i] == 0x25 and i + 2 < len(b) and b[i + 1] in hexd and b[i + 2] in hexd:
            out.append(int(b[i + 1:i + 3], 16))
            i += 3
            continue
        out.append(b[i])
        i += 1
    return out.decode("utf-8", "surrogateescape")


def normalise_path(p):
    """FsHelpers::normalisePath: empty components dropped, '..' pops, '.' KEPT, no leading '/'."""
    comps = []
    for c in p.split("/"):
        if not c:
            continue
        if c == "..":
            if comps:
                comps.pop()
        else:
            comps.append(c)
    return "/".join(comps)


def strip_ns(tag):
    return tag.split("}", 1)[-1]


def read_opf(zf):
    cont = ET.fromstring(zf.read("META-INF/container.xml"))
    opf_name = None
    for el in cont.iter():
        if strip_ns(el.tag) == "rootfile" and el.get("full-path"):
            opf_name = el.get("full-path")
            break
    base = os.path.dirname(opf_name)
    opf = ET.fromstring(zf.read(opf_name))
    manifest, mtypes, itemrefs = {}, {}, []
    for el in opf.iter():
        t = strip_ns(el.tag)
        if t == "item" and el.get("id") and el.get("href"):
            manifest[el.get("id")] = el.get("href")
            mtypes[el.get("id")] = (el.get("media-type") or "").lower()
        elif t == "itemref" and el.get("idref"):
            itemrefs.append((el.get("idref"), (el.get("linear") or "yes").lower()))
    return base, manifest, mtypes, itemrefs


def crosspoint_spine(zf):
    base, manifest, mtypes, itemrefs = read_opf(zf)
    sizes = {i.filename: i.file_size for i in zf.infolist()}
    out = []
    for idref, _linear in itemrefs:
        href = manifest.get(idref)
        if href is None:
            continue
        joined = (base + "/" + href) if base else href
        path = normalise_path(decode_uri_escapes(joined))
        out.append((path, sizes.get(path, 0)))
    return out


def reading_spine(zf):
    """The WiPhone's and COVEY's reading order: linear!=no, present in the zip, HTML media type.
    (Paths as the zip names them — neither device decodes %XX; every fixture gives every item a
    media type, where the two devices' rules would otherwise differ.)"""
    base, manifest, mtypes, itemrefs = read_opf(zf)
    names = set(zf.namelist())
    out = []
    for idref, linear in itemrefs:
        if linear == "no" or idref not in manifest:
            continue
        href = manifest[idref].split("#", 1)[0]
        name = os.path.normpath(os.path.join(base, href)).replace("\\", "/") if base else href
        if name not in names:
            continue
        if mtypes.get(idref) and "html" not in mtypes[idref]:
            continue
        out.append(name)
    return out


def cum_of(cp):
    cum, s = [], 0
    for _p, n in cp:
        s += n
        cum.append(s)
    return cum


def pct_for(cp, cum, reading, r, within):
    total = cum[-1] if cum else 0
    if total == 0:
        return 0.0
    name = reading[r]
    c = next(i for i, (p, _n) in enumerate(cp) if p == name)
    prev = cum[c - 1] if c > 0 else 0
    return (prev + within * cp[c][1]) / total


def locate(cp, cum, reading, p):
    """p -> (crosspoint index, within, reading index, reading within)."""
    total = cum[-1]
    target = p * total
    c = next((i for i, v in enumerate(cum) if v >= target), len(cum) - 1)
    prev = cum[c - 1] if c > 0 else 0
    size = cp[c][1]
    within = 0.0 if size == 0 else min(1.0, max(0.0, (target - prev) / size))
    name = cp[c][0]
    if name in reading:
        return c, within, reading.index(name), within
    for j in range(c + 1, len(cp)):
        if cp[j][0] in reading:
            return c, within, reading.index(cp[j][0]), 0.0
    return c, within, len(reading) - 1, 1.0


def fmt_pct(p):
    s = "%.6f" % p
    s = s.rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


# ---------------------------------------------------------------------------------- fixtures
CONTAINER = ('<?xml version="1.0"?>\n'
             '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
             '<rootfiles><rootfile full-path="%s" media-type="application/oebps-package+xml"/>'
             '</rootfiles></container>')


def xhtml(title, body_bytes):
    return ('<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml">'
            '<head><title>%s</title></head><body><h1>%s</h1>%s</body></html>'
            % (title, title, body_bytes))


def filler(words, seed):
    """Deterministic prose, `words` words long."""
    vocab = ("the ridge line ran north past the creek where the elk bedded down in the timber "
             "and the wind came off the snow field cold enough to sting").split()
    out, x = [], seed
    for i in range(words):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append(vocab[x % len(vocab)])
        if i % 12 == 11:
            out.append(".</p><p>")
    return "<p>" + " ".join(out) + ".</p>"


def zput(z, name, data, compress=zipfile.ZIP_DEFLATED):
    zi = zipfile.ZipInfo(name, FIXED_DATE)
    zi.compress_type = compress
    zi.external_attr = 0o644 << 16
    z.writestr(zi, data)


def build_mixed(path):
    """The hard case, on purpose: a linear="no" cover page, a 40-byte front item, an IMAGE in the
    spine (CrossPoint counts it; nobody reads it), a %-escaped href whose decoded file exists (the
    WiPhone and COVEY cannot find it — CrossPoint can), a spine item missing from the zip (0 bytes
    on CrossPoint), an idref that names nothing, and a >64 KiB chapter so the partial MD5 reads
    past its 64 KiB offset. Unequal chapters make spine-equal and byte-weighted disagree loudly."""
    opf = ('<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0">'
           '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Timber Line</dc:title>'
           '<dc:creator>A. Hunter</dc:creator><dc:identifier>urn:uuid:kosync-mixed-0001</dc:identifier>'
           '</metadata><manifest>'
           '<item id="cover" href="Text/cover.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="front" href="Text/front.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="map" href="Images/map.jpg" media-type="image/jpeg"/>'
           '<item id="c1" href="Text/one.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="c2" href="Text/chapter%202.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="gone" href="Text/missing.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="c3" href="Text/three.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="c4" href="Text/four.xhtml" media-type="application/xhtml+xml"/>'
           '</manifest><spine>'
           '<itemref idref="cover" linear="no"/><itemref idref="front"/><itemref idref="map"/>'
           '<itemref idref="c1"/><itemref idref="c2"/><itemref idref="gone"/>'
           '<itemref idref="nosuchid"/><itemref idref="c3"/><itemref idref="c4"/>'
           '</spine></package>')
    front = '<?xml version="1.0"?><html><body>Dedication</body></html>'
    front = front[:40].ljust(40)
    with zipfile.ZipFile(path, "w") as z:
        zput(z, "mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        zput(z, "META-INF/container.xml", CONTAINER % "OEBPS/content.opf")
        zput(z, "OEBPS/content.opf", opf)
        zput(z, "OEBPS/Text/cover.xhtml", xhtml("Cover", "<p>Cover.</p>"))
        zput(z, "OEBPS/Text/front.xhtml", front)
        # STORED and large on purpose: the file itself must pass 256 KiB so the partial MD5 reads
        # its 64 KiB and 256 KiB chunks from real content, not from a short file.
        img = bytearray(b"\xff\xd8\xff\xe0")
        seed = b"map"
        while len(img) < 200000:
            seed = hashlib.sha256(seed).digest()
            img += seed
        zput(z, "OEBPS/Images/map.jpg", bytes(img[:200000]) + b"\xff\xd9", zipfile.ZIP_STORED)
        zput(z, "OEBPS/Text/one.xhtml", xhtml("One", filler(450, 1)))
        zput(z, "OEBPS/Text/chapter 2.xhtml", xhtml("Two", filler(900, 2)))
        zput(z, "OEBPS/Text/three.xhtml", xhtml("Three", filler(15000, 3)), zipfile.ZIP_STORED)
        zput(z, "OEBPS/Text/four.xhtml", xhtml("Four", filler(120, 4)))


def build_plain(path):
    """A well-behaved book (every itemref linear, every file present): the common case, where
    the CrossPoint spine and the reading order coincide."""
    opf = ('<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="2.0">'
           '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Plain</dc:title>'
           '</metadata><manifest>'
           '<item id="a" href="a.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="b" href="b.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="c" href="c.xhtml" media-type="application/xhtml+xml"/>'
           '</manifest><spine><itemref idref="a"/><itemref idref="b"/><itemref idref="c"/>'
           '</spine></package>')
    with zipfile.ZipFile(path, "w") as z:
        zput(z, "mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        zput(z, "META-INF/container.xml", CONTAINER % "content.opf")
        zput(z, "content.opf", opf)
        zput(z, "a.xhtml", xhtml("A", filler(60, 5)))
        zput(z, "b.xhtml", xhtml("B", filler(600, 6)))
        zput(z, "c.xhtml", xhtml("C", filler(200, 7)))


WITHINS = (0.0, 0.25, 0.5, 0.999)


def book_vectors(path):
    with zipfile.ZipFile(path) as zf:
        cp = crosspoint_spine(zf)
        rd = reading_spine(zf)
    cum = cum_of(cp)
    fwd = []
    for r in range(len(rd)):
        for w in WITHINS:
            fwd.append({"r": r, "within": w, "pct": pct_for(cp, cum, rd, r, w)})
    total = cum[-1]
    ps = [0.0, 1e-6, 0.5, 1.0]
    for i, v in enumerate(cum):                       # every boundary, exactly and either side
        for d in (0.0, -1.0, 1.0):
            t = v + d
            if 0 <= t <= total:
                ps.append(t / total)
    inv = []
    seen = set()
    for p in ps:
        key = round(p, 12)
        if key in seen:
            continue
        seen.add(key)
        c, w, r, rw = locate(cp, cum, rd, p)
        inv.append({"p": p, "cp": c, "within": w, "r": r, "rwithin": rw})
    return {
        "file": os.path.basename(path),
        "size": os.path.getsize(path),
        "partial_md5": partial_md5(path),
        "filename_md5": filename_md5(path),
        "crosspoint_spine": [{"path": p, "size": n} for p, n in cp],
        "cum": cum,
        "total": total,
        "reading": rd,
        "reading_to_cp": [next(i for i, (p, _n) in enumerate(cp) if p == name) for name in rd],
        "forward": fwd,
        "inverse": inv,
    }


def txt_vectors(path):
    return {"file": os.path.basename(path), "size": os.path.getsize(path),
            "partial_md5": partial_md5(path), "filename_md5": filename_md5(path)}


MD5_RFC1321 = ["", "a", "abc", "message digest", "abcdefghijklmnopqrstuvwxyz",
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
               "1234567890" * 8]

PCT_FORMAT = [0.0, 1.0, 0.5, 0.231, 0.1234565, 0.9999996, 1e-6, 4e-7, 0.25]

# Response bodies a client must read, in the orders real servers use (the reference server,
# crosspoint-sync, and a minimal one), plus an unknown document (the reference answers 200 {}).
PARSE_BODIES = [
    ('{"document":"d41d8cd98f00b204e9800998ecf8427e","percentage":0.5,"progress":"",'
     '"device":"COVEY","device_id":"covey-1","timestamp":1790000000}', 0.5, "COVEY", 1790000000),
    ('{"timestamp":1790000001,"device_id":"crosspoint-reader","progress":"/body/DocFragment[8]/body",'
     '"percentage":0.231,"device":"CrossPoint","document":"x"}', 0.231, "CrossPoint", 1790000001),
    ('{"percentage": 6.0E-4, "device": "KOReader", "progress": "12", "timestamp": 5}',
     0.0006, "KOReader", 5),
    ('{"percentage":1,"device":"WiPhone-{user}","timestamp":7}', 1.0, "WiPhone-{user}", 7),
    ('{}', None, None, None),
]


def c_str(s):
    out = []
    for b in (s or "").encode("utf-8"):
        ch = chr(b)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif 0x20 <= b < 0x7F:
            out.append(ch)
        else:
            out.append('\\x%02x""' % b)
    return '"' + "".join(out) + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--out-h", required=True)
    ap.add_argument("--out-json", required=True)
    a = ap.parse_args()
    mixed = os.path.join(a.fixtures, "kosync-mixed.epub")
    plain = os.path.join(a.fixtures, "kosync-plain.epub")
    build_mixed(mixed)
    build_plain(plain)
    books = [book_vectors(mixed), book_vectors(plain)]
    for extra in ("epub3-nav.epub", "epub2-subdir.epub"):
        p = os.path.join(a.fixtures, extra)
        if os.path.exists(p):
            books.append(book_vectors(p))
    txt = [txt_vectors(os.path.join(a.fixtures, n)) for n in ("my-notes.txt",)
           if os.path.exists(os.path.join(a.fixtures, n))]
    data = {
        "md5": [{"in": s, "hex": hashlib.md5(s.encode()).hexdigest()} for s in MD5_RFC1321],
        "pct_format": [{"p": p, "text": fmt_pct(p)} for p in PCT_FORMAT],
        "parse": [{"body": b, "pct": p, "device": d, "timestamp": t} for b, p, d, t in PARSE_BODIES],
        "books": books,
        "txt": txt,
    }
    with open(a.out_json, "w") as f:
        json.dump(data, f, indent=1)
        f.write("\n")

    L = []
    w = L.append
    w("// GENERATED by tools/gen_kosync_vectors.py - do not edit. See that file for the rules.")
    w("#pragma once")
    w("#include <stdint.h>")
    w("typedef struct { const char* in; const char* hex; } KsMd5Vec;")
    w("static const KsMd5Vec KS_MD5[] = {")
    for v in data["md5"]:
        w("  { %s, %s }," % (c_str(v["in"]), c_str(v["hex"])))
    w("};")
    w("typedef struct { double p; const char* text; } KsFmtVec;")
    w("static const KsFmtVec KS_FMT[] = {")
    for v in data["pct_format"]:
        w("  { %.17g, %s }," % (v["p"], c_str(v["text"])))
    w("};")
    w("typedef struct { const char* body; int has; double pct; const char* device; long long ts; } KsParseVec;")
    w("static const KsParseVec KS_PARSE[] = {")
    for v in data["parse"]:
        if v["pct"] is None:
            w("  { %s, 0, 0, \"\", 0 }," % c_str(v["body"]))
        else:
            w("  { %s, 1, %.17g, %s, %dLL }," % (c_str(v["body"]), v["pct"], c_str(v["device"]), v["timestamp"]))
    w("};")
    w("typedef struct { const char* path; uint32_t size; } KsCpItem;")
    w("typedef struct { int r; double within; double pct; } KsFwd;")
    w("typedef struct { double p; int cp; double within; int r; double rwithin; } KsInv;")
    w("typedef struct {")
    w("  const char* file; uint32_t size; const char* partialMd5; const char* filenameMd5;")
    w("  const KsCpItem* cp; int nCp; const uint32_t* cum; uint32_t total;")
    w("  const char* const* reading; int nReading; const int* readingToCp;")
    w("  const KsFwd* fwd; int nFwd; const KsInv* inv; int nInv;")
    w("} KsBook;")
    for k, b in enumerate(books):
        w("static const KsCpItem KS_B%d_CP[] = {" % k)
        for it in b["crosspoint_spine"]:
            w("  { %s, %du }," % (c_str(it["path"]), it["size"]))
        w("};")
        w("static const uint32_t KS_B%d_CUM[] = { %s };" % (k, ", ".join("%du" % c for c in b["cum"])))
        w("static const char* const KS_B%d_RD[] = { %s };" % (k, ", ".join(c_str(n) for n in b["reading"])))
        w("static const int KS_B%d_R2C[] = { %s };" % (k, ", ".join(str(i) for i in b["reading_to_cp"])))
        w("static const KsFwd KS_B%d_FWD[] = {" % k)
        for v in b["forward"]:
            w("  { %d, %.17g, %.17g }," % (v["r"], v["within"], v["pct"]))
        w("};")
        w("static const KsInv KS_B%d_INV[] = {" % k)
        for v in b["inverse"]:
            w("  { %.17g, %d, %.17g, %d, %.17g }," % (v["p"], v["cp"], v["within"], v["r"], v["rwithin"]))
        w("};")
    w("static const KsBook KS_BOOKS[] = {")
    for k, b in enumerate(books):
        w("  { %s, %du, %s, %s, KS_B%d_CP, %d, KS_B%d_CUM, %du, KS_B%d_RD, %d, KS_B%d_R2C,"
          % (c_str(b["file"]), b["size"], c_str(b["partial_md5"]), c_str(b["filename_md5"]),
             k, len(b["crosspoint_spine"]), k, b["total"], k, len(b["reading"]), k))
        w("    KS_B%d_FWD, %d, KS_B%d_INV, %d }," % (k, len(b["forward"]), k, len(b["inverse"])))
    w("};")
    w("typedef struct { const char* file; uint32_t size; const char* partialMd5; const char* filenameMd5; } KsTxt;")
    w("static const KsTxt KS_TXT[] = {")
    for t in txt:
        w("  { %s, %du, %s, %s }," % (c_str(t["file"]), t["size"], c_str(t["partial_md5"]), c_str(t["filename_md5"])))
    w("};")
    with open(a.out_h, "w") as f:
        f.write("\n".join(L) + "\n")
    for b in books:
        print("%-20s %7d B  partial-md5 %s  cp items %d, reading %d, total %d"
              % (b["file"], b["size"], b["partial_md5"], len(b["crosspoint_spine"]), len(b["reading"]), b["total"]))


if __name__ == "__main__":
    main()

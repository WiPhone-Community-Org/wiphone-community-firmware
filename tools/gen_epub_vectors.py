#!/usr/bin/env python3
"""Build EPUB fixtures and generate C test vectors by running COVEY's OWN epub.py.

Same approach as gen_booksync_vectors.py: the expected values are produced by the real
implementation, so the C parser is measured against COVEY rather than against my reading of
COVEY. The fixtures themselves are written to tests/fixtures/ and opened by the C tests, so
both sides parse the identical bytes.

The fixture set deliberately includes the failure shapes COVEY's own test_epub.py pins:
an OPF in a subdirectory (the href trap — resolve against the zip root instead of the OPF's
directory and you get a book with zero chapters and NO error), and a manifest with no usable
spine (which must still open, via the every-XHTML-document fallback).

Usage:
    python3 tools/gen_epub_vectors.py --epub /path/to/covey_ui/epub.py \
        --fixtures tests/fixtures --out tests/vectors_epub.h
"""
import argparse
import importlib.util
import os
import sys
import zipfile


def load_mod(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def c_str(s):
    out = []
    for b in (s or "").encode("utf-8"):
        ch = chr(b)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif 0x20 <= b < 0x7F:
            out.append(ch)
        else:
            out.append('\\x%02x""' % b)
    return '"' + "".join(out) + '"'


CONTAINER = ('<?xml version="1.0"?>\n'
             '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
             '<rootfiles><rootfile full-path="%s" media-type="application/oebps-package+xml"/>'
             '</rootfiles></container>')


def chapter(title, paras, extra=""):
    body = "".join("<p>%s</p>" % p for p in paras)
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>%s</title>'
            '<style>p{color:red}</style></head><body><h1>%s</h1>%s%s'
            '<script>var x = "not reading text";</script></body></html>'
            % (title, title, extra, body))


def tiny_jpeg(w, h):
    """A structurally valid JPEG header with known dimensions.

    Only epubImageSize() reads this, and it walks marker segments to the SOF — so an APP0
    segment sits in front of the SOF deliberately, to prove the walk skips it rather than
    assuming the frame header comes first.
    """
    return (b"\xff\xd8"                                          # SOI
            b"\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"   # APP0
            + b"\xff\xc0\x00\x11\x08" + bytes([h >> 8, h & 0xFF, w >> 8, w & 0xFF])
            + b"\x03\x01\x11\x00\x02\x11\x01\x03\x11\x01"        # SOF0 components
            + b"\xff\xd9")                                       # EOI


def build_epub3_nav(path):
    """OPF at the zip root, EPUB3 nav document, full dc metadata."""
    opf = ('<?xml version="1.0"?>\n'
           '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="pid">'
           '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
           '<dc:identifier id="pid">urn:uuid:12345678-ABCD</dc:identifier>'
           '<dc:title>The   Long &amp; Winding Road</dc:title>'
           '<dc:creator>Ada Lovelace</dc:creator>'
           '</metadata>'
           '<manifest>'
           '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
           '<item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="c2" href="ch2.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="cover" href="cover.jpg" media-type="image/jpeg"/>'
           '</manifest>'
           '<spine><itemref idref="c1"/><itemref idref="c2"/>'
           '<itemref idref="cover" linear="no"/></spine>'
           '</package>')
    nav = ('<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><body><nav>'
           '<ol><li><a href="ch1.xhtml">Opening</a></li>'
           '<li><a href="ch2.xhtml">The Middle</a></li></ol></nav></body></html>')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER % "content.opf")
        z.writestr("content.opf", opf)
        z.writestr("nav.xhtml", nav)
        z.writestr("ch1.xhtml", chapter("Opening",
                                        ["It was a bright cold day.",
                                         "The clocks were striking &amp; thirteen."]))
        z.writestr("ch2.xhtml", chapter("The Middle",
                                        ["Second chapter here.",
                                         "With   collapsing\n\n  whitespace."]))
        z.writestr("cover.jpg", b"\xff\xd8\xff\xe0not-really-a-jpeg")


def build_epub2_subdir(path):
    """⚠ THE HREF TRAP: the OPF lives in OEBPS/, so every manifest href resolves against
    OEBPS/ and not against the zip root."""
    opf = ('<?xml version="1.0"?>\n'
           '<package xmlns="http://www.idpf.org/2007/opf" version="2.0">'
           '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
           '<dc:title>Deep In A Folder</dc:title>'
           '<dc:creator>Grace Hopper</dc:creator>'
           '<dc:identifier>isbn:9780000000001</dc:identifier>'
           '</metadata>'
           '<manifest>'
           '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
           '<item id="a" href="text/one.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="b" href="text/two.xhtml" media-type="application/xhtml+xml"/>'
           '</manifest>'
           '<spine toc="ncx"><itemref idref="a"/><itemref idref="b"/></spine>'
           '</package>')
    ncx = ('<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap>'
           '<navPoint><navLabel><text>Chapter One</text></navLabel>'
           '<content src="text/one.xhtml"/></navPoint>'
           '<navPoint><navLabel><text>Chapter Two</text></navLabel>'
           '<content src="text/two.xhtml"/></navPoint></navMap></ncx>')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("META-INF/container.xml", CONTAINER % "OEBPS/content.opf")
        z.writestr("OEBPS/content.opf", opf)
        z.writestr("OEBPS/toc.ncx", ncx)
        # An <img> whose src is relative to the CHAPTER's directory (OEBPS/text/), not the
        # OPF's and not the root — the same trap as the manifest hrefs, one level deeper.
        # It must also add exactly nothing to the extracted text, which is what the
        # regenerated-from-COVEY vectors below prove.
        z.writestr("OEBPS/img/pic.jpg", tiny_jpeg(40, 30))
        z.writestr("OEBPS/text/one.xhtml",
                   chapter("One", ["Down in a subdirectory."], '<img src="../img/pic.jpg"/>'))
        z.writestr("OEBPS/text/two.xhtml", chapter("Two", ["And the second one too."]))


def build_empty_spine(path):
    """A manifest with no usable spine — must still open via the fallback."""
    opf = ('<?xml version="1.0"?>\n'
           '<package xmlns="http://www.idpf.org/2007/opf" version="2.0">'
           '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
           '<dc:title>No Spine At All</dc:title></metadata>'
           '<manifest><item id="a" href="only.xhtml" media-type="application/xhtml+xml"/>'
           '</manifest><spine></spine></package>')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("META-INF/container.xml", CONTAINER % "content.opf")
        z.writestr("content.opf", opf)
        z.writestr("only.xhtml", chapter("Only", ["The one and only chapter."]))


def build_no_metadata(path):
    """No dc:identifier and no dc:title: the first id becomes ta: from the FILENAME stem,
    which is the one case where the user's 'same filename' instinct is literally correct."""
    opf = ('<?xml version="1.0"?>\n'
           '<package xmlns="http://www.idpf.org/2007/opf" version="2.0"><metadata/>'
           '<manifest><item id="a" href="c.xhtml" media-type="application/xhtml+xml"/>'
           '</manifest><spine><itemref idref="a"/></spine></package>')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("META-INF/container.xml", CONTAINER % "content.opf")
        z.writestr("content.opf", opf)
        z.writestr("c.xhtml", chapter("C", ["Anonymous book."]))


FIXTURES = [
    ("epub3_nav",     "epub3-nav.epub",     build_epub3_nav,   False),
    ("epub2_subdir",  "epub2-subdir.epub",  build_epub2_subdir, False),
    ("empty_spine",   "empty-spine.epub",   build_empty_spine, False),
    ("no_metadata",   "no-metadata.epub",   build_no_metadata, False),
]

# Whitespace-collapsing, block breaks, dropped elements, and the entities this parser
# supports. Exotic named entities are deliberately absent — see EPUB_ENTITY_NOTE.
HTML_CASES = [
    ("simple",     "<p>Hello world</p>"),
    ("two-paras",  "<p>One</p><p>Two</p>"),
    ("collapse",   "<p>  lots   of \n\n whitespace  </p>"),
    ("heading",    "<h1>Title</h1><p>Body text</p>"),
    ("drop-script", "<p>Keep</p><script>var drop = 1;</script><p>This</p>"),
    ("drop-style", "<style>p{color:red}</style><p>Only me</p>"),
    ("nested",     "<div><p>Inner <em>emphasis</em> here</p></div>"),
    ("br",         "<p>Line one<br/>Line two</p>"),
    ("entities",   "<p>Tom &amp; Jerry &lt;3 &quot;quotes&quot;</p>"),
    ("numeric-ent", "<p>caf&#233; and &#x2014; dash</p>"),
    ("list",       "<ul><li>alpha</li><li>beta</li></ul>"),
    ("empty",      "<p></p><p>   </p><p>real</p>"),
    ("no-tags",    "just bare text"),
    ("comment",    "<p>before</p><!-- a comment --><p>after</p>"),
    ("table",      "<table><tr><td>a</td><td>b</td></tr></table>"),
    # Entity coverage. A reading position is a character offset, so an entity that decodes to
    # a different LENGTH than COVEY produces shifts every offset after it in the chapter.
    ("ent-punct",   "<p>Wait&hellip; &ldquo;really&rdquo; &mdash; yes&nbsp;indeed.</p>"),
    ("ent-accents", "<p>na&iuml;ve caf&eacute; r&eacute;sum&eacute; &Uuml;ber stra&szlig;e</p>"),
    ("ent-symbols", "<p>&copy; 2026 &middot; &dagger; &sect; &para; &frac12; &times; &pound;</p>"),
    ("ent-greek",   "<p>&alpha; &beta; &Omega; &pi; &mu;</p>"),
    ("ent-longname", "<p>&CounterClockwiseContourIntegral; and &InvisibleTimes; end</p>"),
    ("ent-unknown", "<p>not an entity: &zzznotreal; stays put</p>"),
    ("ent-dense",   "<p>&lsquo;a&rsquo;&mdash;&lsquo;b&rsquo;&hellip;&lsquo;c&rsquo;</p>"),
]

SLUG_CASES = ["Pride and Prejudice", "  Leading and trailing  ", "MiXeD CaSe 123",
              "punctuation!!!  everywhere???", "---dashes---", "",
              "a" * 80, "The Long & Winding Road|Ada Lovelace", "9780000000001"]

NORM_CASES = [("", "ch1.xhtml"), ("OEBPS", "text/one.xhtml"), ("OEBPS", "../root.xhtml"),
              ("OEBPS/text", "../images/x.png"), ("OEBPS", "./same.xhtml"),
              ("", "a/b/../c.xhtml"), ("OEBPS", "one.xhtml#frag"), ("", "with#frag.xhtml")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epub", required=True)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    ep = load_mod(args.epub, "covey_epub")
    os.makedirs(args.fixtures, exist_ok=True)

    L = []
    w = L.append
    w("/* GENERATED by tools/gen_epub_vectors.py from COVEY's epub.py — do not edit. */")
    w("#pragma once")
    w("")

    # ---- slug
    w("typedef struct { const char* in; const char* want; } EpStrVec;")
    w("static const EpStrVec EP_SLUG_VECS[] = {")
    for s in SLUG_CASES:
        w("  { %s, %s }," % (c_str(s), c_str(ep._slug(s))))
    w("};")
    w("static const int EP_SLUG_VEC_N = %d;" % len(SLUG_CASES))
    w("")

    # ---- norm
    w("typedef struct { const char* base; const char* href; const char* want; } EpNormVec;")
    w("static const EpNormVec EP_NORM_VECS[] = {")
    for base, href in NORM_CASES:
        w("  { %s, %s, %s }," % (c_str(base), c_str(href), c_str(ep._norm(base, href))))
    w("};")
    w("static const int EP_NORM_VEC_N = %d;" % len(NORM_CASES))
    w("")

    # ---- html extraction
    w("static const EpStrVec EP_HTML_VECS[] = {")
    for label, html in HTML_CASES:
        got = ep._extract_xhtml(html.encode("utf-8")).text()
        w("  /* %s */ { %s, %s }," % (label, c_str(html), c_str(got)))
    w("};")
    w("static const int EP_HTML_VEC_N = %d;" % len(HTML_CASES))
    w("")

    # ---- whole books
    w("typedef struct {")
    w("  const char* label; const char* file; const char* displayName; int isText;")
    w("  const char* title; const char* author; const char* identifier;")
    w("  const char* ids[3]; int nIds;")
    w("  const char* spine[16]; int nSpine;")
    # chapter_title() per spine item, so the WiPhone's nav/NCX reading is checked against
    # COVEY's output rather than against a second reading of the fixture.
    w("  const char* chapTitle[16];")
    w("  const char* chapter0;")
    w("  double fractionMid; int locateSpine;")
    w("} EpBookVec;")
    w("static const EpBookVec EP_BOOK_VECS[] = {")

    entries = list(FIXTURES)
    txt_path = os.path.join(args.fixtures, "my-notes.txt")
    with open(txt_path, "w") as f:
        f.write("First paragraph of the notes.\n\nSecond paragraph here.\n")
    entries.append(("plain_txt", "my-notes.txt", None, True))

    for label, fname, builder, is_text in entries:
        path = os.path.join(args.fixtures, fname)
        if builder:
            builder(path)
        book = ep.open_book(path)
        ids = book.ids()
        spine = [n for n, _t in book.spine]
        ch0 = book.chapter_text(0)
        frac = book.fraction(0, len(ch0) // 2)
        loc_sp, _loc_off = book.locate(0.5)
        w("  { %s, %s, %s, %d," % (c_str(label), c_str(fname), c_str(fname), 1 if is_text else 0))
        w("    %s, %s, %s," % (c_str(book.title), c_str(book.author), c_str(book.identifier)))
        w("    { %s }, %d," % (", ".join(c_str(i) for i in ids), len(ids)))
        w("    { %s }, %d," % (", ".join(c_str(s) for s in spine), len(spine)))
        titles = [book.chapter_title(i) for i in range(len(spine))]
        w("    { %s }," % ", ".join(c_str(t) for t in titles))
        w("    %s, %r, %d }," % (c_str(ch0), frac, loc_sp))
        book.close()
    w("};")
    w("static const int EP_BOOK_VEC_N = %d;" % len(entries))
    w("")

    with open(args.out, "w") as f:
        f.write("\n".join(L) + "\n")
    print("wrote %s: %d slug, %d norm, %d html, %d book vectors"
          % (args.out, len(SLUG_CASES), len(NORM_CASES), len(HTML_CASES), len(entries)))


if __name__ == "__main__":
    main()

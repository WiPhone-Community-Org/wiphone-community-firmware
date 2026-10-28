#!/usr/bin/env python3
"""check_music_title.py - the Now playing title is drawn only when the track changes, over cleared strips.

{user} on 0.9.79 (2026-09-26, the first listening pass): after the side button's "next", the title read
"Pulsirt Explorer" - "Pulse" painted over "Space Explorer" - with a stray letter on the line below. The
1 Hz refresh redrew the two title lines over themselves with no clear: fine while the track stayed the
same (the face is proportional, so the new glyphs cover only their own pixels), wrong the moment it
changed, and a one-line title left the second line of the two-line title before it standing. Every OTHER
line of that screen already clears its strip first (the comment above them explains why trailing spaces
do not work). drawNowPlaying() lives behind the LCD, so the host suite cannot run it; this pins the
shape, POSITIVELY, on comment/string-blanked source (tests/check_wifi_restore.py's helpers):

  1. MusicApp::drawNowPlaying() has exactly one `if (cur != titleShown)` block;
  2. that block clears BOTH title strips first: a fillRect( whose height is `2 * lh`, BEFORE the first
     guiDrawEllipsized(;
  3. every guiDrawEllipsized( of the function (the two title lines) is INSIDE that block - nothing draws
     a title over the last one;
  4. the block records what it drew (`titleShown = cur`), and the whole-panel clear (`!nowClean`) forgets
     it (`titleShown = -2`) so a blanked panel gets its title back;
  5. the constructor starts at `titleShown = -2`.

A self-test plants the three hand mutations that reproduce the bug into a scratch copy and shows each trips.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from check_wifi_restore import function_body, ifs, match_close, rx, strip_code  # noqa: E402

SRC = os.path.join(ROOT, "WiPhone", "app_music.cpp")


def problems(text):
    code = strip_code(text)
    out = []
    span = function_body(code, "MusicApp::drawNowPlaying")
    if span is None:
        return ["MusicApp::drawNowPlaying() not found - if it was renamed, update this guard"]
    bs, be = span
    blocks = ifs(code, bs, be, rx(r"cur\s*!=\s*titleShown"))
    if len(blocks) != 1:
        out.append("expected one `if (cur != titleShown)` block in drawNowPlaying, found %d" % len(blocks))
        return out
    _, kb, ke = blocks[0]
    fill = rx(r"fillRect\s*\([^;]*2\s*\*\s*lh[^;]*\)").search(code, kb, ke)
    draws_all = [m.start() for m in rx(r"guiDrawEllipsized\s*\(").finditer(code, bs, be)]
    draws_in = [p for p in draws_all if kb <= p < ke]
    if not fill:
        out.append("the title block does not clear both strips (no fillRect(..., 2 * lh, ...) inside it)")
    elif draws_in and fill.start() > min(draws_in):
        out.append("the title strips are cleared AFTER a title line is drawn")
    if len(draws_all) < 2:
        out.append("expected two guiDrawEllipsized( title lines in drawNowPlaying, found %d" % len(draws_all))
    if len(draws_in) != len(draws_all):
        out.append("a guiDrawEllipsized( title line sits OUTSIDE the `if (cur != titleShown)` block")
    if not rx(r"titleShown\s*=\s*cur\s*;").search(code, kb, ke):
        out.append("the title block does not record `titleShown = cur`")
    clears = ifs(code, bs, be, rx(r"!\s*nowClean"))
    if not clears or not any(rx(r"titleShown\s*=\s*-2\s*;").search(code, k, e) for _, k, e in clears):
        out.append("the `!nowClean` panel clear does not reset `titleShown = -2`")
    ctor = function_body(code, "MusicApp::MusicApp")
    if ctor is None or not rx(r"titleShown\s*=\s*-2\s*;").search(code, ctor[0], ctor[1]):
        out.append("MusicApp::MusicApp() does not start at `titleShown = -2`")
    return out


MUTATIONS = [
    ("no clear before the title",
     lambda t: re.sub(r"\n(\s*)lcd\.fillRect\(MUSIC_MARGIN, y, \(int\)lcd\.width\(\) - MUSIC_MARGIN, 2 \* lh, BLACK\);",
                      r"\n\1/* cleared */", t, count=1)),
    ("a title line drawn outside the block",
     lambda t: t.replace("  if (cur != titleShown) {",
                         "  guiDrawEllipsized(lcd, t->name, lcd.width(), MUSIC_MARGIN, y);\n  if (cur != titleShown) {", 1)),
    ("the panel clear forgets nothing",
     lambda t: t.replace("    titleShown = -2;                          // the panel is blank", "    // titleShown kept", 1)),
]


def main():
    text = open(SRC).read()
    real = problems(text)
    for p in real:
        print("  CONTRACT BROKEN: app_music.cpp: " + p)
    failed = bool(real)
    for what, mutate in MUTATIONS:
        mutated = mutate(text)
        if mutated == text:
            print("  SELF-TEST FAILED: the mutation '%s' did not apply - the source moved; update it" % what)
            failed = True
            continue
        if not [p for p in problems(mutated) if p not in real]:
            print("  SELF-TEST FAILED: the mutation '%s' does not trip the guard" % what)
            failed = True
    if failed:
        return 1
    print("  ok  the Now playing title is drawn only on a track change, over both strips cleared "
          "(%d hand mutations trip)" % len(MUTATIONS))
    return 0


if __name__ == "__main__":
    sys.exit(main())

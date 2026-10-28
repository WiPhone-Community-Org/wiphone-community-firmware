#!/usr/bin/env python3
"""check_menu_keys.py — no menu row may be added with a key of 0.

MenuWidget::addOption() REFUSES a key of 0: it logs and adds no row. So
`addOption(text, 0, 1)` is not "an inert row", it is NO ROW. Photos lost every message it
ever tried to show that way (0.9.18); Books, Files and Music were still doing it a day later
(0.9.19). Use MenuWidget::addNote() for a display-only row.

⚠ THIS REPLACED A ONE-LINE `git grep`, WHICH WOULD NOT HAVE CAUGHT THE BUG IT WAS WRITTEN FOR.
That pattern was `addOption\\(.*,\\s*0\\s*,` and it misses three of the four spellings:

  1. a NAMED CONSTANT — the original fault was `addOption(note, ROW_INERT, 1)` with
     `ROW_INERT = 0`. No literal zero on the line at all.
  2. a WRAPPED CALL — the `Channel 'booksync': MISSING` row had its `, 0, 1);` on a
     continuation line, and the pattern needed `addOption(` on the same line.
  3. the TWO-ARGUMENT form — `style` defaults to 1 (GUI.h), so `addOption("...", 0);` compiles
     and drops the row, and the pattern's mandatory trailing comma cannot see it.

So this joins continuations, accepts both terminators, and RESOLVES an identifier key against
enum/#define/const assignments in the same file. A guard that only catches the spelling you
already fixed is how eight sites survived a day the first time.
"""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"
SRC = sorted(list(ROOT.glob("*.cpp")) + list(ROOT.glob("*.h")) + list(ROOT.glob("*.ino")))

# IDENT = 0   (enum member, const, plain assignment)   |   #define IDENT 0
#
# ⚠ NOT anchored to the start of a line. It was, and that let the ORIGINAL bug through in
# testing: `enum { ROW_INERT = 0, ROW_ACTION = 10000 };` puts the member mid-line, and
# app_photos.cpp only matched by luck because its enum happened to be one member per line.
# An identifier only matters here if it is then passed as a key, so a loose match costs
# nothing and a tight one costs the whole point of the check.
ZERO_ENUM = re.compile(r"(\w+)\s*=\s*0\s*[,;}]")
ZERO_DEF  = re.compile(r"^\s*#\s*define\s+(\w+)\s+0\s*(?://.*)?$", re.M)
CALL      = re.compile(r"addOption\s*\(")

def key_arg(text, start):
    """Return the 2nd argument of the call whose '(' is at `start`, or None."""
    depth, arg, args = 0, [], []
    for i in range(start, min(start + 4000, len(text))):
        c = text[i]
        if c == '(':
            depth += 1
            if depth == 1:
                continue
        elif c == ')':
            depth -= 1
            if depth == 0:
                args.append("".join(arg).strip())
                break
        if depth == 1 and c == ',':
            args.append("".join(arg).strip())
            arg = []
            continue
        if depth >= 1:
            arg.append(c)
    return args[1] if len(args) >= 2 else None

def strip_comments(t):
    """Blank out // and /* */ comments, preserving line count and offsets.

    Necessary, and found the hard way: the first run of this script flagged the explanatory
    comment above MenuWidget::addNote() in GUI.h, which quotes the bad spelling in order to
    warn about it. A checker that cannot tell code from prose fails on its own documentation.
    """
    out, i, n = [], 0, len(t)
    while i < n:
        if t.startswith("//", i):
            j = t.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif t.startswith("/*", i):
            j = t.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in t[i:j])); i = j
        elif t[i] == '"':
            j = i + 1
            while j < n and t[j] != '"':
                j += 2 if t[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(t[i:j]); i = j
        else:
            out.append(t[i]); i += 1
    return "".join(out)

bad = []
for path in SRC:
    raw = path.read_text(errors="replace")
    text = strip_comments(raw)
    zeros = set(ZERO_ENUM.findall(text)) | set(ZERO_DEF.findall(text))
    for m in CALL.finditer(text):
        # skip declarations/definitions of addOption itself
        line_start = text.rfind("\n", 0, m.start()) + 1
        line = raw[line_start:raw.find("\n", m.start())]
        if "::addOption" in line or line.lstrip().startswith(("void ", "bool ", "virtual ")):
            continue
        k = key_arg(text, text.find("(", m.start()))
        if k is None:
            continue
        k = k.split("//")[0].strip()
        if k == "0" or (k.isidentifier() and k in zeros):
            bad.append((path.name, text.count("\n", 0, m.start()) + 1, k, line.strip()[:70]))

if bad:
    print("  FAIL: these rows will never appear - use MenuWidget::addNote() instead")
    for f, ln, k, src in bad:
        print("    %s:%d  key=%s   %s" % (f, ln, k, src))
    sys.exit(1)
print("  ok  no menu row is added with a key of 0 (%d sources scanned)" % len(SRC))

#!/usr/bin/env python3
"""check_positions_write.py - no sync carrier may write, or reach for, a reading position.

THE HARD RULE (docs/booksync-simple-plan.md; {user}: "we can't let it screw up the position people
are at in their own wiphones after they update"): an update or a sync never moves the place someone
is at on their own WiPhone. Every carrier - LoRa (booksync.cpp, booksync_inbox.cpp), KOSync
(kosync.cpp, kosync_sync.cpp), and whatever comes next (the WiFi broadcast on UDP 8083, ESP-NOW
"Nearby") - ends at the same sync CARD, and only the person's "Go there" on that card writes a
position: BooksApp::applyPending() (app_books.cpp), which turns the offered FRACTION into a place
with epubLocate() and commits it with savePosition(true) -> store->put(). A carrier that could call
any of those, or open positions.cbs itself, would be one bug away from moving a reader's place with
nobody pressing anything.

test_kosync's golden_positions.h proves the PARSER gives every fixture the same place as 0.9.78; it
cannot see who calls the writer. This can. The identifiers are matched on comment-and-string-blanked
source (check_wifi_restore.py's strip_code, brace and paren matching), because comments in kosync.h
and kosync_sync.h mention positions.cbs and applyPending() on purpose - a guard that trips on its
own documentation is one people learn to ignore (check_menu_keys.py's lesson). The file NAME is
looked for with comments blanked but strings KEPT: the only ways code names a file are a string
literal and the BOOKS_POS_FILE macro, and both are banned in a carrier.

  (a) none of the CARRIER files (CARRIERS below) references savePosition(, booksSaveOpenPosition(,
      applyPending(, epubLocate(, store->put( or BOOKS_POS_FILE in code, nor positions.cbs in code
      or in a string. The list is the whole path a position takes from the air to the inbox: the
      record and the inbox, the KOSync protocol, window and home client, the LoRa receive that
      parks (meshtastic_service.cpp) and the :80 pump that carries every KOSync request to the
      window (app_gbc_xfer.cpp). 🛑 A NEW CARRIER FILE (UDP 8083, ESP-NOW) GOES ON THAT LIST the
      day it is created, or this guard does not cover it - the list is the contract's reach.
  (b) in app_books.cpp: store->put( has exactly ONE call site and it is inside
      BooksApp::savePosition(); applyPending( is CALLED from exactly ONE place - the sync card's
      OK branch (`case BOOKS_SYNCCARD:` ... `if (LOGIC_BUTTON_OK(event))`) in
      BooksApp::processEvent(), and there only after the arming guard
      (`if (millis() - syncCardMs < BOOKS_SYNCCARD_ARM_MS) { return ...; }` - a press inside the
      window that raised the card is swallowed); the reader menu's "Go to X's place" row
      (`case BOOKS_MENU_PENDING:`) only ENTERS that card state (enterState(BOOKS_SYNCCARD)) and
      never calls applyPending() itself. And a synced place is written only inside applyPending():
      epubLocate( - the one call that turns an offered fraction into a spine and offset - is
      called in app_books.cpp only inside applyPending(), which commits with savePosition(true).
      What function_body can NOT say: that no other savePosition(true) writes a synced place. The
      other calls (page turns, chapter jumps, closes, undo, the OK-to-menu flush) write the
      reader's OWN place, which is what they are for; none of them can see `pending` through
      epubLocate, and that is the property pinned.
  (c) a self-test plants each banned spelling into a scratch copy of a carrier (in memory), and a
      second store->put(, a second applyPending() call, an epubLocate() outside applyPending, a
      missing arming guard and a menu row that applies directly into a scratch app_books.cpp, and
      requires the guard to trip; a mention in a comment, or an identifier inside a string, must
      NOT. 🛑 THE REAL SOURCES ARE CHECKED FIRST AND EACH PLANT IS JUDGED AS A DELTA against them
      (what the mutation ADDS, with line numbers ignored so a plant that shifts the lines below it
      moves nothing). The first version ran the self-test first, on absolute presence: a genuine
      savePosition(true) in kosync_sync.cpp made the three "comments and strings do not trip"
      cases fail, the run stopped there, and the output was three SELF-TEST FAILED lines and never
      the CONTRACT BROKEN file:line - a guard that hid the very thing it was for.

Wired into tests/run_tests.sh next to the other source guards; exit 1 fails the suite.
"""
from collections import Counter
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, ifs, strip_code  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"
BOOKS = "app_books.cpp"

# 🛑 EVERY sync carrier, header and source: the files that receive, verify, park or serve a
# position from ANOTHER device. A new carrier file - the WiFi broadcast on UDP 8083, ESP-NOW
# "Nearby" (docs/booksync-simple-plan.md 5.7, 5.13) - is added HERE the day it is created.
CARRIERS = [
    "booksync.cpp",         # the CBS1 record: pack, verify, the LoRa send
    "booksync.h",
    "booksync_inbox.cpp",   # the four-slot inbox every carrier parks into
    "booksync_inbox.h",
    "kosync.cpp",           # the KOSync protocol: routes, JSON, the window's answers, the park text
    "kosync.h",
    "kosync_sync.cpp",      # the KOSync window and home client (sockets, the radio, the card)
    "kosync_sync.h",
    "meshtastic_service.cpp",   # the LoRa receive: bookSyncIsSyncText -> bookSyncInboxPush (the park)
    "app_gbc_xfer.cpp",         # the :80 pump the window rides: every KOSync request -> kosyncWindowServe
]

# The writers and the way to them, as code (matched on strip_code text).
BANNED = [
    (re.compile(r"\bsavePosition\s*\("), "savePosition("),
    (re.compile(r"\bbooksSaveOpenPosition\s*\("), "booksSaveOpenPosition("),
    (re.compile(r"\bapplyPending\s*\("), "applyPending("),
    (re.compile(r"\bepubLocate\s*\("), "epubLocate("),
    (re.compile(r"\bstore\s*->\s*put\s*\("), "store->put("),
    (re.compile(r"\bBOOKS_POS_FILE\b"), "BOOKS_POS_FILE (the positions.cbs macro)"),
]
# The file's name, as code OR a string (matched on comment-blanked text with strings kept).
POS_FILE = re.compile(r"positions\.cbs")


def strip_comments(t):
    """Blank comments only, keeping offsets and lines. String and char literals are KEPT, but
    walked over so a `//` inside one (a URL) is not taken for a comment start."""
    out, i, n = [], 0, len(t)

    def blank(s):
        return "".join(c if c == "\n" else " " for c in s)

    while i < n:
        if t.startswith("//", i):
            j = t.find("\n", i)
            j = n if j < 0 else j
            out.append(blank(t[i:j]))
            i = j
        elif t.startswith("/*", i):
            j = t.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(blank(t[i:j]))
            i = j
        elif t.startswith('R"', i) and (i == 0 or not (t[i - 1].isalnum() or t[i - 1] == "_")):
            m = re.match(r'R"([^()\\ ]{0,16})\(', t[i:])
            if m:
                end = t.find(")" + m.group(1) + '"', i + m.end())
                j = n if end < 0 else end + len(m.group(1)) + 2
            else:
                j = i + 1
            out.append(t[i:j])
            i = j
        elif t[i] in "\"'":
            q, j = t[i], i + 1
            while j < n and t[j] != q and t[j] != "\n":
                j += 2 if t[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(t[i:j])
            i = j
        else:
            out.append(t[i])
            i += 1
    return "".join(out)


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def within(pos, span):
    return span is not None and span[0] <= pos < span[1]


# ── (a) the carriers ─────────────────────────────────────────────────────────────────────────

def check_carriers(texts):
    """texts: {name: raw source}. Every banned spelling in a carrier, as (name, line, what)."""
    bad = []
    for name in CARRIERS:
        raw = texts.get(name)
        if raw is None:
            bad.append((name, 0, "carrier file not found - if it was renamed or removed, update CARRIERS"))
            continue
        code = strip_code(raw)
        for rx, what in BANNED:
            for m in rx.finditer(code):
                bad.append((name, line_of(code, m.start()), f"references {what}"))
        for m in POS_FILE.finditer(strip_comments(raw)):
            bad.append((name, line_of(raw, m.start()), "names positions.cbs (in code or a string)"))
    return bad


# ── (b) the one writer in app_books.cpp ──────────────────────────────────────────────────────

CALL = re.compile(r"(?<![\w:.>])applyPending\s*\(")           # a call, not `BooksApp::applyPending(`
ARMING = re.compile(r"if\s*\(\s*millis\s*\(\s*\)\s*-\s*syncCardMs\s*<\s*BOOKS_SYNCCARD_ARM_MS\s*\)"
                    r"\s*\{[^{}]*\breturn\b[^{}]*\}")
CASE = re.compile(r"\b(?:case\s+(\w+)|default)\s*:")


def check_books(raw):
    bad = []
    code = strip_code(raw)
    sp = function_body(code, "BooksApp::savePosition")
    ap = function_body(code, "BooksApp::applyPending")
    pe = function_body(code, "BooksApp::processEvent")
    for fn, span in (("BooksApp::savePosition", sp), ("BooksApp::applyPending", ap),
                     ("BooksApp::processEvent", pe)):
        if span is None:
            bad.append(f"{fn}() not found in {BOOKS} - if it was renamed, update this contract")
    # store->put(: one call, in savePosition()
    puts = [m.start() for m in re.finditer(r"\bstore\s*->\s*put\s*\(", code)]
    if len(puts) != 1:
        bad.append(f"{BOOKS}: store->put( must have exactly ONE call site (found {len(puts)}) - the "
                   "position store is written by savePosition() and nowhere else")
    elif sp is not None and not within(puts[0], sp):
        bad.append(f"{BOOKS}:{line_of(code, puts[0])} the one store->put( is not inside "
                   "BooksApp::savePosition()")
    # applyPending(): one call, the card's OK branch, after the arming guard
    calls = [m.start() for m in CALL.finditer(code)]
    oks = ifs(code, pe[0], pe[1], re.compile(r"\bLOGIC_BUTTON_OK\s*\(\s*event\s*\)")) if pe else []
    card = [h for h in oks if any(h[1] <= c <= h[2] for c in calls)]
    if len(calls) != 1 or len(card) != 1 or not within(calls[0], pe):
        bad.append(f"{BOOKS}: applyPending() must be CALLED from exactly one place - the sync card's "
                   f"OK branch (if (LOGIC_BUTTON_OK(event))) in BooksApp::processEvent() (found "
                   f"{len(calls)} call(s)) - 'Go there' on the card is the only thing that moves a place")
    else:
        h = card[0]
        labels = [m.group(1) for m in CASE.finditer(code, pe[0], h[0])]
        if not labels or labels[-1] != "BOOKS_SYNCCARD":
            bad.append(f"{BOOKS}:{line_of(code, calls[0])} the applyPending() call's OK branch is under "
                       f"`case {labels[-1] if labels else '?'}:`, not `case BOOKS_SYNCCARD:` - only the "
                       "sync card applies a place")
        guard = ARMING.search(code, h[1], h[2])
        if guard is None or guard.end() > calls[0]:
            bad.append(f"{BOOKS}: the card's OK branch must swallow a press inside the arming window "
                       "BEFORE applyPending() - `if (millis() - syncCardMs < BOOKS_SYNCCARD_ARM_MS) "
                       "{ return ...; }` - the keystroke that raised the card used to take the place")
    # the menu row enters the card; it never applies
    m = re.search(r"\bcase\s+BOOKS_MENU_PENDING\s*:", code)
    if m is None:
        bad.append(f"{BOOKS}: `case BOOKS_MENU_PENDING:` (the reader menu's 'Go to X's place' row) not "
                   "found - update this contract")
    else:
        nxt = CASE.search(code, m.end())
        seg = code[m.end():nxt.start() if nxt else len(code)]
        if not re.search(r"\benterState\s*\(\s*BOOKS_SYNCCARD\s*\)", seg) or CALL.search(seg):
            bad.append(f"{BOOKS}:{line_of(code, m.start())} the 'Go to X's place' row must ENTER the card "
                       "(enterState(BOOKS_SYNCCARD)) and never call applyPending() itself - the person "
                       "reads the offer and presses OK on the card")
    # a synced place is located, and written, only inside applyPending()
    locs = [m.start() for m in re.finditer(r"\bepubLocate\s*\(", code)]
    if not locs:
        bad.append(f"{BOOKS}: epubLocate( not found - if applyPending() locates another way, update "
                   "this contract")
    for l in locs:
        if not within(l, ap):
            bad.append(f"{BOOKS}:{line_of(code, l)} calls epubLocate() outside BooksApp::applyPending() - "
                       "the only place an offered fraction may become a spine and offset")
    if ap is not None and not re.search(r"\bsavePosition\s*\(\s*true\s*\)", code[ap[0]:ap[1]]):
        bad.append(f"{BOOKS}: applyPending() must commit the jump with savePosition(true) - the one "
                   "write of a synced place, and the one this guard can name")
    if ap is not None and not re.search(r"\bpending\s*\.\s*fraction\b", code[ap[0]:ap[1]]):
        bad.append(f"{BOOKS}: applyPending() must locate from pending.fraction (the offered record) - "
                   "update this contract if the record moved")
    return bad


def check_all(texts):
    problems = [f"{n}:{ln} {what}" if ln else f"{n}: {what}" for n, ln, what in check_carriers(texts)]
    if BOOKS in texts:
        problems.extend(check_books(texts[BOOKS]))
    else:
        problems.append(f"{BOOKS} not found - update this contract")
    return problems


# ── (c) self-test: plant each banned spelling; comments and strings must not trip ────────────

LINE_NO = re.compile(r"^(\S+):\d+ ")


def keyed(problems):
    """The problems as a multiset without line numbers: a plant is judged by what it ADDS, and a
    line it pushes down is the same problem, not a new one."""
    return Counter(LINE_NO.sub(r"\1 ", p) for p in problems)


def selftest(texts, baseline):
    """`baseline`: check_all(texts) of the real sources, already reported. Each mutation is judged
    against it: a plant must ADD a message naming `want`; a comment or a string must add nothing."""
    ok = True
    base = keyed(baseline)

    def expect(label, mutated, want, holds=False):
        nonlocal ok
        new = list((keyed(check_all(mutated)) - base).elements())
        if holds:
            if new:
                print(f"  SELF-TEST FAILED: {label} - added '{new[0]}'")
                ok = False
        elif not any(want in g for g in new):
            print(f"  SELF-TEST FAILED: {label} - did not add '{want}'")
            ok = False

    sync = texts["kosync_sync.cpp"]
    # Planted into a scratch copy of kosync_sync.cpp, at the top of the first function body.
    at = sync.index("{\n", sync.index("static uint32_t nowUtc()")) + 2
    plants = [
        ("savePosition(true);", "references savePosition("),
        ("booksSaveOpenPosition();", "references booksSaveOpenPosition("),
        ("applyPending();", "references applyPending("),
        ("epubLocate(&book, f, &sp, &off);", "references epubLocate("),
        ("store->put(idp, n, 0, 0, 0.0, 0);", "references store->put("),
        ("SD.open(BOOKS_POS_FILE);", "references BOOKS_POS_FILE"),
        ('SD.open("/books/positions.cbs");', "names positions.cbs"),
        ("open(POS_DIR_positions.cbs);", "names positions.cbs"),
    ]
    for code, want in plants:
        expect(f"planting `{code}` in kosync_sync.cpp", dict(texts, **{"kosync_sync.cpp": sync[:at] + "  " + code + "\n" + sync[at:]}),
               want)
    PLANTED_FILES = ("booksync.cpp", "booksync_inbox.h", "meshtastic_service.cpp", "app_gbc_xfer.cpp")
    for name in PLANTED_FILES:
        src = texts[name]
        expect(f"planting savePosition( in {name}", dict(texts, **{name: src + "\nvoid x() { savePosition(true); }\n"}),
               "references savePosition(")
    for code in ("// savePosition(true); applyPending(); store->put(x); positions.cbs BOOKS_POS_FILE",
                 "/* epubLocate(&b, f, &s, &o) -> savePosition(true) */",
                 'static const char* kNote = "applyPending( savePosition( epubLocate( store->put(";'):
        expect(f"`{code[:40]}...` in kosync_sync.cpp is not code",
               dict(texts, **{"kosync_sync.cpp": sync[:at] + "  " + code + "\n" + sync[at:]}),
               "references", holds=True)

    books = texts[BOOKS]
    i = books.index("{\n", books.index("void BooksApp::applyUndo()")) + 2

    def plant_undo(code):
        return dict(texts, **{BOOKS: books[:i] + "  " + code + "\n" + books[i:]})

    expect("a second store->put( in app_books.cpp", plant_undo("store->put(idp, nIds, 0, 0, 0.0, 0);"),
           "exactly ONE call site")
    expect("a second applyPending() call in app_books.cpp", plant_undo("applyPending();"),
           "CALLED from exactly one place")
    expect("an epubLocate() outside applyPending()", plant_undo("epubLocate(&book, 0.5, &sp, &off);"),
           "outside BooksApp::applyPending()")
    j = books.index("epubLocate(&book, pending.fraction")
    k = books.index("savePosition(true);", j)
    expect("applyPending() without its savePosition(true)",
           dict(texts, **{BOOKS: books[:k] + "/* gone */" + books[k + len("savePosition(true);"):]}),
           "must commit the jump with savePosition(true)")
    g = ARMING.search(strip_code(books))
    if g is None:
        print("  SELF-TEST FAILED: the arming guard is not where this self-test looks - update it")
        ok = False
    else:
        expect("the card's OK branch without the arming guard",
               dict(texts, **{BOOKS: books[:g.start()] + " " * (g.end() - g.start()) + books[g.end():]}),
               "swallow a press inside the arming window")
    m = re.search(r"\bcase\s+BOOKS_MENU_PENDING\s*:", books)
    e = books.index("enterState(BOOKS_SYNCCARD)", m.end())
    expect("the menu row applying directly",
           dict(texts, **{BOOKS: books[:e] + "applyPending(); enterState(BOOKS_SYNCCARD)" + books[e + len("enterState(BOOKS_SYNCCARD)"):]}),
           "CALLED from exactly one place")
    expect("the menu row not entering the card",
           dict(texts, **{BOOKS: books[:e] + "enterState(BOOKS_READ)" + books[e + len("enterState(BOOKS_SYNCCARD)"):]}),
           "must ENTER the card")
    if ok:
        print(f"  ok  self-test: each of the {len(plants) + len(PLANTED_FILES)} planted spellings trips "
              "the carrier guard, comments and strings do not; a second writer, a second apply, a bare "
              "locate, a missing arming guard and a menu row that applies each trip the app_books.cpp "
              "contract")
    return ok


def main():
    texts = {}
    for name in CARRIERS + [BOOKS]:
        p = ROOT / name
        if p.exists():
            texts[name] = p.read_text(errors="replace")
    # The real sources FIRST, and their verdict printed whatever the self-test then says: a real
    # violation must come out as its file:line, never as a self-test that could not tell.
    problems = check_all(texts)
    for p in problems:
        print(f"  CONTRACT BROKEN: {p}")
    passed = selftest(texts, problems)
    if problems or not passed:
        return 1
    print(f"  ok  no carrier ({', '.join(CARRIERS)}) writes, saves, applies, locates or names a "
          "reading position")
    print("  ok  app_books.cpp: one store->put( (in savePosition), one applyPending() call (the card's OK, "
          "after the arming guard; the menu row only enters the card), epubLocate( only inside "
          "applyPending(), which commits with savePosition(true)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

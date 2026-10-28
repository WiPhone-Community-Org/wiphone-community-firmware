#!/usr/bin/env python3
"""check_kosync_glue.py - the KOSync home client never waits on a name, and a re-read of
kosync.txt nobody asked for never wipes the window's warnings (0.9.79).

test_kosync pins the PURE half of home= by name - the mDNS query and answer bytes, the
lookup/fallback/reached/gave-up rules (kosyncHomePlan and friends), and kosyncReloadClears - and a
review's 19 hand-made mutants of kosync.cpp were caught all but once (an equivalent one). But BOTH
bugs the 0.9.79 work fixed were in the GLUE, in files the host suite cannot compile:

  - kosync_sync.cpp called resolveDomain() at the start of every home job: the loop blocked
    >= 0.5 s on each book open and close, and a .local name never resolved at all;
  - the window's `!` warnings were cleared on the re-read of kosync.txt that Books makes on EVERY
    entry (app_books.cpp), i.e. on the way to the one screen that shows them.

The same review put resolveDomain(T->cfg.home) back at the start of the job and changed both
Books re-reads to kosyncReloadConfig(true) in a scratch copy: test_kosync stayed 1984/1984 and
every check_*.py guard exited 0. So each guard is a POSITIVE contract here, matched on
comment-and-string-blanked source by brace and paren matching (check_wifi_restore.py's helpers):

  1. no blocking resolver in kosync_sync.cpp: no resolveDomain(, hostByName(, gethostbyname(,
     getaddrinfo(, queryHost(, mdns_query_*(, netconn_gethostbyname(, and no tcpip_callback( or
     tcpip_api_call( (both post and WAIT on the tcpip thread's mailbox);
  2. every kosyncReloadConfig() call but ONE takes no argument (or false) - the two Books
     re-reads, the implicit one in kosyncConfigured() - and the header's default stays false;
  3. serial `kosync reload` is that one: it passes true;
  4. every recvfrom()/recv() in kosync_sync.cpp passes MSG_DONTWAIT, lookupPoll() reads its
     socket that way, dns_gethostbyname() is called only inside dnsStart(), and dnsStart() is
     only ever HANDED to tcpip_callback_with_block(dnsStart, <tag>, 0) - never called on the loop;
  5. kosyncWindowOpen()'s new-window branch (the else of `if (sameBook)`) calls
     clearWindowProblems(), and nothing else in that function does (an extended window for the
     same book keeps its warnings);
  6. and the rest of the "what clears the warnings" rule: clearWindowProblems() is called only
     from kosyncWindowOpen(), reloadDone() - there only `if (kosyncReloadClears(asked, ...))` -
     and kosyncReloadConfig() - there only `if (asked)`; kosyncReloadConfig() passes `asked`
     through to every reloadDone(); and s_other/s_unauth, and the window's PUT log (T->puts),
     are zeroed nowhere but clearWindowProblems().

The integration review's three KOSync findings were glue too (test_kosync pins their pure half:
kosyncOfferFill, kosyncParkPending/kosyncOfferAnswered, the PUT log):

  7. KS-1 - a home record is judged against the last move AS OF THE ASK: evaluateOffer() fills
     its KosyncOfferIn only through kosyncOfferFill(&in, &g, thenSend, ..., b->movedAt, ...) -
     the explicit flag is `thenSend`, the asked stamp is the job's snapshot - and nothing in
     kosync_sync.cpp assigns myMovedAt; kosyncNotePosition() never refreshes s_waitBook's stamp
     (its notePlace(s_waitBook, ...) passes 0). A page turned while the pull on open was on its
     way used to hide the X4's newer place ("older than your last page turn").
  8. KS-2 - "declined" means ANSWERED: nothing in kosync_sync.cpp assigns offeredSig (it was set
     and saved the moment a park succeeded); evaluateOffer() asks kosyncParkPending() and every
     `return false` there (= "go on and PUT ours") sits under an `if` that says `!parkLive`;
     app_books.cpp calls kosyncCardAnswered(pendingId) in applyPending() and in the
     LOGIC_BUTTON_BACK branch that retires the parks, and nowhere else calls it.
  9. KS-3 - a reader's second id is not another book: kosyncWindowServe() logs `if (o.gotPut)`
     with kosyncPutLogOwn() and `if (o.otherDoc)` with kosyncPutLogOther(), and
     kosyncProblemLine() counts the other-book PUTs with kosyncPutLogDifferent().
 10. KS-2's rule for EVERY push (the repair round): clientStep()'s KS_CONNECT block refuses a job's
     FIRST PUT - `if (!s_phaseGet && s_w->sent == 0 && parkAwaitsAnswer(...)) { finishJob(...);
     return; }`, after the `!mayUseNetwork` hold and before startConnect() - and parkAwaitsAnswer()
     asks kosyncParkPending() WITH the local key. A close's push queued behind the pull on open
     used to PUT our place over the X4's the pull had just parked, before anyone saw the card.
 11. N1 - a WiFi join in progress is not "off WiFi": joinInProgress() is the join-age test
     (wifiJoinAgeMs(now, lastWifiConnectAttemptMs()) < KOSYNC_JOIN_WAIT_MS), and clientStep()'s
     idle branch holds the ask on it before dropping it. Integration review 3 (R3-3, KOS-N1a)
     found the hold incomplete, so also: the idle branch's `if (!mayUseNetwork)` hold zeroes
     s_joinWaitSinceMs (a wait begun before a game made the join after it look 60 s old, and the
     ask was dropped on its first pass); the wait starts nowhere but in the idle branch's
     `if (joinInProgress(now))`; KS_RESOLVE, KS_RETRY and KS_CONNECT each give up "not on WiFi"
     only under `if (!heldOffWifi(mayUseNetwork, now))`, KS_CONNECT's check comes before
     startConnect(); and heldOffWifi() holds while a game has the radio OR a join is coming, going
     back to KS_IDLE with the ask KEPT (dropSocket, no finishJob, no want cleared).
     The same review's R3-4 then showed contract 11 pinned the SHAPE and not the parts: replacing the
     60 s cap with `if (true)` (a phone out of range held the ask for ever) and deleting
     `!xferUsingAP() && wifiStationWanted() &&` both survived. So also: joinInProgress() is ONE `&&`
     chain holding every term (JOIN_TERMS); the wait returns only under the cap, starts only
     `if (!s_joinWaitSinceMs)`, and is zeroed on the way past it before the ask can be dropped.
     Its verification then made the cap 0xFFFFFFFFu through the #define and contract 11 held (it
     pinned the comparison, not the constant), so the two #defines are read too: plain decimal
     literals, each defined once, KOSYNC_JOIN_WAIT_MS < KOSYNC_JOIN_WAIT_MAX_MS <= 120 s.

 12. The plan's 5.2 (docs/booksync-simple-plan.md, 2026-09-26) - "Sync my place" asks home FIRST
     and the window waits for the job's verdict: kosyncSyncMyPlace() calls kosyncSyncHome() before
     any kosyncWindowOpen(); its ONE `if (kosyncWindowWaitsForHome(...) && rememberWindowAfterHome(
     ...)) {...} else {...}` holds every inline kosyncWindowOpen() in the ELSE (the not-waiting
     branch); the held ask is released (windowAfterHome, = requestWindow(s_afterBook,
     KOSYNC_WINDOW_SYNC_MS, ...)) from finishJob() - the verdict, whatever it is - AND from
     kosyncLoop() under `if (s_afterWant && ... >= KOSYNC_WINDOW_AFTER_HOME_MAX_MS)` AND in the
     same block as every `s_pushWant = s_pullWant = false;` of clientStep()'s idle branch (an ask
     dropped without a job - off WiFi, no memory - has no finishJob to release it); and that
     #define is one plain decimal that covers "home did not answer" (KS_DNS_GIVEUP_MS +
     KOSYNC_CLIENT_TRIES x KS_CONNECT_MS + kosyncRetryDelayMs's waits, read from the sources) and
     stays within 60 s. The window used to be opened first, and a hotspot window takes the station
     the push needs; test_kosync pins the predicate, this pins the order.

A contract that cannot find its function fails too - a rename must update it, not silently
retire it. A self-test replays the pre-fix shapes first, then REMOVES each guard from the real
sources one at a time (the review's experiment among them) and requires its contract to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, ifs, match_close, strip_code  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"
SYNC = "kosync_sync.cpp"
SYNC_H = "kosync_sync.h"
BOOKS = "app_books.cpp"
SERIAL = "serial_cmd.cpp"
KOSYNC = "kosync.cpp"            # contract 12 reads the retry waits from it
KOSYNC_H = "kosync.h"            # ...and KOSYNC_CLIENT_TRIES

BLOCKING = [
    (re.compile(r"\bresolveDomain\s*\("), "resolveDomain() (blocks >= 0.5 s; never found a .local name)"),
    (re.compile(r"\bhostByName\s*\("), "hostByName() (waits for the answer)"),
    (re.compile(r"\b(?:lwip_)?gethostbyname(?:_r)?\s*\("), "gethostbyname() (waits for the answer)"),
    (re.compile(r"\b(?:lwip_)?getaddrinfo\s*\("), "getaddrinfo() (waits for the answer)"),
    (re.compile(r"\bqueryHost\s*\("), "MDNS.queryHost() (waits for the answer)"),
    (re.compile(r"\bmdns_query\w*\s*\("), "an IDF mdns_query_*() (waits for the answer)"),
    (re.compile(r"\bnetconn_gethostbyname\w*\s*\("), "netconn_gethostbyname() (waits for the answer)"),
    (re.compile(r"\btcpip_callback\s*\("), "tcpip_callback() (= tcpip_callback_with_block(f, ctx, 1))"),
    (re.compile(r"\btcpip_api_call\s*\("), "tcpip_api_call() (waits for the tcpip thread)"),
]
RECV = re.compile(r"\b(?:lwip_)?recv(?:from)?\s*\(")
RELOAD_CALL = re.compile(r"(?<![\w:.>])kosyncReloadConfig\s*\(")


def body(code, name):
    b = function_body(code, name)
    return None if b is None else (b[0] + 1, b[1])


def line_of(code, pos):
    return code.count("\n", 0, pos) + 1


def call_args(code, open_pos):
    """The top-level arguments of the call whose '(' is at open_pos, stripped ([] for none)."""
    close = match_close(code, open_pos)
    if close < 0:
        return None
    args, depth, cur = [], 0, []
    for c in code[open_pos + 1:close]:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
    last = "".join(cur).strip()
    if args or last:
        args.append(last)
    return args


def is_definition(code, pos):
    """The name at pos is declared or defined here (a return type stands before it)."""
    return re.search(r"\b(?:void|bool|int|err_t|uint32_t)\s*\**\s*$", code[max(0, pos - 40):pos]) is not None


def within(pos, span):
    return span is not None and span[0] <= pos < span[1]


def else_block(code, block_end):
    """(start, end) of the `else { ... }` straight after the if-block ending at block_end, or None."""
    m = re.compile(r"\s*else\s*\{").match(code, block_end + 1)
    if not m:
        return None
    end = match_close(code, m.end() - 1)
    return None if end < 0 else (m.end() - 1, end)


def brace_depth(code, pos):
    return code.count("{", 0, pos) - code.count("}", 0, pos)


def block_end(code, pos):
    """The index of the `}` that closes the block code[pos] stands in (nested blocks skipped), or
    len(code)."""
    depth = 0
    for j in range(pos, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            if depth == 0:
                return j
            depth -= 1
    return len(code)


def check(files):
    """files: {name: stripped code, name + ".raw": raw text}. Returns the broken contracts."""
    bad = []
    sync = files.get(SYNC, "")
    synch = files.get(SYNC_H, "")
    serial = files.get(SERIAL, "")
    serial_raw = files.get(SERIAL + ".raw", "")

    # ── 1. no blocking resolver in the home client ────────────────────────────────────────────
    for rx, what in BLOCKING:
        for m in rx.finditer(sync):
            bad.append(f"{SYNC}:{line_of(sync, m.start())} calls {what} - a home= lookup is "
                       "polled (KS_RESOLVE), never waited on")

    # ── 4. the lookup's socket and lwIP's resolver are never waited on ────────────────────────
    for m in RECV.finditer(sync):
        a = call_args(sync, m.end() - 1)
        if a is None or len(a) < 4 or not re.search(r"\bMSG_DONTWAIT\b", a[3]):
            bad.append(f"{SYNC}:{line_of(sync, m.start())} reads a socket without MSG_DONTWAIT - "
                       "the lookup's recvfrom must never wait")
    lp = body(sync, "lookupPoll")
    if lp is None:
        bad.append("lookupPoll() not found - if it was renamed, update this contract")
    elif not any(within(m.start(), lp) for m in re.finditer(r"\brecvfrom\s*\(", sync)):
        bad.append("lookupPoll() does not read its socket with recvfrom(..., MSG_DONTWAIT, ...)")
    ds = body(sync, "dnsStart")
    if ds is None:
        bad.append("dnsStart() not found - if it was renamed, update this contract")
    else:
        gh = list(re.finditer(r"\bdns_gethostbyname\s*\(", sync))
        if not gh:
            bad.append("dns_gethostbyname() not found in dnsStart() - update this contract")
        for m in gh:
            if not within(m.start(), ds):
                bad.append(f"{SYNC}:{line_of(sync, m.start())} calls dns_gethostbyname() outside "
                           "dnsStart() - lwIP's resolver is started on the tcpip thread only")
        handed = 0
        for m in re.finditer(r"\bdnsStart\b", sync):
            if is_definition(sync, m.start()):
                continue
            cb = re.search(r"\btcpip_callback_with_block\s*\(\s*$", sync[max(0, m.start() - 60):m.start()])
            if cb:
                open_pos = max(0, m.start() - 60) + cb.end() - 1
                a = call_args(sync, open_pos)
                if a and len(a) == 3 and a[0] == "dnsStart" and a[2] == "0":
                    handed += 1
                    continue
            bad.append(f"{SYNC}:{line_of(sync, m.start())} uses dnsStart other than as "
                       "tcpip_callback_with_block(dnsStart, <tag>, 0) - dnsStart must only be "
                       "handed to the tcpip thread, never run on the loop")
        if not handed:
            bad.append("dnsStart must only be started by tcpip_callback_with_block(dnsStart, <tag>, 0) "
                       "- no such start found")
    for m in re.finditer(r"\btcpip_callback_with_block\s*\(", sync):
        a = call_args(sync, m.end() - 1)
        if not a or len(a) != 3 or a[2] != "0":
            bad.append(f"{SYNC}:{line_of(sync, m.start())} tcpip_callback_with_block(..., "
                       f"{a[-1] if a else '?'}) - the last argument must be 0: the loop never "
                       "waits for room in the tcpip thread's mailbox")

    # ── 2 + 3. only serial `kosync reload` asks ───────────────────────────────────────────────
    decl = re.search(r"\bkosyncReloadConfig\s*\(\s*bool\s+\w+\s*(?:=\s*(\w+)\s*)?\)", synch)
    if not decl:
        bad.append(f"{SYNC_H}: the kosyncReloadConfig(bool asked ...) declaration not found - "
                   "update this contract")
    elif decl.group(1) is not None and decl.group(1) != "false":
        bad.append(f"{SYNC_H}: kosyncReloadConfig's default must be false (it is "
                   f"{decl.group(1)}) - every re-read nobody asked for would clear the warnings")
    def branch(pat, lo, hi):
        """The block of the `if` whose condition holds `pat` (read in the RAW text, since
        strip_code blanks the command words, at a code position), within [lo, hi)."""
        for m in re.finditer(pat, serial_raw[:hi]):
            if m.start() < lo or serial[m.start():m.start() + 4] != serial_raw[m.start():m.start() + 4]:
                continue                                         # outside, or in a comment
            for h in ifs(serial, lo, hi, re.compile(r"strn?casecmp")):
                if h[0] < m.start() < h[1]:
                    return (h[1], h[2] + 1)
        return None

    # Other commands have a "reload" of their own: the one that counts is inside `kosync`'s.
    ks_blk = branch(r'\bstrcasecmp\s*\(\s*line\s*,\s*"kosync"\s*\)', 0, len(serial))
    reload_blk = ks_blk and branch(r'\bstrcasecmp\s*\(\s*arg\s*,\s*"reload"\s*\)', *ks_blk)
    if not reload_blk:
        reload_blk = None
        bad.append(f"{SERIAL}: the `kosync reload` branch (strcasecmp(arg, \"reload\") inside "
                   "the `kosync` command) not found - update this contract")
    else:
        inside = [m for m in RELOAD_CALL.finditer(serial, *reload_blk)]
        if not inside or any(call_args(serial, m.end() - 1) != ["true"] for m in inside):
            bad.append("serial `kosync reload` must call kosyncReloadConfig(true) - the one re-read "
                       "a person asked for, which clears the window's warnings")
    books_calls = 0
    for name in sorted(k for k in files if k.endswith((".cpp", ".ino"))):
        code = files[name]
        for m in RELOAD_CALL.finditer(code):
            if is_definition(code, m.start()):
                continue
            if name == SERIAL and within(m.start(), reload_blk):
                continue
            if name == BOOKS:
                books_calls += 1
            a = call_args(code, m.end() - 1)
            if a not in ([], ["false"]):
                bad.append(f"{name}:{line_of(code, m.start())} kosyncReloadConfig({', '.join(a or ['?'])}) "
                           "- only serial `kosync reload` asks; every other re-read (Books on each "
                           "entry, Sync settings on each visit) keeps the window's warnings")
    if books_calls < 2:
        bad.append(f"{BOOKS}: the two Books re-reads of kosync.txt (entry, Sync settings) not found "
                   f"({books_calls}) - update this contract if they moved")

    # ── 5 + 6. what clears the window's warnings ──────────────────────────────────────────────
    wo = body(sync, "kosyncWindowOpen")
    rd = body(sync, "reloadDone")
    rc = body(sync, "kosyncReloadConfig")
    cw = body(sync, "clearWindowProblems")
    for fn, span in (("kosyncWindowOpen", wo), ("reloadDone", rd), ("kosyncReloadConfig", rc),
                     ("clearWindowProblems", cw)):
        if span is None:
            bad.append(f"{fn}() not found - if it was renamed, update this contract")
    clears = [m.start() for m in re.finditer(r"\bclearWindowProblems\s*\(", sync)
              if not is_definition(sync, m.start())]
    if wo is not None:
        sb = ifs(sync, wo[0], wo[1], re.compile(r"(?<=\()\s*sameBook\s*$"))
        e = else_block(sync, sb[0][2]) if sb else None
        if e is None:
            bad.append("kosyncWindowOpen(): the `if (sameBook) {...} else {...}` not found - "
                       "update this contract")
        else:
            if not any(within(p, e) for p in clears):
                bad.append("kosyncWindowOpen()'s new-window branch (the else of `if (sameBook)`) "
                           "must call clearWindowProblems() - a new window starts with no warnings")
            if any(within(p, wo) and not within(p, e) for p in clears):
                bad.append("kosyncWindowOpen() clears the warnings outside its new-window branch - "
                           "a window extended for the same book keeps them")
    for p in clears:
        if within(p, wo) or within(p, rd) or within(p, rc):
            continue
        bad.append(f"{SYNC}:{line_of(sync, p)} calls clearWindowProblems() - only a new window, or "
                   "a reload that was asked for or found the file changed, clears the warnings")
    if rd is not None:
        gate = ifs(sync, rd[0], rd[1], re.compile(r"\bkosyncReloadClears\s*\(\s*asked\b"))
        mine = [p for p in clears if within(p, rd)]
        if not mine or any(not any(g[1] <= p <= g[2] for g in gate) for p in mine):
            bad.append("reloadDone() must clear only `if (kosyncReloadClears(asked, ...))` - "
                       "Books re-reads kosync.txt on every entry")
    if rc is not None:
        gate = ifs(sync, rc[0], rc[1], re.compile(r"(?<=\()\s*asked\s*$"))
        if any(within(p, rc) and not any(g[1] <= p <= g[2] for g in gate) for p in clears):
            bad.append("kosyncReloadConfig() may call clearWindowProblems() itself only `if (asked)`")
        dones = [m for m in re.finditer(r"\breloadDone\s*\(", sync) if within(m.start(), rc)]
        if not dones or any(call_args(sync, m.end() - 1) != ["asked"] for m in dones):
            bad.append("kosyncReloadConfig() must pass `asked` through to every reloadDone(asked)")
    for m in re.finditer(r"\bs_(?:other|unauth|unmarked)\s*=(?!=)", sync):
        if brace_depth(sync, m.start()) > 0 and not within(m.start(), cw):
            bad.append(f"{SYNC}:{line_of(sync, m.start())} zeroes s_other/s_unauth/s_unmarked outside "
                       "clearWindowProblems() - every clear of the warnings goes through it")
    puts_clears = list(re.finditer(r"\bmemset\s*\(\s*&\s*T\s*->\s*puts\b", sync))
    for m in puts_clears:
        if not within(m.start(), cw):
            bad.append(f"{SYNC}:{line_of(sync, m.start())} clears the window's PUT log outside "
                       "clearWindowProblems() - every clear of the warnings goes through it")
    if cw is not None and not any(within(m.start(), cw) for m in puts_clears):
        bad.append("clearWindowProblems() must clear the window's PUT log (memset(&T->puts, ...)) - "
                   "a new window starts with no 'DIFFERENT book' warning")

    bad.extend(check_review(files))
    bad.extend(check_window_after_home(files))
    return bad


def check_review(files):
    """Contracts 7-9: the integration review's KS-1, KS-2 and KS-3 (see the docstring)."""
    bad = []
    sync = files.get(SYNC, "")
    books = files.get(BOOKS, "")
    ev = body(sync, "evaluateOffer")
    if ev is None:
        bad.append("evaluateOffer() not found - if it was renamed, update this contract")
    else:
        # ── 7. KS-1: judged as of the ask ─────────────────────────────────────────────────────
        fills = [m for m in re.finditer(r"\bkosyncOfferFill\s*\(", sync) if within(m.start(), ev)]
        if len(fills) != 1:
            bad.append(f"evaluateOffer() must fill its KosyncOfferIn with ONE kosyncOfferFill() call "
                       f"(found {len(fills)}) - the rule for which last move judges a record lives there")
        for m in fills:
            a = call_args(sync, m.end() - 1) or []
            if len(a) < 6 or a[2] != "thenSend" or a[5] not in ("b->movedAt", "s_w->book.movedAt"):
                bad.append(f"{SYNC}:{line_of(sync, m.start())} kosyncOfferFill({', '.join(a)}) - the "
                           "explicit flag must be `thenSend` and the asked stamp the job's snapshot "
                           "(b->movedAt): an automatic ask is judged against the last move AS OF THE ASK")
        # ── 8. KS-2: nothing sent over a place still on the card ─────────────────────────────
        pl = re.search(r"\bparkLive\s*=[^;]*?\bkosyncParkPending\s*\(", sync[ev[0]:ev[1]])
        pa = call_args(sync, ev[0] + pl.end() - 1) if pl else None
        if not pa or len(pa) != 4 or pa[2] in ("NULL", "nullptr", "0"):
            bad.append("evaluateOffer() must ask kosyncParkPending() (`parkLive = ... "
                       "kosyncParkPending(ledger, book, key, &sig)`, WITH the local key) - Sync my "
                       "place must not send over a place still waiting on the card, and a park the "
                       "card can never show (the passcode changed) must not hold it")
        gates = ifs(sync, ev[0], ev[1], re.compile(r"!\s*parkLive\b"))
        for m in re.finditer(r"\breturn\s+false\s*;", sync[ev[0]:ev[1]]):
            pos = ev[0] + m.start()
            if not any(g[1] <= pos <= g[2] for g in gates):
                bad.append(f"{SYNC}:{line_of(sync, pos)} evaluateOffer() returns false (= PUT ours) "
                           "outside an `if (... !parkLive)` - Sync my place would send over a place "
                           "the person has not answered yet")
    for m in re.finditer(r"\bmyMovedAt\s*=(?!=)", sync):
        bad.append(f"{SYNC}:{line_of(sync, m.start())} assigns myMovedAt - the offer's inputs come "
                   "from kosyncOfferFill() only (KS-1: judged as of the ask)")
    np = body(sync, "kosyncNotePosition")
    if np is None:
        bad.append("kosyncNotePosition() not found - if it was renamed, update this contract")
    else:
        waits = [m for m in re.finditer(r"\bnotePlace\s*\(\s*s_waitBook\b", sync) if within(m.start(), np)]
        if not waits:
            bad.append("kosyncNotePosition(): the notePlace(s_waitBook, ...) call not found - update "
                       "this contract")
        for m in waits:
            a = call_args(sync, m.end() - len(m.group(0)) + m.group(0).index("(")) or []
            if not a or a[-1] != "0":
                bad.append(f"{SYNC}:{line_of(sync, m.start())} notePlace(s_waitBook, ..., "
                           f"{a[-1] if a else '?'}) - the ask waiting for WiFi keeps the stamp it was "
                           "made with (pass 0): pages read meanwhile hid the X4's newer place")
    for m in re.finditer(r"\bofferedSig\s*=(?!=)", sync):
        bad.append(f"{SYNC}:{line_of(sync, m.start())} assigns offeredSig - a record is 'declined' only "
                   "when the person ANSWERS the card (kosyncCardAnswered -> kosyncOfferAnswered), "
                   "never when it is parked")
    ap = body(books, "BooksApp::applyPending")
    if ap is None:
        bad.append("BooksApp::applyPending() not found - if it was renamed, update this contract")
    backs = [h for h in ifs(books, 0, len(books), re.compile(r"\bLOGIC_BUTTON_BACK\s*\(\s*event\s*\)"))
             if re.search(r"\bdropParkedForThisBook\s*\(", books[h[1]:h[2]])]
    if not backs:
        bad.append(f"{BOOKS}: the sync card's Back branch (LOGIC_BUTTON_BACK ... dropParkedForThisBook) "
                   "not found - update this contract")
    in_apply = in_back = 0
    for name in sorted(k for k in files if k.endswith((".cpp", ".ino"))):
        code = files[name]
        for m in re.finditer(r"(?<![\w:.>])kosyncCardAnswered\s*\(", code):
            if is_definition(code, m.start()):
                continue
            a = call_args(code, m.end() - 1)
            if name == BOOKS and a == ["pendingId"] and within(m.start(), ap):
                in_apply += 1
            elif name == BOOKS and a == ["pendingId"] and any(h[1] <= m.start() <= h[2] for h in backs):
                in_back += 1
            else:
                bad.append(f"{name}:{line_of(code, m.start())} kosyncCardAnswered({', '.join(a or ['?'])}) "
                           "- only the two ANSWERS to the card (applyPending, and Back) decline a record")
    if not in_apply:
        bad.append(f"{BOOKS}: applyPending() must call kosyncCardAnswered(pendingId) - 'Go there' answers "
                   "the card")
    if not in_back:
        bad.append(f"{BOOKS}: the sync card's Back branch must call kosyncCardAnswered(pendingId) - "
                   "'Stay where I am' answers the card (and declines the record)")
    # ── 9. KS-3: a reader's second id is not another book ────────────────────────────────────
    ws = body(sync, "kosyncWindowServe")
    pr = body(sync, "kosyncProblemLine")
    for fn, span in (("kosyncWindowServe", ws), ("kosyncProblemLine", pr)):
        if span is None:
            bad.append(f"{fn}() not found - if it was renamed, update this contract")
    if ws is not None:
        for call, cond in (("kosyncPutLogOwn", "o.gotPut"), ("kosyncPutLogOther", "o.otherDoc")):
            gate = ifs(sync, ws[0], ws[1], re.compile(r"(?<=\()\s*" + re.escape(cond) + r"\s*$"))
            calls = [m.start() for m in re.finditer(r"\b" + call + r"\s*\(", sync) if within(m.start(), ws)]
            if not calls or not all(any(g[1] <= c <= g[2] for g in gate) for c in calls):
                bad.append(f"kosyncWindowServe() must log `if ({cond})` with {call}() - the window "
                           "tells a reader's second id from a different book by who sent what")
    if pr is not None:
        wp = [m for m in re.finditer(r"\bkosyncWindowProblems\s*\(", sync) if within(m.start(), pr)]
        a = call_args(sync, wp[0].end() - 1) if wp else None
        if not a or not re.search(r"\b" + re.escape(a[0]) + r"\s*=\s*kosyncPutLogDifferent\s*\(",
                                  sync[pr[0]:pr[1]]):
            bad.append("kosyncProblemLine() must count the other-book PUTs with kosyncPutLogDifferent() "
                       "- the X4 fork's second id is not 'a DIFFERENT book'")
    bad.extend(check_close_push(sync))
    bad.extend(check_join_wait(sync))
    return bad


def check_join_wait(sync):
    """Contract 11: N1 - a join in progress is not "off WiFi", in any state; one wait per join."""
    bad = []
    # 11a (N1): the join-in-progress predicate, and the idle ask HELD by it before it is dropped.
    # Without the hold, a push queued at a book close before a game was lost on the first pass
    # after it (the rejoin had only just begun).
    jp = body(sync, "joinInProgress")
    if jp is None:
        bad.append("joinInProgress() not found - if it was renamed, update this contract")
    elif not re.search(r"wifiJoinAgeMs\s*\(\s*now\s*,\s*lastWifiConnectAttemptMs\s*\(\s*\)\s*\)\s*<\s*"
                       r"KOSYNC_JOIN_WAIT_MS", sync[jp[0]:jp[1]]):
        bad.append("joinInProgress() must hold an idle ask while a join is in progress "
                   "(wifiJoinAgeMs(now, lastWifiConnectAttemptMs()) < KOSYNC_JOIN_WAIT_MS) - review N1")
    cs = body(sync, "clientStep")
    if cs is None:
        bad.append("clientStep() not found - if it was renamed, update this contract")
        return bad
    idle = ifs(sync, cs[0], cs[1], re.compile(r"\bs_cs\s*==\s*KS_IDLE\s*$"))
    if not idle:
        bad.append("clientStep(): the `if (s_cs == KS_IDLE) {...}` block not found - update this contract")
        return bad
    lo, hi = idle[0][1], idle[0][2]
    seg = sync[lo:hi]
    hold = re.search(r"\bjoinInProgress\s*\(\s*now\s*\)", seg)
    drop = re.search(r"\bs_pushWant\s*=\s*s_pullWant\s*=\s*false\s*;", seg)
    if not hold or not drop or hold.start() > drop.start():
        bad.append("clientStep() must hold an idle ask while a join is in progress "
                   "(joinInProgress(now)) BEFORE it drops it as not on WiFi - review N1")
    # 11b (review 3, R3-3/KOS-N1a): the wait is for ONE join. The Games-app hold ends it, or the
    # join ~GbcApp begins inherits a start from before the game and the ask is dropped at once.
    # And the wait starts nowhere but in the idle branch's joinInProgress() block.
    held = [h for h in ifs(sync, lo, hi, re.compile(r"(?<=\()\s*!\s*mayUseNetwork\s*$"))
            if re.search(r"\breturn\s*;", sync[h[1]:h[2] + 1])]
    if not held or not re.search(r"\bs_joinWaitSinceMs\s*=\s*0\s*;", sync[held[0][1]:held[0][2] + 1]):
        bad.append("clientStep(): the join wait must end at the Games-app hold - `if (!mayUseNetwork) "
                   "{ s_joinWaitSinceMs = 0; return; }` in the idle branch - or a wait begun before a "
                   "game makes the join after it look 60 s old (review 3, R3-3)")
    waits = ifs(sync, lo, hi, re.compile(r"(?<=\()\s*joinInProgress\s*\(\s*now\s*\)\s*$"))
    for m in re.finditer(r"\bs_joinWaitSinceMs\s*=(?!=)\s*([^;]*);", sync):
        if m.group(1).strip() != "0" and not any(w[1] <= m.start() <= w[2] for w in waits):
            bad.append(f"{SYNC}:{line_of(sync, m.start())} starts the join wait outside the idle "
                       "branch's `if (joinInProgress(now))` - it is non-zero only while that holds")
    bad.extend(check_join_wait_cap(sync, lo, hi, waits))
    bad.extend(check_join_wait_constants(sync))
    bad.extend(check_join_terms(sync, jp))
    # 11c (review 3, KOS-N1a): a job past KS_IDLE that finds the station down gives up only when
    # heldOffWifi() says nothing is coming. It used to give up in KS_RETRY (a back-off begun in the
    # Games picker) and burn a try in KS_CONNECT (connecting on a station not yet rejoined).
    hw = body(sync, "heldOffWifi")
    if hw is None:
        bad.append("heldOffWifi() not found - if it was renamed, update this contract")
    else:
        hb = sync[hw[0]:hw[1]]
        if not re.search(r"if\s*\(\s*(?:mayUseNetwork\s*&&\s*!\s*joinInProgress\s*\(\s*now\s*\)|"
                         r"!\s*joinInProgress\s*\(\s*now\s*\)\s*&&\s*mayUseNetwork)\s*\)\s*\{\s*"
                         r"return\s+false\s*;\s*\}", hb):
            bad.append("heldOffWifi() must hold while a game has the radio OR a join is in progress - "
                       "`if (mayUseNetwork && !joinInProgress(now)) { return false; }` (review 3, KOS-N1a)")
        if not re.search(r"\bdropSocket\s*\(\s*\)\s*;", hb) or \
                not re.search(r"\bs_cs\s*=\s*KS_IDLE\s*;", hb) or \
                re.search(r"\bs_p(?:ush|ull)Want\s*=|\bfinishJob\s*\(", hb):
            bad.append("heldOffWifi() must drop the socket and go back to KS_IDLE KEEPING the ask "
                       "(no finishJob(), no s_pushWant/s_pullWant) - the idle branch waits for the join")
    gate = re.compile(r"(?<=\()\s*!\s*heldOffWifi\s*\(\s*mayUseNetwork\s*,\s*now\s*\)\s*$")
    for state in ("KS_RESOLVE", "KS_RETRY", "KS_CONNECT"):
        blk = ifs(sync, cs[0], cs[1], re.compile(r"\bs_cs\s*==\s*" + state + r"\s*$"))
        if not blk:
            bad.append(f"clientStep(): the `if (s_cs == {state}) {{...}}` block not found - update this "
                       "contract")
            continue
        off = ifs(sync, blk[0][1], blk[0][2], re.compile(r"(?<=\()\s*!\s*onWifi\s*\(\s*\)\s*$"))
        ok = bool(off)
        for o in off:
            gates = ifs(sync, o[1], o[2], gate)
            gives = [m.start() for m in re.compile(r"\bfinishJob\s*\(").finditer(sync, o[1], o[2])]
            if not gates or not all(any(g[1] <= f <= g[2] for g in gates) for f in gives):
                ok = False
        if not ok:
            bad.append(f"clientStep(): {state} gives up off WiFi without heldOffWifi() - `if (!onWifi()) "
                       "{ if (!heldOffWifi(mayUseNetwork, now)) { finishJob(...); } return; }` - a join "
                       "in progress, or a game holding the radio, is not a reason to drop the ask "
                       "(review 3, KOS-N1a)")
        if state == "KS_CONNECT" and off:
            conn = re.search(r"\bstartConnect\s*\(", sync[blk[0][1]:blk[0][2]])
            if conn is None or not off[0][2] < blk[0][1] + conn.start():
                bad.append("clientStep(): KS_CONNECT must check onWifi() BEFORE startConnect() - a job held "
                           "across a game burnt a try connecting on a station not yet rejoined "
                           "(review 3, KOS-N1a)")
    return bad


# 11d/11e (integration review 3, R3-4): the parts of N1 a hand mutation removed and contract 11 did not
# notice. Each term of joinInProgress() whose loss a phone would show, as a whole `&&` operand with its
# polarity (T->cfg.ok and T->cfg.home[0] are left free: without a home the idle branch drops the ask
# with the same words either way).
JOIN_TERMS = [
    (r"!\s*onWifi\s*\(\s*\)", "!onWifi()"),
    (r"!\s*xferUsingAP\s*\(\s*\)", "!xferUsingAP()"),
    (r"wifiStationWanted\s*\(\s*\)", "wifiStationWanted()"),
    (r"lastWifiConnectAttemptMs\s*\(\s*\)\s*!=\s*0", "lastWifiConnectAttemptMs() != 0"),
    (r"wifiJoinAgeMs\s*\(\s*now\s*,\s*lastWifiConnectAttemptMs\s*\(\s*\)\s*\)\s*<\s*KOSYNC_JOIN_WAIT_MS",
     "wifiJoinAgeMs(now, lastWifiConnectAttemptMs()) < KOSYNC_JOIN_WAIT_MS"),
]


def top_level(expr, op):
    """expr split at every `op` outside parentheses and brackets."""
    parts, depth, cur, i = [], 0, [], 0
    while i < len(expr):
        c = expr[i]
        depth += c in "(["
        depth -= c in ")]"
        if depth == 0 and expr.startswith(op, i):
            parts.append("".join(cur).strip())
            cur, i = [], i + len(op)
            continue
        cur.append(c)
        i += 1
    parts.append("".join(cur).strip())
    return parts


def check_join_terms(sync, jp):
    """11d: joinInProgress() is ONE `&&` chain holding every term. Deleting `!xferUsingAP()` or
    `wifiStationWanted()` survived contract 11 (it pinned the age test alone): with WiFi turned off in
    Settings, a game holding the radio or our own hotspot up, no join is coming, and the ask sat
    "waiting for WiFi to join" instead of giving up honestly."""
    if jp is None:
        return []
    ret = re.compile(r"\breturn\b([^;]*);").search(sync, jp[0], jp[1])
    if ret is None:
        return ["joinInProgress() must `return` its test - update this contract"]
    expr = ret.group(1)
    if len(top_level(expr, "||")) > 1:
        return ["joinInProgress() must be ONE `&&` chain - an `||` makes any one term enough to hold an "
                "ask (review 3, R3-4)"]
    ops = top_level(expr, "&&")
    missing = [name for pat, name in JOIN_TERMS if not any(re.fullmatch(pat, o) for o in ops)]
    if missing:
        return [f"joinInProgress() lacks {', '.join('`' + m + '`' for m in missing)} as an `&&` term - no "
                "join is coming without each of them (WiFi off in Settings or a game holding the radio: "
                "wifiStationWanted; our own hotspot up: xferUsingAP; already joined: onWifi), and the "
                "ask would wait instead of being handled (review 3, R3-4)"]
    return []


def check_join_wait_cap(sync, lo, hi, waits):
    """11e: the wait's own rules inside the idle branch. Replacing the 60 s test with `if (true)`
    survived contract 11 - and without the cap a phone out of range holds the ask for ever, because
    the loop's retry keeps re-stamping joins under 30 s old. So: the idle branch's `if
    (joinInProgress(now))` block returns only under `if ((uint32_t)(now - s_joinWaitSinceMs) <
    KOSYNC_JOIN_WAIT_MAX_MS)`; it starts the wait only `if (!s_joinWaitSinceMs)` (re-stamped every
    pass, now - start is always 0 and the cap never comes); and the fall-through zeroes the wait
    BEFORE the ask is dropped (left set, the next close's wait inherits it and gives up at once -
    R3-3's loss by another road)."""
    if not waits:
        return ["clientStep(): the idle branch's `if (joinInProgress(now)) {...}` block not found - "
                "update this contract"]
    w0, w1 = waits[0][1], waits[0][2]
    bad = []
    cap = ifs(sync, w0, w1, re.compile(r"(?<=\()\s*\(\s*uint32_t\s*\)\s*\(\s*now\s*-\s*s_joinWaitSinceMs\s*\)"
                                       r"\s*<\s*KOSYNC_JOIN_WAIT_MAX_MS\s*$"))
    rets = [m.start() for m in re.compile(r"\breturn\b").finditer(sync, w0, w1)]
    if not cap or not rets or not all(any(c[1] <= r <= c[2] for c in cap) for r in rets):
        bad.append("clientStep(): the join wait must hold (return) only under `if ((uint32_t)(now - "
                   "s_joinWaitSinceMs) < KOSYNC_JOIN_WAIT_MAX_MS)` - without the 60 s cap a phone out "
                   "of range holds the ask for ever (review 3, R3-4)")
    first = ifs(sync, w0, w1, re.compile(r"(?<=\()\s*!\s*s_joinWaitSinceMs\s*$"))
    starts = [m.start() for m in re.finditer(r"\bs_joinWaitSinceMs\s*=(?!=)\s*([^;]*);", sync[:w1])
              if m.start() > w0 and m.group(1).strip() != "0"]
    if not starts or not all(any(f[1] <= s <= f[2] for f in first) for s in starts):
        bad.append("clientStep(): the join wait must start only `if (!s_joinWaitSinceMs)` - re-stamped on "
                   "every pass, now - start stays 0 and the 60 s cap never comes (review 3, R3-4)")
    drop = re.compile(r"\bs_pushWant\s*=\s*s_pullWant\s*=\s*false\s*;").search(sync, w1, hi)
    reset = re.compile(r"\bs_joinWaitSinceMs\s*=\s*0\s*;").search(sync, w1, hi)
    if drop is None or reset is None or reset.start() > drop.start():
        bad.append("clientStep(): past the join wait the idle branch must zero it (`s_joinWaitSinceMs = "
                   "0;`) before it can drop the ask - left set, the next close's wait inherits a start "
                   "60 s old and gives up on its first pass (review 3, R3-4)")
    return bad


# 11f (R3-4's verification): the cap's SIZE. check_join_wait_cap() pins the comparison's form, so
# `#define KOSYNC_JOIN_WAIT_MAX_MS 0xFFFFFFFFu` passed it - the b1 failure (an out-of-range phone holds
# the ask for ever) reached through the define. A join takes seconds and the wait is for ONE; two
# minutes is double today's cap, and anything longer should be decided here, with its reason.
JOIN_WAIT_BOUND_MS = 120000


def join_wait_define(sync, name):
    """The value of `#define name <decimal>[u]`, or a problem string: it must be defined exactly once,
    as a plain decimal literal (an expression or a hex value is refused, not evaluated)."""
    ds = list(re.finditer(r"^[ \t]*#[ \t]*define[ \t]+" + name + r"\b[ \t]*([^\n]*)$", sync, re.M))
    if len(ds) != 1 or re.search(r"^[ \t]*#[ \t]*undef[ \t]+" + name + r"\b", sync, re.M):
        return None, f"{name} must be #defined exactly once in {SYNC} (found {len(ds)}, or an #undef)"
    m = re.fullmatch(r"\s*(\d+)[uU]?[lL]?\s*", ds[0].group(1))
    if not m:
        return None, (f"{name} must be a plain decimal literal (`{ds[0].group(1).strip()}`) - this check "
                      "reads its value, it does not evaluate expressions")
    return int(m.group(1)), None


def check_join_wait_constants(sync):
    wait, p1 = join_wait_define(sync, "KOSYNC_JOIN_WAIT_MS")
    cap, p2 = join_wait_define(sync, "KOSYNC_JOIN_WAIT_MAX_MS")
    bad = [p for p in (p1, p2) if p]
    if bad:
        return bad
    if not 0 < wait < cap <= JOIN_WAIT_BOUND_MS:
        return [f"KOSYNC_JOIN_WAIT_MS ({wait}) < KOSYNC_JOIN_WAIT_MAX_MS ({cap}) <= {JOIN_WAIT_BOUND_MS} "
                "must hold - the cap is what lets an out-of-range phone drop the ask, and a join younger "
                "than the cap is the only thing held (review 3, R3-4 verification)"]
    return []


def check_close_push(sync):
    """Contract 10: KS-2's rule for EVERY push - no PUT over a place still waiting on the card."""
    bad = []
    pa = body(sync, "parkAwaitsAnswer")
    if pa is None:
        bad.append("parkAwaitsAnswer() not found - if it was renamed, update this contract")
    else:
        keyed = 0
        for m in re.finditer(r"\bkosyncParkPending\s*\(", sync):
            a = call_args(sync, m.end() - 1) or []
            if within(m.start(), pa) and len(a) == 4 and a[2] not in ("NULL", "nullptr", "0"):
                keyed += 1
        if not keyed:
            bad.append("parkAwaitsAnswer() must ask kosyncParkPending(..., key, ...) WITH the local "
                       "key - a park the card can never show (the passcode changed) must not hold "
                       "every push")
    cs = body(sync, "clientStep")
    if cs is None:
        bad.append("clientStep() not found - if it was renamed, update this contract")
        return bad
    blk = ifs(sync, cs[0], cs[1], re.compile(r"\bs_cs\s*==\s*KS_CONNECT\s*$"))
    if not blk:
        bad.append("clientStep(): the `if (s_cs == KS_CONNECT) {...}` block not found - update this "
                   "contract")
        return bad
    lo, hi = blk[0][1], blk[0][2]
    conn = [m.start() for m in re.compile(r"\bstartConnect\s*\(").finditer(sync, lo, hi)]
    held = ifs(sync, lo, hi, re.compile(r"!\s*mayUseNetwork\b"))
    guards = [h for h in ifs(sync, lo, hi, re.compile(r"\bparkAwaitsAnswer\s*\("))
              if re.search(r"!\s*s_phaseGet\b", sync[h[0]:h[1]])
              and re.search(r"\bs_w\s*->\s*sent\s*==\s*0\b", sync[h[0]:h[1]])
              and re.search(r"\bfinishJob\s*\(", sync[h[1]:h[2] + 1])
              and re.search(r"\breturn\s*;", sync[h[1]:h[2] + 1])]
    if not guards or not conn or not guards[0][2] < conn[0]:
        bad.append("clientStep() must refuse a job's first PUT - `if (!s_phaseGet && s_w->sent == 0 && "
                   "parkAwaitsAnswer(...)) { finishJob(...); return; }` before startConnect() in the "
                   "KS_CONNECT block - or a close's push sends ours over the X4's place the pull just "
                   "parked, unseen")
    elif not held or not held[0][2] < guards[0][0]:
        bad.append("clientStep(): the parked-place check must come AFTER `if (!mayUseNetwork) return;` "
                   "- a held job would read NVS on every loop pass")
    return bad


# ── 12. the plan's 5.2: "Sync my place" asks home FIRST; the window opens on the job's verdict ──
# The most a person waits at the reader menu for the window: the bound must cover "home did not
# answer" (computed below from the client's own clocks) and stay under this.
WINDOW_AFTER_HOME_BOUND_MS = 60000


def const_ms(code, name):
    """`static const uint32_t NAME = 3000;` -> 3000, or None."""
    m = re.search(r"\b" + name + r"\s*=\s*(\d+)\s*;", code)
    return int(m.group(1)) if m else None


def check_window_after_home(files):
    """Contract 12: the home job before any window; the inline open only in the not-waiting
    branch; the held ask released from the job's finish AND from the loop's timeout; the timeout
    exists, is used, and covers the longest verdict that matters."""
    bad = []
    sync = files.get(SYNC, "")
    kos = files.get(KOSYNC, "")
    kosh = files.get(KOSYNC_H, "")
    sm = body(sync, "kosyncSyncMyPlace")
    if sm is None:
        bad.append("kosyncSyncMyPlace() not found - if it was renamed, update this contract")
    else:
        homes = [m.start() for m in re.compile(r"\bkosyncSyncHome\s*\(").finditer(sync, sm[0], sm[1])]
        opens = [m.start() for m in re.compile(r"\bkosyncWindowOpen\s*\(").finditer(sync, sm[0], sm[1])]
        if not homes or not opens or min(homes) > min(opens):
            bad.append("kosyncSyncMyPlace() must queue the home job (kosyncSyncHome) BEFORE any "
                       "kosyncWindowOpen() - a hotspot window takes the station the push needs (5.2)")
        gates = ifs(sync, sm[0], sm[1], re.compile(r"\bkosyncWindowWaitsForHome\s*\("))
        e = else_block(sync, gates[0][2]) if len(gates) == 1 else None
        if e is None:
            bad.append("kosyncSyncMyPlace(): ONE `if (kosyncWindowWaitsForHome(...) ...) {...} else {...}` "
                       "not found - the inline window open lives in its else branch (5.2)")
        else:
            if not re.search(r"\brememberWindowAfterHome\s*\(", sync[gates[0][0]:gates[0][2] + 1]):
                bad.append("kosyncSyncMyPlace(): the waiting branch must keep the ask "
                           "(rememberWindowAfterHome) - or the window never opens")
            for o in opens:
                if not within(o, e):
                    bad.append(f"{SYNC}:{line_of(sync, o)} kosyncWindowOpen() outside the not-waiting "
                               "branch (the else of `if (kosyncWindowWaitsForHome(...))`) - on WiFi with "
                               "a home the window opens only after the job's verdict (5.2)")
    wa = body(sync, "windowAfterHome")
    fj = body(sync, "finishJob")
    lp = body(sync, "kosyncLoop")
    calls = [m.start() for m in re.finditer(r"\bwindowAfterHome\s*\(", sync)
             if not is_definition(sync, m.start())]
    if wa is None:
        bad.append("windowAfterHome() not found - if it was renamed, update this contract")
    else:
        rq = [m for m in re.finditer(r"\brequestWindow\s*\(", sync) if within(m.start(), wa)]
        a = call_args(sync, rq[0].end() - 1) if rq else None
        if not a or len(a) != 3 or a[0] != "s_afterBook" or a[1] != "KOSYNC_WINDOW_SYNC_MS":
            bad.append("windowAfterHome() must open the held ask through requestWindow(s_afterBook, "
                       "KOSYNC_WINDOW_SYNC_MS, ...) - the 5-minute window the press asked for, opened "
                       "from the loop (a call, a game or Settings > WiFi still refuse it there)")
    if fj is None:
        bad.append("finishJob() not found - if it was renamed, update this contract")
    elif not any(within(c, fj) for c in calls):
        bad.append("finishJob() must release the held window (windowAfterHome) - the job's verdict, "
                   "whatever it is, opens it (5.2)")
    # The two drops without a job (off WiFi, no memory): no finishJob follows, so each must
    # release the held window itself, in the block that drops the ask. Deleting either release
    # left this contract green until the mutations below were added (review, 2026-09-26).
    cs = body(sync, "clientStep")
    if cs is None:
        bad.append("clientStep() not found - if it was renamed, update this contract")
    else:
        idle = ifs(sync, cs[0], cs[1], re.compile(r"\bs_cs\s*==\s*KS_IDLE\b"))
        drops = [m for m in re.finditer(r"\bs_pushWant\s*=\s*s_pullWant\s*=\s*false\s*;", sync)
                 if any(i[1] <= m.start() <= i[2] for i in idle)]
        if not drops:
            bad.append("clientStep()'s idle branch (`if (s_cs == KS_IDLE) {`) must drop an ask it cannot "
                       "run with `s_pushWant = s_pullWant = false;` (off WiFi, no memory) - if that "
                       "moved, update this contract")
        for m in drops:
            if not any(m.end() < c < block_end(sync, m.end()) for c in calls):
                bad.append(f"{SYNC}:{line_of(sync, m.start())} clientStep()'s idle branch drops the ask "
                           "(s_pushWant = s_pullWant = false) without releasing the held window "
                           "(windowAfterHome) in the same block - no job starts, so no finishJob will, "
                           "and the window would wait the whole bound (5.2)")
    if lp is None:
        bad.append("kosyncLoop() not found - if it was renamed, update this contract")
    else:
        tos = ifs(sync, lp[0], lp[1],
                  re.compile(r"\bs_afterWant\b[\s\S]*>=\s*KOSYNC_WINDOW_AFTER_HOME_MAX_MS\b"))
        if not tos or not any(t[1] <= c <= t[2] for t in tos for c in calls):
            bad.append("kosyncLoop() must release the held window after the bound - `if (s_afterWant && "
                       "(uint32_t)(now - s_afterSinceMs) >= KOSYNC_WINDOW_AFTER_HOME_MAX_MS) { "
                       "windowAfterHome(...); }` - a job that never finishes must not hold it (5.2)")
    cap_ms, p = join_wait_define(sync, "KOSYNC_WINDOW_AFTER_HOME_MAX_MS")
    if p:
        bad.append(p)
        return bad
    dns = const_ms(sync, "KS_DNS_GIVEUP_MS")
    conn = const_ms(sync, "KS_CONNECT_MS")
    tries = re.search(r"#\s*define\s+KOSYNC_CLIENT_TRIES\s+(\d+)", kosh)
    waits = re.search(r"\bWAIT\s*\[\s*KOSYNC_CLIENT_TRIES\s*-\s*1\s*\]\s*=\s*\{([^}]*)\}", kos)
    if dns is None or conn is None or not tries or not waits:
        bad.append("the home client's clocks not all found (KS_DNS_GIVEUP_MS and KS_CONNECT_MS in "
                   f"{SYNC}, KOSYNC_CLIENT_TRIES in {KOSYNC_H}, kosyncRetryDelayMs's WAIT table in "
                   f"{KOSYNC}) - update this contract")
    else:
        n = int(tries.group(1))
        pause = sum(int(x) for x in re.findall(r"\d+", waits.group(1)))
        floor = dns + n * conn + pause
        if not floor <= cap_ms <= WINDOW_AFTER_HOME_BOUND_MS:
            bad.append(f"KOSYNC_WINDOW_AFTER_HOME_MAX_MS ({cap_ms}) must cover 'home did not answer' - "
                       f"the DNS give-up ({dns}) + {n} connects of {conn} + the retry waits ({pause}) = "
                       f"{floor} ms - and stay within {WINDOW_AFTER_HOME_BOUND_MS} ms (a person is "
                       "waiting at the reader menu)")
    return bad


# ── self-test: the pre-fix shapes must break the contracts ─────────────────────────────────────

OLD_SYNC = """
bool kosyncSyncMyPlace(const KosyncBook* b, char* note, size_t cap) {
  const bool wifi = onWifi();
  const bool opened = kosyncWindowOpen(b, KOSYNC_WINDOW_SYNC_MS, note, cap);
  bool pushed = false;
  if (wifi && T->cfg.home[0]) {
    pushed = kosyncSyncHome(b, pn, sizeof(pn));
  }
  return opened || pushed;
}
static uint32_t s_other = 0, s_unauth = 0;
static void clearWindowProblems();
static void reloadDone(bool asked) {
  if (s_cfgRead) { clearWindowProblems(); }
}
bool kosyncReloadConfig(bool asked) {
  s_other = 0;
  reloadDone(true);
  clearWindowProblems();
  return true;
}
bool kosyncWindowOpen(const KosyncBook* b, uint32_t durationMs, char* note, size_t cap) {
  if (sameBook) {
    dur = left;
    clearWindowProblems();
  } else {
    s_gets = s_puts = s_parked = 0;
  }
  return true;
}
static void clearWindowProblems() {
  s_other = s_unauth = 0;
}
void elsewhere() { clearWindowProblems(); }
static void dnsStart(void* arg) {
  err_t e = dns_gethostbyname(T->lookName, &a, dnsFound, arg);
}
static bool lookupStart(uint32_t now, bool dns) {
  dns_gethostbyname(T->lookName, &a, dnsFound, 0);
  dnsStart((void*)tag);
  tcpip_callback(dnsStart, (void*)tag);
  return tcpip_callback_with_block(dnsStart, (void*)tag, 1) == ERR_OK;
}
static int lookupPoll(uint32_t now, uint32_t* ip) {
  const int n = recvfrom(s_udp, s_w->io, KS_IO_CAP, 0, NULL, NULL);
  return 0;
}
static bool resolveHome(uint32_t now) {
  IPAddress ip = resolveDomain(T->cfg.home);
  ip = WiFi.hostByName(T->cfg.home, ip);
  return MDNS.queryHost(T->cfg.home);
}
const char* kosyncWindowServe(const char* method, const char* path, int* code) {
  if (o.gotPut) {
    snprintf(T->peer, sizeof(T->peer), "%s", o.putDevice);
  }
  if (o.otherDoc) {
    s_other++;
    snprintf(T->otherDoc, sizeof(T->otherDoc), "%s", o.otherDocId);
  }
  return s_reply;
}
void kosyncNotePosition(const char* partial, const char* byName, double pct, bool pctOk,
                        uint32_t movedAt) {
  if (s_waitWifi) {
    notePlace(s_waitBook, partial, byName, pct, pctOk, movedAt);
  }
}
static bool evaluateOffer(bool thenSend) {
  if (best < 0) {
    if (thenSend) {
      return false;
    }
    return true;
  }
  KosyncOfferIn in;
  in.myMovedAt = me ? me->movedAt : 0;
  in.offeredSig = me ? me->offeredSig : 0;
  const int v = kosyncOfferVerdict(&in);
  if (v != KOSYNC_OFFER) {
    if (thenSend) {
      return false;
    }
    return true;
  }
  if (kosyncPark(b, g.pct, dev, ts)) {
    me->offeredSig = kosyncOfferSig(in.theirTs, g.pct, g.deviceId, g.device);
  }
  return true;
}
size_t kosyncProblemLine(char* out, size_t cap) {
  return kosyncWindowProblems(s_other, T->otherDoc, s_unauth, out, cap);
}
static bool parkAwaitsAnswer(const char* byName) {
  return kosyncParkPending(&T->ledger, byName, NULL, NULL);
}
static void clientStep(bool mayUseNetwork, uint32_t now) {
  if (s_cs == KS_CONNECT) {
    if (!mayUseNetwork) {
      return;
    }
    if (!startConnect()) {
      noAnswer(now, "could not connect");
      return;
    }
  }
}
"""
OLD_SYNC_H = "bool kosyncReloadConfig(bool asked = true);"
OLD_BOOKS = """
BooksApp::BooksApp() { kosyncReloadConfig(true); }
void BooksApp::changeState() { kosyncReloadConfig(true); }
void BooksApp::applyPending() {
  savePosition(true);
  dropParkedForThisBook();
}
void BooksApp::other() {
  if (LOGIC_BUTTON_BACK(event)) {
    if (pendingIdx >= 0) {
      dropParkedForThisBook();
    }
  }
  kosyncCardAnswered(0);
}
"""
OLD_SERIAL = """
  if (!strcasecmp(line, "sip") || !strncasecmp(line, "sip ", 4)) {
    if (!strcasecmp(arg, "reload")) {
      sipReload();
    }
  }
  if (!strcasecmp(line, "kosync") || !strncasecmp(line, "kosync ", 7)) {
    if (!*arg || !strcasecmp(arg, "status")) {
      kosyncDumpStatus(sayLine);
    } else if (!strcasecmp(arg, "reload")) {
      kosyncReloadConfig();
      kosyncDumpStatus(sayLine);
    }
  }
"""


def load(texts):
    files = {}
    for name, raw in texts.items():
        files[name] = strip_code(raw)
        files[name + ".raw"] = raw
    return files


def selftest():
    got = check(load({SYNC: OLD_SYNC, SYNC_H: OLD_SYNC_H, BOOKS: OLD_BOOKS, SERIAL: OLD_SERIAL}))
    want = ["calls resolveDomain()", "calls hostByName()", "calls MDNS.queryHost()",
            "calls tcpip_callback()", "without MSG_DONTWAIT", "outside dnsStart()",
            "uses dnsStart other than", "the last argument must be 0", "default must be false",
            "must call kosyncReloadConfig(true)", "kosyncReloadConfig(true) - only serial",
            "new-window branch", "outside its new-window branch", "calls clearWindowProblems() - only",
            "reloadDone() must clear only", "only `if (asked)`", "pass `asked` through",
            "zeroes s_other/s_unauth", "must clear the window's PUT log",
            "with ONE kosyncOfferFill() call", "must ask kosyncParkPending()",
            "outside an `if (... !parkLive)`", "assigns myMovedAt", "notePlace(s_waitBook, ...,",
            "assigns offeredSig", "applyPending() must call kosyncCardAnswered",
            "Back branch must call kosyncCardAnswered", "only the two ANSWERS",
            "with kosyncPutLogOwn()", "with kosyncPutLogOther()",
            "kosyncProblemLine() must count", "parkAwaitsAnswer() must ask kosyncParkPending",
            "must refuse a job's first PUT", "BEFORE any kosyncWindowOpen()",
            "ONE `if (kosyncWindowWaitsForHome"]
    missing = [w for w in want if not any(w in g for g in got)]
    if missing:
        for w in missing:
            print(f"  SELF-TEST FAILED: the pre-fix shape did not trip '{w}'")
        return False
    print(f"  ok  self-test: the pre-fix shapes break all {len(want)} contract kinds")
    return True


# Each guard REMOVED from the real source (stripped text, as check() sees it) must trip its own
# contract - the review's experiment (resolveDomain back at the job's start, both Books re-reads
# made `true`) first. `nth` picks the occurrence. A pattern that no longer matches fails too: the
# guard was rewritten, so the mutation needs rewriting with it. serial_cmd.cpp's replacement keeps
# the text's length: its "reload" is found in the RAW text by offset.
MUTATIONS = [
    (SYNC, "hold an idle ask while a join is in progress",
     r"wifiJoinAgeMs\s*\(\s*now\s*,\s*lastWifiConnectAttemptMs\s*\(\s*\)\s*\)\s*<\s*KOSYNC_JOIN_WAIT_MS",
     "false", 0),
    # Review 3 (R3-3, KOS-N1a): the wait not ended by the Games-app hold; each state's give-up
    # ungated (RESOLVE, RETRY, CONNECT); KS_CONNECT's check moved past startConnect(); the game no
    # longer holding; the ask retired, or the wait started, where the job goes back to idle.
    (SYNC, "the join wait must end at the Games-app hold",
     r"(if\s*\(\s*!\s*mayUseNetwork\s*\)\s*\{)\s*s_joinWaitSinceMs\s*=\s*0\s*;", r"\1", 0),
    (SYNC, "KS_RESOLVE gives up off WiFi without heldOffWifi()",
     r"if\s*\(\s*!\s*heldOffWifi\s*\([^)]*\)\s*\)\s*\{\s*(finishJob\s*\([^;]*\)\s*;)\s*\}", r"\1", 0),
    (SYNC, "KS_RETRY gives up off WiFi without heldOffWifi()",
     r"if\s*\(\s*!\s*heldOffWifi\s*\([^)]*\)\s*\)\s*\{\s*(finishJob\s*\([^;]*\)\s*;)\s*\}", r"\1", 1),
    (SYNC, "KS_CONNECT gives up off WiFi without heldOffWifi()",
     r"if\s*\(\s*!\s*heldOffWifi\s*\([^)]*\)\s*\)\s*\{\s*(finishJob\s*\([^;]*\)\s*;)\s*\}", r"\1", 2),
    (SYNC, "KS_CONNECT must check onWifi() BEFORE startConnect()",
     r"(if\s*\(\s*!\s*onWifi\s*\(\s*\)\s*\)\s*\{\s*if[^{]*\{[^{}]*\}\s*return\s*;\s*\})(\s*)"
     r"(if\s*\(\s*!\s*mayUseNetwork\s*\)\s*\{[^{}]*\}\s*if\s*\(\s*!\s*s_phaseGet[^{]*\{[^{}]*\}\s*"
     r"if\s*\(\s*!\s*startConnect\s*\(\s*\)\s*\)\s*\{[^{}]*\})", r"\3\2\1", 0),
    (SYNC, "heldOffWifi() must hold while a game has the radio",
     r"mayUseNetwork\s*&&\s*(!\s*joinInProgress\s*\(\s*now\s*\))", r"\1", 0),
    (SYNC, "heldOffWifi() must drop the socket and go back to KS_IDLE KEEPING",
     r"dropSocket\s*\(\s*\)\s*;\s*s_cs\s*=\s*KS_IDLE\s*;\s*(return\s+true\s*;)", r"finishJob(   ); \1", 0),
    (SYNC, "starts the join wait outside",
     r"(s_cs\s*=\s*KS_IDLE\s*;\s*)(return\s+true\s*;)", r"\1s_joinWaitSinceMs = now; \2", 0),
    # Integration review 3 (R3-4b): the 60 s cap gone (`if (true)`, or OR-ed away), each of
    # joinInProgress()'s terms deleted, its chain OR-ed, the wait re-stamped every pass, and the
    # fall-through reset gone. Contract 11 as 9c1887a left it missed every one.
    (SYNC, "must hold (return) only under",
     r"if\s*\(\s*\(\s*uint32_t\s*\)\s*\(\s*now\s*-\s*s_joinWaitSinceMs\s*\)\s*<\s*KOSYNC_JOIN_WAIT_MAX_MS\s*\)",
     "if (true)", 0),
    (SYNC, "must hold (return) only under", r"(<\s*KOSYNC_JOIN_WAIT_MAX_MS)(\s*\))", r"\1 || true\2", 0),
    (SYNC, "lacks `!xferUsingAP()`", r"!\s*xferUsingAP\s*\(\s*\)\s*&&\s*(wifiStationWanted)", r"\1", 0),
    (SYNC, "lacks `wifiStationWanted()`", r"(!\s*xferUsingAP\s*\(\s*\)\s*&&\s*)wifiStationWanted\s*\(\s*\)\s*&&",
     r"\1", 0),
    (SYNC, "lacks `!onWifi()`", r"(T\s*->\s*cfg\s*\.\s*home\s*\[\s*0\s*\]\s*&&\s*)!\s*onWifi\s*\(\s*\)\s*&&",
     r"\1", 0),
    (SYNC, "lacks `lastWifiConnectAttemptMs() != 0`", r"lastWifiConnectAttemptMs\s*\(\s*\)\s*!=\s*0\s*&&", "", 0),
    (SYNC, "must be ONE `&&` chain", r"!\s*onWifi\s*\(\s*\)\s*&&\s*!\s*xferUsingAP", "!onWifi() || !xferUsingAP", 0),
    (SYNC, "must start only `if (!s_joinWaitSinceMs)`",
     r"if\s*\(\s*!\s*s_joinWaitSinceMs\s*\)\s*\{\s*(s_joinWaitSinceMs\s*=[^;]*;)", r"\1 {", 0),
    # R3-4's verification: the cap passed the comparison's form at any size - made huge through the
    # define (either one), or no longer above the join-age window
    (SYNC, "KOSYNC_JOIN_WAIT_MAX_MS (4294967295)", r"(#define\s+KOSYNC_JOIN_WAIT_MAX_MS\s+)\w+",
     r"\g<1>4294967295u", 0),
    (SYNC, "must be a plain decimal literal", r"(#define\s+KOSYNC_JOIN_WAIT_MAX_MS\s+)\w+", r"\g<1>0xFFFFFFFFu", 0),
    (SYNC, "KOSYNC_JOIN_WAIT_MS (4294967295)", r"(#define\s+KOSYNC_JOIN_WAIT_MS\s+)\w+", r"\g<1>4294967295u", 0),
    (SYNC, "KOSYNC_JOIN_WAIT_MS (30000) < KOSYNC_JOIN_WAIT_MAX_MS (20000)",
     r"(#define\s+KOSYNC_JOIN_WAIT_MAX_MS\s+)\w+", r"\g<1>20000u", 0),
    (SYNC, "must be #defined exactly once", r"(#define\s+KOSYNC_JOIN_WAIT_MAX_MS\s+\w+)",
     r"\1\n#undef KOSYNC_JOIN_WAIT_MAX_MS\n#define KOSYNC_JOIN_WAIT_MAX_MS 60000u", 0),
    (SYNC, "past the join wait the idle branch must zero it",
     r"(\}\s*)s_joinWaitSinceMs\s*=\s*0\s*;(\s*if\s*\(\s*!\s*T\s*->\s*cfg\s*\.\s*ok)", r"\1\2", 0),
    (SYNC, "calls resolveDomain()", r"(staSsid\s*\(\s*T\s*->\s*jobSsid\s*\)\s*;)",
     r"resolveDomain(T->cfg.home); \1", 0),
    (BOOKS, "kosyncReloadConfig(true) - only serial", r"kosyncReloadConfig\s*\(\s*\)",
     "kosyncReloadConfig(true)", 0),
    (BOOKS, "kosyncReloadConfig(true) - only serial", r"kosyncReloadConfig\s*\(\s*\)",
     "kosyncReloadConfig(true)", 1),
    (SERIAL, "must call kosyncReloadConfig(true)", r"kosyncReloadConfig\(true\)",
     "kosyncReloadConfig(    )", 0),
    (SYNC_H, "default must be false", r"(kosyncReloadConfig\s*\(\s*bool\s+\w+\s*=\s*)false", r"\1true", 0),
    (SYNC, "without MSG_DONTWAIT", r"(recvfrom\s*\([^;]*?)\bMSG_DONTWAIT\b", r"\g<1>0", 0),
    (SYNC, "the last argument must be 0",
     r"(tcpip_callback_with_block\s*\(\s*dnsStart\s*,[^;]*?,\s*)0(\s*\))", r"\g<1>1\2", 0),
    (SYNC, "uses dnsStart other than",
     r"tcpip_callback_with_block\s*\(\s*dnsStart\s*,\s*([^;]*?)\s*,\s*0\s*\)\s*==\s*ERR_OK",
     r"(dnsStart(\1), true)", 0),
    (SYNC, "new-window branch",
     r"(T\s*->\s*peer\s*\[\s*0\s*\]\s*=[^;]*;\s*)clearWindowProblems\s*\(\s*\)\s*;", r"\1", 0),
    (SYNC, "reloadDone() must clear only",
     r"if\s*\(\s*kosyncReloadClears\s*\([^;{]*\)\s*\)\s*\{\s*(clearWindowProblems\s*\(\s*\)\s*;)\s*\}",
     r"\1", 0),
    (SYNC, "pass `asked` through", r"reloadDone\s*\(\s*asked\s*\)", "reloadDone(true)", 1),
    (SYNC, "must clear the window's PUT log", r"memset\s*\(\s*&\s*T\s*->\s*puts\s*,[^;]*;", "", 0),
    # KS-1: the freshest stamp back in an automatic ask, two ways; the WiFi wait's stamp refreshed.
    (SYNC, "the explicit flag must be `thenSend`",
     r"(kosyncOfferFill\s*\([^;]*?)\bb\s*->\s*movedAt\b", r"\1me->movedAt", 0),
    (SYNC, "assigns myMovedAt", r"(const\s+int\s+v\s*=\s*kosyncOfferVerdict)",
     r"in.myMovedAt = me->movedAt; \1", 0),
    (SYNC, "notePlace(s_waitBook, ...,", r"(notePlace\s*\(\s*s_waitBook\s*,[^;]*?,\s*)0(\s*\)\s*;)",
     r"\1movedAt\2", 0),
    # KS-2: a park marked declined as it is made; Sync my place sending over a live park; either
    # answer to the card left out.
    (SYNC, "assigns offeredSig", r"(if\s*\(\s*kosyncPark\s*\(\s*b\s*,)", r"me->offeredSig = 1; \1", 0),
    (SYNC, "outside an `if (... !parkLive)`", r"if\s*\(\s*thenSend\s*&&\s*!\s*parkLive\s*\)",
     "if (thenSend)", 1),
    (SYNC, "must ask kosyncParkPending()",
     r"(parkLive\s*=[^;]*?kosyncParkPending\s*\([^,;]*,[^,;]*,\s*)key\b", r"\1NULL", 0),
    (BOOKS, "applyPending() must call kosyncCardAnswered", r"kosyncCardAnswered\s*\(\s*pendingId\s*\)\s*;",
     "", 0),
    (BOOKS, "Back branch must call kosyncCardAnswered", r"kosyncCardAnswered\s*\(\s*pendingId\s*\)\s*;",
     "", 1),
    # KS-3: the fork's own-book PUT not logged; the problem line back on the raw count.
    (SYNC, "with kosyncPutLogOwn()", r"kosyncPutLogOwn\s*\([^;]*\)\s*;", "", 0),
    (SYNC, "kosyncProblemLine() must count",
     r"=\s*kosyncPutLogDifferent\s*\(\s*&\s*T\s*->\s*puts\s*,\s*&\s*doc\s*,\s*NULL\s*\)",
     "= T->puts.nOther", 0),
    # 10: the close's push over a parked place - the guard gone, its "first PUT" half gone, the
    # key dropped, and the check moved in front of the hold.
    (SYNC, "must refuse a job's first PUT",
     r"if\s*\(\s*!\s*s_phaseGet\s*&&\s*s_w\s*->\s*sent\s*==\s*0\s*&&\s*parkAwaitsAnswer\s*\([^)]*\)\s*\)"
     r"\s*\{[^{}]*\}", "", 0),
    (SYNC, "must refuse a job's first PUT", r"(!\s*s_phaseGet\s*&&\s*)s_w\s*->\s*sent\s*==\s*0\s*&&\s*",
     r"\1", 0),
    (SYNC, "parkAwaitsAnswer() must ask kosyncParkPending",
     r"(kosyncParkPending\s*\(\s*&\s*T\s*->\s*ledger\s*,\s*byName\s*,\s*)key\b", r"\1NULL", 0),
    (SYNC, "must come AFTER `if (!mayUseNetwork)",
     r"(if\s*\(\s*!\s*mayUseNetwork\s*\)\s*\{[^{}]*\})(\s*)(if\s*\(\s*!\s*s_phaseGet[^{]*\{[^{}]*\})",
     r"\3\2\1", 0),
    # 12 (the plan's 5.2): the window opened before the home job (the pre-fix order, by hand); an
    # open added to the waiting branch; the ask no longer kept; the release gone from finishJob and
    # from the loop's timeout; the bound below "home did not answer", or huge through the define;
    # the held ask opened as a 60 s window.
    (SYNC, "BEFORE any kosyncWindowOpen()", r"(const\s+bool\s+wifi\s*=\s*onWifi\s*\(\s*\)\s*;)",
     r"\1 kosyncWindowOpen(b, KOSYNC_WINDOW_SYNC_MS, note, cap);", 0),
    (SYNC, "outside the not-waiting branch", r"(opened\s*=\s*true\s*;)",
     r"\1 kosyncWindowOpen(b, KOSYNC_WINDOW_SYNC_MS, note, cap);", 0),
    (SYNC, "waiting branch must keep the ask",
     r"&&\s*rememberWindowAfterHome\s*\(\s*b\s*,\s*millis\s*\(\s*\)\s*\)", "", 0),
    (SYNC, "finishJob() must release", r"(s_pushWant\s*=\s*false\s*;\s*)windowAfterHome\s*\(\s*\)\s*;",
     r"\1", 0),
    # ...and the idle branch's two releases, each on its own (the drop's snprintf/log_e between).
    (SYNC, "without releasing the held window",
     r"(s_pushWant\s*=\s*s_pullWant\s*=\s*false\s*;[^{}]*?)windowAfterHome\s*\(\s*\)\s*;", r"\1", 0),
    (SYNC, "without releasing the held window",
     r"(s_pushWant\s*=\s*s_pullWant\s*=\s*false\s*;[^{}]*?)windowAfterHome\s*\(\s*\)\s*;", r"\1", 1),
    (SYNC, "kosyncLoop() must release",
     r"if\s*\(\s*s_afterWant\s*&&[^{]*KOSYNC_WINDOW_AFTER_HOME_MAX_MS\s*\)\s*\{\s*windowAfterHome\s*\(\s*\)\s*;\s*\}",
     "", 0),
    (SYNC, "must cover 'home did not answer'", r"(#define\s+KOSYNC_WINDOW_AFTER_HOME_MAX_MS\s+)\w+",
     r"\g<1>5000u", 0),
    (SYNC, "must be a plain decimal literal", r"(#define\s+KOSYNC_WINDOW_AFTER_HOME_MAX_MS\s+)\w+",
     r"\g<1>0xFFFFFFFFu", 0),
    (SYNC, "requestWindow(s_afterBook, KOSYNC_WINDOW_SYNC_MS",
     r"requestWindow\s*\(\s*s_afterBook\s*,\s*KOSYNC_WINDOW_SYNC_MS",
     "requestWindow(s_afterBook, KOSYNC_WINDOW_OPEN_MS", 0),
]


def subn_nth(pat, repl, text, nth):
    ms = list(re.finditer(pat, text))
    if len(ms) <= nth:
        return text, 0
    m = ms[nth]
    return text[:m.start()] + m.expand(repl) + text[m.end():], 1


def mutation_test(files):
    ok = True
    for name, want, pat, repl, nth in MUTATIONS:
        mutated, n = subn_nth(pat, repl, files[name], nth)
        if n != 1:
            print(f"  SELF-TEST FAILED: the guard for '{want}' is no longer where this mutation "
                  f"looks in {name} - update MUTATIONS with the guard")
            ok = False
            continue
        got = check(dict(files, **{name: mutated}))
        if not any(want in g for g in got):
            print(f"  SELF-TEST FAILED: removing the guard from {name} (#{nth + 1}) did not trip '{want}'")
            ok = False
    if ok:
        print(f"  ok  self-test: removing each of the {len(MUTATIONS)} guards from the real source "
              "trips its contract (the review's experiment among them)")
    return ok


def main():
    if not selftest():
        return 1
    texts = {}
    for name in (SYNC, SYNC_H, BOOKS, SERIAL, KOSYNC, KOSYNC_H):
        texts[name] = (ROOT / name).read_text(errors="replace")
    # Every other caller of kosyncReloadConfig in the tree is held to "no argument" too.
    for p in sorted(list(ROOT.glob("*.cpp")) + list(ROOT.glob("*.ino"))):
        if p.name not in texts:
            raw = p.read_text(errors="replace")
            if "kosyncReloadConfig" in raw or "kosyncCardAnswered" in raw:
                texts[p.name] = raw
    files = load(texts)
    problems = check(files)
    for p in problems:
        print(f"  CONTRACT BROKEN: {p}")
    if problems:
        return 1
    if not mutation_test(files):
        return 1
    print("  ok  no blocking lookup in the home client; only serial `kosync reload` clears on "
          "asking; a new window, or a changed kosync.txt, clears the rest")
    print("  ok  KS-1 a home record is judged as of the ask; KS-2 declined = answered, and Sync "
          "my place sends nothing over a place on the card; KS-3 a second id is not another book")
    print("  ok  no push's first PUT goes out over a place from another device still waiting on "
          "the card (the close's push behind the pull on open)")
    print("  ok  N1: a WiFi join in progress, or a game holding the radio, keeps a home ask in every "
          "state; the wait is for ONE join (the Games-app hold ends it), capped at 120 s or less")
    print("  ok  5.2: Sync my place asks home before any window; the window opens on the job's verdict "
          "or after a bound that covers 'home did not answer'")
    return 0


if __name__ == "__main__":
    sys.exit(main())

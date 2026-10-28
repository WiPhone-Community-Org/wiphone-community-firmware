#!/usr/bin/env python3
"""check_health_log.py - the HEALTH line stays out of loop()'s frame, and WiFi link lines reach the
card at most one pair a minute (review F1 and F3, 0.9.79).

F1. The HEALTH line's buffers (hl[256], kl[176]) have their addresses passed to snprintf, so while
    they sat inline in loop() the compiler reserved them for the WHOLE pass - under every call
    loop() makes, the deepest being a Books picture page (~7.6 KB of the 8 KB loop task). objdump:
    loop() was 560 B on main, 640 B with this branch's longer hl, and is 240 B with the line built
    in healthLineTick(). That only holds while healthLineTick() is `noinline` (a static function
    with one caller is exactly what -Os inlines), while loop() declares no big buffer of its own,
    and while the calls that reach app redraws and the power-off saves (BATTERY_UPDATE_EVENT,
    powerOff) stay in loop() and out of the helper - inside it they would run on top of hl and kl.

F3. At the edge of an AP the core's auto-reconnect can associate and drop every 10-20 s, and each
    WIFI LOST / WIFI JOIN (and each MARK assoc/disassoc) was an SD open/append/close with no limit
    - a flapping hour pushed the boot reset_reason lines off a log that keeps ~9 h. WifiCardGate
    (wifi_diag.h, host-tested in tests/test_wifidiag.cpp) decides; these contracts pin that every
    such card write actually ASKS it: each wifiDiagDurable() call passes s_card.lost()/join(), the
    helper writes the card only under that answer, diagTick() writes no card line directly and
    wakes for the gate's summary, and the 2 s status poll goes through wifiMarkLink(), whose card
    write is gated the same way.

Integration review 3 (R3-4) dropped wifiMarkLink()'s `noinline` in a scratch copy and this file stayed
green - it pinned the attribute for healthLineTick() alone. Rebuilt that way, loop()'s frame is 288 B
instead of 240 (objdump, 2026-09-25: GCC inlines the one-caller helper and its line[144] with it), the
exact F1 cost this file exists to keep out. So NOINLINE lists every helper whose attribute keeps a
buffer out of a frame that sits under deep calls, and a general rule backs it: a `static` function in
WiPhone.ino that loop() calls DIRECTLY and that declares a buffer of BIG bytes or more, or a
CriticalFile, must be `__attribute__((noinline))` - a new helper is held to it without an edit here.
(callLevelsFlush() and Networks.cpp's wifiCardHeldLine() have two callers each, and the same rebuild
without their attribute still kept them out of line - measured; the attribute is what guarantees it
the day one caller goes.) GUI.cpp's drawRowTextCut() is on the list too: the same loop-task stack,
under every row of every list paint, and its attribute was just as unpinned.

That rule's verification found it reached only helpers loop() calls DIRECTLY: applyStoredCallVolumes()
holds a CriticalFile and is reached through callTakesAudioDevice(), which is itself inlined into loop()
(no symbol of its own), so a second-hand inline would have put the CriticalFile in loop()'s frame with
nothing to say so. Following calls is a reachability analysis a regex does badly (an inlined static in
between, a non-static function like healthLogLine() in between), so the rule is now FILE-WIDE: EVERY
`static` function of WiPhone.ino that declares a buffer of BIG bytes or more, or a CriticalFile, carries
noinline - wherever its callers are. All of WiPhone.ino but three tiny key/jack ISRs runs on the 8 KB
loop task, and -Os inlines a one-caller static as a matter of course. Today that is five functions; applyStoredCallVolumes()
and healthLogTrim() (buf[512], one caller: healthLogLine(), which every durable line - GUI's app-open
probe among them - goes through) gained the attribute with this, both already out of line (96 and 608 B
frames of their own, loop() still 240 B - objdump, 2026-09-26), so the build is unchanged.

Each is a POSITIVE contract; one that cannot find its function fails too. The self-test REMOVES
each guard from the real sources, one at a time, and requires its contract to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, ifs, match_close, strip_code  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"
FILES = ("WiPhone.ino", "Networks.cpp", "GUI.cpp")
BIG = 128   # a char/uint8_t array this size or more does not belong in loop()'s own frame

# (file, helper, what its noinline keeps out of which frame). Every one must be DEFINED with
# `__attribute__((noinline))` (and never `inline` / always_inline).
NOINLINE = [
    ("WiPhone.ino", "healthLineTick",
     "inlined, its hl/kl buffers are back in loop()'s frame (560/640 B -> 240 B was the F1 fix)"),
    ("WiPhone.ino", "wifiMarkLink",
     "inlined, its line[144] is in loop()'s frame: 240 -> 288 B, measured by objdump on a rebuild "
     "without it (review 3, R3-4)"),
    ("WiPhone.ino", "callLevelsFlush",
     "inlined, its CriticalFile sits in loop()'s frame under every call loop() makes (review 3, R3-1)"),
    ("Networks.cpp", "wifiCardHeldLine",
     "inlined, its l[160] joins its caller's frame (wifiDiagDurable(), or Networks::diagTick() at 416 B "
     "already) under the healthLogLine() SD write that follows it"),
    ("GUI.cpp", "drawRowTextCut",
     "inlined into drawRowText(), its 160-byte buffer is 224 B deeper for EVERY row of every list paint, "
     "on a loop task whose recorded floor is 408 B (review, 2026-09-19)"),
    ("WiPhone.ino", "applyStoredCallVolumes",
     "inlined into callTakesAudioDevice() - itself inlined into loop() - its CriticalFile is in loop()'s "
     "frame for the whole pass (review 3, R3-4 verification)"),
    ("WiPhone.ino", "healthLogTrim",
     "inlined into healthLogLine(), its buf[512] is in that frame on EVERY durable line (the app-open "
     "probe's under an app's constructor among them), not just the one in ~9 hours that trims"),
]
BUFFER = re.compile(r"\b(?:char|uint8_t|int8_t|byte)\s+(\w+)\s*\[\s*(\d+)\s*\]")
CRITICAL = re.compile(r"\bCriticalFile\s+\w+\s*[(;{]")
NOINLINE_ATTR = re.compile(r"__attribute__\s*\(\(\s*noinline\s*\)\)")
STATIC_LOCAL = re.compile(r"\bstatic\s+(?:const\s+)?$")     # a static local is not in the frame


def span(code, name):
    return function_body(code, name)


def def_head(code, name):
    """The declaration text in front of the DEFINITION of `name` (return type, storage class,
    attributes), from the previous `;` or `}` up to the name. None when there is no definition."""
    bs = function_body(code, name)
    if bs is None:
        return None
    for m in re.compile(r"(?<![\w:~.>])" + re.escape(name) + r"\s*\(").finditer(code, 0, bs[0]):
        close = match_close(code, m.end() - 1)
        if close < 0 or code.find("{", close) != bs[0]:
            continue
        begin = max(code.rfind(";", 0, m.start()), code.rfind("}", 0, m.start())) + 1
        return code[begin:m.start()]
    return None


def noinline_problem(code, name, why):
    head = def_head(code, name)
    if head is None:
        return f"{name}() not found - if it was renamed or moved, update NOINLINE"
    if not NOINLINE_ATTR.search(head) or re.search(r"\binline\b|\balways_inline\b", head):
        return f"{name}() must be defined `static ... __attribute__((noinline))` - {why}"
    return None


def static_helper_problems(ino):
    """The general rule (FILE-WIDE since R3-4's verification): EVERY `static` function of WiPhone.ino
    that declares a buffer of BIG bytes or more, or a CriticalFile, carries noinline. -Os inlines a
    static function with one caller as a matter of course, and its locals then sit in the caller's
    frame for the caller's WHOLE body - loop()'s, under every app redraw it reaches (the deepest a
    Books picture page), when the caller is loop() or anything inlined into it. Whether a helper is
    reached from loop() directly, through an inlined static (callTakesAudioDevice), or through a
    non-static function (healthLogLine) is not asked: all of this file but three tiny ISRs runs on the
    8 KB loop task."""
    out = []
    for name in sorted(set(re.findall(r"(?<![\w:.>])(\w+)\s*\(", ino))):
        head = def_head(ino, name)
        if head is None or not re.search(r"\bstatic\b", head):
            continue
        a, z = span(ino, name)
        bufs = [f"{m.group(1)}[{m.group(2)}]" for m in BUFFER.finditer(ino, a, z)
                if int(m.group(2)) >= BIG and not STATIC_LOCAL.search(ino, max(a, m.start() - 24), m.start())]
        if CRITICAL.search(ino, a, z):
            bufs.append("a CriticalFile")
        if bufs and (not NOINLINE_ATTR.search(head) or re.search(r"\binline\b|\balways_inline\b", head)):
            out.append(f"{name}() is a static function of WiPhone.ino, and it declares {', '.join(bufs)} - it "
                       "must be defined `static ... __attribute__((noinline))` or those bytes sit in its "
                       "caller's frame (loop()'s, directly or through an inlined helper) for the caller's "
                       "whole body (review F1; file-wide since review 3's R3-4 verification)")
    return out


def check(f):
    bad = []
    ino, net = f["WiPhone.ino"], f["Networks.cpp"]

    # ── F1: the HEALTH line's buffers live in a noinline helper, and so do the other loop helpers'
    # (review 3, R3-4: wifiMarkLink's attribute was unpinned, and dropping it cost loop() 48 B) ──
    for fname, name, why in NOINLINE:
        p = noinline_problem(f[fname], name, why)
        if p:
            bad.append(p)
    bad.extend(static_helper_problems(ino))
    lp = span(ino, "loop")
    hb = span(ino, "healthLineTick")
    if lp is None or hb is None:
        bad.append("loop() or healthLineTick() not found")
    else:
        a, z = lp
        for m in re.finditer(r"\b(?:char|uint8_t|int8_t|byte)\s+(\w+)\s*\[\s*(\d+)\s*\]", ino[a:z]):
            if int(m.group(2)) >= BIG:
                bad.append(f"loop() declares {m.group(1)}[{m.group(2)}] - a buffer that size reserves "
                           "its bytes in loop()'s frame under every call (the picture-page chain); "
                           "build it in a noinline helper like healthLineTick()")
        if not re.search(r"\bhealthLineTick\s*\(", ino[a:z]):
            bad.append("loop() no longer calls healthLineTick()")
        for what, pat in (("BATTERY_UPDATE_EVENT", r"\bBATTERY_UPDATE_EVENT\b"),
                          ("the low-battery powerOff()", r"\bpowerOff\s*\(")):
            if not re.search(pat, ino[a:z]):
                bad.append(f"loop() must keep {what} itself")
        h0, h1 = hb
        for what, pat in (("processEvent()", r"\bprocessEvent\s*\("), ("powerOff()", r"\bpowerOff\s*\("),
                          ("the power-off saves", r"\b(?:booksSaveOpenPosition|mapsSaveOpenView|gbcSaveForPowerOff)\s*\(")):
            if re.search(pat, ino[h0:h1]):
                bad.append(f"healthLineTick() calls {what} - app redraws and the saves would run on "
                           "top of hl and kl; keep them in loop()")

    # ── F3: every WiFi link line asks the card gate ───────────────────────────────────────────
    dd = span(net, "wifiDiagDurable")
    if dd is None:
        bad.append("wifiDiagDurable() not found")
    else:
        a, z = dd
        hl = [m.start() for m in re.finditer(r"\bhealthLogLine\s*\(", net[a:z])]
        gated = ifs(net, a, z, re.compile(r"\bcard\b"))
        if not hl or any(not any(g[1] <= a + p <= g[2] for g in gated) for p in hl):
            bad.append("wifiDiagDurable() must write the card only under `if (card ...)`")
    uses = list(re.finditer(r"\bwifiDiagDurable\s*\(\s*line\s*,\s*([^,]+),", net))
    if len(uses) < 3:
        bad.append(f"only {len(uses)} wifiDiagDurable() call(s) - the LOST, JOIN and radio-stopped "
                   "LOST lines all go through it")
    for u in uses:
        if not re.fullmatch(r"\s*s_card\s*\.\s*(?:lost|join)\s*\(\s*e\s*\.\s*ms\s*\)\s*", u.group(1)):
            bad.append(f"wifiDiagDurable(line, {u.group(1).strip()}, ...) - the card answer must come "
                       "from s_card.lost()/join() (one pair a minute)")
    dt = span(net, "Networks::diagTick")
    if dt is None:
        bad.append("Networks::diagTick() not found")
    else:
        a, z = dt
        if re.search(r"\bhealthLogLine\s*\(", net[a:z]):
            bad.append("diagTick() writes healthLogLine() directly - only through the card gate")
        early = [b for b in ifs(net, a, z, re.compile(r"\bhead\s*==\s*s_next\b"))
                 if re.search(r"\breturn\b", net[b[1]:b[2] + 1])]
        if not early or not re.search(r"summaryDue\s*\(", net[early[0][0]:early[0][1]]):
            bad.append("diagTick()'s early return must also wait for s_card.summaryDue() - else a "
                       "storm's end never reaches the card")
        sm = [b for b in ifs(net, a, z, re.compile(r"\bs_card\s*\.\s*summaryDue\s*\("))
              if re.search(r"\bwifiCardHeldLine\s*\(", net[b[1]:b[2] + 1])]
        if not sm:
            bad.append("diagTick() must write the held count (wifiCardHeldLine) when summaryDue()")

    # the MARK assoc/disassoc lines
    if re.search(r"\bheapEvent\s*\(\s*ws\s*==", ino):
        bad.append("WiPhone.ino: the status poll calls heapEvent() again - every status change to the "
                   "card, ungated; it goes through wifiMarkLink()")
    if not re.search(r"\bwifiMarkLink\s*\(\s*s_lastWifiStatus\s*,\s*ws\s*,", ino):
        bad.append("WiPhone.ino: the 2 s status poll must call wifiMarkLink(s_lastWifiStatus, ws, now)")
    mk = span(ino, "wifiMarkLink")
    if mk is None:
        bad.append("wifiMarkLink() not found")
    else:
        a, z = mk
        if not re.search(r"\bcard\s*=\s*edge\s*&&\s*\(\s*assoc\s*\?\s*s_gate\s*\.\s*join\s*\(\s*now\s*\)"
                         r"\s*:\s*s_gate\s*\.\s*lost\s*\(\s*now\s*\)\s*\)", ino[a:z]):
            bad.append("wifiMarkLink(): `card` must be `edge && (assoc ? s_gate.join(now) : s_gate.lost(now))`")
        hl = [m.start() for m in re.finditer(r"\bhealthLogLine\s*\(", ino[a:z])]
        gated = [g for g in ifs(ino, a, z, re.compile(r"\bcard\b"))
                 if re.fullmatch(r"\s*card\s*", ino[ino.index("(", g[0]) + 1:ino.rindex(")", g[0], g[1] + 1)])]
        if not hl or any(not any(g[1] <= a + p <= g[2] for g in gated) for p in hl):
            bad.append("wifiMarkLink() must write the card only under `if (card)`")
    return bad


MUTATIONS = [
    ("WiPhone.ino", "must be defined `static ... __attribute__((noinline))`",
     r"static void __attribute__\(\(noinline\)\) healthLineTick\(", "static void healthLineTick("),
    ("WiPhone.ino", "loop() declares hl[256]",
     r"healthLineTick\(now, soc, v\);", "char hl[256]; healthLineTick(now, soc, v);"),
    ("WiPhone.ino", "loop() must keep BATTERY_UPDATE_EVENT",
     r"redrawWhat \|= gui\.processEvent\(now, BATTERY_UPDATE_EVENT\);", "(void)0;"),
    ("WiPhone.ino", "healthLineTick() calls processEvent()",
     r"  log_e\(\"%s\", hl\);\n", "  log_e(\"%s\", hl);\n  gui.processEvent(now, BATTERY_UPDATE_EVENT);\n"),
    ("Networks.cpp", "the card answer must come from s_card",
     r"wifiDiagDurable\(line, s_card\.lost\(e\.ms\), e\.ms\);", "wifiDiagDurable(line, true, e.ms);"),
    ("Networks.cpp", "the card answer must come from s_card",
     r"wifiDiagDurable\(line, s_card\.join\(e\.ms\), e\.ms\);", "wifiDiagDurable(line, true, e.ms);"),
    ("Networks.cpp", "wifiDiagDurable() must write the card only under",
     r"if \(card && !gGbcActive\) \{\n    healthLogLine\(line\);", "if (!gGbcActive) {\n    healthLogLine(line);"),
    ("Networks.cpp", "early return must also wait",
     r" &&\n      !s_card\.summaryDue\(now\)\) \{", ") {"),
    ("Networks.cpp", "held count (wifiCardHeldLine) when summaryDue",
     r"wifiCardHeldLine\(now, s_linkView \? \"UP\" : \"down\"\);", "(void)0;"),
    ("WiPhone.ino", "the status poll calls heapEvent() again",
     r"wifiMarkLink\(s_lastWifiStatus, ws, now\);", "heapEvent(ws == WL_CONNECTED ? \"assoc\" : \"disassoc\");"),
    ("WiPhone.ino", "wifiMarkLink(): `card` must be",
     r"const bool card = edge && \(assoc \? s_gate\.join\(now\) : s_gate\.lost\(now\)\);", "const bool card = edge;"),
    ("WiPhone.ino", "wifiMarkLink() must write the card only under",
     r"  if \(card\) \{\n    healthLogLine\(line\);\n  \}\n\}", "  healthLogLine(line);\n}"),
    # ── integration review 3 (R3-4c): the other helpers' attribute, which nothing pinned ──
    ("WiPhone.ino", "wifiMarkLink() must be defined `static ... __attribute__((noinline))`",
     r"static void __attribute__\(\(noinline\)\) wifiMarkLink\(", "static void wifiMarkLink("),
    ("WiPhone.ino", "wifiMarkLink() must be defined `static ... __attribute__((noinline))`",
     r"static void __attribute__\(\(noinline\)\) wifiMarkLink\(",
     "static inline void __attribute__((always_inline)) wifiMarkLink("),
    ("WiPhone.ino", "callLevelsFlush() must be defined `static ... __attribute__((noinline))`",
     r"static void __attribute__\(\(noinline\)\) callLevelsFlush\(", "static void callLevelsFlush("),
    ("Networks.cpp", "wifiCardHeldLine() must be defined `static ... __attribute__((noinline))`",
     r"static void __attribute__\(\(noinline\)\) wifiCardHeldLine\(", "static void wifiCardHeldLine("),
    ("GUI.cpp", "drawRowTextCut() must be defined `static ... __attribute__((noinline))`",
     r"static void __attribute__\(\(noinline\)\) drawRowTextCut\(", "static void drawRowTextCut("),
    # the general rule on its own: a helper loop() calls, not in NOINLINE, given a big buffer
    ("WiPhone.ino", "notifyMessageArrived() is a static function of WiPhone.ino",
     r"static void notifyMessageArrived\(uint8_t mode\) \{\n",
     "static void notifyMessageArrived(uint8_t mode) {\n  char scratch[200];\n  (void)scratch;\n"),
    # ── R3-4's verification: the helpers loop() reaches SECOND-HAND. Each attribute dropped (the rule and
    # the list both see it), and the rule alone on a static loop() never calls: callTakesAudioDevice(),
    # inlined into loop(), given a buffer of its own ──
    ("WiPhone.ino", "applyStoredCallVolumes() must be defined `static ... __attribute__((noinline))`",
     r"static void __attribute__\(\(noinline\)\) applyStoredCallVolumes\(", "static void applyStoredCallVolumes("),
    ("WiPhone.ino", "applyStoredCallVolumes() is a static function of WiPhone.ino",
     r"static void __attribute__\(\(noinline\)\) applyStoredCallVolumes\(", "static void applyStoredCallVolumes("),
    ("WiPhone.ino", "healthLogTrim() must be defined `static ... __attribute__((noinline))`",
     r"static bool __attribute__\(\(noinline\)\) healthLogTrim\(", "static bool healthLogTrim("),
    ("WiPhone.ino", "healthLogTrim() is a static function of WiPhone.ino",
     r"static bool __attribute__\(\(noinline\)\) healthLogTrim\(", "static inline bool healthLogTrim("),
    ("WiPhone.ino", "callTakesAudioDevice() is a static function of WiPhone.ino",
     r"static void callTakesAudioDevice\(const char\* who, bool readStoredLevels\) \{\n",
     "static void callTakesAudioDevice(const char* who, bool readStoredLevels) {\n  char why[160];\n  (void)why;\n"),
]


def selftest():
    """The general rule on synthetic text: a static function with a big buffer or a CriticalFile and
    no noinline is reported - called by loop(), by another static (the second-hand case R3-4's
    verification found), or by nothing at all; with noinline, a small buffer, a static local, or not
    static it is not."""
    ok = True
    src = ("static void %s helper(uint32_t now) {\n %s\n}\n"
           "static void small() {\n char few[64];\n}\n"
           "void loop() {\n helper(now);\n small();\n}\n")
    for attr, local, flagged in (("", "char b[200];", True), ("__attribute__((noinline))", "char b[200];", False),
                                 ("", "char b[64];", False), ("", "static char b[512];", False),
                                 ("", "CriticalFile ini(Storage::ConfigsFile);", True),
                                 ("__attribute__((noinline))", "CriticalFile ini(Storage::ConfigsFile);", False)):
        got = static_helper_problems(strip_code(src % (attr, local)))
        if bool(got) != flagged or any("small()" in g for g in got):
            print(f"  SELF-TEST FAILED: the static-helper rule on `{attr or 'no attribute'}` + `{local}` "
                  f"should {'report' if flagged else 'pass'} (got {got})")
            ok = False
    # second-hand: loop() -> an inlined static with nothing big -> the static with the CriticalFile
    second = ("static void levels(const char* who) {\n CriticalFile ini(Storage::ConfigsFile);\n}\n"
              "static void takes(const char* who) {\n levels(who);\n}\n"
              "void notLoop() {\n char big[512];\n}\n"
              "void loop() {\n takes(\"call\");\n}\n")
    got = static_helper_problems(strip_code(second))
    if not any("levels()" in g for g in got) or any("takes()" in g or "notLoop()" in g for g in got):
        print(f"  SELF-TEST FAILED: the rule must report the second-hand static levels() and nothing else "
              f"(a non-static function is not inlined away) (got {got})")
        ok = False
    if noinline_problem(strip_code("static inline void __attribute__((always_inline)) f() {\n}\n"), "f", "") is None:
        print("  SELF-TEST FAILED: always_inline taken for noinline")
        ok = False
    if noinline_problem(strip_code("// __attribute__((noinline))\nstatic void f() {\n}\n"), "f", "") is None:
        print("  SELF-TEST FAILED: a noinline that is only a comment was taken for the attribute")
        ok = False
    if ok:
        print("  ok  self-test: the static-helper rule reports a big buffer or a CriticalFile without "
              "noinline - called by loop() or second-hand - and passes noinline, a small buffer, a static "
              "local and a non-static function")
    return ok


def mutation_test(files):
    ok = True
    for mut in MUTATIONS:
        name, want, pat, repl = mut[:4]
        raw = files[name + ".raw"]
        ms = list(re.finditer(pat, raw))
        if not ms:
            print(f"  SELF-TEST FAILED: the '{want}' guard is no longer where this mutation looks "
                  f"in {name} - update MUTATIONS with the guard")
            ok = False
            continue
        m = ms[0]
        mutated = raw[:m.start()] + repl + raw[m.end():]
        got = check(dict(files, **{name: strip_code(mutated), name + ".raw": mutated}))
        if not any(want in g for g in got):
            print(f"  SELF-TEST FAILED: removing the guard from {name} did not trip '{want}' (got {got})")
            ok = False
    if ok:
        print(f"  ok  self-test: removing each of the {len(MUTATIONS)} guards from the real "
              "source trips its contract")
    return ok


def main():
    if not selftest():
        return 1
    files = {}
    for name in FILES:
        raw = (ROOT / name).read_text(errors="replace")
        files[name] = strip_code(raw)
        files[name + ".raw"] = raw
    problems = check(files)
    for p in problems:
        print(f"  CONTRACT BROKEN: {p}")
    if problems:
        return 1
    if not mutation_test(files):
        return 1
    print("  ok  the HEALTH buffers stay out of loop()'s frame (noinline helper, the redraw/power-off "
          "calls kept in loop), and every WIFI LOST/JOIN and MARK assoc/disassoc card write asks the gate")
    print(f"  ok  the {len(NOINLINE)} noinline helpers keep their attribute, and every static function of "
          f"WiPhone.ino with a buffer of {BIG}+ bytes or a CriticalFile has one - loop()'s own or second-hand "
          f"(review 3, R3-4)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

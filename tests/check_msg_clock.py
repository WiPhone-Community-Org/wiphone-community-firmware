#!/usr/bin/env python3
"""check_msg_clock.py - the SIP message store never keeps a MESH clock's time (review SA-3, 0.9.79).

A mesh-set clock is KNOWN but not TRUSTED (clock_source.h): it may be SHOWN, and message times
follow it on purpose (the time-lens review's decision, 87393a6 - a woods trip's texts must not sit
at the unknown-time sentinel, pinned newest). But the SIP store WRITES its times to SPIFFS, and a
wrong mesh time - AHEAD to 2079 by the AES-CTR replay the header describes - used to stay there for
good, AND used up the loop's one forced repair, so the first NTP answer fixed nothing.

The fix is provisional stamps: whatever the store takes off a mesh clock is marked "tm" (the mesh
set's random id), and the first load under NTP/GPS moves it by the offset that replacement found
(clockMsgFinal / clockMeshCorrS, host-tested in tests/test_clocksrc.cpp). test_clocksrc proves the
arithmetic; it proves NOTHING about the callers, and every piece that makes the fix real is glue in
files the host suite cannot compile (Storage.cpp, GUI.cpp, WiPhone.ino, clock.cpp):

  1. every Messages::load() is given ntpClock.msgStamp() - never `isTimeKnown() ? time : 0`;
  2. Messages::load() takes a ClockMsgStamp, finalises ONLY under a trusted clock
     (`if (unixTime && !st.meshId)`), and does so AFTER the sentinel repair: they share one
     band of stamps, and the sentinels (THIS boot's texts) must take its top, an earlier boot's
     re-stamps the band under it (the misc repair round);
  3. the sentinel repair marks what it stamps under a mesh clock ("tm", and the index row);
  4. saveMessage() marks a meshId stamp and its index row, and never marks a sentinel;
  5. the phone's own stamps (a SIP arrival, a composed text) pass msgStamp()'s meshId;
  6. the loop's forced reload stays armed until the clock is TRUSTED;
  7. clock.cpp records the offset before NTP or GPS overwrites a mesh clock, and a mesh set takes
     a fresh random id;
  8. the provisional pass never reads a stamp with getHexValueSafe() (strtol into a 32-bit long
     saturates at 2038 - the 2079 stamp this exists for would read as 0x7fffffff);
  9. what the finalise pass RE-STAMPS gets clockMsgRestamp()'s stamps from `bandEnd`, and nothing
     else: every "t" it writes is either clockMsgFinal's `fin` or clockMsgRestamp's `out[...]`,
     and `bandEnd` is lowered after. The pass once walked the file handing out ASCENDING stamps
     - the sentinel pass's rule, right only for equal ffffffff keys - and a partition is
     NEWEST-FIRST, so the newest text got the earliest stamp and the thread was reversed for
     good (the misc repair round; clockMsgRestamp's ordering is host-tested in test_clocksrc).

Each is a POSITIVE contract; one that cannot find its function fails too. The self-test then
REMOVES each guard from the real sources, one at a time, and requires its contract to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, ifs, strip_code  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"
FILES = ("Storage.cpp", "Storage.h", "GUI.cpp", "WiPhone.ino", "sms_mirror_rx.cpp", "clock.cpp",
         "serial_cmd.cpp")


def body(code, name):
    b = function_body(code, name)
    return None if b is None else (b[0], b[1])


def raw_in_code(raw, code, start, end, pat):
    """A match of `pat` in raw[start:end] that is CODE (strip_code keeps offsets and blanks
    comments and literals, so a match whose first character survived stripping is code)."""
    for m in re.compile(pat).finditer(raw, start, end):
        if code[m.start()] == raw[m.start()] and not code[m.start()].isspace():
            return m
    return None


def ifs_exact(code, start, end, cond_full):
    """ifs() whose WHOLE condition fullmatches `cond_full` (ifs() searches inside the condition,
    and `^` does not anchor at a search position)."""
    out = []
    for b in ifs(code, start, end, re.compile(r"\S")):
        po = code.index("(", b[0])
        if re.fullmatch(cond_full, code[po + 1:code.rindex(")", b[0], b[1] + 1)]):
            out.append(b)
    return out


def calls(code, pat):
    """(start, text) of every call matching `pat` (ending at its '('), with its argument text."""
    out = []
    for m in re.compile(pat).finditer(code):
        depth, j = 0, m.end() - 1
        while j < len(code):
            if code[j] == "(":
                depth += 1
            elif code[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append((m.start(), code[m.end():j]))
    return out


def check(f):
    """f: {name: stripped, name + '.raw': raw}. Returns the broken contracts."""
    bad = []

    # 1. every load() gets msgStamp()
    n = 0
    for name in FILES:
        if name.startswith("Storage"):
            continue
        for _, args in calls(f[name], r"\bmessages\s*\.\s*load\s*\("):
            n += 1
            if not re.fullmatch(r"\s*ntpClock\s*\.\s*msgStamp\s*\(\s*\)\s*", args):
                bad.append(f"{name}: messages.load({args.strip()}) - every load takes "
                           "ntpClock.msgStamp(), so a mesh clock's repair is provisional")
    if n < 3:
        bad.append(f"only {n} messages.load() call(s) found (GUI::reloadMessages, MessagesApp, the "
                   "mirror ingest) - the contract cannot see them")

    # 2. load(): the signature, the trusted-only finalise, after the sentinel pass
    st, sr = f["Storage.cpp"], f["Storage.cpp.raw"]
    if not re.search(r"\bbool\s+load\s*\(\s*const\s+ClockMsgStamp\s*&", f["Storage.h"]):
        bad.append("Storage.h: Messages::load must take `const ClockMsgStamp&`")
    lb = body(st, "Messages::load")
    if lb is None:
        bad.append("Messages::load() not found")
    else:
        a, z = lb
        fin = ifs(st, a, z, re.compile(r"\bunixTime\b[^)]*!\s*st\s*\.\s*meshId\b"))
        fin = [b for b in fin if re.search(r"\bclockMsgFinal\s*\(", st[b[1]:b[2]])]
        if not fin:
            bad.append("load(): the provisional stamps must be finalised only inside "
                       "`if (unixTime && !st.meshId)` - never under the mesh clock they may be wrong by")
        all_final = [m.start() for m in re.finditer(r"\bclockMsgFinal\s*\(", st[a:z])]
        if fin and any(not (fin[0][1] <= a + p <= fin[0][2]) for p in all_final):
            bad.append("load(): a clockMsgFinal() outside the trusted-only block")
        sent = ifs_exact(st, a, z, r"\s*unixTime\s*")
        if not sent:
            bad.append("load(): the sentinel repair `if (unixTime)` block not found")
        elif fin and fin[0][0] < sent[0][0]:
            bad.append("load(): finalise AFTER the sentinel repair - they share one band of stamps, "
                       "and this boot's sentinels take its top, an earlier boot's re-stamps the band "
                       "under it")
        # 9. the finalise pass re-stamps only through clockMsgRestamp, from bandEnd
        if fin:
            f0, f1 = fin[0][1], fin[0][2]
            rs = [args for p, args in calls(st, r"\bclockMsgRestamp\s*\(") if f0 <= p <= f1]
            if not any(re.search(r"\bbandEnd\b", x) for x in rs):
                bad.append("load(): the finalise pass must re-stamp through clockMsgRestamp(..., "
                           "bandEnd, out) - walking the NEWEST-FIRST file with ascending stamps "
                           "reverses the thread")
            writes = 0
            for m in re.compile(r'putValueFullHex\s*\(\s*"t"\s*,').finditer(sr, f0, f1 + 1):
                if st[m.start()] != sr[m.start()]:
                    continue                      # in a comment
                writes += 1
                depth, j = 1, m.end()
                while j < len(sr) and depth:
                    depth += {"(": 1, ")": -1}.get(sr[j], 0)
                    j += 1
                arg = sr[m.end():j - 1]
                if not re.fullmatch(r"\s*(fin|out\s*\[[^\]]*\])\s*", arg):
                    bad.append(f'load(): the finalise pass writes "t" = `{arg.strip()}` - only '
                               "clockMsgFinal's `fin` or clockMsgRestamp's `out[...]` keep the "
                               "order the texts came in")
            if writes < 2:
                bad.append(f'load(): {writes} "t" write(s) in the finalise pass - expected the '
                           "final stamp and the re-stamp")
            call = re.compile(r"\bclockMsgRestamp\s*\(").search(st, f0, f1)
            low = re.compile(r"\bbandEnd\s*-=").search(st, f0, f1)
            if not call or not low or low.start() < call.start():
                bad.append("load(): the finalise pass must lower bandEnd after its re-stamp, so no "
                           "stamp is handed out twice")
        # 3. the sentinel repair marks under a mesh clock
        if sent:
            s0, s1 = sent[0][1], sent[0][2]
            mk = ifs_exact(st, s0, s1, r"\s*st\s*\.\s*meshId\s*")
            if not any(raw_in_code(sr, st, b[1], b[2] + 1,
                                   r'putValueFullHex\s*\(\s*"tm"\s*,\s*st\s*\.\s*meshId\s*\)') for b in mk):
                bad.append('load(): the sentinel repair must mark its stamps under a mesh clock '
                           '(`if (st.meshId) putValueFullHex("tm", st.meshId)`)')
            if not any(raw_in_code(sr, st, b[1], b[2] + 1, r'\(\s*\*\s*ipart\s*\)\s*\[\s*"tm"\s*\]\s*=')
                       for b in mk):
                bad.append('load(): the sentinel repair must mark the index row ("tm") under a mesh '
                           'clock - the provisional pass reads only marked rows')
        # 8. no getHexValueSafe in load()
        if re.search(r"\bgetHexValueSafe\s*\(", st[a:z]):
            bad.append("load(): getHexValueSafe() reads a stamp through strtol - it saturates at "
                       "2038 (use msgHexStamp)")

    # 4. saveMessage(): marks a meshId stamp and its row, never a sentinel
    sb = body(st, "Messages::saveMessage")
    if sb is None:
        bad.append("Messages::saveMessage() not found")
    else:
        a, z = sb
        mk = ifs_exact(st, a, z, r"\s*meshId\s*")
        if not any(raw_in_code(sr, st, b[1], b[2] + 1, r'putValueFullHex\s*\(\s*"tm"\s*,\s*meshId\s*\)')
                   for b in mk):
            bad.append('saveMessage(): `if (meshId)` must write "tm" onto the message')
        if not any(raw_in_code(sr, st, b[1], b[2] + 1, r'index\s*\[\s*partPos\s*\]\s*\[\s*"tm"\s*\]\s*=')
                   for b in mk):
            bad.append('saveMessage(): `if (meshId)` must mark the index row ("tm")')
        zero = ifs_exact(st, a, z, r"\s*!\s*time\s*")
        if not zero or not re.search(r"\bmeshId\s*=\s*0\s*;", st[zero[0][1]:zero[0][2]]):
            bad.append("saveMessage(): the unknown-time sentinel must clear meshId - a sentinel is "
                       "repaired, not finalised")

    # 5. the phone's own stamps pass meshId
    for name, what in (("WiPhone.ino", "a SIP arrival"), ("GUI.cpp", "a composed text")):
        cs = calls(f[name], r"\bmessages\s*\.\s*saveMessage\s*\(")
        if not cs:
            bad.append(f"{name}: no saveMessage() call found ({what})")
        for _, args in cs:
            if not re.search(r"\b\w+\s*\.\s*meshId\b", args):
                bad.append(f"{name}: saveMessage() for {what} must pass msgStamp()'s meshId")

    # 6. the loop's forced reload stays armed until trusted
    ino = f["WiPhone.ino"]
    if re.search(r"\bwaitingForClockUpdate\s*=\s*false\b", ino):
        bad.append("WiPhone.ino: `waitingForClockUpdate = false` disarms the reload on a MESH clock - "
                   "it must be `= !ntpClock.isTimeTrusted()`")
    if not re.search(r"\bwaitingForClockUpdate\s*=\s*!\s*ntpClock\s*\.\s*isTimeTrusted\s*\(\s*\)", ino):
        bad.append("WiPhone.ino: the forced message reload must stay armed until the clock is trusted")

    # 7. clock.cpp: the offset before the overwrite, a fresh id per mesh set
    ck = f["clock.cpp"]
    ab = body(ck, "Clock::applyLocked")
    if ab is None:
        bad.append("Clock::applyLocked() not found")
    else:
        a, z = ab
        g = ifs(ck, a, z, re.compile(r"\bclockSourceTrusted\s*\(\s*src\s*\)"))
        g = [b for b in g if re.search(r"\bnoteMeshReplacedLocked\s*\(", ck[b[1]:b[2]])]
        ov = re.compile(r"\bsource\s*=\s*src\b").search(ck, a, z)
        if not g or not ov or g[0][0] > ov.start():
            bad.append("applyLocked(): a trusted set must noteMeshReplacedLocked() BEFORE `source = src`")
    ub = body(ck, "Clock::update")
    if ub is None:
        bad.append("Clock::update() not found")
    else:
        a, z = ub
        nm = re.compile(r"\bnoteMeshReplacedLocked\s*\(").search(ck, a, z)
        ov = re.compile(r"\bsource\s*=\s*CLOCK_SRC_NTP\b").search(ck, a, z)
        if not nm or not ov or nm.start() > ov.start():
            bad.append("Clock::update(): NTP must noteMeshReplacedLocked() BEFORE `source = CLOCK_SRC_NTP`")
    mb = body(ck, "Clock::meshPositionTime")
    if mb is None:
        bad.append("Clock::meshPositionTime() not found")
    else:
        a, z = mb
        rid = re.compile(r"\bmeshSetId\s*=\s*esp_random\s*\(").search(ck, a, z)
        ap = re.compile(r"\bapplyLocked\s*\(\s*CLOCK_SRC_MESH\b").search(ck, a, z)
        if not rid or not ap or rid.start() > ap.start():
            bad.append("meshPositionTime(): a mesh set must take a fresh random meshSetId before applyLocked()")
    return bad


# (file, contract text that must appear, pattern, replacement[, nth])
MUTATIONS = [
    ("GUI.cpp", "every load takes",
     r"flash\.messages\.load\(ntpClock\.msgStamp\(\)\);",
     "flash.messages.load(ntpClock.isTimeKnown() ? ntpClock.getExactUtcTime() : 0);"),
    ("sms_mirror_rx.cpp", "every load takes",
     r"messages\.load\(ntpClock\.msgStamp\(\)\)", "messages.load(ntpClock.getExactUtcTime())"),
    ("Storage.cpp", "finalised only inside",
     r"if \(unixTime && !st\.meshId\) \{", "if (unixTime) {"),
    ("Storage.cpp", "must mark its stamps under a mesh clock",
     r'putValueFullHex\("tm", st\.meshId\);       // a mesh clock', 'putValueFullHex("tx", st.meshId);  // a mesh clock'),
    ("Storage.cpp", "must mark the index row (\"tm\") under a mesh",
     r'\(\*ipart\)\["tm"\] = "1";                         // the provisional', '(void)0;  // the provisional'),
    ("Storage.cpp", "finalise AFTER the sentinel repair",
     r"(?s)(  /\* ⚠ REPAIR THE UNKNOWN-TIME SENTINELS.*?\n)(  /\* ⚠ FINALISE THE PROVISIONAL STAMPS"
     r".*?\n)(  if \(indexUpdated \|\| removePart\) \{)",
     lambda m: m.group(2) + m.group(1) + m.group(3)),
    ("Storage.cpp", "must re-stamp through clockMsgRestamp",
     r"clockMsgRestamp\(prov, \(uint32_t\)i, bandEnd, out\);", "(void)0;"),
    ("Storage.cpp", "keep the order the texts came in",
     r'im->putValueFullHex\("t", out\[i\+\+\]\);',
     'im->putValueFullHex("t", bandEnd - (uint32_t)(got - 1) + (uint32_t)i++);'),
    ("Storage.cpp", "must lower bandEnd after its re-stamp",
     r"bandEnd -= \(uint32_t\)got;", "(void)0;"),
    ("Storage.cpp", "saturates at",
     r'msgHexStamp\(im->getValueSafe\("t", NULL\), &t\);', 't = im->getHexValueSafe("t", 0);'),
    ("Storage.cpp", "must write \"tm\" onto the message",
     r'ini\[-1\]\.putValueFullHex\("tm", meshId\);', "(void)0;"),
    ("Storage.cpp", "saveMessage(): `if (meshId)` must mark the index row",
     r'index\[partPos\]\["tm"\] = "1";', "(void)0;"),
    ("Storage.cpp", "sentinel must clear meshId",
     r"meshId = 0;   // the sentinel", "(void)0;   // the sentinel"),
    ("WiPhone.ino", "a SIP arrival must pass",
     r"msg->useTime \? rxStamp\.meshId : 0", "0"),
    ("GUI.cpp", "a composed text must pass",
     r"incoming, time, 0, 0, stamp\.meshId\)", "incoming, time)"),
    ("WiPhone.ino", "disarms the reload on a MESH clock",
     r"waitingForClockUpdate = !ntpClock\.isTimeTrusted\(\);", "waitingForClockUpdate = false;"),
    ("clock.cpp", "a trusted set must noteMeshReplacedLocked",
     r"noteMeshReplacedLocked\(utcMsAtRx, rxMs\);", "(void)0;"),
    ("clock.cpp", "NTP must noteMeshReplacedLocked",
     r"noteMeshReplacedLocked\(\(int64_t\)\(ntpTime - SEVENTY_YEARS\) \* 1000, nowMillis\);", "(void)0;"),
    ("clock.cpp", "fresh random meshSetId",
     r"meshSetId = esp_random\(\) \| 1u;", "(void)0;"),
]


def mutation_test(files):
    ok = True
    for mut in MUTATIONS:
        name, want, pat, repl = mut[:4]
        nth = mut[4] if len(mut) > 4 else 0
        raw = files[name + ".raw"]
        ms = list(re.finditer(pat, raw))
        if len(ms) <= nth:
            print(f"  SELF-TEST FAILED: the '{want}' guard is no longer where this mutation looks "
                  f"in {name} - update MUTATIONS with the guard")
            ok = False
            continue
        m = ms[nth]
        mutated = raw[:m.start()] + (repl(m) if callable(repl) else repl) + raw[m.end():]
        got = check(dict(files, **{name: strip_code(mutated), name + ".raw": mutated}))
        if not any(want in g for g in got):
            print(f"  SELF-TEST FAILED: removing the guard from {name} did not trip '{want}'")
            ok = False
    if ok:
        print(f"  ok  self-test: removing each of the {len(MUTATIONS)} guards from the real "
              "source trips its contract")
    return ok


def main():
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
    print("  ok  every message-store load and own stamp carries msgStamp(); mesh stamps are marked, "
          "finalised only under NTP/GPS and after the sentinel repair, re-stamped in arrival order; "
          "the reload waits for a "
          "trusted clock; clock.cpp keeps the offset")
    return 0


if __name__ == "__main__":
    sys.exit(main())

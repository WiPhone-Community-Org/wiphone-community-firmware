#!/usr/bin/env python3
"""check_tz_glue.py - the clock's time zone keeps its rules in the device files (0.9.82).

test_tzauto proves the pure half (tz_auto.cpp: which fix counts, the 10 km margin for an iPhone
fix, two looks 2 min apart, the offset at every zoneinfo instant, the words). The rules below live
in GLUE the host suite cannot compile - tz_service.cpp, WiPhone.ino, serial_cmd.cpp, GUI.cpp - and
each is a decision a later edit could undo with every test still green:

  1. NO SPIFFS WRITE FROM THE LOOP. tzSvcLoop() never writes configs.ini (no storeIniOffset, no
     CriticalFile, no .store()): SPIFFS is ~1.6 s just to open, a LOOP STALL that drops WiFi. The
     INI is written only by tzSvcSetCustomMinutes(), only under `if (storeIni)` - a person typed it.
  2. AUTO ASKS ITS OWN QUESTION. tzSvcLoop() runs tzaStep() over meshService.getSelfSlots(), only
     under `!quiet` (the loop passes gGbcActive: no NVS write under a game); tz_service.cpp never
     uses the screens' or the beacon's picker (getSelfFix / selfPick / getSourceFix), never the
     clock setter (gpsSentence), and has no coordinate in it at all (latI / lonI).
  3. NEVER A BENCH FIX: tz_auto.cpp never mentions SELF_PICK_INJECTED, and tzaPickFix asks
     selfSource() with SELF_PICK_TX (the 30 s bar).
  4. WiPhone.ino: tzSvcBegin() in setup() after the INI's [time] zone is read (tzIniHad set from
     hasKey("zone")); tzSvcLoop(now, gGbcActive) in loop(), its change repainting the time
     (TIME_UPDATE_EVENT); the notice taken only with no game and the screen awake.
  5. NO COORDINATE PAST 2 DECIMALS on the console: every say() in serial_cmd.cpp's tzTest and
     reportTz has no %f wider than %.2f, and reportTz names none at all.
  6. THE ODbL CREDIT is on the screen that shows a zone the map found (TimeConfigApp's
     constructor: "OpenStreetMap"), and Save applies through tzSvcSetMode (no reboot).
  7. BACK TO AUTO NEVER BRINGS BACK AN OLD ZONE (review, 2026-10-10). tzSvcSetMode() runs
     enterAuto(fromMode, fromNamed) -> tzaEnterAuto(&s_auto, ...) + saveAutoZone() under `if
     (modeChanged && mode == TZ_MODE_AUTO)`, with fromMode/fromNamed taken BEFORE s_mode is
     overwritten: nothing looked at a fix while the setting was a named zone or Custom, so the
     old zone back on the clock would be a guess labelled "From phone GPS" (test_tzauto proves
     what tzaEnterAuto puts there instead). tzSvcBegin likewise drops a saved zone when the saved
     mode was not Auto, and `tz forget` is tzaForget(&s_auto) + saveAutoZone().
  8. AUTO'S LOOK HAS ITS OWN millis(), not the loop pass's `now`: phonePosLoop() earlier in the
     same pass can stamp the PHONE slot later than `now`, and the slot's age then wraps.
  9. A STALE INI NEVER BEATS NVS. NVS "ini" (the offset this firmware last saw in configs.ini) is
     written only after a SUCCESSFUL write (Settings: under `if (stored)`; tzSvcSetCustomMinutes:
     under `if (ok)`), and tzSvcBegin keeps "cust" when the INI still equals it.
 10. THE RIGHT SOFT KEY SAYS WHAT IT DOES: TimeConfigApp sets its footer from the focused row
     (footerForFocus: "Back" on the zone choice, where Back quits) in the constructor and on every
     focus change, which also asks for REDRAW_FOOTER.

A contract that cannot find its file or function fails too. The self-test first re-applies each
mistake to an in-memory copy of the real sources and requires the check to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, match_close, strip_code  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
ROOT = REPO / "WiPhone"
FILES = ("tz_service.cpp", "tz_auto.cpp", "WiPhone.ino", "serial_cmd.cpp", "GUI.cpp")

_STRIPPED = {}


def stripped(text):
    key = hash(text)
    if key not in _STRIPPED:
        _STRIPPED[key] = strip_code(text)
    return _STRIPPED[key]


def load():
    return {name: (ROOT / name).read_text(encoding="utf-8", errors="replace") for name in FILES}


def calls(raw, code, name, start, end):
    """The raw text of every call `name(...)` in code[start:end]."""
    out = []
    for m in re.finditer(r"\b%s\s*\(" % re.escape(name), code[start:end]):
        po = start + m.end() - 1
        pc = match_close(code, po)
        if pc > 0:
            out.append(raw[po + 1:pc])
    return out


def fmt_of(call):
    fm = re.match(r'\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', call)
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', fm.group(1))) if fm else ""


def body(code, fn, out, where):
    span = function_body(code, fn)
    if not span:
        out.append("%s: %s not found" % (where, fn))
    return span


def check(texts):
    out = []
    code = {k: stripped(v) for k, v in texts.items()}

    # 1 + 2: tz_service.cpp
    svc = code.get("tz_service.cpp")
    if svc is None:
        return ["tz_service.cpp not found"]
    span = body(svc, "tzSvcLoop", out, "tz_service.cpp")
    if span:
        s, e = span
        for word in ("storeIniOffset", "CriticalFile"):
            if re.search(r"\b%s\b" % word, svc[s:e]):
                out.append("tzSvcLoop: %s - a SPIFFS write in the loop" % word)
        if re.search(r"\.\s*store\s*\(", svc[s:e]):
            out.append("tzSvcLoop: .store() - a SPIFFS write in the loop")
        m = re.search(r"\btzaStep\s*\(\s*&s_auto\s*,\s*s_mode\s*,\s*meshService\s*\.\s*getSelfSlots\s*\(\s*\)",
                      svc[s:e])
        if not m:
            out.append("tzSvcLoop: Auto does not run tzaStep over meshService.getSelfSlots()")
        else:
            # the tzaStep call must sit inside an `if (!quiet ...)`
            before = svc[s:s + m.start()]
            ifm = list(re.finditer(r"\bif\s*\(\s*!\s*quiet\b", before))
            ok = False
            for im in ifm:
                po = s + im.end() - len(im.group(0)) + im.group(0).index("(")
                pc = match_close(svc, po)
                k = pc + 1
                while k < len(svc) and svc[k] in " \t\n":
                    k += 1
                if k < len(svc) and svc[k] == "{":
                    be = match_close(svc, k)
                    if be > s + m.start():
                        ok = True
            if not ok:
                out.append("tzSvcLoop: Auto's check is not under `if (!quiet ...)` (a game: no NVS write)")
    for word in ("getSelfFix", "selfPick", "getSourceFix", "gpsSentence", "latI", "lonI"):
        if re.search(r"\b%s\b" % word, svc):
            out.append("tz_service.cpp mentions %s" % word)
    span = body(svc, "tzSvcSetCustomMinutes", out, "tz_service.cpp")
    if span:
        s, e = span
        if not re.search(r"\bif\s*\(\s*storeIni\s*\)\s*\{[^}]*\bstoreIniOffset\s*\(", svc[s:e]):
            out.append("tzSvcSetCustomMinutes: configs.ini is not written only under `if (storeIni)`")
    n_store = len(re.findall(r"\bstoreIniOffset\s*\(", svc))
    if n_store != 2:                      # the definition and the one call
        out.append("tz_service.cpp: storeIniOffset appears %d times (definition + one call expected)" % n_store)

    # 7: back to Auto starts over
    span = body(svc, "tzSvcSetMode", out, "tz_service.cpp")
    if span:
        b = svc[span[0]:span[1]]
        if not re.search(r"\bif\s*\(\s*modeChanged\s*&&\s*mode\s*==\s*TZ_MODE_AUTO\s*\)\s*\{[^}]*"
                         r"\benterAuto\s*\(\s*fromMode\s*,\s*fromNamed\s*\)", b):
            out.append("tzSvcSetMode: a change into Auto does not go through enterAuto(fromMode, fromNamed)")
        mf = re.search(r"\bconst\s+uint8_t\s+fromMode\s*=\s*s_mode\s*;", b)
        mn = re.search(r"\bconst\s+int\s+fromNamed\s*=\s*s_named\s*;", b)
        ms = re.search(r"\bs_mode\s*=\s*mode\s*;", b)
        if not mf or not mn or not ms or mf.start() > ms.start() or mn.start() > ms.start():
            out.append("tzSvcSetMode: the mode Auto came from is not taken before s_mode is overwritten")
    for fn, inner in (("enterAuto", r"\btzaEnterAuto\s*\(\s*&\s*s_auto\s*,\s*fromMode\s*,\s*fromNamed\s*\)"),
                      ("dropAutoZone", r"\btzaForget\s*\(\s*&\s*s_auto\s*\)")):
        span = body(svc, fn, out, "tz_service.cpp")
        if span:
            b = svc[span[0]:span[1]]
            if not re.search(inner, b) or not re.search(r"\bsaveAutoZone\s*\(", b):
                out.append("%s: not the pure call on s_auto + saveAutoZone() (RAM and NVS)" % fn)
    span = body(svc, "tzSvcBegin", out, "tz_service.cpp")
    if span:
        b = svc[span[0]:span[1]]
        if not re.search(r"\bsavedMode\s*!=\s*TZ_MODE_AUTO\b[^;]*\)\s*\{[^}]*\bzone\s*=\s*-\s*1\s*;", b):
            out.append("tzSvcBegin: a saved zone survives a saved mode that was not Auto")
        # 9 (boot): the INI equal to the record means NVS is the newer typed offset
        if not re.search(r"\bif\s*\(\s*hasIniRec\s*&&\s*iniRec\s*==\s*iniMin\s*\)", b):
            out.append("tzSvcBegin: a configs.ini unchanged since this firmware saw it can beat NVS's newer offset")

    # 8: the look's own millis()
    span = body(svc, "tzSvcLoop", out, "tz_service.cpp")
    if span:
        b = svc[span[0]:span[1]]
        m = re.search(r"\btzaStep\s*\(\s*&s_auto\s*,\s*s_mode\s*,\s*meshService\s*\.\s*getSelfSlots\s*\(\s*\)\s*,"
                      r"\s*(\w+)\s*\)", b)
        if not m or not re.search(r"\b%s\s*=\s*millis\s*\(\s*\)" % re.escape(m.group(1)), b[:m.start()]):
            out.append("tzSvcLoop: tzaStep is not given its own millis() (the pass's `now` can be older than a slot)")

    # 9 (writes): the record only after a write that worked
    span = body(svc, "tzSvcSetCustomMinutes", out, "tz_service.cpp")
    if span and not re.search(r"\bif\s*\(\s*ok\s*\)\s*\{\s*tzSvcIniStored\s*\(", svc[span[0]:span[1]]):
        out.append("tzSvcSetCustomMinutes: NVS \"ini\" is not written only after configs.ini was")
    if len(re.findall(r"\btzSvcIniStored\s*\(", svc)) != 2:   # the definition and that one call
        out.append("tz_service.cpp: tzSvcIniStored called somewhere other than after a successful write")

    # 3: tz_auto.cpp
    ta = code.get("tz_auto.cpp")
    if ta is None:
        out.append("tz_auto.cpp not found")
    else:
        if re.search(r"\bSELF_PICK_INJECTED\b", ta):
            out.append("tz_auto.cpp mentions SELF_PICK_INJECTED - a bench fix must never set the zone")
        span = body(ta, "tzaPickFix", out, "tz_auto.cpp")
        if span and not re.search(r"\bselfSource\s*\([^;]*\bSELF_PICK_TX\b", ta[span[0]:span[1]]):
            out.append("tzaPickFix: not selfSource(..., SELF_PICK_TX, ...) (the 30 s bar, no bench fix)")

    # 4: WiPhone.ino
    ino, inor = code.get("WiPhone.ino"), texts.get("WiPhone.ino")
    if ino is None:
        out.append("WiPhone.ino not found")
    else:
        span = body(ino, "setup", out, "WiPhone.ino")
        if span:
            s, e = span
            mb = re.search(r"\btzSvcBegin\s*\(\s*tzIniHad\s*,\s*tzIniHours\s*\)", ino[s:e])
            mh = re.search(r"\btzIniHad\s*=\s*ini\s*\[[^\]]*\]\s*\.\s*hasKey\s*\(", inor[s:e])
            if not mb or not mh or mh.start() > mb.start():
                out.append("setup(): tzSvcBegin(tzIniHad, tzIniHours) not called after the INI's zone is read")
        span = body(ino, "loop", out, "WiPhone.ino")
        if span:
            s, e = span
            if not re.search(r"\bif\s*\(\s*tzSvcLoop\s*\(\s*now\s*,\s*gGbcActive\s*\)\s*\)\s*\{\s*redrawWhat\s*\|=\s*"
                             r"gui\s*\.\s*processEvent\s*\(\s*now\s*,\s*TIME_UPDATE_EVENT\s*\)", ino[s:e]):
                out.append("loop(): tzSvcLoop(now, gGbcActive) does not repaint the time on a change")
            ok = False
            for im in re.finditer(r"\bif\s*\(", ino[s:e]):
                po = s + im.end() - 1
                pc = match_close(ino, po)
                cond = ino[po:pc]
                if "!gGbcActive" in cond.replace(" ", "") and re.search(r"screenBrightness\s*>\s*0", cond):
                    k = pc + 1
                    while ino[k] in " \t\n":
                        k += 1
                    be = match_close(ino, k) if ino[k] == "{" else -1
                    if be > 0 and re.search(r"\btzSvcTakeNotice\s*\(", ino[k:be]):
                        ok = True
            if not ok:
                out.append("loop(): the zone notice is not taken only with no game and the screen awake")

    # 5: serial_cmd.cpp
    sc, scr = code.get("serial_cmd.cpp"), texts.get("serial_cmd.cpp")
    if sc is None:
        out.append("serial_cmd.cpp not found")
    else:
        for fn in ("tzTest", "reportTz"):
            span = body(sc, fn, out, "serial_cmd.cpp")
            if not span:
                continue
            for call in calls(scr, sc, "say", span[0], span[1]):
                f = fmt_of(call)
                if re.search(r"%\.[3-9]\d*l?f", f) or re.search(r"%l?f", f):
                    out.append("%s: a say() with a float wider than 2 decimals" % fn)
                if fn == "reportTz" and re.search(r"%\.\d*f", f):
                    out.append("reportTz: a float in a line - it names no position at all")

    # 6: GUI.cpp
    g, gr = code.get("GUI.cpp"), texts.get("GUI.cpp")
    if g is None:
        out.append("GUI.cpp not found")
    else:
        span = body(g, "TimeConfigApp::TimeConfigApp", out, "GUI.cpp")
        if span and "OpenStreetMap" not in gr[span[0]:span[1]]:
            out.append("TimeConfigApp: the zone map's OpenStreetMap credit is not on the screen")
        span = body(g, "TimeConfigApp::processEvent", out, "GUI.cpp")
        if span and not re.search(r"\btzSvcSetMode\s*\(", g[span[0]:span[1]]):
            out.append("TimeConfigApp::processEvent: Save does not apply through tzSvcSetMode")
        if span:
            b = g[span[0]:span[1]]
            # 9: Settings records the INI only when its own store() worked
            ms = re.search(r"\bconst\s+bool\s+stored\s*=\s*ini\s*\.\s*store\s*\(\s*\)\s*;", b)
            calls_ = list(re.finditer(r"\btzSvcIniStored\s*\(", b))
            guarded = re.search(r"\bif\s*\(\s*stored\s*\)\s*\{\s*tzSvcIniStored\s*\(", b)
            if not ms or len(calls_) != 1 or not guarded:
                out.append("TimeConfigApp::processEvent: tzSvcIniStored is not called only after ini.store() worked")
            # 10: the footer follows the focus
            if not re.search(r"\bnextFocus\s*\([^;]*;\s*footerForFocus\s*\(\s*\)\s*;\s*res\s*\|=\s*REDRAW_FOOTER\b", b):
                out.append("TimeConfigApp::processEvent: a focus change does not re-word and redraw the footer")
        span = body(g, "TimeConfigApp::TimeConfigApp", out, "GUI.cpp")
        if span and not re.search(r"\bsetFocus\s*\(\s*zoneChoice\s*\)\s*;\s*footerForFocus\s*\(\s*\)", g[span[0]:span[1]]):
            out.append("TimeConfigApp: the footer is not worded for the row the screen opens on")
        span = body(g, "TimeConfigApp::footerForFocus", out, "GUI.cpp")
        if span and not re.search(r"zoneChoice\s*\?\s*\"Back\"\s*:\s*\"Clear\"", gr[span[0]:span[1]]):
            out.append("TimeConfigApp::footerForFocus: the zone row's right soft key is not \"Back\"")
    return out


MUTATIONS = [
    ("the INI written from the loop", "tz_service.cpp",
     lambda t: t.replace("  const bool known = clockKnown();\n  if (known != s_known",
                         "  storeIniOffset(s_custom);\n  const bool known = clockKnown();\n  if (known != s_known", 1)),
    ("Auto's check under a game", "tz_service.cpp",
     lambda t: t.replace("if (!quiet && (s_checkMs == 0", "if ((s_checkMs == 0", 1)),
    ("Auto through the screens' picker", "tz_service.cpp",
     lambda t: t.replace("  const bool known = clockKnown();\n  if (known != s_known",
                         "  SelfFix fq;\n  meshService.getSelfFix(&fq, false);\n  const bool known = clockKnown();\n  if (known != s_known", 1)),
    ("the INI written without a person", "tz_service.cpp",
     lambda t: t.replace("  if (storeIni) {\n    ok = storeIniOffset(minutes);", "  {\n    ok = storeIniOffset(minutes);", 1)),
    ("bench fixes allowed", "tz_auto.cpp",
     lambda t: t.replace("SELF_PICK_TX, &fx)", "SELF_PICK_TX | SELF_PICK_INJECTED, &fx)", 1)),
    ("the loop not quiet under a game", "WiPhone.ino",
     lambda t: t.replace("tzSvcLoop(now, gGbcActive)", "tzSvcLoop(now, false)", 1)),
    ("the banner under a game", "WiPhone.ino",
     lambda t: t.replace("if (!meshPopupActive && !gGbcActive && gui.state.screenBrightness > 0",
                         "if (!meshPopupActive && gui.state.screenBrightness > 0", 1)),
    ("tzSvcBegin before the INI", "WiPhone.ino",
     lambda t: t.replace("  tzSvcBegin(tzIniHad, tzIniHours);\n", "", 1).replace(
         "  bool tzIniHad = false;", "  bool tzIniHad = false;\n  float tzIniHours0 = 0; tzSvcBegin(tzIniHad, tzIniHours0);", 1)),
    ("5 decimals in `tz test`", "serial_cmd.cpp",
     lambda t: t.replace('"tz test %.2f,%.2f: %s (%s)', '"tz test %.5f,%.5f: %s (%s)', 1)),
    ("no on-screen credit", "GUI.cpp",
     lambda t: t.replace('"Zone map (c) OpenStreetMap"', '"Zone map"', 1)),
    ("Save needing a reboot", "GUI.cpp",
     lambda t: t.replace("      tzSvcSetMode(mode, named);", "      (void)named;", 1)),
    ("back to Auto puts the old zone back", "tz_service.cpp",
     lambda t: t.replace("    enterAuto(fromMode, fromNamed);\n  }", "  }", 1)),
    ("the mode Auto came from read after it is overwritten", "tz_service.cpp",
     lambda t: t.replace("  const uint8_t fromMode = s_mode;\n  const int fromNamed = s_named;\n  s_mode = mode;",
                         "  s_mode = mode;\n  const uint8_t fromMode = s_mode;\n  const int fromNamed = s_named;", 1)),
    ("enterAuto only in RAM", "tz_service.cpp",
     lambda t: t.replace("ntpClock.getExactUtcTime() : 0;\n  saveAutoZone();\n}", "ntpClock.getExactUtcTime() : 0;\n}", 1)),
    ("`tz forget` only in RAM", "tz_service.cpp",
     lambda t: t.replace("  tzaForget(&s_auto);\n  s_azAtUtc = 0;\n  saveAutoZone();\n}",
                         "  tzaForget(&s_auto);\n  s_azAtUtc = 0;\n}", 1)),
    ("a boot out of a named zone keeps the old Auto zone", "tz_service.cpp",
     lambda t: t.replace("savedMode != TZ_MODE_AUTO) {", "savedMode == 99) {", 1)),
    ("Auto's look on the pass's `now`", "tz_service.cpp",
     lambda t: t.replace("getSelfSlots(), lookMs)", "getSelfSlots(), nowMs)", 1)),
    ("the stale INI beats NVS at boot", "tz_service.cpp",
     lambda t: t.replace("if (hasIniRec && iniRec == iniMin) {", "if (false) {", 1)),
    ("the INI record after a failed serial write", "tz_service.cpp",
     lambda t: t.replace("    if (ok) {\n      tzSvcIniStored(minutes);\n    } else {",
                         "    tzSvcIniStored(minutes);\n    if (ok) {\n    } else {", 1)),
    ("the INI record after a failed Settings write", "GUI.cpp",
     lambda t: t.replace("        if (stored) {\n          tzSvcIniStored(minutes);\n        } else {",
                         "        tzSvcIniStored(minutes);\n        if (stored) {\n        } else {", 1)),
    ("\"Clear\" over the zone row", "GUI.cpp",
     lambda t: t.replace('zoneChoice ? "Back" : "Clear"', 'zoneChoice ? "Clear" : "Clear"', 1)),
    ("the footer not re-worded on a focus change", "GUI.cpp",
     lambda t: t.replace("    footerForFocus();\n    res |= REDRAW_FOOTER;\n", "", 1)),
]


def selftest(texts):
    if check(texts):
        return False
    ok = True
    for what, name, f in MUTATIONS:
        mutated = dict(texts)
        mutated[name] = f(texts[name])
        if mutated[name] == texts[name]:
            print("  SELF-TEST: %s - the mutation no longer applies (update it with the code)" % what)
            ok = False
            continue
        if not check(mutated):
            print("  SELF-TEST: %s - not caught" % what)
            ok = False
    return ok


def main():
    texts = load()
    problems = check(texts)
    for p in problems:
        print("  FAIL " + p)
    if problems:
        print("check_tz_glue: %d problem(s)" % len(problems))
        return 1
    if not selftest(texts):
        print("check_tz_glue: SELF-TEST FAILED")
        return 1
    print("check_tz_glue: ok (10 contracts, %d mutations caught)" % len(MUTATIONS))
    return 0


if __name__ == "__main__":
    sys.exit(main())

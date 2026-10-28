#!/usr/bin/env python3
"""check_phonepos_review.py - the phone-over-hotspot location source keeps its rules (0.9.82).

test_selfpos proves the pure half (self_pos.cpp: which source wins, the one-GGA quality gate, the
stale guards, the port search, two fixes in a row agreeing). The rules below live in GLUE the host
suite cannot compile - phone_pos.cpp, meshtastic_service.cpp, serial_cmd.cpp, WiPhone.ino - and
each is a decision a later edit could undo with every test still green:

  1. NO IP GEOLOCATION, ANYWHERE: no IP-geolocation host (ipwho.is, ip-api.com, ipinfo.io,
     get.geojs.io, freeipapi.com, ipapi.co) and no `considerIp` true under WiPhone/. On a phone
     hotspot an IP lookup answers with the carrier's internet exit, ~200 km away, looking sure.
     Host names, not bare words: "geojs" alone would hit "geojson" in the weather fixtures.
  2. phone_pos.cpp never mentions the clock setter (gpsSentence), the woods plate's reader
     (gGpsNmea / gGpsReader), a blocking client or resolver (WiFiClient, hostByName,
     resolveDomain, gethostbyname, getaddrinfo, dns_gethostbyname), a task of its own
     (xTaskCreate) or the CPU clock (setCpuFrequencyMhz, cpuClockGate) - raw text, comments
     included, so nobody "just reuses" one; and it touches ntpClock only to ASK
     (getTrustedUtcTime / isTimeTrusted).
  3. phone_pos.cpp never waits: every recv() passes MSG_DONTWAIT, the socket is made O_NONBLOCK
     before lwip_connect_r(), select() is handed a zero timeval, and there is no delay() or
     vTaskDelay(). WiPhone.ino calls phonePosLoop() in loop(), and neither `busy` nor
     `hardBusy` mentions it (a 1 Hz trickle needs no full speed; a clock switch under a running
     radio is phone 1's WiFi drop).
  4. NO COORDINATE, MAC OR SSID IN A NEW LOG LINE: every log_*() in phone_pos.cpp and
     self_pos.cpp, and in meshtastic_service.cpp's gpsUpdate / phoneUpdate / injectSelfFix /
     sendSelfPosition / sendPositionOnEx, has no float wider than 2 decimals (no bare %f either:
     that is 6), no %d.%05d pair, no MAC pattern, and no ssid / gateway / localIP argument; a
     latI/lonI argument rides only with a %.2f. `pos`'s per-source lines (reportSelfSources) and
     `phonepos` (phonePosDumpStatus) never name a coordinate or an address at all.
  5. THE MESH FIELDS: sendSelfPosition() hands sendPositionOnEx() the pick's locSrc and
     precBits; sendPositionOnEx() builds with meshPosBuildEx(pos, sizeof(pos), ...); and the
     pick (self_pos.cpp fillFix) gives a bench fix MESH_LOC_MANUAL - a typed position is never
     sent as an INTERNAL/EXTERNAL measurement.
  6. A BENCH FIX NEVER GOES OUT BY ITSELF: the beacon tick picks with SELF_PICK_TX alone (no
     SELF_PICK_INJECTED) and calls sendSelfPosition(false); posBlockedReasonFor() refuses an
     injected pick on a public channel; phone fixes from a `phonepos at` address are passed to
     phoneUpdate() as bench fixes (s_benchOn).
  7. THE SERIAL CONSENT RULE: no cable line starts a location going out to a public channel.
     serialPosConsentRefusal() tests channelIsPublic(), and `pos every <n>` (before
     setPosInterval(s)), `gps on` (before gGpsNmea is set) and `phonepos auto` (before
     phonePosSetAuto(true)) each ask it; `pos now` refuses any public channel
     (sendSelfPositionNow and posNowBlockedReason test channelIsPublic()); and serial_cmd.cpp
     never calls setPosChannelName() (`pos chan` stays refused).
  8. THE REAL 2026-08-25 FIX stays out of the tree (it lived in two comments until 0.9.82).

A contract that cannot find its file or function fails too. The self-test first re-applies each
mistake to an in-memory copy of the real sources and requires its contract to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, match_close, strip_code  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
ROOT = REPO / "WiPhone"
FILES = ("phone_pos.cpp", "self_pos.cpp", "meshtastic_service.cpp", "serial_cmd.cpp", "WiPhone.ino")

IP_GEO = [r"ipwho\.is", r"ip-api\.com", r"ipinfo\.io", r"get\.geojs\.io", r"freeipapi\.com",
          r"ipapi\.co\b", r"considerIp\\?\"?\s*:\s*true"]
PHONE_BANNED = ["gpsSentence", "gGpsNmea", "gGpsReader", "WiFiClient", "hostByName", "resolveDomain",
                "gethostbyname", "getaddrinfo", "dns_gethostbyname", "xTaskCreate",
                "setCpuFrequencyMhz", "cpuClockGate"]
# The real fix, written in pieces so this file does not carry it whole either.
REAL_FIX = ["47" + r"\.33821", "122" + r"\.16501"]
LOG_FUNCS = ("MeshtasticService::gpsUpdate", "MeshtasticService::phoneUpdate",
             "MeshtasticService::injectSelfFix", "MeshtasticService::sendSelfPosition",
             "MeshtasticService::sendPositionOnEx")


def log_calls(raw, code, start=0, end=None):
    """(format, args) of every log_x(...) call in code[start:end], read from the raw text."""
    end = len(code) if end is None else end
    out = []
    for m in re.finditer(r"\blog_[a-z]\s*\(", code[start:end]):
        po = start + m.end() - 1
        pc = match_close(code, po)
        if pc < 0:
            continue
        call = raw[po + 1:pc]
        fm = re.match(r'\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', call)
        fmt = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', fm.group(1))) if fm else ""
        args = call[fm.end():] if fm else call
        out.append((fmt, args))
    return out


def log_problem(fmt, args):
    if re.search(r"%\.[3-9]\d*l?f", fmt) or re.search(r"%l?f", fmt):
        return "a float wider than 2 decimals"
    if re.search(r"%d\.%0\d+d", fmt):
        return "a %d.%05d coordinate pair"
    if re.search(r"%0?2[xX]\s*:\s*%0?2[xX]", fmt):
        return "a MAC address"
    if re.search(r"(?i)\bssid\b|gatewayIP|localIP|s_benchIp", args):
        return "an SSID or an address"
    if re.search(r"\b(?:lat|lon)I?\b", args) and not re.search(r"%\.[0-2]f", fmt):
        return "a coordinate without %.2f"
    return None


_STRIPPED = {}


def stripped(text):
    """strip_code() once per distinct text: the self-test re-checks 15 mutated copies."""
    key = hash(text)
    if key not in _STRIPPED:
        _STRIPPED[key] = strip_code(text)
    return _STRIPPED[key]


def check(texts):
    out = []
    code = {k: stripped(v) for k, v in texts.items() if k in FILES}

    # 1. no IP geolocation anywhere under WiPhone/
    for name, raw in texts.items():
        if not name.startswith("tree:"):
            continue
        for pat in IP_GEO:
            if re.search(pat, raw):
                out.append("%s: an IP-geolocation host or considerIp:true (%s)" % (name[5:], pat))

    pp, ppc = texts.get("phone_pos.cpp"), code.get("phone_pos.cpp")
    if pp is None:
        return out + ["phone_pos.cpp not found"]
    # 2. what phone_pos.cpp may not even mention, and the clock only asked
    for word in PHONE_BANNED:
        if re.search(r"\b%s\b" % word, pp):
            out.append("phone_pos.cpp mentions %s" % word)
    for m in re.finditer(r"\bntpClock\s*\.\s*(\w+)", ppc):
        if m.group(1) not in ("getTrustedUtcTime", "isTimeTrusted"):
            out.append("phone_pos.cpp: ntpClock.%s() - the phone's NMEA only ASKS the clock" % m.group(1))
    # 3. never waits
    for m in re.finditer(r"\brecv(?:from)?\s*\(", ppc):
        pc = match_close(ppc, m.end() - 1)
        if pc < 0 or "MSG_DONTWAIT" not in ppc[m.end():pc]:
            out.append("phone_pos.cpp: a recv() without MSG_DONTWAIT")
    if not re.search(r"\brecv\s*\(", ppc):
        out.append("phone_pos.cpp: no recv() found (the contract must be updated with the code)")
    sc = function_body(ppc, "startConnect")
    if not sc:
        out.append("phone_pos.cpp: startConnect() not found")
    else:
        body = ppc[sc[0]:sc[1]]
        nb, cn = body.find("O_NONBLOCK"), body.find("lwip_connect_r")
        if nb < 0 or cn < 0 or nb > cn:
            out.append("phone_pos.cpp: startConnect() must set O_NONBLOCK before lwip_connect_r()")
    if re.search(r"(?<![\w.])connect\s*\(", ppc):
        out.append("phone_pos.cpp: a plain connect() (only lwip_connect_r on the O_NONBLOCK socket)")
    if not re.search(r"struct\s+timeval\s+\w+\s*=\s*\{\s*0\s*,\s*0\s*\}", ppc) or \
            not re.search(r"\bselect\s*\(", ppc):
        out.append("phone_pos.cpp: the connect must be polled with select() and a {0, 0} timeval")
    if re.search(r"\b(?:delay|vTaskDelay|delayMicroseconds)\s*\(", ppc):
        out.append("phone_pos.cpp: delay()/vTaskDelay() on the loop task")
    ino, inoc = texts.get("WiPhone.ino"), code.get("WiPhone.ino")
    lp = function_body(inoc, "loop") if inoc else None
    if not lp:
        out.append("WiPhone.ino: loop() not found")
    else:
        if not re.search(r"\bphonePosLoop\s*\(", inoc[lp[0]:lp[1]]):
            out.append("WiPhone.ino: loop() does not call phonePosLoop()")
        for var in ("busy", "hardBusy"):
            m = re.search(r"\bconst\s+bool\s+%s\s*=" % var, inoc[lp[0]:lp[1]])
            if not m:
                out.append("WiPhone.ino: `const bool %s =` not found in loop()" % var)
                continue
            s = lp[0] + m.end()
            e = inoc.find(";", s)
            if re.search(r"phonePos", inoc[s:e]):
                out.append("WiPhone.ino: `%s` mentions the phone client - it must not hold the CPU" % var)

    # 4. no coordinate / MAC / SSID in a new log line
    for name in ("phone_pos.cpp", "self_pos.cpp"):
        raw, c = texts.get(name), code.get(name)
        if raw is None:
            out.append("%s not found" % name)
            continue
        for fmt, args in log_calls(raw, c):
            why = log_problem(fmt, args)
            if why:
                out.append("%s: a log line carries %s: %s" % (name, why, fmt[:70]))
    ms, msc = texts.get("meshtastic_service.cpp"), code.get("meshtastic_service.cpp")
    for fn in LOG_FUNCS:
        span = function_body(msc, fn) if msc else None
        if not span:
            out.append("meshtastic_service.cpp: %s not found" % fn)
            continue
        for fmt, args in log_calls(ms, msc, span[0], span[1]):
            why = log_problem(fmt, args)
            if why:
                out.append("meshtastic_service.cpp %s: a log line carries %s: %s" % (fn, why, fmt[:70]))
    ser, serc = texts.get("serial_cmd.cpp"), code.get("serial_cmd.cpp")
    for fname, fcode, fn in (("serial_cmd.cpp", serc, "reportSelfSources"),
                             ("phone_pos.cpp", ppc, "phonePosDumpStatus")):
        span = function_body(fcode, fn) if fcode else None
        if not span:
            out.append("%s: %s not found" % (fname, fn))
        elif re.search(r"\b(?:latI|lonI|s_benchIp|gatewayIP|localIP)\b", fcode[span[0]:span[1]]):
            out.append("%s: %s names a coordinate or an address" % (fname, fn))

    # 5. the mesh fields
    span = function_body(msc, "MeshtasticService::sendSelfPosition") if msc else None
    if not span:
        out.append("meshtastic_service.cpp: sendSelfPosition not found")
    else:
        b = msc[span[0]:span[1]]
        m = re.search(r"\bsendPositionOnEx\s*\(", b)
        call = b[m.end():match_close(b, m.end() - 1)] if m else ""
        if not m or "fx.locSrc" not in call or "fx.precBits" not in call:
            out.append("sendSelfPosition: sendPositionOnEx() is not handed the pick's locSrc and precBits")
    span = function_body(msc, "MeshtasticService::sendPositionOnEx") if msc else None
    if not span or not re.search(r"\bmeshPosBuildEx\s*\(\s*pos\s*,\s*sizeof\s*\(\s*pos\s*\)",
                                 msc[span[0]:span[1]]):
        out.append("sendPositionOnEx: must build with meshPosBuildEx(pos, sizeof(pos), ...)")
    spc = code.get("self_pos.cpp")
    span = function_body(spc, "fillFix") if spc else None
    if not span or not re.search(r"\binjected\s*\?\s*MESH_LOC_MANUAL\b", spc[span[0]:span[1]]):
        out.append("self_pos.cpp fillFix: a bench (injected) fix must be sent as MESH_LOC_MANUAL")

    # 6. a bench fix never goes out by itself
    span = function_body(msc, "MeshtasticService::loop") if msc else None
    if not span:
        out.append("meshtastic_service.cpp: MeshtasticService::loop not found")
    else:
        b = msc[span[0]:span[1]]
        picks = [b[m.end():match_close(b, m.end() - 1)] for m in re.finditer(r"\bselfPick\s*\(", b)]
        if not any("SELF_PICK_TX" in p for p in picks) or any("SELF_PICK_INJECTED" in p for p in picks):
            out.append("the beacon tick must pick with SELF_PICK_TX and never SELF_PICK_INJECTED")
        if re.search(r"\bsendSelfPosition\s*\(\s*true", b) or \
                not re.search(r"\bsendSelfPosition\s*\(\s*false\s*\)", b):
            out.append("the beacon tick must call sendSelfPosition(false) (the schedule never sends a bench fix)")
    span = function_body(msc, "MeshtasticService::posBlockedReasonFor") if msc else None
    if not span or not re.search(r"\bfx\s*\.\s*injected\s*&&\s*channelIsPublic\s*\(\s*ch\s*\)",
                                 msc[span[0]:span[1]]):
        out.append("posBlockedReasonFor: an injected pick on a public channel is not refused")
    rs = function_body(ppc, "readStream")
    if not rs or not re.search(r"\bphoneUpdate\s*\([^;]*\bs_benchOn\s*\)", ppc[rs[0]:rs[1]]):
        out.append("phone_pos.cpp readStream: a `phonepos at` fix must reach phoneUpdate() as a bench fix")

    # 7. the serial consent rule
    if serc is None:
        out.append("serial_cmd.cpp not found")
    else:
        if re.search(r"\bsetPosChannelName\s*\(", serc):
            out.append("serial_cmd.cpp calls setPosChannelName() - the channel is chosen on the phone")
        sp = function_body(serc, "serialPosConsentRefusal")
        if not sp or not re.search(r"\bchannelIsPublic\s*\(", serc[sp[0]:sp[1]]):
            out.append("serial_cmd.cpp: serialPosConsentRefusal() must test channelIsPublic()")
        # Each handler, from its own keyword (read in the raw text: literals are blanked in the
        # stripped copy, at the same offsets) to the line that acts.
        for what, start_rx, act_rx in (
                ("`pos every`", r'strncasecmp\(arg,\s*"every"', r"\bsetPosInterval\s*\(\s*s\s*\)"),
                ("`gps on`", r'strcasecmp\(arg,\s*"on"\)\s*\|\|\s*!strcasecmp\(arg,\s*"off"\)',
                 r"\bgGpsNmea\s*=\s*!\s*strcasecmp"),
                ("`phonepos auto`", r'strcasecmp\(arg,\s*"auto"\)', r"\bphonePosSetAuto\s*\(\s*true\s*\)")):
            ms_ = re.search(start_rx, ser)
            ma = re.compile(act_rx).search(serc, ms_.end()) if ms_ else None
            if not ms_ or not ma:
                out.append("serial_cmd.cpp: %s handler not found (update the contract with the code)" % what)
            elif not re.search(r"\bserialPosConsentRefusal\s*\(", serc[ms_.end():ma.start()]):
                out.append("serial_cmd.cpp: %s does not ask serialPosConsentRefusal() first" % what)
    for fn in ("MeshtasticService::sendSelfPositionNow", "MeshtasticService::posNowBlockedReason"):
        span = function_body(msc, fn) if msc else None
        if not span or not re.search(r"\bchannelIsPublic\s*\(", msc[span[0]:span[1]]):
            out.append("meshtastic_service.cpp %s: `pos now` must refuse any public channel" % fn)

    # 8. the real fix
    for name, raw in texts.items():
        if name.startswith("doc:") or name.startswith("tree:"):
            for pat in REAL_FIX:
                if re.search(pat, raw):
                    out.append("%s: the real 2026-08-25 fix is back" % name.split(":", 1)[1])
    return out


def load():
    texts = {}
    for f in FILES:
        p = ROOT / f
        if p.exists():
            texts[f] = p.read_text(errors="replace")
    for p in sorted(ROOT.rglob("*")):
        if p.suffix in (".cpp", ".h", ".ino", ".c") and p.is_file():
            texts["tree:" + str(p.relative_to(REPO))] = p.read_text(errors="replace")
    for rel in ("CHANGELOG.md", "README.md", "docs/HANDOFF.md", "tests/test_pos.cpp",
                "tests/vectors_pos.h", "tools/gen_pos_vectors.py"):
        p = REPO / rel
        if p.exists():
            texts["doc:" + rel] = p.read_text(errors="replace")
    return texts


MUTATIONS = [
    ("an IP lookup", "tree:WiPhone/phone_pos.cpp",
     lambda t: t + '\nstatic const char* U = "https://ipwho.is/";\n'),
    ("the clock set from the phone", "phone_pos.cpp",
     lambda t: t.replace("const uint32_t utc = ntpClock.getTrustedUtcTime();",
                         "const uint32_t utc = ntpClock.getTrustedUtcTime(); ntpClock.gpsSentence(s_stream.reader.fix(), now);", 1)),
    ("a blocking recv", "phone_pos.cpp",
     lambda t: t.replace("recv(s_fd, buf, want, MSG_DONTWAIT)", "recv(s_fd, buf, want, 0)", 1)),
    ("O_NONBLOCK after the connect", "phone_pos.cpp",
     lambda t: t.replace("fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);", "", 1)),
    ("a waiting select", "phone_pos.cpp",
     lambda t: t.replace("struct timeval tv = { 0, 0 };", "struct timeval tv = { 0, 500000 };", 1)),
    ("the phone client in hardBusy", "WiPhone.ino",
     lambda t: t.replace("const bool hardBusy = gGbcActive ||", "const bool hardBusy = phonePosBenchOn() || gGbcActive ||", 1)),
    ("a coordinate in a log line", "phone_pos.cpp",
     lambda t: t.replace('log_e("PHONE LOCATION: a location app answers on port %u", (unsigned)s_port);',
                         'log_e("PHONE LOCATION: at %.5f", s_candLatI / 1e7);', 1)),
    ("5 decimals in the beacon log", "meshtastic_service.cpp",
     lambda t: t.replace("~%.2f,%.2f src=%u", "~%.5f,%.5f src=%u", 1)),
    ("the address in `phonepos`", "phone_pos.cpp",
     lambda t: t.replace("  char st[64];\n  phonePosStatusLine(st, sizeof(st));",
                         "  char st[64];\n  (void)s_benchIp;\n  phonePosStatusLine(st, sizeof(st));", 1)),
    ("the pin's fields on a measured fix", "meshtastic_service.cpp",
     lambda t: t.replace("fx.locSrc, fx.precBits, whenUtc);", "MESH_LOC_UNSET, 0, whenUtc);", 1)),
    ("the schedule sending a bench fix", "meshtastic_service.cpp",
     lambda t: t.replace("sendSelfPosition(false);", "sendSelfPosition(true);", 1)),
    ("a bench fix to a public channel", "meshtastic_service.cpp",
     lambda t: t.replace("if (fx.injected && channelIsPublic(ch)) {", "if (false) {", 1)),
    ("`phonepos at` fixes counted as real", "phone_pos.cpp",
     lambda t: t.replace("out.hdopX10, s_benchOn);", "out.hdopX10, false);", 1)),
    ("`phonepos auto` without the consent check", "serial_cmd.cpp",
     lambda t: t.replace("? serialPosConsentRefusal() : NULL;", "? NULL : NULL;", 1)),
    ("`pos every` without the consent check", "serial_cmd.cpp",
     lambda t: t.replace('const char* no = serialPosConsentRefusal();\n      if (no) {\n        say("pos: refused',
                         'const char* no = NULL;\n      if (no) {\n        say("pos: refused', 1)),
    ("`gps on` without the consent check", "serial_cmd.cpp",
     lambda t: t.replace('const char* no = serialPosConsentRefusal();\n        if (no) {\n          say("gps on: refused',
                         'const char* no = NULL;\n        if (no) {\n          say("gps on: refused', 1)),
    ("the consent helper blind to public channels", "serial_cmd.cpp",
     lambda t: t.replace("return meshService.channelIsPublic(pc) ? ", "return false ? ", 1)),
    ("`pos now` to a confirmed public channel", "meshtastic_service.cpp",
     lambda t: t.replace('  if (ch && channelIsPublic(ch)) {\n    log_e("POSITION BEACON REFUSED: %s", POS_REFUSED_CABLE);',
                         '  if (false) {\n    log_e("POSITION BEACON REFUSED: %s", POS_REFUSED_CABLE);', 1)),
    ("a bench fix sent as a measurement", "self_pos.cpp",
     lambda t: t.replace("s->injected ? MESH_LOC_MANUAL : selfSrcLocSource(src)", "selfSrcLocSource(src)", 1)),
    ("the real fix back in a comment", "doc:CHANGELOG.md",
     lambda t: t + "\n" + "47" + ".33821\n"),
]


def selftest(texts):
    ok = True
    if check(texts):
        return False
    for what, name, f in MUTATIONS:
        if name not in texts:
            print("  SELF-TEST: %s - %s not loaded" % (what, name))
            ok = False
            continue
        mutated = dict(texts)
        mutated[name] = f(texts[name])
        if mutated[name] == texts[name]:
            print("  SELF-TEST: %s - the mutation no longer applies (update it with the code)" % what)
            ok = False
            continue
        if name.startswith("tree:WiPhone/") and name[13:] in mutated:
            mutated[name[13:]] = mutated[name]
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
        print("check_phonepos_review: %d problem(s)" % len(problems))
        return 1
    if not selftest(texts):
        print("check_phonepos_review: SELF-TEST FAILED")
        return 1
    print("check_phonepos_review: ok (8 contracts, %d mutations caught)" % len(MUTATIONS))
    return 0


if __name__ == "__main__":
    sys.exit(main())

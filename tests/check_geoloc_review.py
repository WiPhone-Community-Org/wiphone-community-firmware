#!/usr/bin/env python3
"""check_geoloc_review.py - Wi-Fi location keeps its rules (0.9.82).

test_geoloc proves the pure half (geoloc.cpp: the filters, the request bodies, the parser, the
two-half agreement, when to look) and test_selfpos the Wi-Fi slot's rules (self_pos.cpp). The
rules below live in GLUE the host suite cannot compile - geoloc_net.cpp, https_worker.cpp,
Networks.cpp, meshtastic_service.cpp, serial_cmd.cpp, tz_auto.cpp - and each is a decision a later
edit could undo with every test still green:

  1. NO IP GEOLOCATION, ANYWHERE: no IP-geolocation host (ipwho.is, ip-api.com, ipinfo.io,
     get.geojs.io, freeipapi.com, ipapi.co) and no `considerIp` true under WiPhone/; and the
     request's envelope in geoloc.cpp turns beaconDB's own fallback off TWICE - it carries both
     "considerIp":false and "fallbacks":{"ipf":false - and no SSID ever goes into a body. Host
     names, not bare words: "geojs" alone would hit "geojson" in the weather fixtures.
  2. THE REQUEST (review item 6): geoloc_net.cpp's post() sends Content-Type GEO_CONTENT_TYPE
     ("application/json": beaconDB answers 400 without it), the generic GEO_USER_AGENT (no
     version, no digit at all) and pins GEO_ROOTS_PEM, which is ISRG Root X1 + X2 + Root YE.
  3. THE WORKER'S PREDICATE: https_worker.cpp's netRequestActive() includes geoRequestActive()
     (WiPhone.ino's busy, hardBusy and the 10-tick sleep: a handshake at the 80 MHz idle clock
     beside a 1-tick loop failed 3 of 3); startLook() calls tlsInstallPsramHook() before
     netWorkerSubmit(NET_JOB_GEO, ...).
  4. NO MAC, SSID OR COORDINATE IN A LOG LINE: every log_*() in geoloc_net.cpp passes the
     phone-location check's test (no float wider than 2 decimals, no MAC pattern, no ssid argument,
     no lat/lon argument without %.2f); geolocReport() and geolocRowText() never name a
     coordinate, a MAC or a BSSID; geoloc.cpp logs nothing at all (it is pure).
  5. THE CONSENT RULE: serial_cmd.cpp's `netloc` handler asks geoSettingSerialOk() before it calls
     geolocSetSetting(), and never hands it GEO_SET_MESH: "map + mesh" is chosen on the screen.
  6. THE AUTO-SWITCHER (review item 8): in Networks::autoSwitchTick the mode comes from
     geoSwitcherMode(); Wi-Fi location's copy (geolocOnScan) is taken BEFORE WiFi.scanDelete();
     autoSwitchEvaluate() runs only inside `if (full)`; the SCAN_ONLY round's block disconnects
     nothing and comes before the first WiFi.disconnect() of the tick; and geoloc_net.cpp reads the
     scan through WiFi.getScanInfoByIndex(), never WiFi.SSID()/BSSIDstr() Strings.
  7. THE BEACON (review items 2 and 15): sendSelfPosition() sends a Wi-Fi pick's cell
     (selfNetTxCell), stamps field 4 with selfFixSendTime(), and records what went out
     (selfNetTxNoteSent / selfNetTxNotePrecise); posBlockedReasonFor() asks selfNetPreciseWins();
     the beacon tick measures movement on the cell (selfNetTxCell before meshPosShouldBeacon).
  8. THE TIME ZONE NEVER TAKES A WI-FI FIX: tz_auto.cpp's tzaPickFix loops GPS..PHONE
     (`src <= SELF_PHONE`) and names SELF_NET nowhere in its code.
  9. THE MUSIC RULE (review item 7): geoloc_net.cpp fills GeoWhenIn.musicPlaying from
     musicPlayerIsPlaying(), and pauses the music only for an asked look (`asked && ...`).
 10. OFF REACHES A LOOK ALREADY OUT (the 2026-10-10 review): geoJob asks jobGateWhy() - which
     checks `s_setting == GEO_SET_OFF` and `s_cancel` before the weather's gates - before EACH
     half, never geoGateWhy() alone; geolocSetSetting() sets s_cancel; fold() abandons a cancelled
     look through geoFoldCancelled() (nothing judged or kept) and wipes the request bodies.
 11. NO SCAN BESIDE A HANDSHAKE, NO HANDSHAKE BESIDE A SCAN: geolocLoop asks for a scan only with
     !netRequestActive(), and holds a look while wifiState.scanBusy(); Networks.cpp starts a
     Wi-Fi location round only when geolocScanMayStart(), and a geo ask the switcher's own cadence
     has not made due (geoOnly) is a SCAN_ONLY round - never autoSwitchEvaluate()'s INI reload.
 12. WHAT TWO HALVES CANNOT SEE: fold() hands geoFold() a guard whose anchor comes from
     geoAnchorFrom(..., false, ...) (never a bench fix for a real look) and whose latest is the
     newest snapshot; geolocLoop calls geoExpire() and geoOnLink() every tick; startLook() splits
     the snapshot again with the joined access point asked about anew (joinedLeftOut).
 13. "MAP + MESH" IS COMMITTED BY LEAVING THE SCREEN: in geoloc_net.cpp only geolocScreenLeft()
     and the plain (non-pending) path of geolocSetSetting() hand the service GEO_SET_MESH - the
     pending branch hands it GEO_SET_MAP; app_meshtastic.cpp calls geolocScreenLeft() in
     enterState() and in its destructor.

A contract that cannot find its file or function fails too. The self-test first re-applies each
mistake to an in-memory copy of the real sources and requires its contract to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, match_close, strip_code  # noqa: E402
from check_phonepos_review import log_calls, log_problem  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent
ROOT = REPO / "WiPhone"
FILES = ("geoloc.h", "geoloc.cpp", "geoloc_net.cpp", "https_worker.cpp", "Networks.cpp",
         "meshtastic_service.cpp", "serial_cmd.cpp", "tz_auto.cpp", "app_meshtastic.cpp")

IP_GEO = [r"ipwho\.is", r"ip-api\.com", r"ipinfo\.io", r"get\.geojs\.io", r"freeipapi\.com",
          r"ipapi\.co\b", r"considerIp\\?\"?\s*:\s*true"]

_STRIPPED = {}


def stripped(text):
    key = hash(text)
    if key not in _STRIPPED:
        _STRIPPED[key] = strip_code(text)
    return _STRIPPED[key]


def body_of(code, name):
    span = function_body(code, name) if code else None
    return code[span[0]:span[1]] if span else None


def check(texts):
    out = []
    raw = {k: v for k, v in texts.items() if k in FILES}
    code = {k: stripped(v) for k, v in raw.items()}
    for f in FILES:
        if f not in raw:
            out.append("%s not found" % f)
    if out:
        return out

    # 1. no IP geolocation; the fallback off twice; no SSID in a body
    for name, t in texts.items():
        if name.startswith("tree:"):
            for pat in IP_GEO:
                if re.search(pat, t):
                    out.append("%s: an IP-geolocation host or considerIp:true (%s)" % (name[5:], pat))
    env = re.search(r"static\s+const\s+char\s+ENVELOPE\[\]\s*=\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+);",
                    raw["geoloc.cpp"])
    envtxt = "".join(re.findall(r"\"((?:[^\"\\]|\\.)*)\"", env.group(1))) if env else ""
    if '\\"considerIp\\":false' not in envtxt or '\\"fallbacks\\":{\\"ipf\\":false' not in envtxt:
        out.append("geoloc.cpp: the request envelope must carry \"considerIp\":false AND "
                   "\"fallbacks\":{\"ipf\":false")
    span = function_body(code["geoloc.cpp"], "geoBuildHalf")
    if not span:
        out.append("geoloc.cpp: geoBuildHalf not found")
    elif re.search(r"(?i)ssid", raw["geoloc.cpp"][span[0]:span[1]]) or re.search(r"(?i)ssid", envtxt):
        out.append("geoloc.cpp: an SSID in a request body")

    # 2. the request
    m = re.search(r'#define\s+GEO_USER_AGENT\s+"([^"]*)"', raw["geoloc.h"])
    if not m or re.search(r"\d", m.group(1)) or "FIRMWARE_VERSION" in raw["geoloc.h"][m.start():m.end()]:
        out.append("geoloc.h: GEO_USER_AGENT must be a generic literal - no version, no digit")
    m = re.search(r"#define\s+GEO_ROOTS_PEM\s+([^\n]*)", raw["geoloc.h"])
    roots = m.group(1) if m else ""
    for r in ("ISRG_ROOT_X1_PEM", "ISRG_ROOT_X2_PEM", "ISRG_ROOT_YE_PEM"):
        if r not in roots:
            out.append("geoloc.h: GEO_ROOTS_PEM must pin %s (X1 + X2 + Root YE, review item 6)" % r)
    gn, gnc = raw["geoloc_net.cpp"], code["geoloc_net.cpp"]
    span = function_body(gnc, "post")
    if not span:
        out.append("geoloc_net.cpp: post() not found")
    else:
        b = gnc[span[0]:span[1]]
        rb = gn[span[0]:span[1]]
        if not re.search(r"q\.rootsPem\s*=\s*GEO_ROOTS_PEM\s*;", b):
            out.append("geoloc_net.cpp post(): the request must pin GEO_ROOTS_PEM")
        if not re.search(r"q\.userAgent\s*=\s*GEO_USER_AGENT\s*;", b):
            out.append("geoloc_net.cpp post(): the User-Agent must be GEO_USER_AGENT")
        if not re.search(r'q\.hdrName\[(\d)\]\s*=\s*"Content-Type"', rb) or \
                not re.search(r"q\.hdrValue\[\d\]\s*=\s*GEO_CONTENT_TYPE\s*;", b):
            out.append("geoloc_net.cpp post(): Content-Type: GEO_CONTENT_TYPE must be sent")
        if not re.search(r"q\.method\s*=", b) or '"POST"' not in rb:
            out.append("geoloc_net.cpp post(): the lookup is a POST")

    # 3. the worker's predicate, the PSRAM hook first
    nb = body_of(code["https_worker.cpp"], "netRequestActive")
    if nb is None or not re.search(r"\bgeoRequestActive\s*\(\s*\)", nb):
        out.append("https_worker.cpp: netRequestActive() must include geoRequestActive()")
    sl = body_of(gnc, "startLook")
    if sl is None:
        out.append("geoloc_net.cpp: startLook() not found")
    else:
        h = re.search(r"\btlsInstallPsramHook\s*\(", sl)
        s_ = re.search(r"\bnetWorkerSubmit\s*\(\s*NET_JOB_GEO\b", sl)
        if not h or not s_ or h.start() > s_.start():
            out.append("geoloc_net.cpp startLook(): tlsInstallPsramHook() must come before "
                       "netWorkerSubmit(NET_JOB_GEO, ...)")
    gr = body_of(gnc, "geoRequestActive")
    if gr is None or not re.search(r"s_busy\s*\|\|\s*s_done", gr):
        out.append("geoloc_net.cpp: geoRequestActive() must be s_busy || s_done (never blinks off)")

    # 4. logs and the console
    for fmt, args in log_calls(gn, gnc):
        why = log_problem(fmt, args)
        if why:
            out.append("geoloc_net.cpp: a log line carries %s: %s" % (why, fmt[:70]))
        if re.search(r"(?i)\b(?:mac|bssid|ssid)\b", args) or re.search(r"\bq\.body\b|bodyA|bodyB", args):
            out.append("geoloc_net.cpp: a log line names a MAC, an SSID or a request body: %s" % fmt[:70])
    if re.search(r"\blog_[a-z]\s*\(|\bprintf\s*\(|Serial\s*\.", code["geoloc.cpp"]):
        out.append("geoloc.cpp: logs or prints (it is pure; the glue's lines carry verdicts only)")
    for fn in ("geolocReport", "geolocRowText"):
        b = body_of(gnc, fn)
        if b is None:
            out.append("geoloc_net.cpp: %s() not found" % fn)
        elif re.search(r"\b(?:latI|lonI|lat|lon|mac|bssid|bodyA|bodyB)\b", b):
            out.append("geoloc_net.cpp: %s() names a coordinate, a MAC or a request body" % fn)

    # 5. the consent rule
    ser, serc = raw["serial_cmd.cpp"], code["serial_cmd.cpp"]
    ms_ = re.search(r'strcasecmp\(line,\s*"netloc"\)', ser)
    calls = [mm for mm in re.finditer(r"\bgeolocSetSetting\s*\(", serc)]
    if not ms_ or not calls:
        out.append("serial_cmd.cpp: the `netloc` handler (or its geolocSetSetting call) not found")
    else:
        for mm in calls:
            if mm.start() < ms_.end():
                out.append("serial_cmd.cpp: geolocSetSetting() outside the `netloc` handler")
                continue
            if not re.search(r"\bgeoSettingSerialOk\s*\(", serc[ms_.end():mm.start()]):
                out.append("serial_cmd.cpp: `netloc` sets the setting without asking geoSettingSerialOk()")
            arg = serc[mm.end():match_close(serc, mm.end() - 1)]
            if "GEO_SET_MESH" in arg or re.search(r"\b2\b", arg):
                out.append("serial_cmd.cpp: the cable sets \"map + mesh\"")

    # 6. the auto-switcher
    nw, nwc = raw["Networks.cpp"], code["Networks.cpp"]
    span = function_body(nwc, "Networks::autoSwitchTick")
    if not span:
        out.append("Networks.cpp: Networks::autoSwitchTick not found")
    else:
        b = nwc[span[0]:span[1]]
        if not re.search(r"\bgeoSwitcherMode\s*\(", b):
            out.append("Networks.cpp autoSwitchTick: the mode must come from geoSwitcherMode()")
        cp = re.search(r"\bgeolocOnScan\s*\(", b)
        dl = re.search(r"\bWiFi\s*\.\s*scanDelete\s*\(", b)
        if not cp or not dl or cp.start() > dl.start():
            out.append("Networks.cpp autoSwitchTick: geolocOnScan() must run before WiFi.scanDelete()")
        ev = [mm for mm in re.finditer(r"\bautoSwitchEvaluate\s*\(", b)]
        fulls = []
        for mm in re.finditer(r"(?<![\w#])if\s*\(\s*full\s*\)\s*\{", b):
            fulls.append((mm.end() - 1, match_close(b, mm.end() - 1)))
        if not ev or any(not any(s0 < e.start() < e0 for s0, e0 in fulls) for e in ev):
            out.append("Networks.cpp autoSwitchTick: autoSwitchEvaluate() only inside `if (full)`")
        so = re.search(r"if\s*\(\s*!_scanPending\s*&&\s*\(\s*mode\s*==\s*GEO_SW_SCAN_ONLY\s*\|\|\s*geoOnly\s*\)\s*\)\s*\{", b)
        dc = re.search(r"\bWiFi\s*\.\s*disconnect\s*\(", b)
        if not so:
            out.append("Networks.cpp autoSwitchTick: the SCAN_ONLY round's block not found")
        else:
            se = match_close(b, so.end() - 1)
            if re.search(r"disconnect|bounceRadio", b[so.end():se]):
                out.append("Networks.cpp autoSwitchTick: the SCAN_ONLY round disconnects")
            if dc and dc.start() < so.start():
                out.append("Networks.cpp autoSwitchTick: a WiFi.disconnect() before the SCAN_ONLY branch")
            if not re.search(r"\bgeolocScanMayStart\s*\(", b[so.end():se]):
                out.append("Networks.cpp autoSwitchTick: a Wi-Fi location round must wait for "
                           "geolocScanMayStart() (no scan beside a handshake)")
        go = re.search(r"\bgeoOnly\s*=\s*mode\s*==\s*GEO_SW_FULL\s*&&\s*connected\s*&&\s*_geoScanReq\b", b)
        if not go:
            out.append("Networks.cpp autoSwitchTick: a geo ask under FULL must be a SCAN_ONLY round "
                       "(geoOnly) - never autoSwitchEvaluate()'s INI reload")
    if not re.search(r"\bWiFi\s*\.\s*getScanInfoByIndex\s*\(", gnc) or \
            re.search(r"\bWiFi\s*\.\s*(?:SSID|BSSIDstr|BSSID)\s*\(", gnc):
        out.append("geoloc_net.cpp: the scan must be read with WiFi.getScanInfoByIndex() (no Strings)")

    # 7. the beacon
    msc = code["meshtastic_service.cpp"]
    b = body_of(msc, "MeshtasticService::sendSelfPosition")
    if b is None:
        out.append("meshtastic_service.cpp: sendSelfPosition not found")
    else:
        for need in ("selfNetTxCell", "selfFixSendTime", "selfNetTxNoteSent", "selfNetTxNotePrecise"):
            if not re.search(r"\b%s\s*\(" % need, b):
                out.append("sendSelfPosition: %s() missing" % need)
        m = re.search(r"\bsendPositionOnEx\s*\(", b)
        call = b[m.end():match_close(b, m.end() - 1)] if m else ""
        if not re.search(r"^\s*latI\s*,\s*lonI\s*,", call):
            out.append("sendSelfPosition: sendPositionOnEx() must be handed the CELL (latI, lonI), not fx's")
    b = body_of(msc, "MeshtasticService::posBlockedReasonFor")
    if b is None or not re.search(r"\bselfNetPreciseWins\s*\(", b):
        out.append("posBlockedReasonFor: the precise-fix-wins rule (selfNetPreciseWins) is missing")
    b = body_of(msc, "MeshtasticService::loop")
    if b is None:
        out.append("meshtastic_service.cpp: MeshtasticService::loop not found")
    else:
        c_ = re.search(r"\bselfNetTxCell\s*\(", b)
        g_ = re.search(r"\bmeshPosShouldBeacon\s*\(", b)
        if not c_ or not g_ or c_.start() > g_.start():
            out.append("the beacon tick must measure movement on the Wi-Fi cell (selfNetTxCell first)")

    # 8. the time zone
    tz = code["tz_auto.cpp"]
    b = body_of(tz, "tzaPickFix")
    if b is None:
        out.append("tz_auto.cpp: tzaPickFix not found")
    else:
        if not re.search(r"src\s*<=\s*SELF_PHONE\s*;", b):
            out.append("tz_auto.cpp tzaPickFix: the loop must stop at SELF_PHONE (never a Wi-Fi fix)")
    if re.search(r"\bSELF_NET\b", tz):
        out.append("tz_auto.cpp: SELF_NET in code - a Wi-Fi fix never chooses the clock's zone")

    # 9. the music rule
    if not re.search(r"\bin\.musicPlaying\s*=\s*musicPlayerIsPlaying\s*\(\s*\)", gnc):
        out.append("geoloc_net.cpp: GeoWhenIn.musicPlaying must be musicPlayerIsPlaying() (review item 7)")
    for mm in re.finditer(r"\bmusicPlayerPause\s*\(", gnc):
        before = gnc[max(0, mm.start() - 400):mm.start()]
        if not re.search(r"\basked\s*&&\s*musicPlayerIsPlaying\s*\(", before):
            out.append("geoloc_net.cpp: the music paused for a look nobody asked for")

    # 10. off reaches a look already out
    jg = body_of(gnc, "jobGateWhy")
    if jg is None or not re.search(r"\bs_setting\s*==\s*GEO_SET_OFF\b", jg) or \
            not re.search(r"\bs_cancel\b", jg):
        out.append("geoloc_net.cpp: jobGateWhy() must refuse when switched off (s_setting, s_cancel)")
    gj = body_of(gnc, "geoJob")
    if gj is None or len(re.findall(r"\bjobGateWhy\s*\(", gj)) < 2 or re.search(r"\bgeoGateWhy\s*\(", gj):
        out.append("geoloc_net.cpp geoJob(): jobGateWhy() before EACH half (the owner's off first)")
    ss = body_of(gnc, "geolocSetSetting")
    if ss is None or not re.search(r"\bs_cancel\s*=\s*true\s*;", ss):
        out.append("geoloc_net.cpp geolocSetSetting(): off must cancel a look that is out (s_cancel)")
    fb = body_of(gnc, "fold")
    if fb is None or not re.search(r"\bgeoFoldCancelled\s*\(", fb) or \
            not re.search(r"memset\s*\(\s*s_m->bodyA\b", fb):
        out.append("geoloc_net.cpp fold(): a cancelled look goes through geoFoldCancelled(), and the "
                   "request bodies are wiped")

    # 11. no scan beside a handshake, no handshake beside a scan
    gl = body_of(gnc, "geolocLoop")
    if gl is None:
        out.append("geoloc_net.cpp: geolocLoop() not found")
    else:
        rq = re.search(r"if\s*\(([^{]*)\)\s*\{\s*wifiState\s*\.\s*requestGeoScan\s*\(", gl)
        if not rq or not re.search(r"!\s*netRequestActive\s*\(\s*\)", rq.group(1)):
            out.append("geoloc_net.cpp geolocLoop(): a scan is asked for only with !netRequestActive()")
        sb = re.search(r"\bwifiState\s*\.\s*scanBusy\s*\(", gl)
        sl_ = re.search(r"\bstartLook\s*\(", gl)
        if not sb or not sl_ or sb.start() > sl_.start():
            out.append("geoloc_net.cpp geolocLoop(): a look waits while wifiState.scanBusy()")
        for fn in ("geoExpire", "geoOnLink"):
            if not re.search(r"\b%s\s*\(" % fn, gl):
                out.append("geoloc_net.cpp geolocLoop(): %s() every tick" % fn)
    ms_ = body_of(gnc, "geolocScanMayStart")
    if ms_ is None or not re.search(r"!\s*netRequestActive\s*\(\s*\)", ms_):
        out.append("geoloc_net.cpp: geolocScanMayStart() must be !netRequestActive()")

    # 12. what two halves cannot see
    if fb is not None:
        if not re.search(r"\bgeoAnchorFrom\s*\([^;]*,\s*false\s*,", fb):
            out.append("geoloc_net.cpp fold(): the jump anchor (geoAnchorFrom, bench fixes refused)")
        if not re.search(r"\bguard\.latest\s*=", fb) or not re.search(r"\bgeoFold\s*\([^;]*&guard\s*\)", fb):
            out.append("geoloc_net.cpp fold(): geoFold() must get the guard (anchor + latest scan)")
    if sl is not None and not re.search(r"\bgeoSplitScan\s*\([^;]*joinedLeftOut\s*\(", sl):
        out.append("geoloc_net.cpp startLook(): the joined access point asked about again (joinedLeftOut)")

    # 13. "map + mesh" committed by leaving the screen
    for mm in re.finditer(r"\bsetNetLocSetting\s*\(\s*GEO_SET_MESH\s*\)", gnc):
        fn = None
        for name in ("geolocScreenLeft",):
            span = function_body(gnc, name)
            if span and span[0] < mm.start() < span[1]:
                fn = name
        if fn is None:
            out.append("geoloc_net.cpp: GEO_SET_MESH handed to the service outside geolocScreenLeft()")
    if ss is not None:
        pend = re.search(r"if\s*\(\s*v\s*==\s*GEO_SET_MESH\b[^{]*\)\s*\{", ss)
        if not pend:
            out.append("geoloc_net.cpp geolocSetSetting(): the pending \"map + mesh\" branch not found")
        else:
            pb = ss[pend.end():match_close(ss, pend.end() - 1)]
            if not re.search(r"\bs_meshPending\s*=\s*true", pb) or \
                    not re.search(r"setNetLocSetting\s*\(\s*GEO_SET_MAP\s*\)", pb):
                out.append("geoloc_net.cpp geolocSetSetting(): a chosen \"map + mesh\" stays map-only "
                           "(s_meshPending, GEO_SET_MAP) until the screen is left")
    am = code["app_meshtastic.cpp"]
    for fn in ("MeshtasticApp::enterState", "MeshtasticApp::~MeshtasticApp"):
        b = body_of(am, fn)
        if b is None or not re.search(r"\bgeolocScreenLeft\s*\(", b):
            out.append("app_meshtastic.cpp %s: geolocScreenLeft() (commits a chosen \"map + mesh\")" % fn)
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
    return texts


MUTATIONS = [
    ("an IP lookup", "tree:WiPhone/geoloc_net.cpp",
     lambda t: t + '\nstatic const char* U = "https://ipinfo.io/json";\n'),
    ("considerIp dropped from the envelope", "geoloc.cpp",
     lambda t: t.replace('"{\\"considerIp\\":false,', '"{', 1)),
    ("the ipf fallback left on", "geoloc.cpp",
     lambda t: t.replace('\\"ipf\\":false', '\\"ipf\\":true', 1)),
    ("an SSID in the body", "geoloc.cpp",
     lambda t: t.replace('"%s{\\"macAddress\\":', '"%s{\\"ssid\\":\\"x\\",\\"macAddress\\":', 1)),
    ("a versioned User-Agent", "geoloc.h",
     lambda t: t.replace('#define GEO_USER_AGENT     "WiPhone-NH"', '#define GEO_USER_AGENT     "WiPhone-NH/0.9.82"', 1)),
    ("Root YE not pinned", "geoloc.h",
     lambda t: t.replace("ISRG_ROOT_X1_PEM ISRG_ROOT_X2_PEM ISRG_ROOT_YE_PEM", "ISRG_ROOT_X1_PEM ISRG_ROOT_X2_PEM", 1)),
    ("no Content-Type", "geoloc_net.cpp",
     lambda t: t.replace('q.hdrName[0] = "Content-Type";', 'q.hdrName[0] = "Accept";', 1)),
    ("the roots replaced", "geoloc_net.cpp",
     lambda t: t.replace("q.rootsPem = GEO_ROOTS_PEM;", "q.rootsPem = ISRG_ROOT_X1_PEM;", 1)),
    ("netRequestActive without Wi-Fi location", "https_worker.cpp",
     lambda t: t.replace("return aiRequestActive() || wxRequestActive() || geoRequestActive();",
                         "return aiRequestActive() || wxRequestActive();", 1)),
    ("the PSRAM hook after the submit", "geoloc_net.cpp",
     lambda t: t.replace("  tlsInstallPsramHook();                   // BEFORE the first WiFiClientSecure (tile_fetch.h)\n", "", 1)
                .replace("  netWorkerSubmit(NET_JOB_GEO, geoJob);\n", "  netWorkerSubmit(NET_JOB_GEO, geoJob);\n  tlsInstallPsramHook();\n", 1)),
    ("a coordinate in a log line", "geoloc_net.cpp",
     lambda t: t.replace('log_e("WI-FI LOCATION: not asked - %s", why);',
                         'log_e("WI-FI LOCATION: not asked - %s %.5f", why, fx.lat);', 1)),
    ("a MAC in the report", "geoloc_net.cpp",
     lambda t: t.replace("  const GeoSched* sc = &s_m->sched;\n", "  const GeoSched* sc = &s_m->sched;\n  (void)s_m->scan.ap[0].mac;\n", 1)),
    ("the cable choosing map + mesh", "serial_cmd.cpp",
     lambda t: t.replace("if (!geoSettingSerialOk(v)) {", "if (false) {", 1)),
    ("the copy after scanDelete", "Networks.cpp",
     lambda t: t.replace("      geolocOnScan(n);\n", "", 1).replace("    WiFi.scanDelete();\n    return;\n  }\n\n  if (mode == GEO_SW_NONE)",
                                                                   "    WiFi.scanDelete();\n    geolocOnScan(n);\n    return;\n  }\n\n  if (mode == GEO_SW_NONE)", 1)),
    ("a hop from a Wi-Fi location scan", "Networks.cpp",
     lambda t: t.replace("        autoSwitchEvaluate(n);\n      }\n", "      }\n      autoSwitchEvaluate(n);\n", 1)),
    ("the SCAN_ONLY round disconnecting", "Networks.cpp",
     lambda t: t.replace("    _geoScanReq = false;                // spent: geoloc asks again GEO_SCAN_REQ_GAP_MS later\n",
                         "    _geoScanReq = false;                // spent: geoloc asks again GEO_SCAN_REQ_GAP_MS later\n    WiFi.disconnect();\n", 1)),
    ("the raw fix sent instead of its cell", "meshtastic_service.cpp",
     lambda t: t.replace("const bool ok = sendPositionOnEx(latI, lonI, ch,", "const bool ok = sendPositionOnEx(fx.latI, fx.lonI, ch,", 1)),
    ("field 4 stamped now", "meshtastic_service.cpp",
     lambda t: t.replace("const uint32_t whenUtc = selfFixSendTime(&fx, ntpClock.getTrustedUtcTime());",
                         "const uint32_t whenUtc = ntpClock.getTrustedUtcTime();", 1)),
    ("precise fix no longer wins", "meshtastic_service.cpp",
     lambda t: t.replace("if (fx.src == SELF_NET && selfNetPreciseWins(&netTx, &fx, now)) {", "if (false) {", 1)),
    ("the movement gate on the raw fix", "meshtastic_service.cpp",
     lambda t: t.replace("          selfNetTxCell(&netTx, &tx, &txLat, &txLon);\n", "", 1)),
    ("the time zone taking a Wi-Fi fix", "tz_auto.cpp",
     lambda t: t.replace("for (int src = SELF_GPS; src <= SELF_PHONE; src++) {", "for (int src = SELF_GPS; src <= SELF_NET; src++) {", 1)),
    ("an automatic look beside the music", "geoloc_net.cpp",
     lambda t: t.replace("in.musicPlaying = musicPlayerIsPlaying();", "in.musicPlaying = false;", 1)),
    ("the music paused for an automatic look", "geoloc_net.cpp",
     lambda t: t.replace("s_musicPaused = asked && musicPlayerIsPlaying();", "s_musicPaused = musicPlayerIsPlaying();", 1)),
    ("off not reaching the worker", "geoloc_net.cpp",
     lambda t: t.replace("if (s_setting == GEO_SET_OFF || s_cancel) {", "if (false) {", 1)),
    ("half B without the owner's gate", "geoloc_net.cpp",
     lambda t: t.replace("    gate = jobGateWhy();\n", "    gate = geoGateWhy();\n", 1)),
    ("off not cancelling", "geoloc_net.cpp",
     lambda t: t.replace("      s_cancel = true;\n", "", 1)),
    ("a cancelled look folded", "geoloc_net.cpp",
     lambda t: t.replace("geoFoldCancelled(&s_m->sched, &s_m->ha, &s_m->hb, now);", "(void)0;", 1)),
    ("a scan asked beside a handshake", "geoloc_net.cpp",
     lambda t: t.replace("if (wantScan && up && !netRequestActive() && geoScanRequestOk(", "if (wantScan && up && geoScanRequestOk(", 1)),
    ("a look beside a scan", "geoloc_net.cpp",
     lambda t: t.replace("    if (wifiState.scanBusy()) {\n      s_lastDue = GEO_DUE_NET_BUSY;\n    } else if", "    if", 1)),
    ("a geo scan beside a handshake", "Networks.cpp",
     lambda t: t.replace("const bool asked = _geoScanReq && geolocScanMayStart();", "const bool asked = _geoScanReq;", 1)),
    ("a geo ask as a FULL round", "Networks.cpp",
     lambda t: t.replace("const bool geoOnly = mode == GEO_SW_FULL && connected && _geoScanReq &&", "const bool geoOnly = false &&", 1)),
    ("no jump anchor", "geoloc_net.cpp",
     lambda t: t.replace("geoAnchorFrom(meshService.getSelfSlots(), now, false, &anchor);", "anchor.ok = false;", 1)),
    ("no expiry", "geoloc_net.cpp",
     lambda t: t.replace("if (geoExpire(&s_m->sched, net, now)) {", "if (false) {", 1)),
    ("the joined AP not asked about again", "geoloc_net.cpp",
     lambda t: t.replace("geoSplitScan(&s_m->scan, joinedLeftOut(joined), &s_m->split);\n  if (!s_m->split.built) {",
                         "(void)joined;\n  if (!s_m->split.built) {", 1)),
    ("map + mesh on the air at the tap", "geoloc_net.cpp",
     lambda t: t.replace("    persistSetting(GEO_SET_MAP);\n    meshService.setNetLocSetting(GEO_SET_MAP);",
                         "    persistSetting(GEO_SET_MAP);\n    meshService.setNetLocSetting(GEO_SET_MESH);", 1)),
    ("leaving My node not committing", "app_meshtastic.cpp",
     lambda t: t.replace("  if (appState == MESH_MYNODE && state != MESH_MYNODE) {\n    geolocScreenLeft();",
                         "  if (appState == MESH_MYNODE && state != MESH_MYNODE) {\n    (void)0;", 1)),
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
        print("check_geoloc_review: %d problem(s)" % len(problems))
        return 1
    if not selftest(texts):
        print("check_geoloc_review: SELF-TEST FAILED")
        return 1
    print("check_geoloc_review: ok (13 contracts, %d mutations caught)" % len(MUTATIONS))
    return 0


if __name__ == "__main__":
    sys.exit(main())

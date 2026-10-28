#!/usr/bin/env python3
"""check_almanac_review.py - the device-side halves of the 2026-09-27 review fixes (0.9.80: the
Almanac, the map's altitude, the elevation download) stay where they are.

The pure halves are host-tested (test_almanac_lines: the slices = the one-call tables, the moon's
age without a search, the place fallback, the DST reminder with no arithmetic and the day in ITS
offset, ONE countdown; test_almanac: the day job cut anywhere; test_elevtext: "climb*";
test_tileplan: the `maps dl` elevation argument, the resume's space count, the Detail row's units;
test_filepaths: the case-blind guard). What USES them lives in files this suite cannot compile,
and deleting a call spells nothing a test can see. So each is stated POSITIVELY here, on
comment-and-string-blanked source (tests/check_wifi_restore.py's helpers), with a function that
cannot be found failing too:

  1. The plate's GPS fix, its motion and its altitude are self_pos.cpp's selfGpsApply since 0.9.82
     (test_selfpos: the motion only from a stamping RMC, the altitude only from a stamping GGA,
     each with its own stamp; only a sentence that carries a fix stamps it). What stays glue:
     MeshtasticService::gpsUpdate hands the reader's fix to selfGpsApply(&gpsRx, &selfSlots[SELF_GPS
     - 1], fx, ...) and meshtastic_service.cpp writes no gpsRx field itself; WiPhone.ino passes the
     whole fix (meshService.gpsUpdate(fx)) and reads none of the reader's sticky fields (fx.valid,
     latI, lonI, sats, hdopX10, altM). (A GGA used to re-stamp the last RMC's speed as current, and
     a no-fix GGA with the RMCs lost the last position - review 2026-10-10.)
  2. A1 (phone 2: the core day was 148 ms in ONE pass, a 334 ms LOOP STALL): the Almanac's key
     handlers, its build and its entry compute NO table and never search - no almDayCompute,
     almDayEnsure, astroSunDay/MoonDay, a phase or season search, astroMoonPhaseAt, and no
     almDayWorkRun either (that is the timer's); fillCtx sets noSearch; the timer's slice runs
     almDayWorkRun with the deadline (almSliceMore). In almanac_lines.cpp no screen builder
     (almLines*) calls any of those, and phaseAt reaches astroMoonPhaseAt only past its
     noSearch test.
  3. almanacLegalToday's countdown is almCountdownDay - never almCountdown itself - the function
     the Almanac's TODAY row calls. (It was worked out twice; only one copy was tested.)
  4. startJob's card-space check counts the layer through tilePlanElevSpaceTiles, not a bare
     `left += tilePlanElevTiles(`; runElev sets st->elevWhole; recSave upgrades the record to
     TF_REC_ELEV_WHOLE. (A resume asked 22 MB it did not need.)
  5. serial `maps dl` reads its tail with tilePlanParseElevArg (no six-number sscanf).
  6. The Files app's download guard (downloadWritesUnder) is case-blind: filePathWithinNoCase only.
  7. The map: the ruler's elevRulerText gets a coarse flag built from ELEV_Z_COARSE, and the
     Follow me re-centre samples in the same tick (elevStep(true)) before its REDRAW_SCREEN; the
     download form's Detail row is tilePlanDetailText (the units setting), not a literal.
  8. A2: the Almanac's third place is the Maps app's saved view - the SAME namespace and keys
     MapsApp::saveView writes ("maps": putInt "lat", "lon", "saved" = 1) - read once, when the
     app opens (the constructor), never per build.
  9. B1: "US daylight saving" persists (usDstSetPref -> NVS "almdst", loaded with the others), the
     Almanac's OK on ALM_SET_DST toggles it, and both Almanac contexts carry it (c->usDst).
The second review of 2026-09-27 (its pure halves in test_almanac_lines / test_elevtext / test_units):
 10. The same place is almSamePlace - a tolerance around the keyed place - in the Almanac's day
     slots (bindDay: no 0.001-degree grid key) and in the legal cache the Meshtastic Sun screen
     reads (almLegalCached: no exact-double key, which never matched a live fix).
 11. ONE THING A PASS: AlmanacApp::slice rebuilds (rebuildKeepingSelection) only in its `if
     (landed)` branch, which returns before any almDayWorkRun; a slice that lands a part sets
     `landed`; its timer period is its own work + the gap (armTimer(millis() - passMs), and
     armTimer sets `workedMs + ALM_SLICE_GAP_MS`); the Almanac pushes only the band
     (drewInsideBand returns true).
 12. Poor fixes: almPlaceNow hands almPickPlace THE FIX'S OWN rating - getGpsFix's sats and HDOP,
     which are gpsRx.fixSats/fixHdopX10 (the slot's) - and never asks getGpsSky (the last GGA's
     word on the sky: one empty no-fix GGA after a 3-satellite fix reads -1/-1 there, "usable" -
     review 2026-10-11); the map's altitude row names a poor or an old fix ("poor fix" / "old
     fix", elevYouUsable) instead of "you".
 13. Follow me reads the card at most once a fix: elevStep's `now` path keeps the crosshair's
     height within MAPS_ELEV_YOU_MOVE_M of its last sample, and "you" is the crosshair's own
     sample there (elevYou = elevHere).
 14. A rebuild keeps the list's scroll (rebuildKeepingSelection: almMenuTop -> setTop).
 15. The titles are almTitle's (setHeader); the units row is unitsSettingRow in the Almanac's
     Settings AND the Maps menu; `almanac bench` zeroes its AlmDay (heap_caps_calloc).
The weather (0.9.81; its pure half in test_weather):
 16. NO card, HTTP or TLS in the guarded functions: the six A1 functions (key handlers, build, entry,
     the rebuild, bindDay, owedFor) and fillCtx call no SD.*, HTTPClient, WiFiClientSecure, netHttps,
     wxRequest, wxCacheLoadCard, wxClear, wxReport, almReadElevation or elevSampleCard. The card's
     cache is read on the TIMER (AlmanacApp::slice: wxCacheLoadCard), the fetch is queued there too
     (wxStep: the ground read, then wxRequest), and WEATHER's Refresh only sets wxWant. Back's
     ENTRY_OF table is sized ALM_SCREEN_COUNT (an index past it lands on "Sun...").
 17. The loop: WiPhone.ino folds the fetch (wxLoopTick) and keys busy, hardBusy and the 10-tick
     sleep on netRequestActive() - no bare aiRequestActive() left there (a weather handshake at the
     80 MHz idle clock against a 1-tick loop is the AI's 3-of-3 failure); wxLoopTick returns while
     gGbcActive (the card write vs the game's SPI bus); weather_net and https_worker never call
     setInsecure, and weather_net's log lines never carry the URL or a coordinate.
 18. The second weather review (2026-10-03): netHttps calls http.begin() BEFORE the client's own
     connect (begin on a connected client stop()s it - a second, untimed handshake past the heap
     bar); wxJob asks the gates again (wxGateWhy) before each host, not only at submit time; the
     serial `wx` refuses while gGbcActive before it can touch the card.

A self-test plants one hand mutation per contract into the real text and shows each trips.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from check_wifi_restore import function_body, ifs, match_close, rx, strip_code  # noqa: E402

FILES = ["meshtastic_service.cpp", "WiPhone.ino", "app_almanac.cpp", "almanac_lines.cpp",
         "prefs_almanac.cpp", "tile_fetch.cpp", "serial_cmd.cpp", "app_files.cpp", "app_maps.cpp",
         "app_almanac.h", "weather_net.cpp", "https_worker.cpp"]

# What a key handler, a build or an entry must never do (the weather, 0.9.81): the card, HTTP, TLS.
NO_IO = (r"\bSD\s*\.|\bHTTPClient\b|\bWiFiClientSecure\b|\b(?:netHttps|wxRequest|wxCacheLoadCard|wxClear|"
         r"wxReport|almReadElevation|elevSampleCard)\s*\(")

# What a key handler or a screen builder must never do (A1): a table, a search or a scan.
HEAVY = (r"\b(?:almDayCompute|almDayEnsure|astroSunDay|astroMoonDay|astroNextMoonPhase|"
         r"astroPrevMoonPhase|astroNextSeason|astroMoonPhaseAt|almMoonAgeWork|almDayWorkRun|"
         r"astroDayJobRun)\s*\(")


def body(code, name, out, where):
    span = function_body(code, name)
    if span is None:
        out.append("%s: %s() not found - if it was renamed, update this guard" % (where, name))
    return span


def problems(texts):
    out = []
    code = {f: strip_code(t) for f, t in texts.items()}

    # 1. the GPS fix, its motion and altitude: selfGpsApply's (host-tested); the glue hands it over
    c = code["meshtastic_service.cpp"]
    span = body(c, "MeshtasticService::gpsUpdate", out, "meshtastic_service.cpp")
    if span:
        bs, be = span
        if not rx(r"\bselfGpsApply\s*\(\s*&\s*gpsRx\s*,\s*&\s*selfSlots\s*\[\s*SELF_GPS\s*-\s*1\s*\]\s*,"
                  r"\s*fx\s*,").search(c, bs, be):
            out.append("gpsUpdate: selfGpsApply(&gpsRx, &selfSlots[SELF_GPS - 1], fx, ...) not found")
    for m in rx(r"\bgpsRx\s*\.\s*\w+\s*(?:[-+*/|&]?=(?!=)|\+\+|--)").finditer(c):
        out.append("meshtastic_service.cpp writes `%s` itself - only selfGpsApply stamps the GPS fix"
                   % m.group(0).strip())
    c = code["WiPhone.ino"]
    if not rx(r"\bmeshService\s*\.\s*gpsUpdate\s*\(\s*fx\s*\)\s*;").search(c):
        out.append("WiPhone.ino: meshService.gpsUpdate(fx) - the whole fix - not found")
    for m in rx(r"\bfx\s*\.\s*(valid|latI|lonI|sats|hdopX10|altM)\b").finditer(c):
        out.append("WiPhone.ino reads the reader's sticky fx.%s - a stale no-fix GGA re-stamped the "
                   "last position that way" % m.group(1))

    # 2. A1: no table, search or scan in a key handler, a build or a builder; the slices do it
    c = code["app_almanac.cpp"]
    for fn in ("AlmanacApp::processEvent", "AlmanacApp::build", "AlmanacApp::enter",
               "AlmanacApp::rebuildKeepingSelection", "AlmanacApp::bindDay", "AlmanacApp::owedFor"):
        span = body(c, fn, out, "app_almanac.cpp")
        if span:
            m = rx(HEAVY).search(c, span[0], span[1])
            if m:
                out.append("app_almanac.cpp: %s calls %s - a table or a search outside the slices "
                           "(the 148 ms pass, phone 2)" % (fn, m.group(0).rstrip("( ")))
    for fn in ("AlmanacApp::processEvent", "AlmanacApp::build", "AlmanacApp::enter",
               "AlmanacApp::rebuildKeepingSelection", "AlmanacApp::bindDay", "AlmanacApp::owedFor",
               "AlmanacApp::fillCtx"):
        span = body(c, fn, out, "app_almanac.cpp")
        if span:
            m = rx(NO_IO).search(c, span[0], span[1])
            if m:
                out.append("app_almanac.cpp: %s calls %s - card, HTTP or TLS outside the timer (16)"
                           % (fn, m.group(0).rstrip("( .")))
    span = body(c, "AlmanacApp::fillCtx", out, "app_almanac.cpp")
    if span and not rx(r"\bnoSearch\s*=\s*true\b").search(c, span[0], span[1]):
        out.append("AlmanacApp::fillCtx: noSearch is not set - a builder would search in a key handler")
    span = body(c, "AlmanacApp::slice", out, "app_almanac.cpp")
    if span and not rx(r"\balmDayWorkRun\s*\([^;]*\balmSliceMore\b").search(c, span[0], span[1]):
        out.append("AlmanacApp::slice: almDayWorkRun without the deadline (almSliceMore)")
    span = body(c, "almSliceMore", out, "app_almanac.cpp")
    if span and not rx(r"\bALM_SLICE_US\b").search(c, span[0], span[1]):
        out.append("almSliceMore: the slice's deadline (ALM_SLICE_US) is not what stops it")
    c = code["almanac_lines.cpp"]
    for fn in ("almLinesToday", "almLinesSun", "almLinesMoon", "almLinesSolunar", "almLinesPosition",
               "almLinesDate", "almLinesSettings"):
        span = body(c, fn, out, "almanac_lines.cpp")
        if span:
            m = rx(HEAVY).search(c, span[0], span[1])
            if m:
                out.append("almanac_lines.cpp: %s calls %s - a builder must not search or scan"
                           % (fn, m.group(0).rstrip("( ")))
    span = body(c, "phaseAt", out, "almanac_lines.cpp")
    if span:
        bs, be = span
        guard = rx(r"\bif\s*\(\s*c->noSearch\s*\)").search(c, bs, be)
        at = rx(r"\bastroMoonPhaseAt\s*\(").search(c, bs, be)
        if not guard or (at and at.start() < guard.start()):
            out.append("phaseAt: astroMoonPhaseAt (the search) is not behind the noSearch test")

    # 3. one countdown
    c = code["app_almanac.cpp"]
    span = body(c, "almanacLegalToday", out, "app_almanac.cpp")
    if span:
        bs, be = span
        if not rx(r"\balmCountdownDay\s*\(").search(c, bs, be):
            out.append("almanacLegalToday: its countdown is not almCountdownDay (TODAY's function)")
        if rx(r"\balmCountdown\s*\(").search(c, bs, be):
            out.append("almanacLegalToday: calls almCountdown( itself - a second copy of the countdown")

    # 4. the resume's space check and the whole-layer record
    c = code["tile_fetch.cpp"]
    span = body(c, "startJob", out, "tile_fetch.cpp")
    if span:
        bs, be = span
        if not rx(r"\bleft\s*\+=\s*tilePlanElevSpaceTiles\s*\(").search(c, bs, be):
            out.append("startJob: the space check does not count the layer via tilePlanElevSpaceTiles")
        if rx(r"\bleft\s*\+=\s*tilePlanElevTiles\s*\(").search(c, bs, be):
            out.append("startJob: `left += tilePlanElevTiles(` - the whole layer again on every resume")
    span = body(c, "runElev", out, "tile_fetch.cpp")
    if span and not rx(r"\belevWhole\s*=(?!=)").search(c, span[0], span[1]):
        out.append("runElev: st->elevWhole is never set")
    span = body(c, "recSave", out, "tile_fetch.cpp")
    if span and not rx(r"\belev\s*=\s*TF_REC_ELEV_WHOLE\b").search(c, span[0], span[1]):
        out.append("recSave: the record is never upgraded to TF_REC_ELEV_WHOLE")

    # 5. `maps dl`'s tail
    c = code["serial_cmd.cpp"]
    if not rx(r"\btilePlanParseElevArg\s*\(").search(c):
        out.append("serial_cmd.cpp: `maps dl` does not read its tail with tilePlanParseElevArg")

    # 6. the Files app's case-blind download guard
    c = code["app_files.cpp"]
    span = body(c, "downloadWritesUnder", out, "app_files.cpp")
    if span:
        bs, be = span
        if not rx(r"\bfilePathWithinNoCase\s*\(").search(c, bs, be):
            out.append("downloadWritesUnder: not case-blind (no filePathWithinNoCase)")
        if rx(r"\bfilePathWithin\s*\(").search(c, bs, be):
            out.append("downloadWritesUnder: a case-sensitive filePathWithin( - FAT's 'Elev' slips past")

    # 7. the map
    c = code["app_maps.cpp"]
    m = rx(r"\belevRulerText\s*\(").search(c)
    if not m:
        out.append("app_maps.cpp: elevRulerText( not found")
    else:
        call_end = c.find(";", m.end())
        pre = c[max(0, m.start() - 600):m.start()]
        if not rx(r"\bcoarse\s*=[^;]*\bELEV_Z_COARSE\b").search(pre) or \
                not rx(r",\s*coarse\s*\)\s*$").search(c[m.start():call_end]):
            out.append("app_maps.cpp: the ruler's climb is not marked coarse (coarse from ELEV_Z_COARSE, "
                       "passed last to elevRulerText)")
    span = body(c, "MapsApp::buildDownload", out, "app_maps.cpp")
    if span and not rx(r"\btilePlanDetailText\s*\([^;]*\bgUnits\b").search(c, span[0], span[1]):
        out.append("MapsApp::buildDownload: the Detail row is not tilePlanDetailText(..., gUnits)")
    m = rx(r"\bfollowLastStamp\s*=\s*stamp\s*;").search(c)
    if not m:
        out.append("app_maps.cpp: the Follow me re-centre (followLastStamp = stamp) not found")
    else:
        ret = rx(r"\breturn\s+REDRAW_SCREEN\s*;").search(c, m.end())
        if not ret or not rx(r"\belevStep\s*\(\s*true\s*\)").search(c, m.end(), ret.start()):
            out.append("app_maps.cpp: the Follow me re-centre does not sample in its own tick "
                       "(elevStep(true) before its REDRAW_SCREEN)")

    # 8. A2: the map's saved view, the Maps app's own namespace and keys, read once
    raw_a, raw_m = texts["app_almanac.cpp"], texts["app_maps.cpp"]
    ns = re.search(r'static const char MAPS_NVS\[\]\s*=\s*"([^"]*)"', raw_m)
    sv = function_body(code["app_maps.cpp"], "MapsApp::saveView")
    if not ns or not sv:
        out.append("app_maps.cpp: MAPS_NVS or MapsApp::saveView not found")
    else:
        save = raw_m[sv[0]:sv[1]]
        for key in ("lat", "lon", "saved"):
            if not re.search(r'putInt\("%s"' % key, save):
                out.append("MapsApp::saveView no longer writes putInt(\"%s\") - the Almanac reads it" % key)
        rd = function_body(code["app_almanac.cpp"], "almReadMapView")
        if not rd:
            out.append("app_almanac.cpp: almReadMapView not found")
        else:
            reader = raw_a[rd[0]:rd[1]]
            if not re.search(r'begin\("%s",\s*true\)' % re.escape(ns.group(1)), reader):
                out.append("almReadMapView: not the Maps app's namespace \"%s\" (read-only)" % ns.group(1))
            for key in ("lat", "lon", "saved"):
                if not re.search(r'getInt\("%s"' % key, reader):
                    out.append("almReadMapView: does not read getInt(\"%s\")" % key)
        ctor = function_body(code["app_almanac.cpp"], "AlmanacApp::AlmanacApp")
        if not ctor or not rx(r"\balmReadMapView\s*\(").search(code["app_almanac.cpp"], ctor[0], ctor[1]):
            out.append("AlmanacApp::AlmanacApp: the map's view is not read when the app opens")
        for fn in ("AlmanacApp::build", "AlmanacApp::refreshPlace", "AlmanacApp::slice"):
            sp = function_body(code["app_almanac.cpp"], fn)
            if sp and rx(r"\balmReadMapView\s*\(").search(code["app_almanac.cpp"], sp[0], sp[1]):
                out.append("app_almanac.cpp: %s reads NVS every time (almReadMapView) - once, at open" % fn)

    # 9. B1: the US daylight-saving setting
    raw_p = texts["prefs_almanac.cpp"]
    if not re.search(r'getInt\("almdst",\s*1\)', raw_p) or \
            not re.search(r'putPref\("almdst"', raw_p):
        out.append('prefs_almanac.cpp: "almdst" is not loaded (default 1) and saved')
    c = code["app_almanac.cpp"]
    span = body(c, "AlmanacApp::processEvent", out, "app_almanac.cpp")
    if span and not rx(r"\bcase\s+ALM_SET_DST\s*:[^}]*?\busDstSetPref\s*\(").search(c, span[0], span[1]):
        out.append("AlmanacApp::processEvent: OK on ALM_SET_DST does not toggle usDstSetPref")
    for fn in ("AlmanacApp::fillCtx", "almanacConsole"):
        span = body(c, fn, out, "app_almanac.cpp")
        if span and not rx(r"\busDst\s*=\s*gUsDst\b").search(c, span[0], span[1]):
            out.append("app_almanac.cpp: %s does not carry the setting (usDst = gUsDst)" % fn)

    # 10. the same place: a tolerance, in the day slots and the legal cache
    c = code["app_almanac.cpp"]
    span = body(c, "AlmanacApp::bindDay", out, "app_almanac.cpp")
    if span:
        bs, be = span
        if not rx(r"\balmSamePlace\s*\(").search(c, bs, be):
            out.append("AlmanacApp::bindDay: the place is not almSamePlace - GPS noise re-keys the day")
        if rx(r"\broundDiv|\blatE3\b|\blonE3\b").search(c, bs, be):
            out.append("AlmanacApp::bindDay: a grid key (0.001-degree rounding) - noise across a line re-keys it")
    span = body(c, "almLegalCached", out, "app_almanac.cpp")
    if span:
        bs, be = span
        if not rx(r"\balmSamePlace\s*\(").search(c, bs, be) or \
                rx(r"->\s*lat\s*==|->\s*lon\s*==").search(c, bs, be):
            out.append("almLegalCached: an exact-double key (a live fix never matches it) - use almSamePlace")

    # 11. one thing a pass
    span = body(c, "AlmanacApp::slice", out, "app_almanac.cpp")
    if span:
        bs, be = span
        blocks = [b for b in ifs(c, bs, be, rx(r"(?<![\w!.>])landed(?!\w)"))
                  if re.fullmatch(r"\s*landed\s*", c[c.find("(", b[0]) + 1:match_close(c, c.find("(", b[0]))])]
        if len(blocks) != 1:
            out.append("AlmanacApp::slice: expected one `if (landed)` branch (the rebuild pass), found %d" % len(blocks))
        else:
            _, kb, ke = blocks[0]
            if not rx(r"\brebuildKeepingSelection\s*\(").search(c, kb, ke) or \
                    not rx(r"\breturn\b").search(c, kb, ke):
                out.append("AlmanacApp::slice: the `if (landed)` branch must rebuild and return")
            for m in rx(r"\brebuildKeepingSelection\s*\(").finditer(c, bs, be):
                if not (kb <= m.start() < ke):
                    out.append("AlmanacApp::slice: a rebuild in the slice's own pass - slice + rebuild + "
                               "push was ~100-120 ms")
            w = rx(r"\balmDayWorkRun\s*\(").search(c, bs, be)
            if not w or w.start() < ke:
                out.append("AlmanacApp::slice: almDayWorkRun must come after the `if (landed)` branch")
        if not rx(r"\blanded\s*=\s*true\b").search(c, bs, be):
            out.append("AlmanacApp::slice: a landed part never sets `landed`")
        after = blocks[0][2] if len(blocks) == 1 else bs
        # The LAST arming in the pass (the slice's own) - the weather's early return arms its own too.
        arms = [m for m in rx(r"\barmTimer\s*\([^;]*;").finditer(c, after, be)]
        if not arms or not rx(r"\barmTimer\s*\([^;]*millis\s*\(\s*\)\s*-\s*passMs").match(c, arms[-1].start()):
            out.append("AlmanacApp::slice: the slice's timer is not re-armed with the pass's own work (the gap is fake)")
    span = body(c, "AlmanacApp::armTimer", out, "app_almanac.cpp")
    if span and not rx(r"\bmsAppTimerEventPeriod\s*=\s*workedMs\s*\+\s*ALM_SLICE_GAP_MS\b").search(c, span[0], span[1]):
        out.append("AlmanacApp::armTimer: the period is not workedMs + ALM_SLICE_GAP_MS - the ino re-arms "
                   "from the pass's START, so the next slice ran on the very next pass")
    h = code["app_almanac.h"]
    span = body(h, "drewInsideBand", out, "app_almanac.h")
    if span and not rx(r"\breturn\s+true\s*;").search(h, span[0], span[1]):
        out.append("app_almanac.h: drewInsideBand does not return true - every repaint pushes the full ~60 ms")

    # 12. poor fixes: the Almanac's place, the map's "you"
    span = body(c, "almPlaceNow", out, "app_almanac.cpp")
    if span:
        bs, be = span
        if not rx(r"\bin\.gpsSats\s*=").search(c, bs, be) or not rx(r"\bin\.gpsHdopX10\s*=").search(c, bs, be):
            out.append("almPlaceNow: almPickPlace is not handed the receiver's sats/HDOP - a poor fix passes")
        elif not rx(r"\bin\.gpsFix\s*=\s*meshService\s*\.\s*getGpsFix\s*\([^;]*,\s*&\s*sats\s*,\s*&\s*hdop\s*\)\s*;"
                    ).search(c, bs, be) or not rx(r"\bin\.gpsSats\s*=\s*sats\s*;").search(c, bs, be) or \
                not rx(r"\bin\.gpsHdopX10\s*=\s*hdop\s*;").search(c, bs, be):
            out.append("almPlaceNow: the fix is not judged by getGpsFix's own sats/HDOP (its rating)")
        if rx(r"\bgetGpsSky\s*\(").search(c, bs, be):
            out.append("almPlaceNow asks getGpsSky - the sky's last word is no rating for a fix (an empty "
                       "no-fix GGA after a 3-satellite fix reads -1/-1 = usable)")
    cs = code["meshtastic_service.cpp"]
    span = body(cs, "MeshtasticService::getGpsFix", out, "meshtastic_service.cpp")
    if span:
        bs, be = span
        if not rx(r"\*\s*sats\s*=\s*gpsRx\s*\.\s*fixSats\s*;").search(cs, bs, be) or \
                not rx(r"\*\s*hdopX10\s*=\s*gpsRx\s*\.\s*fixHdopX10\s*;").search(cs, bs, be) or \
                rx(r"\bgpsRx\s*\.\s*(?:sats|hdopX10)\b").search(cs, bs, be):
            out.append("getGpsFix: sats/hdopX10 are not the fix's own rating (gpsRx.fixSats/fixHdopX10) - "
                       "the Almanac judges the fix by them")
    raw_m = texts["app_maps.cpp"]
    sv = function_body(code["app_maps.cpp"], "MapsApp::elevYouUsable")
    if not sv:
        out.append("app_maps.cpp: MapsApp::elevYouUsable not found")
    else:
        yu = raw_m[sv[0]:sv[1]]
        if not re.search(r'MAPS_SELF_POOR_GPS\)\s*\?\s*"poor fix"', yu) or \
                not re.search(r'MAPS_SELF_OLD_GPS\)\s*\?\s*"old fix"', yu):
            out.append('MapsApp::elevYouUsable: a poor or old fix is not named ("poor fix" / "old fix") - '
                       'the row says "above you" about a fix that can be 20 km off')

    # 13. Follow me: at most one card read a fix
    cm = code["app_maps.cpp"]
    span = body(cm, "MapsApp::elevStep", out, "app_maps.cpp")
    if span:
        bs, be = span
        hy = [b for b in ifs(cm, bs, be, rx(r"\belevHere\.valid\b"))
              if re.fullmatch(r"\s*now\s*&&\s*elevHere\.valid\s*&&[^;]*<=\s*MAPS_ELEV_YOU_MOVE_M\s*",
                              cm[cm.find("(", b[0]) + 1:match_close(cm, cm.find("(", b[0]))])]
        if not hy or not rx(r"MAPS_ELEV_YOU_MOVE_M").search(cm, hy[0][0], hy[0][2]) or \
                not rx(r"\belevHereCx\s*=\s*cx\b").search(cm, hy[0][1], hy[0][2]) or \
                rx(r"\belevSampleInto\s*\(").search(cm, hy[0][1], hy[0][2]):
            out.append("MapsApp::elevStep: Follow me re-reads the crosshair on every GPS-noise pixel "
                       "(no MAPS_ELEV_YOU_MOVE_M hold on the `now` path)")
        if not rx(r"\belevYou\s*=\s*elevHere\s*;").search(cm, bs, be):
            out.append("MapsApp::elevStep: Follow me reads 'you' again - the crosshair's own sample is yours")

    # 14. the scroll kept
    span = body(c, "AlmanacApp::rebuildKeepingSelection", out, "app_almanac.cpp")
    if span and not rx(r"\bsetTop\s*\(\s*almMenuTop\s*\(").search(c, span[0], span[1]):
        out.append("AlmanacApp::rebuildKeepingSelection: the scroll is not kept (setTop(almMenuTop(...)))")

    # 15. titles, the units row, the bench's day
    span = body(c, "AlmanacApp::setHeader", out, "app_almanac.cpp")
    if span and not rx(r"\balmTitle\s*\(").search(c, span[0], span[1]):
        out.append("AlmanacApp::setHeader: the title is not almTitle's (the measured one)")
    cl = code["almanac_lines.cpp"]
    span = body(cl, "almLinesSettings", out, "almanac_lines.cpp")
    if span and not rx(r"\bunitsSettingRow\s*\(").search(cl, span[0], span[1]):
        out.append("almLinesSettings: the units row is not unitsSettingRow (the Maps menu's words)")
    span = body(cm, "MapsApp::buildMenu", out, "app_maps.cpp")
    if span and not rx(r"\bunitsSettingRow\s*\(\s*gUnits\s*\)").search(cm, span[0], span[1]):
        out.append("MapsApp::buildMenu: the units row is not unitsSettingRow(gUnits) (the Almanac's words)")
    span = body(c, "almConsoleBench", out, "app_almanac.cpp")
    if span:
        bs, be = span
        if not rx(r"\bheap_caps_calloc\s*\(").search(c, bs, be) or rx(r"\bheap_caps_malloc\s*\(").search(c, bs, be):
            out.append("almConsoleBench: its AlmDay is not zeroed (heap_caps_calloc) - the builders read garbage")

    # 16. the weather: the card and the fetch on the timer only, Refresh only queues, Back's table
    span = body(c, "AlmanacApp::slice", out, "app_almanac.cpp")
    if span:
        bs, be = span
        if not rx(r"\bwxCacheLoadCard\s*\(").search(c, bs, be):
            out.append("AlmanacApp::slice: the weather cache is not read on the timer (wxCacheLoadCard)")
        if not rx(r"\bwxStep\s*\(").search(c, bs, be):
            out.append("AlmanacApp::slice: the weather's step (wxStep) is not on the timer")
    span = body(c, "AlmanacApp::wxStep", out, "app_almanac.cpp")
    if span:
        bs, be = span
        if not rx(r"\bwxRequest\s*\(").search(c, bs, be) or not rx(r"\balmReadElevation\s*\(").search(c, bs, be):
            out.append("AlmanacApp::wxStep: the fetch is not queued here with the ground read (wxRequest, almReadElevation)")
    span = body(c, "AlmanacApp::processEvent", out, "app_almanac.cpp")
    if span:
        bs, be = span
        if not rx(r"\bwxWant\s*=\s*ALM_WX_REFRESH\b").search(c, bs, be):
            out.append("AlmanacApp::processEvent: WEATHER's Refresh does not set wxWant = ALM_WX_REFRESH")
        if not rx(r"\bENTRY_OF\s*\[\s*ALM_SCREEN_COUNT\s*\]").search(c, bs, be):
            out.append("AlmanacApp::processEvent: Back's ENTRY_OF is not sized ALM_SCREEN_COUNT - Back from "
                       "WEATHER lands on Sun")

    # 17. the loop's hooks, the game, no insecure TLS, no URL in a log
    c = code["WiPhone.ino"]
    if not rx(r"\bwxLoopTick\s*\(\s*\)\s*;").search(c):
        out.append("WiPhone.ino: wxLoopTick() is not called - a finished fetch is never folded")
    if rx(r"\baiRequestActive\s*\(").search(c):
        out.append("WiPhone.ino: a bare aiRequestActive() - busy/hardBusy/the sleep must be netRequestActive() "
                   "(a weather handshake at 80 MHz against a 1-tick loop)")
    n = len(rx(r"\bnetRequestActive\s*\(\s*\)").findall(c))
    if n < 3:
        out.append("WiPhone.ino: netRequestActive() in %d place(s) - busy, hardBusy and the 10-tick sleep need it" % n)
    w = code["weather_net.cpp"]
    span = body(w, "wxLoopTick", out, "weather_net.cpp")
    if span and not rx(r"\bif\s*\(\s*!s_done\s*\|\|\s*gGbcActive\s*\)\s*\{\s*return\s*;").search(w, span[0], span[1]):
        out.append("wxLoopTick: does not wait out a game (gGbcActive) before writing the card")
    for f in ("weather_net.cpp", "https_worker.cpp"):
        if rx(r"\bsetInsecure\s*\(").search(code[f]):
            out.append("%s: setInsecure() - the weather is pinned to ISRG X1 + Root YR, never insecure" % f)
    raw_w = texts["weather_net.cpp"]
    for m in re.finditer(r"\blog_[a-z]\s*\(", raw_w):
        call = raw_w[m.start():raw_w.find(";", m.end())]
        if re.search(r"Url|url\b|\blat\b|\blon\b|latE2|lonE2|%f|%\.\d+f", call):
            out.append("weather_net.cpp: a log line carries the URL or a coordinate: %s" % call[:80])

    # 18. one handshake a request; the gates on the worker; `wx` off the card during a game
    h = code["https_worker.cpp"]
    span = body(h, "netHttps", out, "https_worker.cpp")
    if span:
        bs, be = span
        b = rx(r"\bhttp\s*\.\s*begin\s*\(").search(h, bs, be)
        k = rx(r"\bc\s*->\s*connect\s*\(").search(h, bs, be)
        if not b:
            out.append("netHttps: http.begin() not found")
        elif k and k.start() < b.start():
            out.append("netHttps: the client connects BEFORE http.begin() - begin stop()s it and sendRequest "
                       "handshakes a second time, untimed and past the heap bar")
    span = body(w, "wxJob", out, "weather_net.cpp")
    if span:
        n = len(rx(r"\bwxGateWhy\s*\(\s*\)").findall(w, span[0], span[1]))
        if n < 2:
            out.append("wxJob: the gates asked %d time(s) on the worker - before EACH host (a job queued "
                       "behind the AI can meet a call or a download)" % n)
    a = code["app_almanac.cpp"]
    span = body(a, "almanacWeatherConsole", out, "app_almanac.cpp")
    if span:
        bs, be = span
        g = rx(r"\bif\s*\(\s*gGbcActive\s*\)\s*\{[^}]*\breturn\s*;").search(a, bs, be)
        first = rx(r"\b(?:wxReport|wxClear|wxCacheLoadCard|almReadElevation|wxRequest)\s*\(").search(a, bs, be)
        if not g or (first and first.start() < g.start()):
            out.append("almanacWeatherConsole: no gGbcActive refusal before the card - `wx` beside a game's blit task")
    return out


MUTATIONS = [
    ("the GPS fix stamped by the service itself", "meshtastic_service.cpp",
     lambda t: t.replace("  const unsigned r = selfGpsApply(&gpsRx, &selfSlots[SELF_GPS - 1], fx, millis());\n",
                         "  const unsigned r = selfGpsApply(&gpsRx, &selfSlots[SELF_GPS - 1], fx, millis());\n"
                         "  if (fx.valid) gpsRx.fixMs = millis();\n", 1)),
    ("the GPS slot left out of the gate", "meshtastic_service.cpp",
     lambda t: t.replace("selfGpsApply(&gpsRx, &selfSlots[SELF_GPS - 1], fx, millis())",
                         "selfGpsApply(&gpsRx, NULL, fx, millis())", 1)),
    ("the reader's sticky fields handed over again", "WiPhone.ino",
     lambda t: t.replace("          meshService.gpsUpdate(fx);\n",
                         "          if (fx.valid) meshService.gpsUpdate(fx);\n", 1)),
    ("a day computed in the Left/Right key handler", "app_almanac.cpp",
     lambda t: t.replace("    rebuildKeepingSelection();   // queues the day",
                         "    almDayCompute(&slotOther.day, 0, 0, 0.0, 0.0);\n    rebuildKeepingSelection();   // queues the day", 1)),
    ("the seasons searched in the build", "app_almanac.cpp",
     lambda t: t.replace("  menu = newMenu();\n  infoKey = ROW_INFO_BASE;",
                         "  seasons[0] = astroNextSeason(c.now, 0);\n  menu = newMenu();\n  infoKey = ROW_INFO_BASE;", 1)),
    ("the builders allowed to search", "app_almanac.cpp",
     lambda t: t.replace("  c->noSearch = true;", "  c->noSearch = false;", 1)),
    ("the slices with no deadline", "app_almanac.cpp",
     lambda t: t.replace("almSliceMore, &acct)", "NULL, &acct)", 1)),
    ("a builder searching", "almanac_lines.cpp",
     lambda t: t.replace("  if (c->noSearch) {\n    astroMoonPhaseWith(t, 0, ph);\n    return false;\n  }\n", "", 1)),
    ("TODAY's builder scanning a day", "almanac_lines.cpp",
     lambda t: t.replace("  entries(emit, ctx);\n}\n\nvoid almLinesSun",
                         "  AstroSunDay x;\n  astroSunDay(0, 0, 0, &x);\n  entries(emit, ctx);\n}\n\nvoid almLinesSun", 1)),
    ("the map view under another key", "app_almanac.cpp",
     lambda t: t.replace('p.getInt("saved", 0) == 1', 'p.getInt("save", 0) == 1', 1)),
    ("the map view read on every build", "app_almanac.cpp",
     lambda t: t.replace("bool AlmanacApp::refreshPlace() {\n",
                         "bool AlmanacApp::refreshPlace() {\n  viewOk = almReadMapView(&viewLatI, &viewLonI);\n", 1)),
    ("the DST setting not saved", "prefs_almanac.cpp",
     lambda t: t.replace('  putPref("almdst", gUsDst);', "", 1)),
    ("the DST setting not carried", "app_almanac.cpp",
     lambda t: t.replace("  c->usDst = gUsDst != 0;", "", 1)),
    ("the Detail row a literal again", "app_maps.cpp",
     lambda t: re.sub(r"tilePlanDetailText\([^;]*;", 'snprintf(row, sizeof(row), "Detail: z%d", depth);', t, count=1)),
    ("a second countdown in almanacLegalToday", "app_almanac.cpp",
     lambda t: t.replace("  almCountdownDay(out->now,", "  almCountdown(out->now,", 1)),
    ("the whole layer asked on every resume", "tile_fetch.cpp",
     lambda t: re.sub(r"left \+= tilePlanElevSpaceTiles\([^;]*;",
                      "left += tilePlanElevTiles(s->lat, s->lon, s->radiusKm);", t, count=1)),
    ("the case-sensitive guard back", "app_files.cpp",
     lambda t: t.replace("return filePathWithinNoCase(root, path) || filePathWithinNoCase(path, root);",
                         "return filePathWithin(root, path) || filePathWithin(path, root);", 1)),
    ("Follow me back to the settle wait", "app_maps.cpp",
     lambda t: t.replace("          elevStep(true);\n          elevStep(true);\n", "", 1)),
    ("the ruler never coarse", "app_maps.cpp",
     lambda t: t.replace("climb ? elevHere.m - elevMeas.m : 0.0, gUnits, coarse);",
                         "climb ? elevHere.m - elevMeas.m : 0.0, gUnits, false);", 1)),
    # the second review of 2026-09-27
    ("the day slot keyed on a grid again", "app_almanac.cpp",
     lambda t: t.replace("!almSamePlace(s->lat, s->lon, c->lat, c->lon)",
                         "(int)(s->lat * 1000) != (int)(c->lat * 1000)", 1)),
    ("the legal cache on exact doubles again", "app_almanac.cpp",
     lambda t: t.replace("almSamePlace(s_legal->lat, s_legal->lon, lat, lon)",
                         "s_legal->lat == lat && s_legal->lon == lon", 1)),
    ("the slice rebuilding in its own pass", "app_almanac.cpp",
     lambda t: t.replace("    landed = true;                                // shown by the next timer pass, not this one\n",
                         "    rebuildKeepingSelection();\n", 1)),
    ("the slice's gap from the pass's start", "app_almanac.cpp",
     lambda t: t.replace("  armTimer((uint32_t)(millis() - passMs));\n  return DO_NOTHING;", "  armTimer();\n  return DO_NOTHING;", 1)),
    ("the fixed 15 ms period", "app_almanac.cpp",
     lambda t: t.replace("msAppTimerEventPeriod = workedMs + ALM_SLICE_GAP_MS;", "msAppTimerEventPeriod = ALM_SLICE_GAP_MS;", 1)),
    ("the full-screen push", "app_almanac.h",
     lambda t: t.replace("  bool drewInsideBand() {\n    return true;", "  bool drewInsideBand() {\n    return false;", 1)),
    ("a poor fix handed over as good", "app_almanac.cpp",
     lambda t: t.replace("  in.gpsSats = sats;", "", 1)),
    ("getGpsFix rating the fix by the sky's last word", "meshtastic_service.cpp",
     lambda t: t.replace("    *sats = gpsRx.fixSats;", "    *sats = gpsRx.sats;", 1)),
    ("the Almanac judging the fix by the sky's last word", "app_almanac.cpp",
     lambda t: t.replace("  in.gpsSats = sats;", "  meshService.getGpsSky(&sats, &hdop);\n  in.gpsSats = sats;", 1)),
    ("'above you' about a poor fix", "app_maps.cpp",
     lambda t: t.replace('(kind == MAPS_SELF_POOR_GPS) ? "poor fix"', '(kind == MAPS_SELF_POOR_GPS) ? NULL', 1)),
    ("Follow me reading every noise pixel", "app_maps.cpp",
     lambda t: t.replace("    if (now && elevHere.valid && elevHere.rc", "    if (false && now && elevHere.valid && elevHere.rc", 1)),
    ("Follow me reading 'you' twice", "app_maps.cpp",
     lambda t: t.replace("      elevYou = elevHere;\n", "      elevSampleInto(&elevYou, mapI7ToDeg(la), mapI7ToDeg(lo));\n", 1)),
    ("the scroll back to the top", "app_almanac.cpp",
     lambda t: t.replace("    m->setTop(almMenuTop(oldSel, oldTop, m->selIndex(), m->rows(), m->perScreen()));\n", "", 1)),
    ("a hand-made title", "app_almanac.cpp",
     lambda t: t.replace("  almTitle(screen, ntpClock.isTimeKnown(),", "  (void)(screen, ntpClock.isTimeKnown(),", 1)),
    ("the map's own units words", "app_maps.cpp",
     lambda t: t.replace("menu->addOption(unitsSettingRow(gUnits), ROW_M_UNITS);",
                         'menu->addOption(gUnits == UNITS_US ? "Units: US (ft, mi)" : "Units: metric", ROW_M_UNITS);', 1)),
    # the weather (0.9.81)
    ("the weather's card read in Refresh's key handler", "app_almanac.cpp",
     lambda t: t.replace("      wxWant = ALM_WX_REFRESH;\n", "      wxWant = ALM_WX_REFRESH;\n      wxCacheLoadCard();\n", 1)),
    ("the fetch started in the key handler", "app_almanac.cpp",
     lambda t: t.replace("      wxWant = ALM_WX_REFRESH;\n", "      wxWant = ALM_WX_REFRESH;\n      wxRequest(NULL, true, wxNote, sizeof(wxNote));\n", 1)),
    ("the ground read in the build", "app_almanac.cpp",
     lambda t: t.replace("  wxShownGen = wxGen();", "  { double em; int ez; almReadElevation(0, 0, &em, &ez); }\n  wxShownGen = wxGen();", 1)),
    ("the cache read off the timer", "app_almanac.cpp",
     lambda t: t.replace("    wxCacheLoadCard();\n    wxLoaded = wxCacheLoaded()", "    wxLoaded = wxCacheLoaded()", 1)),
    ("Back's table not grown", "app_almanac.cpp",
     lambda t: t.replace("ENTRY_OF[ALM_SCREEN_COUNT]", "ENTRY_OF[7]", 1)),
    ("the handshake at the idle clock", "WiPhone.ino",
     lambda t: t.replace("netRequestActive() ||  // the handshake at full speed", "aiRequestActive() ||  // the handshake at full speed", 1)),
    ("the fold forgetting the game", "weather_net.cpp",
     lambda t: t.replace("if (!s_done || gGbcActive) {\n    return;", "if (!s_done) {\n    return;", 1)),
    ("an insecure weather client", "https_worker.cpp",
     lambda t: t.replace("  c->setCACert(q->rootsPem);", "  c->setInsecure();", 1)),
    ("the URL in the log", "weather_net.cpp",
     lambda t: t.replace('log_e("weather: failed http %d", s_r->om.code);', 'log_e("weather: failed http %d %s", s_r->om.code, s_q->omUrl);', 1)),
    ("the bench over garbage", "app_almanac.cpp",
     lambda t: t.replace("AlmDay* d = (AlmDay*)heap_caps_calloc(1, sizeof(AlmDay), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n  if (!d) {\n    out(\"almanac bench",
                         "AlmDay* d = (AlmDay*)heap_caps_malloc(sizeof(AlmDay), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n  if (!d) {\n    out(\"almanac bench", 1)),
    # the second weather review (2026-10-03)
    ("the client connected before http.begin()", "https_worker.cpp",
     lambda t: t.replace("    if (!http.begin(*c, q->url)) {\n      r->net = GEM_NET_CONNECT;\n    } else {\n      bool connected = true;\n      if (q->connectFirst) {\n        const uint32_t th = millis();\n        connected = c->connect(q->host, 443, (int32_t)connectMs) == 1;",
                         "    c->connect(q->host, 443, (int32_t)connectMs);\n    if (!http.begin(*c, q->url)) {\n      r->net = GEM_NET_CONNECT;\n    } else {\n      bool connected = true;\n      if (q->connectFirst) {\n        const uint32_t th = millis();\n        connected = true;", 1)),
    ("the gates asked once, at submit time only", "weather_net.cpp",
     lambda t: t.replace("  if (!r->skipped[0] && (gate = wxGateWhy()) != NULL) {", "  if (false) {", 1)),
    ("`wx` beside a game", "app_almanac.cpp",
     lambda t: t.replace('  if (gGbcActive) {\n    out("wx: refused - a game owns the card right now");\n    return;\n  }\n', "", 1)),
]


def main():
    texts = {f: open(os.path.join(ROOT, "WiPhone", f)).read() for f in FILES}
    real = problems(texts)
    for p in real:
        print("  CONTRACT BROKEN: " + p)
    failed = bool(real)
    for what, f, mutate in MUTATIONS:
        mutated = dict(texts)
        mutated[f] = mutate(texts[f])
        if mutated[f] == texts[f]:
            print("  SELF-TEST FAILED: the mutation '%s' did not apply to %s - the source moved; update it"
                  % (what, f))
            failed = True
            continue
        if not [p for p in problems(mutated) if p not in real]:
            print("  SELF-TEST FAILED: the mutation '%s' does not trip the guard" % what)
            failed = True
    if failed:
        return 1
    print("  ok  the 2026-09-27 review's device-side fixes and the weather's (0.9.81) are in place "
          "(%d hand mutations trip)" % len(MUTATIONS))
    return 0


if __name__ == "__main__":
    sys.exit(main())

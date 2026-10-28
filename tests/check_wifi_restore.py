#!/usr/bin/env python3
"""check_wifi_restore.py - nothing outside Networks.cpp may bring the WiFi station back by itself.

Every site that switched the radio off, or lent it to a hotspot, gives it back through
wifiRestoreStation() (WiPhone/Networks.cpp, deciding in WiPhone/wifi_policy.h). Until 0.9.79
each site asked its own subset of the owner's state - the Game Boy's exit the off switch alone,
the loop's retry neither the game nor the switch, the Settings toggles nothing at all (a bare
esp_wifi_start(), which restarts whatever mode the DRIVER last had) - and between them they put
WiFi back on behind "off" after every game on a phone with no saved network.

So this fails the suite on the spellings that start or rejoin the station, anywhere in
WiPhone/*.{cpp,h,ino} except Networks.cpp (which owns them):

    esp_wifi_start(      a bare driver start: the DRIVER's last mode, maybe AP
    esp_wifi_connect(    a join on whatever config the driver holds
    WiFi.reconnect(      the same
    WiFi.begin(          ANY form: no arguments rejoins the driver's config, and (ssid, pwd) is
                         a join that skips connectToWiFi()'s game/hotspot refusal - the tree's
                         one join lives there. Both start the station first.
    WiFi.mode(WIFI_STA)  also WIFI_MODE_STA / WIFI_AP_STA / WIFI_MODE_APSTA
    WiFi.enableSTA(true)

A ban only catches a NEW bad spelling. The review of 0.9.79 reverted six of its fixes together in
a scratch copy - the loop retry's gate, the boot restore, xferStart's game refusal, Settings >
WiFi's hotspot stop, its exit restore, connectToWiFi's refusal - and this file and the policy
suite both stayed green: deleting a guard spells nothing banned. So CONTRACTS below also states,
POSITIVELY, where each guard must be: "every connectToPreferred() in loop() sits inside an `if`
whose condition asks wifiStationWanted()", "xferStart refuses on gGbcActive before transportUp",
and so on. Matched on the same comment-and-string-blanked text, by brace and paren matching
rather than line grepping (check_menu_keys.py's lesson: wrapped calls are where line patterns go
blind). A contract that cannot find its function fails too - a rename must update it, not
silently retire it.

Comments and string/char literals are blanked first (a guard that trips on its own
documentation is one people learn to ignore - check_menu_keys.py found that out). The linker
wrappers in cpu_clock.cpp (`__wrap_esp_wifi_start`, `__real_esp_wifi_start`) are not calls
anyone makes and do not match: the pattern needs a word boundary before `esp_wifi_`.
A self-test runs first, so a pattern that has quietly stopped matching fails loudly.

THE CORE'S AUTO-RECONNECT AND THE LOOP'S RETRY (the 0.9.79 integration review, R1 and R2). Two more
ways the station came back without anyone asking, pinned the same way:

  R1  arduino-esp32's event task answers a NO_AP_FOUND with `WiFi.disconnect(); WiFi.begin();`
      while getAutoReconnect() - and begin() is mode(current | STA). So the flag is held off before
      every line that takes the station away (a hotspot's WiFi.mode(WIFI_AP), a game's disconnect,
      Networks::disable()) and released before the station is given back. `setAutoReconnect(` is
      BANNED outside Networks.cpp, and inside it lives only in wifiAutoReconnectApply(), which
      computes the flag from the holds (wifi_policy.h) - saved copies did not nest.
      `power sleep` refuses on the RADIO (cpuClockRadioOn), not the switch.
      The hold narrows the race without closing it (a core handler already past its flag read
      still begins), so a hotspot that came up switches a returned station off (enableSTA(false)
      after softAP()).
  R2  the loop's join retry may not begin within 10 s of any other join (wifiRetryMayBegin), and
      the loop reads join ages with wifiJoinAgeMs, never `now - stamp`: a restore earlier in the
      same pass stamps AFTER the loop read `now`, and the subtraction called it 49 days old. The
      gate only knows the restore's join through ITS stamp (noteWifiJoinStarted() after the
      begin() in wifiRestoreStation's JOIN) - the one line the first round left unpinned: deleted,
      every check stayed green and the retry began over the fresh join again (the re-review).

MUTATIONS then removes each of those guards from the REAL source, one at a time, and requires the
check to fail - the review's experiment (a guard deleted, the suite still green), kept.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"
OWNER = "Networks.cpp"

BANNED = [
    (re.compile(r"\besp_wifi_start\s*\("), "a bare esp_wifi_start()"),
    (re.compile(r"\besp_wifi_connect\s*\("), "a bare esp_wifi_connect()"),
    (re.compile(r"\bWiFi\s*\.\s*reconnect\s*\("), "WiFi.reconnect()"),
    (re.compile(r"\bWiFi\s*\.\s*begin\s*\("), "WiFi.begin() (any form - the one join is connectToWiFi())"),
    (re.compile(r"\bWiFi\s*\.\s*mode\s*\(\s*WIFI_(?:MODE_)?(?:STA|AP_?STA)\s*\)"), "WiFi.mode(<a STA mode>)"),
    (re.compile(r"\bWiFi\s*\.\s*enableSTA\s*\(\s*true\s*\)"), "WiFi.enableSTA(true)"),
    # R1: the core's auto-reconnect restarts the station on its own - one owner, computed, never a
    # saved copy put back (wifiAutoReconnectHold/Release, Networks.h).
    (re.compile(r"\bsetAutoReconnect\s*\("), "setAutoReconnect() (use wifiAutoReconnectHold/Release)"),
]


def strip_code(t):
    """Blank comments and string/char literals (raw strings too), keeping offsets and lines."""
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
                out.append(blank(t[i:j]))
                i = j
            else:
                out.append(t[i])
                i += 1
        elif t[i] in "\"'":
            q, j = t[i], i + 1
            while j < n and t[j] != q and t[j] != "\n":
                j += 2 if t[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(blank(t[i:j]))
            i = j
        else:
            out.append(t[i])
            i += 1
    return "".join(out)


def findings(text, code=None):
    """The banned spellings in `text`; pass `code` when its blanked form is already at hand."""
    code = strip_code(text) if code is None else code
    hits = []
    for rx, what in BANNED:
        for m in rx.finditer(code):
            hits.append((code.count("\n", 0, m.start()) + 1, what))
    return sorted(hits)


# ── POSITIVE CONTRACTS: where each guard must be ─────────────────────────────────────────────

IF_RX = re.compile(r"(?<![\w#])if\s*\(")


def match_close(code, i):
    """code[i] is '(' or '{': the index of its partner, or -1."""
    open_ch = code[i]
    close_ch = ")" if open_ch == "(" else "}"
    depth = 0
    for j in range(i, len(code)):
        c = code[j]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return j
    return -1


def function_body(code, name):
    """(start, end) of the body of the DEFINITION of `name` ("loop", "GbcApp::~GbcApp", ...), or
    None. A call followed by a brace (`if (f(x)) {`) is not a definition: between the parameter
    list and the `{` only qualifiers or a constructor's initializer list may stand."""
    rx = re.compile(r"(?<![\w:~.>])" + re.escape(name) + r"\s*\(")
    for m in rx.finditer(code):
        close = match_close(code, m.end() - 1)
        if close < 0:
            continue
        k = close + 1
        while k < len(code) and code[k] not in "{;":
            k += 1
        if k >= len(code) or code[k] != "{":
            continue
        between = code[close + 1:k].strip()
        if between and not between.startswith(":") and \
                not re.fullmatch(r"(?:const|override|noexcept|final|\s)*", between):
            continue
        end = match_close(code, k)
        if end > 0:
            return k, end
    return None


def ifs(code, start, end, cond_rx):
    """Every `if (...)` in code[start:end] whose condition matches cond_rx, as
    (if_pos, block_start, block_end). A braceless body runs to its `;`."""
    out = []
    for m in IF_RX.finditer(code, start, end):
        po = m.end() - 1
        pc = match_close(code, po)
        if pc < 0 or pc > end or not cond_rx.search(code, po + 1, pc):
            continue
        k = pc + 1
        while k < end and code[k].isspace():
            k += 1
        be = match_close(code, k) if k < end and code[k] == "{" else code.find(";", k, end)
        if be >= 0:
            out.append((m.start(), k, be))
    return out


def rx(p):
    return re.compile(p)


def contract_problem(code, c):
    """None if contract `c` holds in `code` (blanked source), else what is wrong."""
    span = function_body(code, c["fn"])
    if span is None:
        return f"{c['fn']}() not found - if it was renamed or moved, update this contract"
    bs, be = span
    kind = c["kind"]
    if kind == "calls":
        return None if rx(c["call"]).search(code, bs, be) else f"does not call {c['what']}"
    if kind == "order":
        a = rx(c["first"]).search(code, bs, be)
        b = rx(c["then"]).search(code, bs, be)
        if not a or not b:
            return f"missing {c['what']}"
        return None if a.start() < b.start() else f"wrong order: {c['what']}"
    if kind in ("guarded", "guarded_within"):
        if kind == "guarded_within":
            outer = ifs(code, bs, be, rx(c["outer"]))
            if not outer:
                return f"the branch whose condition matches /{c['outer']}/ is gone"
            _, bs, be = outer[0]
        calls = list(rx(c["call"]).finditer(code, bs, be))
        if not calls:
            return f"no {c['what']} found where it is required"
        guards = ifs(code, bs, be, rx(c["cond"]))
        for m in calls:
            if not any(g_bs < m.start() < g_be for _, g_bs, g_be in guards):
                line = code.count("\n", 0, m.start()) + 1
                return f"line {line}: {c['what']} outside an `if` asking /{c['cond']}/"
        return None
    if kind == "refuses_before":
        later = rx(c["later"]).search(code, bs, be)
        if not later:
            return f"no /{c['later']}/ found"
        for pos, g_bs, g_be in ifs(code, bs, be, rx(c["cond"])):
            if pos < later.start() and re.search(r"\breturn\b", code[g_bs:g_be + 1]):
                return None
        return f"no refusal (`if (/{c['cond']}/) ... return`) before /{c['later']}/: {c['what']}"
    if kind == "absent":
        m = rx(c["pat"]).search(code, bs, be)
        if not m:
            return None
        line = code.count("\n", 0, m.start()) + 1
        return f"line {line}: {c['what']}"
    if kind == "only_within":
        # every occurrence of `pat` in the whole FILE lies inside fn's body
        for m in rx(c["pat"]).finditer(code):
            if not (bs < m.start() < be):
                line = code.count("\n", 0, m.start()) + 1
                return f"line {line}: {c['what']}"
        return None if rx(c["pat"]).search(code, bs, be) else f"{c['fn']}() no longer does it"
    return f"unknown contract kind {kind}"


RESTORE = r"\bwifiRestoreStation\s*\("
AR_HOLD_HOTSPOT = r"\bwifiAutoReconnectHold\s*\(\s*WIFI_AR_HOLD_HOTSPOT\s*\)"
AR_REL_HOTSPOT = r"\bwifiAutoReconnectRelease\s*\(\s*WIFI_AR_HOLD_HOTSPOT\s*\)"
AR_ARM_FALSE = r"\bwifiAutoReconnectArm\s*\(\s*false\s*\)"
AR_ARM_TRUE = r"\bwifiAutoReconnectArm\s*\(\s*true\s*\)"
CONTRACTS = [
    dict(file="WiPhone.ino", fn="loop", kind="guarded",
         call=r"\bconnectToPreferred\s*\(", cond=r"\bwifiStationWanted\s*\(\s*\)",
         what="the loop's retry join (connectToPreferred)",
         why="the retry joined DURING a game and behind the switch - G2"),
    dict(file="WiPhone.ino", fn="setup", kind="calls", call=RESTORE, what="wifiRestoreStation()",
         why="boot with the network Disconnected left Networks::init()'s station up - G6a"),
    dict(file="app_gbc.cpp", fn="GbcApp::~GbcApp", kind="order",
         first=r"\bgGbcActive\s*=\s*false\b", then=RESTORE,
         what="gGbcActive = false, THEN wifiRestoreStation() (under the flag it always says OFF)",
         why="the Game Boy's exit restarted an erased station on a Disconnected phone - G1"),
    dict(file="app_gbc.cpp", fn="GbcApp::startGame", kind="order",
         first=r"\bgGbcActive\s*=\s*true\b", then=r"\bgbcXferStop\s*\(",
         what="gGbcActive = true, THEN gbcXferStop() (so its teardown leaves the radio OFF)",
         why="a headless uploader survived the game over a dead radio"),
    dict(file="app_gbc_xfer.cpp", fn="transportDown", kind="order",
         first=r"\bs_usingAP\s*=\s*false\b", then=RESTORE,
         what="s_usingAP = false, THEN wifiRestoreStation() (it asks whether a hotspot is live)",
         why="the uploader/window teardown asked its own subset of the owner's state"),
    dict(file="app_gbc_xfer.cpp", fn="xferStart", kind="refuses_before",
         cond=r"\bgGbcActive\b", later=r"\btransportUp\s*\(",
         what="serial `up on` mid-game hosted a soft-AP under the emulator - G3",
         why="G3"),
    dict(file="Networks.cpp", fn="connectToWiFi", kind="refuses_before",
         cond=r"\bwifiStationBlockedBy\s*\(", later=r"\bWiFi\s*\.\s*begin\s*\(",
         what="the tree's one join must refuse under a game or a live hotspot",
         why="the last line of defence for every join path"),
    dict(file="Networks.cpp", fn="Networks::connectTo", kind="order",
         first=r"\bconnectToWiFi\s*\(", then=r"\b_userDisabled\s*=\s*false\b",
         what="connectToWiFi() answers, THEN the flags clear (a refused join changes nothing)",
         why="a refused Connect re-enabled a Disconnected network behind the owner"),
    dict(file="GUI.cpp", fn="NetworksApp::~NetworksApp", kind="calls", call=RESTORE,
         what="wifiRestoreStation()", why="leaving Settings > WiFi left its scan's station up - G6b"),
    dict(file="GUI.cpp", fn="NetworksApp::~NetworksApp", kind="guarded",
         call=r"\bresumeReconnect\s*\(", cond=r"\bstationAllowed\s*\(",
         what="resumeReconnect()", why="re-armed the retry with the switch OFF - G5"),
    dict(file="GUI.cpp", fn="NetworksApp::NetworksApp", kind="calls",
         call=r"\bwifiScreenStopsHotspotUploader\s*\(", what="wifiScreenStopsHotspotUploader()",
         why="Settings > WiFi scanned and joined under a live hotspot (the chip panic) - G4"),
    dict(file="GUI.cpp", fn="EditNetworkApp::EditNetworkApp", kind="calls",
         call=r"\bwifiScreenStopsHotspotUploader\s*\(", what="wifiScreenStopsHotspotUploader()",
         why="the edit screen joins too - G4"),
    dict(file="GUI.cpp", fn="EditNetworkApp::processEvent", kind="guarded_within",
         outer=r"\bWIPHONE_KEY_CALL\b", call=r"\bsetRadioOff\s*\(\s*false\s*\)",
         cond=r"\bconnectTo\s*\(",
         what="Connect's setRadioOff(false)",
         why="Connect with WiFi off must switch WiFi on - but only once a join has STARTED: "
             "first, a failed Connect left it on and the loop joined the OLD network"),

    # ── R1: the core's auto-reconnect is held off before the station is taken away ──
    dict(id="R1-hotspot-hold", file="app_gbc_xfer.cpp", fn="transportUp", kind="order",
         first=AR_HOLD_HOTSPOT, then=r"\bWiFi\s*\.\s*mode\s*\(\s*WIFI_AP\s*\)",
         what="wifiAutoReconnectHold(HOTSPOT), THEN WiFi.mode(WIFI_AP)",
         why="R1: a NO_AP_FOUND handled after mode(AP) made the phone AP+STA, the station hunting "
             "beside the sync window's softAP"),
    dict(id="R1-hotspot-fail", file="app_gbc_xfer.cpp", fn="transportUp", kind="order",
         first=AR_REL_HOTSPOT, then=RESTORE,
         what="a failed softAP releases the hold, THEN wifiRestoreStation()",
         why="R1: otherwise the core's auto-reconnect stayed off after a hotspot that never came up"),
    dict(id="R1-hotspot-release", file="app_gbc_xfer.cpp", fn="transportDown", kind="order",
         first=AR_REL_HOTSPOT, then=RESTORE,
         what="wifiAutoReconnectRelease(HOTSPOT), THEN wifiRestoreStation()",
         why="R1: the restore's join must be retried by the core as it always was"),
    dict(id="R1-game-hold", file="app_gbc.cpp", fn="GbcApp::startGame", kind="order",
         first=r"\bwifiAutoReconnectHold\s*\(\s*WIFI_AR_HOLD_GAME\s*\)",
         then=r"\bkosyncWindowClose\s*\(",
         what="wifiAutoReconnectHold(GAME), THEN the window closes and the radio goes off",
         why="a NO_AP_FOUND after the game's disconnect(true) restarted the radio under the emulator"),
    dict(id="R1-game-release", file="app_gbc.cpp", fn="GbcApp::~GbcApp", kind="order",
         first=r"\bwifiAutoReconnectRelease\s*\(\s*WIFI_AR_HOLD_GAME\s*\)", then=RESTORE,
         what="wifiAutoReconnectRelease(GAME), THEN wifiRestoreStation()",
         why="R1: a held flag left after the game meant no core rejoin for the rest of the boot"),
    dict(id="R1-disable", file="Networks.cpp", fn="Networks::disable", kind="order",
         first=AR_ARM_FALSE, then=r"\bdisconnect\s*\(",
         what="wifiAutoReconnectArm(false), THEN disconnect() takes the station away",
         why="R1: a NO_AP_FOUND after disable() restarted the radio behind 'WiFi: off'"),
    dict(id="R1-join-arms", file="Networks.cpp", fn="connectToWiFi", kind="order",
         first=AR_ARM_TRUE, then=r"\bWiFi\s*\.\s*begin\s*\(",
         what="a deliberate join re-arms the core's auto-reconnect before its begin()",
         why="R1: after disable(), nothing else would ever re-arm it"),
    dict(id="R1-resume-arms", file="Networks.cpp", fn="Networks::resumeReconnect", kind="calls",
         call=AR_ARM_TRUE, what="wifiAutoReconnectArm(true)",
         why="R1: WiFi switched back on must re-arm what disable() disarmed"),
    dict(id="R1-one-owner", file="Networks.cpp", fn="wifiAutoReconnectApply", kind="only_within",
         pat=r"\bsetAutoReconnect\s*\(",
         what="setAutoReconnect() outside wifiAutoReconnectApply() (the flag is computed, not set)",
         why="R1: a second writer is a saved copy that does not nest"),
    dict(id="R1-power-sleep", file="serial_cmd.cpp", fn="run", kind="refuses_before",
         cond=r"\bcpuClockRadioOn\s*\(", later=r"\besp_light_sleep_start\s*\(",
         what="`power sleep` must refuse on the RADIO (cpuClockRadioOn), not the switch",
         why="R1(c): light sleep over a radio restarted behind 'off' is a PLL re-lock under it"),
    dict(id="R1-hotspot-sta-off", file="app_gbc_xfer.cpp", fn="transportUp", kind="order",
         first=r"\bWiFi\s*\.\s*softAP\s*\(", then=r"\bWiFi\s*\.\s*enableSTA\s*\(\s*false\s*\)",
         what="a softAP() that came up switches a returned station off (enableSTA(false))",
         why="R1: the hold narrows the race, it does not close it - a core handler already past "
             "its flag read begins after mode(AP) and leaves the hotspot AP+STA"),
    dict(id="R2-tx-cap", file="Networks.cpp", fn="wifiRestoreStation", kind="order",
         first=r"\bWiFi\s*\.\s*begin\s*\(", then=r"\bwifiCapTxPower\s*\(",
         what="the restore's JOIN caps TX power (14 dBm) after its begin()",
         why="R2: the duplicate retry that re-applied the cap is gone"),
    dict(id="R2-restore-stamps", file="Networks.cpp", fn="wifiRestoreStation", kind="order",
         first=r"\bWiFi\s*\.\s*begin\s*\(", then=r"\bnoteWifiJoinStarted\s*\(",
         what="the restore's JOIN stamps its join (noteWifiJoinStarted) after its begin()",
         why="R2: wifiRetryMayBegin only sees the restore's join through this stamp - without it "
             "the same pass's retry began connectToPreferred() over the fresh join again"),

    # ── R2: the loop's retry never re-begins over a join stamped earlier in the same pass ──
    dict(id="R2-retry-young", file="WiPhone.ino", fn="loop", kind="guarded",
         call=r"\bconnectToPreferred\s*\(", cond=r"\bwifiRetryMayBegin\s*\(",
         what="the loop's retry join (connectToPreferred)",
         why="R2: the pass that closed a window re-began over the restore's fresh JOIN"),
    dict(id="R2-no-raw-age", file="WiPhone.ino", fn="loop", kind="absent",
         pat=r"\(\s*uint32_t\s*\)\s*\(\s*now\s*-\s*(?:lastAttempt|lastWifiConnectAttemptMs\s*\(\s*\))",
         what="a join age as `now - stamp` (use wifiJoinAgeMs/wifiRetryMayBegin: a same-pass stamp "
              "is AHEAD of `now`)",
         why="R2: the quiesce and the wake branch read a join milliseconds old as 49 days old"),
]


def check_contracts(texts):
    """texts: {file name: blanked source}. Returns the problems, one line each."""
    problems = []
    for c in CONTRACTS:
        code = texts.get(c["file"])
        if code is None:
            problems.append(f"WiPhone/{c['file']}: file not found (contract {c['fn']})")
            continue
        p = contract_problem(code, c)
        if p:
            tag = f"[{c['id']}] " if "id" in c else ""
            problems.append(f"{tag}WiPhone/{c['file']}: {c['fn']}(): {p}\n      (why it matters: {c['why']})")
    return problems


def contract_selftest():
    ok = True

    def expect(src, c, holds, label):
        nonlocal ok
        got = contract_problem(strip_code(src), c) is None
        if got != holds:
            print(f"  SELF-TEST FAIL (contracts): {label}")
            ok = False

    loop_c = CONTRACTS[0]
    expect("void loop() {\n if (wifiStationWanted() && due) {\n  if (w) {\n"
           "   TIME_STEP(\"x\", r = wifiState.connectToPreferred());\n  }\n }\n}\n",
           loop_c, True, "a gated retry, nested and inside a macro, holds")
    expect("void loop() {\n if (!wifiState.userDisabled() && due) {\n"
           "  wifiState.connectToPreferred();\n }\n}\n", loop_c, False, "the pre-0.9.79 retry fails")
    expect("void loop() {\n if (wifiStationWanted()) { x(); }\n wifiState.connectToPreferred();\n}\n",
           loop_c, False, "a join AFTER the gated block fails")
    expect("void loop() {\n // if (wifiStationWanted()) {\n wifiState.connectToPreferred();\n}\n",
           loop_c, False, "a guard that is only a comment fails")
    expect("void other() {}\n", loop_c, False, "a missing function fails")
    expect("void setup() { if (loop(x)) { y(); } }\nvoid loop() {\n if (wifiStationWanted()) "
           "wifiState.connectToPreferred();\n}\n", loop_c, True,
           "a call followed by `{` is not taken for the definition; a braceless if guards")

    cw = next(c for c in CONTRACTS if c["fn"] == "connectToWiFi")
    good = ("bool connectToWiFi(const char* s, const char* p) {\n"
            " if (const char* by = wifiStationBlockedBy()) {\n  log_e(\"%s\", by);\n  return false;\n }\n"
            " WiFi.begin(s, p);\n return true;\n}\n")
    expect(good, cw, True, "connectToWiFi's refusal holds")
    expect(good.replace("return false;", "(void)0;"), cw, False, "a refusal that does not return fails")
    expect("bool connectToWiFi(const char* s, const char* p) {\n WiFi.begin(s, p);\n"
           " if (wifiStationBlockedBy()) { return false; }\n return true;\n}\n", cw, False,
           "a refusal AFTER the begin() fails")

    ed = next(c for c in CONTRACTS if c["fn"] == "EditNetworkApp::processEvent")
    new = ("appEventResult EditNetworkApp::processEvent(EventType event) {\n"
           " if (a) { x(); } else if (event==WIPHONE_KEY_CALL || event==WIPHONE_KEY_SELECT) {\n"
           "  const bool wasOff = wifiState.radioOff();\n"
           "  if (wifiState.connectTo(ssidInput->getText())) {\n"
           "   if (wasOff) { wifiState.setRadioOff(false); }\n  }\n"
           " }\n if (lastWifiOnOff == 0) { wifiState.setRadioOff(false); }\n return R;\n}\n")
    old = ("appEventResult EditNetworkApp::processEvent(EventType event) {\n"
           " if (a) { x(); } else if (event==WIPHONE_KEY_CALL || event==WIPHONE_KEY_SELECT) {\n"
           "  if (wifiState.radioOff()) { wifiState.setRadioOff(false); }\n"
           "  if (wifiState.connectTo(ssidInput->getText())) { y(); }\n"
           " }\n return R;\n}\n")
    expect(new, ed, True, "Connect switches WiFi on inside a successful connectTo()")
    expect(old, ed, False, "Connect switching WiFi on BEFORE connectTo() fails")
    expect(new.replace("wifiState.setRadioOff(false); }\n  }", "}\n  }"), ed, False,
           "Connect that never switches WiFi on fails")

    gb = next(c for c in CONTRACTS if c["fn"] == "GbcApp::~GbcApp")
    expect("GbcApp::~GbcApp() {\n gGbcActive = false;\n wifiRestoreStation(\"g\");\n}\n", gb, True,
           "restore after the flag drops holds")
    expect("GbcApp::~GbcApp() {\n wifiRestoreStation(\"g\");\n gGbcActive = false;\n}\n", gb, False,
           "restore under the flag fails")
    ctor = ("EditNetworkApp::EditNetworkApp(LCD& lcd, S& s)\n  : WindowedApp(lcd, s), ini(N::f) {\n"
            " wifiScreenStopsHotspotUploader(\"x\");\n}\n")
    ec = next(c for c in CONTRACTS if c["fn"] == "EditNetworkApp::EditNetworkApp")
    expect(ctor, ec, True, "a constructor with an initializer list is found")

    byid = {c["id"]: c for c in CONTRACTS if "id" in c}
    raw_age = byid["R2-no-raw-age"]
    expect("void loop() {\n if (wifiJoinAgeMs(now, lastAttempt) < 30000u) { x(); }\n}\n", raw_age, True,
           "the quiesce through wifiJoinAgeMs holds")
    expect("void loop() {\n if ((uint32_t)(now - lastAttempt) < 30000u) { x(); }\n}\n", raw_age, False,
           "the quiesce's raw `now - lastAttempt` fails")
    expect("void loop() {\n if (w && (uint32_t) ( now - lastWifiConnectAttemptMs() ) >= 10000u) {}\n}\n",
           raw_age, False, "the wake branch's raw subtraction fails")
    expect("void loop() {\n // (uint32_t)(now - lastAttempt)\n}\n", raw_age, True,
           "the raw spelling inside a comment is not code")
    owner = byid["R1-one-owner"]
    one = ("static void wifiAutoReconnectApply() {\n WiFi.setAutoReconnect(wifiArOn(s_ar));\n}\n"
           "void Networks::disable() {\n wifiAutoReconnectArm(false);\n}\n")
    expect(one, owner, True, "setAutoReconnect only inside the applier holds")
    expect(one.replace("wifiAutoReconnectArm(false);", "WiFi.setAutoReconnect(false);"), owner, False,
           "a second writer elsewhere in the file fails")
    expect("static void wifiAutoReconnectApply() {\n}\n", owner, False, "an applier that sets nothing fails")
    ps = byid["R1-power-sleep"]
    good_ps = ("static void run(char* line) {\n if (x) {\n  if (cpuClockRadioOn()) {\n   say(\"on\");\n"
               "   return;\n  }\n  esp_light_sleep_start();\n }\n}\n")
    expect(good_ps, ps, True, "`power sleep` refusing on the radio holds")
    expect(good_ps.replace("cpuClockRadioOn()", "!wifiState.radioOff()"), ps, False,
           "`power sleep` refusing on the switch alone fails")
    st = byid["R2-restore-stamps"]
    good_st = ("bool wifiRestoreStation(const char* who) {\n switch (d) {\n case J: {\n"
               "  if (c) {\n   WiFi.begin();\n   noteWifiJoinStarted();\n   wifiCapTxPower();\n  }\n"
               "  break;\n }\n }\n return ok;\n}\n")
    expect(good_st, st, True, "the restore's JOIN stamping after its begin() holds")
    expect(good_st.replace("   noteWifiJoinStarted();\n", ""), st, False,
           "a restore JOIN with no stamp fails (the re-review's slipped mutation)")
    expect(good_st.replace("   noteWifiJoinStarted();\n", "   // noteWifiJoinStarted();\n"), st, False,
           "a stamp that is only a comment fails")
    return ok


def selftest():
    bad = {
        "esp_wifi_start();": 1,
        "if (esp_wifi_start () != ESP_OK) {}": 1,
        "WiFi.reconnect();": 1,
        "WiFi.begin();": 1,
        "WiFi . begin ( );": 1,
        "WiFi.begin(ssid, pwd);": 1,            # a join that skips connectToWiFi()'s refusal
        "WiFi.begin(\n    s, p);": 1,
        "WiFi.mode(WIFI_STA);": 1,
        "WiFi.mode(WIFI_MODE_STA);": 1,
        "WiFi.mode(WIFI_AP_STA);": 1,
        "WiFi.enableSTA(true);": 1,
        "esp_wifi_connect();": 1,
        "WiFi.setAutoReconnect(saved);": 1,     # R1: the saved copy that did not nest
        "WiFi . setAutoReconnect (false);": 1,
    }
    good = [
        "WiFi.mode(WIFI_OFF);",
        "WiFi.mode(WIFI_AP);",
        "extern \"C\" esp_err_t __real_esp_wifi_start(void);",
        "esp_err_t __wrap_esp_wifi_start(void) {",
        "// WiFi.mode(WIFI_STA); WiFi.reconnect();",
        "/* a bare esp_wifi_start() restarts the driver's last mode */",
        "say(\"esp_wifi_start() failed\");",
        "log_e(\"WiFi.begin() refused\");",
        "char c = '\"'; // WiFi.begin()",
        "static const char P[] = R\"HTML(<p>WiFi.begin()</p>)HTML\";",
        "WiFi.getAutoReconnect();",
        "wifiAutoReconnectHold(WIFI_AR_HOLD_HOTSPOT);",
    ]
    ok = True
    for src, want in bad.items():
        if len(findings(src)) != want:
            print(f"  SELF-TEST FAIL: expected a finding in: {src}")
            ok = False
    for src in good:
        if findings(src):
            print(f"  SELF-TEST FAIL: false positive in: {src}")
            ok = False
    multi = "a();\n/* one\n two */\nWiFi.reconnect();\n"
    if findings(multi) != [(4, "WiFi.reconnect()")]:
        print("  SELF-TEST FAIL: line numbers drift across a block comment")
        ok = False
    return ok


def banned_sites(texts):
    """[(file, line, what)] for every banned spelling outside the owner. texts: blanked sources."""
    out = []
    for name in sorted(texts):
        if name == OWNER:
            continue
        for line, what in findings(None, texts[name]):
            out.append((name, line, what))
    return out


# The review's experiment, kept (check_mesh_tx.py's pattern): each R1/R2 guard REMOVED from the
# real source (blanked text, as the check sees it) must trip the contract named here. A pattern
# that no longer matches fails too - the guard was rewritten, so its mutation needs rewriting.
# (file, the contract id or banned text that must be reported, pattern, replacement)
MUTATIONS = [
    ("app_gbc_xfer.cpp", "R1-hotspot-hold",
     r"wifiAutoReconnectHold\s*\(\s*WIFI_AR_HOLD_HOTSPOT\s*\)\s*;", ""),
    ("app_gbc_xfer.cpp", "R1-hotspot-release",       # the 2nd release is transportDown's
     r"(wifiAutoReconnectRelease\s*\(\s*WIFI_AR_HOLD_HOTSPOT\s*\)\s*;(?![\s\S]*wifiAutoReconnectRelease))",
     ""),
    ("app_gbc_xfer.cpp", "R1-hotspot-fail",
     r"wifiAutoReconnectRelease\s*\(\s*WIFI_AR_HOLD_HOTSPOT\s*\)\s*;", ""),
    # the game back on the saved copy it used to keep: the order contract AND the ban trip
    ("app_gbc.cpp", "R1-game-hold",
     r"wifiAutoReconnectHold\s*\(\s*WIFI_AR_HOLD_GAME\s*\)\s*;", "WiFi.setAutoReconnect(false);"),
    ("app_gbc.cpp", "setAutoReconnect()",
     r"wifiAutoReconnectHold\s*\(\s*WIFI_AR_HOLD_GAME\s*\)\s*;", "WiFi.setAutoReconnect(false);"),
    ("app_gbc.cpp", "R1-game-release",
     r"wifiAutoReconnectRelease\s*\(\s*WIFI_AR_HOLD_GAME\s*\)\s*;", ""),
    ("Networks.cpp", "R1-disable", r"wifiAutoReconnectArm\s*\(\s*false\s*\)\s*;", ""),
    ("Networks.cpp", "R1-join-arms",                  # the 1st arm(true) is connectToWiFi's
     r"wifiAutoReconnectArm\s*\(\s*true\s*\)\s*;", ""),
    ("Networks.cpp", "R1-resume-arms",                # the last arm(true) is resumeReconnect's
     r"wifiAutoReconnectArm\s*\(\s*true\s*\)\s*;(?![\s\S]*wifiAutoReconnectArm\s*\(\s*true)", ""),
    ("Networks.cpp", "R1-one-owner",                  # disable() setting the flag itself
     r"wifiAutoReconnectArm\s*\(\s*false\s*\)\s*;", "WiFi.setAutoReconnect(false);"),
    ("Networks.cpp", "R2-tx-cap",                     # the restore's cap is the file's last call
     r"wifiCapTxPower\s*\(\s*\)\s*;(?![\s\S]*wifiCapTxPower\s*\()", ""),
    ("Networks.cpp", "R2-restore-stamps",             # the restore's stamp is the file's last call
     r"noteWifiJoinStarted\s*\(\s*\)\s*;(?![\s\S]*noteWifiJoinStarted\s*\()", ""),
    ("app_gbc_xfer.cpp", "R1-hotspot-sta-off",
     r"WiFi\s*\.\s*enableSTA\s*\(\s*false\s*\)\s*;", ""),
    ("serial_cmd.cpp", "R1-power-sleep",
     r"if\s*\(\s*cpuClockRadioOn\s*\(\s*\)\s*\)", "if (!wifiState.radioOff())"),
    ("WiPhone.ino", "R2-retry-young",
     r"&&\s*due\s*&&\s*wifiRetryMayBegin\s*\(\s*now\s*,\s*lastWifiConnectAttemptMs\s*\(\s*\)\s*\)",
     "&& due"),
    ("WiPhone.ino", "R2-no-raw-age",
     r"wifiJoinAgeMs\s*\(\s*now\s*,\s*lastAttempt\s*\)", "(uint32_t)(now - lastAttempt)"),
]


def check_all(texts):
    """Every banned site and broken contract, as report lines."""
    out = [f"WiPhone/{n}:{line}: {what}" for n, line, what in banned_sites(texts)]
    return out + check_contracts(texts)


def mutation_test(texts):
    ok = True
    for name, want, pat, repl in MUTATIONS:
        mutated, n = re.subn(pat, repl, texts[name], count=1)
        if n != 1:
            print(f"  SELF-TEST FAILED: the '{want}' guard is no longer where its mutation looks in "
                  f"{name} - update MUTATIONS with the guard")
            ok = False
            continue
        got = check_all(dict(texts, **{name: mutated}))
        if not any(want in g for g in got):
            print(f"  SELF-TEST FAILED: removing the guard from {name} did not trip '{want}'")
            ok = False
    if ok:
        print(f"  ok  self-test: removing each of the {len(MUTATIONS)} R1/R2 guards from the real "
              "source trips its own contract")
    return ok


def main():
    if not selftest() or not contract_selftest():
        return 1
    files = sorted(list(ROOT.glob("*.cpp")) + list(ROOT.glob("*.h")) + list(ROOT.glob("*.ino")))
    texts = {f.name: strip_code(f.read_text(errors="replace")) for f in files}
    bad = banned_sites(texts)
    for name, line, what in bad:
        print(f"  WiPhone/{name}:{line}: {what} - give the radio back through "
              f"wifiRestoreStation() (Networks.h), which asks the owner first")
    if bad:
        print(f"  {len(bad)} site(s) bring the WiFi station back without asking (see wifi_policy.h)")
    problems = check_contracts(texts)
    for p in problems:
        print(f"  CONTRACT BROKEN: {p}")
    if bad or problems:
        return 1
    if not mutation_test(texts):
        return 1
    print(f"  ok  no site outside {OWNER} starts or rejoins the station by itself "
          f"({len(files) - 1} files)")
    print(f"  ok  all {len(CONTRACTS)} restore contracts hold (the guards are where they must be)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

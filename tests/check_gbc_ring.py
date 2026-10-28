#!/usr/bin/env python3
"""check_gbc_ring.py - a Game Boy game's exit puts the I2S ring back where the heap wants it (0.9.79).

After a game, free internal RAM came back but the LARGEST block did not: 63,716 -> 32,816 on phone 1,
64,152 -> 28,732 on phone 2, for the rest of the boot. The game's setMonoOutput() (re)installs the
I2S ring (8 x 4 KB of DMA RAM) with the emulator's stacks, VRAM and audio buffer (32 KB) already at
the bottom of the phone's big free block, and IDF 3.3's heap is best fit: what is left of that block
is then smaller than the hole the old ring leaves, so the ring lands right on top of the emulator's
blocks, and at quit they free BELOW it. Phone 1's hole after quit was exactly those four blocks
(32,768 + 4 x 12 B of heap poisoning); on phone 2 music's own install moving the ring away gave
64,316 back. See Audio::reseatI2S() in WiPhone/Audio.cpp.

The fix is an ORDER in ~GbcApp and two GUARDS in Audio::reseatI2S(), in files the host suite cannot
compile, so - like tests/check_call_audio.py, whose contract engine this uses - they are pinned here
POSITIVELY:
  G1  ~GbcApp: gbcReleaseEmulator(), THEN audio->shutdown(), THEN audio->reseatI2S(), THEN
      wifiRestoreStation(). Before the release the ring is placed over the same blocks again; before
      the shutdown the codec loses its clocks while powered; after WiFi, the station's allocations
      have already taken the holes the ring should go back into.
  G2  ~GbcApp: the reseat only when startGame set the ring up (`ringReseat`) - a visit to the picker
      must not reinstall anything.
  G3  startGame: `ringReseat = true` after the setMonoOutput() that (re)installs the ring.
  G4  reseatI2S() refuses while the device is ON (audioOn) or with no driver to move (!i2sInstalled).
  G5  reseatI2S() asks installI2S(false, true) - the DEFAULT ring, FRESH - and then stops it.
      A non-fresh install finds the game's ring matching and returns: nothing would move.
  G6  reseatI2S()'s i2s_stop() only on an installed driver (IDF 3.3 dereferences NULL otherwise).
  G7  i2s_driver_install()/i2s_driver_uninstall() are called NOWHERE but Audio::installI2S(): its
      cache (and so the reseat, which goes through it) is only true while that holds.
  G8  Audio::start() refuses with no driver (after one reinstall) BEFORE it powers the codec or calls
      i2s_start(): a failed install - the reseat's among them - leaves none, IDF 3.3's i2s_start()
      dereferences NULL, and playMusic() calls turnOn() before it installs its own ring.
  G9  Audio::shutdown()'s i2s_stop() only with a driver installed (the same NULL, for every teardown).
  G10 ~GbcApp's shutdown() asks `soundStarted` and NOT `soundOn`: the emu thread clears soundOn when
      I2S starves and leaves the device ON, and skipping the shutdown there made the reseat refuse.
  G11 startGame: `soundStarted = soundOn` right after `soundOn = audio->start()`.
Review 3 (A2) made the driver a precondition of EVERY writer, not just start() (a reinstall can fail
with the device ON - a pop, the ring or a call swapping music's ring out - and turnOn() skipped
start() then):
  G12 turnOn() refuses with no driver (after one reinstall) BEFORE its `if (!audioOn ...)`: whether
      or not the device is on.
  G13 Audio::loop() returns with no driver before its first writer (LocalPcm, RtpStream, Record and
      the microphone reach i2s_write/i2s_read, which dereference NULL).
  G14 installI2S() forgets the geometry with the driver (i2sRx, i2sBufs, i2sLen) when it uninstalls:
      a stale i2sRx let the mic block read a NULL driver.
  G15 playPop() (the pcm overload), playRingtone() and playRtpStream() refuse with no driver after
      their setters and before turnOn(); playPop's refusal names it and restore()s its snapshot.
And (A6) a ring installed with the device OFF is left stopped, the way shutdown() leaves one:
  G16 installI2S(): i2s_stop() inside the successful install, only when !audioOn.
And (review 3, R3-4 on R3-2) those refusals only protect the writers behind them:
  G17 playChunk(), i2s_read() and the music feed's pass() are called nowhere but Audio::loop() (after
      its G13 return), playSample() nowhere but playChunk() (and the caller-less playSampleChunk(), which
      nothing may call), and i2s_write() nowhere but playSample(),
      music's AudioMusicI2s sink and the Game Boy's emuThread (behind soundOn = audio->start(), G8).
      The sink is reached only through the feed's three public writers - pass(), start() and
      closeRing() - so each of those has ONE allowed site, and `s_musicI2s` is named only at start():
  G18 Audio::ceasePlayback()'s feed->closeRing() sits inside `if (i2sInstalled)`.
  G19 Audio::playMusic() refuses with no driver (its `!i2sInstalled || i2sBufs != MUSIC_DMA_BUFS`
      check) BEFORE feed->start(), whose prefill writes the ring.
      🛑 G17 as 772b855 committed it claimed "every writer behind a refusal" while ceasePlayback()'s
      closeRing() had none: after F1 resumed a track inside the Recorder and its Record setters' three
      reinstalls all failed (an exhausted internal heap), the next F1 or any shutdown() topped up the
      ring through IDF 3.3's NULL p_i2s_obj[0] - a LoadProhibited panic. The allowance was the whole
      AudioMusicI2s class, and only feed->pass() call sites were restricted; a closeRing(), a start()
      or a direct s_musicI2s.write() anywhere, or playMusic()'s start() moved above its install check
      (the pre-fix crash shape), all passed.

MUTATIONS then breaks each one in the REAL source, one at a time, and requires its own contract to
report it (tests/check_wifi_restore.py's pattern): a guard rewritten so its mutation no longer
matches fails too, so the mutation gets updated with the guard.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, match_close, strip_code  # noqa: E402
from check_call_audio import NEG, POS, contract_problem, if_blocks, line_of  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"

RESEAT = r"\baudio\s*->\s*reseatI2S\s*\("
THIS = r"(?:\bthis\s*->\s*)?"          # `this->x` and `x` alike; a `!` before `this` negates it

CONTRACTS = [
    dict(id="G1-quit-order", file="app_gbc.cpp", fn="GbcApp::~GbcApp", kind="sequence",
         seq=[r"\bgbcReleaseEmulator\s*\(", r"\baudio\s*->\s*shutdown\s*\(", RESEAT,
              r"\bwifiRestoreStation\s*\("],
         what="the emulator's RAM freed, THEN the device off, THEN the ring reinstalled, THEN WiFi",
         why="installed with the emulator's 32 KB still below it, the ring cut the phone's largest "
             "block in two for the rest of the boot (63,716 -> 32,816 on phone 1)"),
    dict(id="G2-quit-gated", file="app_gbc.cpp", fn="GbcApp::~GbcApp", kind="guarded",
         pat=RESEAT, need=[POS(r"\bringReseat\b")],
         what="the ring's reinstall at quit",
         why="only a game that set the ring up has one to move; a picker visit must cost nothing"),
    dict(id="G3-start-flag", file="app_gbc.cpp", fn="GbcApp::startGame", kind="sequence",
         seq=[r"\baudio\s*->\s*setMonoOutput\s*\(", r"\bringReseat\s*=(?!=)\s*true\b"],
         what="ringReseat = true once the game's setMonoOutput() has set the ring up",
         why="without it the exit never reinstalls the ring and the split comes back"),
    dict(id="G4-refuse-powered", file="Audio.cpp", fn="Audio::reseatI2S", kind="refuses_before",
         need=[POS(THIS + r"\baudioOn\b"), NEG(THIS + r"\bi2sInstalled\b")],
         later=r"\binstallI2S\s*\(",
         what="reseatI2S() must refuse a powered device and a missing driver before installing",
         why="a powered codec losing its I2S clocks clicks; the game's starved path leaves it on"),
    dict(id="G5-fresh-default", file="Audio.cpp", fn="Audio::reseatI2S", kind="sequence",
         seq=[r"\binstallI2S\s*\(\s*false\s*,\s*true\s*\)", r"\bi2s_stop\s*\("],
         what="installI2S(false, true) - the DEFAULT ring, FRESH - THEN i2s_stop()",
         why="a non-fresh install finds the game's ring matching and returns without moving it; "
             "install starts the DMA, and the ring it replaces was stopped by shutdown()"),
    dict(id="G6-stop-guarded", file="Audio.cpp", fn="Audio::reseatI2S", kind="guarded",
         pat=r"\bi2s_stop\s*\(", need=[POS(THIS + r"\bi2sInstalled\b")],
         what="reseatI2S()'s i2s_stop()",
         why="after a failed install there is no driver, and IDF 3.3's i2s_stop() dereferences NULL"),
    dict(id="G8-start-needs-driver", file="Audio.cpp", fn="Audio::start", kind="refuses_before",
         need=[NEG(THIS + r"\bi2sInstalled\b")],
         later=r"\bcodec\s*\.\s*powerUp\s*\(|\bi2s_start\s*\(",
         what="start() must refuse with no driver before it powers the codec or calls i2s_start()",
         why="a failed install leaves no driver and IDF 3.3's i2s_start() dereferences NULL: the "
             "next track after a failed reseat (playMusic() starts the device BEFORE its install) "
             "was a LoadProhibited panic"),
    dict(id="G9-shutdown-stop-guarded", file="Audio.cpp", fn="Audio::shutdown", kind="guarded",
         pat=r"\bi2s_stop\s*\(", need=[POS(THIS + r"\bi2sInstalled\b")],
         what="shutdown()'s i2s_stop()",
         why="every teardown runs it, and with no driver IDF 3.3's i2s_stop() dereferences NULL "
             "inside its critical section"),
    dict(id="G12-turnon-needs-driver", file="Audio.cpp", fn="Audio::turnOn", kind="refuses_before",
         need=[NEG(THIS + r"\bi2sInstalled\b")], later=r"\bif\s*\(\s*!\s*" + THIS + r"\baudioOn\b",
         what="turnOn() must refuse with no driver before (and so whether or not) the device is on",
         why="a reinstall that failed under a playing track left the device ON: turnOn() said yes and "
             "the next pump's i2s_write() dereferenced NULL (a mesh pop or an incoming call over music)"),
    dict(id="G13-loop-needs-driver", file="Audio.cpp", fn="Audio::loop", kind="refuses_before",
         need=[NEG(THIS + r"\bi2sInstalled\b")], later=r"\bplayChunk\s*\(",
         what="Audio::loop() must return with no driver before its first writer",
         why="LocalPcm, RtpStream, Record and the microphone reach i2s_write()/i2s_read() on a NULL driver"),
    dict(id="G14-uninstall-forgets", file="Audio.cpp", fn="Audio::installI2S", kind="sequence_within",
         outer=[POS(THIS + r"\bi2sInstalled\b")], has=r"\bi2s_driver_uninstall\s*\(",
         seq=[r"\bi2s_driver_uninstall\s*\(", r"\bi2sInstalled\s*=(?!=)\s*false\b",
              r"\bi2sRx\s*=(?!=)\s*false\b", r"\bi2sBufs\s*=(?!=)\s*0\b", r"\bi2sLen\s*=(?!=)\s*0\b"],
         what="the uninstall forgetting RX and the geometry with the driver",
         why="after a failed reinstall a stale i2sRx let the loop's mic block i2s_read() a NULL driver"),
    dict(id="G15-pop-needs-driver", file="Audio.cpp", fn="Audio::playPop", args=r"\bpcmLen\b",
         kind="refuses_before", need=[NEG(THIS + r"\bi2sInstalled\b")], later=r"\bturnOn\s*\(",
         what="playPop() refusing with no driver after its setters, before turnOn()",
         why="over a playing track the device is on: the notify pump wrote a NULL driver"),
    dict(id="G15-pop-refusal-restores", file="Audio.cpp", fn="Audio::playPop", args=r"\bpcmLen\b",
         kind="sequence_within", outer=[NEG(THIS + r"\bi2sInstalled\b")], has=r"\breturn\s+false\b",
         seq=[r"\bpopProblem\s*=(?!=)", r"\brestore\s*\(", r"\breturn\s+false\b"],
         what="the refusal naming the reason and putting the snapshot back",
         why="nothing else calls restore() for a pop that never starts"),
    dict(id="G15-ring-needs-driver", file="Audio.cpp", fn="Audio::playRingtone", kind="refuses_before",
         need=[NEG(THIS + r"\bi2sInstalled\b")], later=r"\bturnOn\s*\(",
         what="playRingtone() refusing with no driver after its setters, before turnOn()",
         why="startRingtone() start()s the device on music's ring first: a failed swap rebooted the "
             "phone on the first ring pass"),
    dict(id="G15-rtp-needs-driver", file="Audio.cpp", fn="Audio::playRtpStream", kind="refuses_before",
         need=[NEG(THIS + r"\bi2sInstalled\b")], later=r"\bturnOn\s*\(",
         what="playRtpStream() refusing with no driver after its setters, before turnOn()",
         why="dialling over a track leaves the device on: the first RTP packet wrote a NULL driver"),
    dict(id="G16-install-off-stopped", file="Audio.cpp", fn="Audio::installI2S", kind="guarded",
         pat=r"\bi2s_stop\s*\(", need=[NEG(THIS + r"\baudioOn\b")],
         what="installI2S()'s i2s_stop() of a ring installed with the device off",
         why="a restore() after a hang-up or a closed mic app left a clocked ring running with the "
             "device off - invisible to the idle watchdog - and stopping one under a powered codec clicks"),
    dict(id="G16-install-off-stopped", file="Audio.cpp", fn="Audio::installI2S", kind="sequence_within",
         outer=[POS(r"\bi2s_driver_install\s*\(")], has=r"\bi2s_stop\s*\(",
         seq=[r"\bi2sInstalled\s*=(?!=)\s*true\b", r"\bi2s_stop\s*\("],
         what="the stop inside the SUCCESSFUL install, after it is recorded",
         why="a failed install leaves no driver, and IDF 3.3's i2s_stop() dereferences NULL"),
    dict(id="G11-start-flag-sound", file="app_gbc.cpp", fn="GbcApp::startGame", kind="sequence",
         seq=[r"\bsoundOn\s*=(?!=)\s*audio\s*->\s*start\s*\(",
              r"\bsoundStarted\s*=(?!=)\s*soundOn\b"],
         what="soundStarted = soundOn right after soundOn = audio->start()",
         why="without it the quit never shuts the device down, starved or not"),
    # G18/G19 (R3-4's verification): the music feed's other two writers, each behind a refusal
    dict(id="G18-closering-needs-driver", file="Audio.cpp", fn="Audio::ceasePlayback", kind="guarded",
         pat=r"\bfeed\s*->\s*closeRing\s*\(", need=[POS(THIS + r"\bi2sInstalled\b")],
         what="ceasePlayback()'s feed->closeRing() (its zeros go through i2s_write())",
         why="a reinstall that failed under a playing track (the Recorder's setters after F1 resumed "
             "it) left no driver: the next F1 or any shutdown() pa{user}ed in IDF 3.3's i2s_write()"),
    dict(id="G19-feed-start-after-install", file="Audio.cpp", fn="Audio::playMusic", kind="refuses_before",
         need=[NEG(THIS + r"\bi2sInstalled\b")], later=r"\bfeed\s*->\s*start\s*\(",
         what="playMusic() refusing a failed install before feed->start()'s prefill writes the ring",
         why="the prefill's i2s_write() on a failed install is the 2026-09-25 LoadProhibited panic"),
]

SHUTDOWN = r"\baudio\s*->\s*shutdown\s*\("
QUIT = ("app_gbc.cpp", "GbcApp::~GbcApp")


def quit_shutdown_problems(texts):
    """G10: every audio->shutdown() in ~GbcApp sits in an `if` that asks soundStarted and does not
    ask soundOn (the emu thread clears soundOn when I2S starves; the device is still ON then)."""
    code = texts.get(QUIT[0])
    span = function_body(code, QUIT[1]) if code is not None else None
    if span is None:
        return [f"G10-quit-shutdown-started: {QUIT[1]}() not found in WiPhone/{QUIT[0]} - if it was "
                f"renamed or moved, update this check"]
    bs, be = span
    calls = list(re.finditer(SHUTDOWN, code[bs:be]))
    if not calls:
        return [f"G10-quit-shutdown-started: WiPhone/{QUIT[0]}: {QUIT[1]}: no audio->shutdown() - "
                f"the game must turn the device off at quit"]
    out = []
    blocks = if_blocks(code, bs, be)
    for m in calls:
        pos = bs + m.start()
        conds = [code[c_s:c_e] for c_s, c_e, b_bs, b_be in blocks if b_bs < pos < b_be]
        ok = any(re.search(r"\bsoundStarted\b", c) for c in conds) and \
            not any(re.search(r"\bsoundOn\b", c) for c in conds)
        if not ok:
            out.append(f"G10-quit-shutdown-started: WiPhone/{QUIT[0]}:{line_of(code, pos)}: "
                       f"audio->shutdown() must sit in an `if` asking soundStarted and not soundOn - "
                       f"the starved path leaves the device ON with soundOn false, so the reseat "
                       f"refuses and the heap stays split")
    return out

DRIVER_RX = re.compile(r"\bi2s_driver_(?:un)?install\s*\(")
INSTALLER = ("Audio.cpp", "Audio::installI2S")


def installer_problems(texts):
    """G7: every i2s_driver_install/uninstall call (comments and strings are blanked) sits inside
    Audio::installI2S()."""
    out = []
    code = texts.get(INSTALLER[0])
    span = function_body(code, INSTALLER[1]) if code is not None else None
    if span is None:
        return [f"G7-one-installer: {INSTALLER[1]}() not found in WiPhone/{INSTALLER[0]} - if it "
                f"was renamed or moved, update this check"]
    for name in sorted(texts):
        for m in DRIVER_RX.finditer(texts[name]):
            if name == INSTALLER[0] and span[0] < m.start() < span[1]:
                continue
            out.append(f"G7-one-installer: WiPhone/{name}:{line_of(texts[name], m.start())}: "
                       f"{m.group(0).rstrip('( ')}() outside {INSTALLER[1]}() - installI2S()'s "
                       f"cache, and the Game Boy's reseat through it, go stale")
    return out


# G17 (integration review 3, R3-4 on R3-2): the writers the no-driver refusals stand in front of are
# reached from NOWHERE ELSE. G13 pins Audio::loop()'s return before playChunk(), and G8/G12 the device's
# start - but a playChunk(), i2s_read() or feed pass added outside loop(), or a new raw i2s_write(),
# would sit behind no refusal at all, and IDF 3.3's i2s_write()/i2s_read() dereference a NULL driver.
# (pattern, name, the (file, function) spans it may be called from, what guards those)
CLASS = "class:"                        # a (file, "class:Name") site: anywhere in that class's body
WRITER_SITES = [
    (r"\bplayChunk\s*\(", "playChunk()", {("Audio.cpp", "Audio::loop")},
     "Audio::loop() returns with no driver before it (G13)"),
    (r"\bi2s_read\s*\(", "i2s_read()", {("Audio.cpp", "Audio::loop")},
     "Audio::loop() returns with no driver before it (G13)"),
    (r"\bfeed\s*->\s*pass\s*\(", "the music feed's pass()", {("Audio.cpp", "Audio::loop")},
     "Audio::loop()'s refusal (G13) and the pass's own i2sInstalled guard"),
    (r"(?<!::)\bplaySample\s*\(", "playSample()",
     {("Audio.cpp", "Audio::playChunk"), ("Audio.cpp", "Audio::playSampleChunk")},
     "only playChunk() writes a sample, and only Audio::loop() calls it (playSampleChunk() has no caller)"),
    (r"\bplaySampleChunk\s*\(", "playSampleChunk()", set(),
     "it has no caller (dead since the original firmware); a caller would need a refusal of its own"),
    (r"\bi2s_write\s*\(", "i2s_write()",
     {("Audio.cpp", "Audio::playSample"), ("Audio.cpp", CLASS + "AudioMusicI2s"),
      ("app_gbc.cpp", "GbcApp::emuThread")},
     "each of those writes only behind a refusal: playChunk's loop (G13), the game's soundOn = "
     "audio->start() (G8), and the AudioMusicI2s sink only through the feed's pass() (G13), start() "
     "(G19) and closeRing() (G18) - the three rows below"),
    # (R3-4's verification) the sink's three ways in, and the sink itself, each at ONE site
    (r"\bfeed\s*->\s*closeRing\s*\(", "the music feed's closeRing()", {("Audio.cpp", "Audio::ceasePlayback")},
     "ceasePlayback() calls it only inside `if (i2sInstalled)` (G18)"),
    (r"\bfeed\s*->\s*start\s*\(", "the music feed's start()", {("Audio.cpp", "Audio::playMusic")},
     "playMusic() refuses a failed install before it (G19)"),
    (r"\bs_musicI2s\b", "the music sink s_musicI2s", {("Audio.cpp", "Audio::playMusic")},
     "only feed->start() there hands it to the feed (G19); written directly, or handed over anywhere "
     "else, it is an i2s_write() behind nothing",
     r"^\s*static\s+AudioMusicI2s\s+$"),
]


def class_span(code, name):
    m = re.search(r"\bclass\s+" + re.escape(name) + r"\b[^;{]*\{", code)
    if not m:
        return None
    end = match_close(code, m.end() - 1)
    return None if end < 0 else (m.end() - 1, end)


def writer_problems(texts):
    """G17. A row's optional fifth element is its DECLARATION: a match whose line starts with it
    (`static AudioMusicI2s  s_musicI2s;`) is the object being defined, not a use."""
    out = []
    for row in WRITER_SITES:
        pat, what, sites, why = row[:4]
        decl = re.compile(row[4]) if len(row) > 4 else None
        spans = {}
        for fname, fn in sites:
            code = texts.get(fname)
            if code is None:
                out.append(f"G17-writers: WiPhone/{fname}: file not found")
                continue
            sp = class_span(code, fn[len(CLASS):]) if fn.startswith(CLASS) else function_body(code, fn)
            if sp is None:
                out.append(f"G17-writers: WiPhone/{fname}: {fn}() not found - if it was renamed or moved, "
                           f"update WRITER_SITES")
                continue
            spans.setdefault(fname, []).append(sp)
        for name in sorted(texts):
            for m in re.finditer(pat, texts[name]):
                line = texts[name][texts[name].rfind("\n", 0, m.start()) + 1:m.start()]
                if name.endswith(".h") and re.search(r"\b(?:bool|void|int|size_t)\s+$", line):
                    continue                                    # the member's declaration
                if decl is not None and decl.search(line):
                    continue                                    # the object's own definition
                if texts[name][m.start() - 2:m.start()] == "::" or \
                        any(a < m.start() < z for a, z in spans.get(name, [])):
                    continue                                    # its definition, or an allowed site
                out.append(f"G17-writers: WiPhone/{name}:{line_of(texts[name], m.start())}: {what} outside "
                           f"{', '.join(sorted(fn for _, fn in sites)) or 'its definition'} - it would reach "
                           f"IDF 3.3's NULL driver behind no refusal; the allowed sites are safe because {why}")
    return out


def check(texts):
    problems = []
    for c in CONTRACTS:
        code = texts.get(c["file"])
        if code is None:
            problems.append(f"{c['id']}: WiPhone/{c['file']}: file not found")
            continue
        p = contract_problem(code, c)
        if p:
            problems.append(f"{c['id']}: WiPhone/{c['file']}: {c['fn']}: {p}\n"
                            f"      (why it matters: {c['why']})")
    return problems + installer_problems(texts) + quit_shutdown_problems(texts) + writer_problems(texts)


# (file, the contract id that must be reported, pattern on the BLANKED source, replacement)
MUTATIONS = [
    ("app_gbc.cpp", "G1-quit-order", RESEAT + r"\s*\)", "audio->isOn()"),
    # the reseat put BEFORE the emulator's release: the ring lands over the same blocks again
    ("app_gbc.cpp", "G1-quit-order", r"(?<!void )\bgbcReleaseEmulator\s*\(\s*\)\s*;",
     "audio->reseatI2S(); gbcReleaseEmulator();"),
    # WiFi given back before the ring moved
    ("app_gbc.cpp", "G1-quit-order", r"(?<!void )\bgbcReleaseEmulator\s*\(\s*\)\s*;",
     "wifiRestoreStation(0); gbcReleaseEmulator();"),
    ("app_gbc.cpp", "G1-quit-order", r"\baudio\s*->\s*shutdown\s*\(\s*\)\s*;", ""),
    ("app_gbc.cpp", "G2-quit-gated", r"\bif\s*\(\s*ringReseat\s*&&\s*audio\s*\)", "if (audio)"),
    ("app_gbc.cpp", "G3-start-flag", r"\bringReseat\s*=\s*true\s*;", ""),
    ("Audio.cpp", "G4-refuse-powered",
     r"\bif\s*\(\s*this\s*->\s*audioOn\s*\|\|\s*!\s*this\s*->\s*i2sInstalled\s*\)",
     "if (!this->i2sInstalled)"),
    ("Audio.cpp", "G4-refuse-powered",
     r"\bif\s*\(\s*this\s*->\s*audioOn\s*\|\|\s*!\s*this\s*->\s*i2sInstalled\s*\)",
     "if (this->audioOn)"),
    ("Audio.cpp", "G5-fresh-default", r"\binstallI2S\s*\(\s*false\s*,\s*true\s*\)",
     "installI2S(false, false)"),
    ("Audio.cpp", "G5-fresh-default", r"\binstallI2S\s*\(\s*false\s*,\s*true\s*\)",
     "installI2S(true, true)"),
    ("Audio.cpp", "G6-stop-guarded",
     r"\bif\s*\(\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*i2s_stop\s*\(\s*i2s_num\s*\)\s*;\s*\}",
     "i2s_stop(i2s_num);"),
    # a second uninstall site: shutdown() uninstalling instead of stopping
    ("Audio.cpp", "G7-one-installer",
     r"\bif\s*\(\s*i2s_stop\s*\(\s*i2s_num\s*\)\s*!=\s*ESP_OK\s*\)",
     "if (i2s_driver_uninstall(i2s_num)!=ESP_OK)"),
    # start() without its no-driver guard, and with the guard's refusal gone (retry only)
    ("Audio.cpp", "G8-start-needs-driver",
     r"\bif\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*this\s*->\s*configureI2S\s*\(\s*\)\s*;"
     r"\s*if\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{[^{}]*\}\s*\}", ""),
    ("Audio.cpp", "G8-start-needs-driver",
     r"(\bif\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{[^{}]*?)\breturn\s+false\s*;", r"\1"),
    ("Audio.cpp", "G9-shutdown-stop-guarded",
     r"\bif\s*\(\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*(if\s*\(\s*i2s_stop\s*\(\s*i2s_num\s*\)\s*!=\s*ESP_OK"
     r"\s*\)\s*\{\s*succ\s*=\s*false\s*;\s*\})\s*\}", r"\1"),
    # the quit back on soundOn (the starved path skips the shutdown again), or asking both
    ("app_gbc.cpp", "G10-quit-shutdown-started", r"\bif\s*\(\s*soundStarted\s*&&\s*audio\s*\)",
     "if (soundOn && audio)"),
    ("app_gbc.cpp", "G10-quit-shutdown-started", r"\bif\s*\(\s*soundStarted\s*&&\s*audio\s*\)",
     "if (soundStarted && soundOn && audio)"),
    ("app_gbc.cpp", "G11-start-flag-sound", r"\bsoundStarted\s*=\s*soundOn\s*;", ""),
    # turnOn() without its no-driver refusal, and with it moved under `!audioOn` (the fd2c54f gap)
    ("Audio.cpp", "G12-turnon-needs-driver",
     r"(bool\s+Audio::turnOn\s*\(\s*\)\s*\{)\s*if\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*this\s*->"
     r"\s*configureI2S\s*\(\s*\)\s*;\s*if\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{[^{}]*\}\s*\}", r"\1"),
    ("Audio.cpp", "G12-turnon-needs-driver",
     r"(bool\s+Audio::turnOn\s*\(\s*\)\s*\{\s*)if\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)",
     r"\1if (!this->audioOn && !this->i2sInstalled)"),
    ("Audio.cpp", "G13-loop-needs-driver",
     r"(\bif\s*\(\s*!\s*this\s*->\s*audioLoop\s*\|\|\s*!\s*this\s*->\s*audioOn\s*)\|\|\s*!\s*this\s*->"
     r"\s*i2sInstalled\s*\)", r"\1)"),
    ("Audio.cpp", "G14-uninstall-forgets", r"\bthis\s*->\s*i2sRx\s*=\s*false\s*;", ""),
    ("Audio.cpp", "G15-pop-needs-driver",
     r"\bif\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*this\s*->\s*popProblem\s*=[^{}]*\}", ""),
    ("Audio.cpp", "G15-pop-refusal-restores",
     r"(\bthis\s*->\s*popProblem\s*=\s*;\s*this\s*->\s*playback\s*=\s*Playback::Nothing\s*;\s*)"   # string blanked
     r"this\s*->\s*restore\s*\(\s*\)\s*;", r"\1"),
    ("Audio.cpp", "G15-ring-needs-driver",
     r"\bif\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*log_e\s*\([^;]*\)\s*;\s*this\s*->\s*playback\s*="
     r"\s*Playback::Nothing\s*;\s*return\s+false\s*;\s*\}", ""),
    ("Audio.cpp", "G15-rtp-needs-driver",
     r"(setMonoOutput\s*\(\s*true\s*\)\s*;\s*)if\s*\(\s*!\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*log_e\s*"
     r"\([^;]*\)\s*;\s*return\s+false\s*;\s*\}", r"\1"),
    # the ring left running with the device off, and stopped under a powered codec
    ("Audio.cpp", "G16-install-off-stopped",
     r"\bif\s*\(\s*!\s*this\s*->\s*audioOn\s*\)\s*\{\s*i2s_stop\s*\(\s*i2s_num\s*\)\s*;\s*\}", ""),
    ("Audio.cpp", "G16-install-off-stopped",
     r"\bif\s*\(\s*!\s*this\s*->\s*audioOn\s*\)\s*\{\s*i2s_stop\s*\(\s*i2s_num\s*\)\s*;\s*\}",
     "i2s_stop(i2s_num);"),
    # G17 (review 3, R3-4): a writer reached from outside the refusals - a ring that plays its first
    # chunk itself, a mic read in shutdown(), a raw write in turnOn(), a feed pass in playMusic(), a
    # sample written outside playChunk()
    ("Audio.cpp", "G17-writers", r"(bool\s+Audio::playRingtone\s*\([^)]*\)\s*\{)", r"\1 this->playChunk();"),
    ("Audio.cpp", "G17-writers", r"(bool\s+Audio::shutdown\s*\(\s*\)\s*\{)", r"\1 i2s_read(i2s_num, 0, 0, 0, 0);"),
    ("Audio.cpp", "G17-writers", r"(bool\s+Audio::turnOn\s*\(\s*\)\s*\{)", r"\1 i2s_write(i2s_num, 0, 0, 0, 0);"),
    ("Audio.cpp", "G17-writers", r"(this\s*->\s*feed\s*->\s*start\s*\()", r"this->feed->pass(1); \1"),
    ("Audio.cpp", "G17-writers", r"(bool\s+Audio::turnMicOn\s*\(\s*\)\s*\{)", r"\1 this->playSample();"),
    ("Audio.cpp", "G17-writers", r"(bool\s+Audio::turnMicOn\s*\(\s*\)\s*\{)", r"\1 this->playSampleChunk();"),
    # G17-G19 (R3-4's verification - each of these passed 772b855): the sink reached through the
    # feed's other writers, or written directly, from a function with no refusal; ceasePlayback()'s
    # closeRing() unguarded (the Recorder panic); playMusic()'s start() above its install check (the
    # 2026-09-25 crash shape)
    ("Audio.cpp", "G17-writers", r"(void\s+Audio::resume\s*\(\s*\)\s*\{)", r"\1 this->feed->closeRing();"),
    ("Audio.cpp", "G17-writers", r"(void\s+Audio::resume\s*\(\s*\)\s*\{)",
     r"\1 this->feed->start(0, this->musicDma());"),
    ("Audio.cpp", "G17-writers", r"(void\s+Audio::resume\s*\(\s*\)\s*\{)", r"\1 s_musicI2s.write(0, 0);"),
    ("Audio.cpp", "G18-closering-needs-driver",
     r"\bif\s*\(\s*this\s*->\s*i2sInstalled\s*\)\s*\{\s*(this\s*->\s*feed\s*->\s*closeRing\s*\(\s*\)\s*;)\s*\}",
     r"\1"),
    ("Audio.cpp", "G19-feed-start-after-install",
     r"(this\s*->\s*configureMusicI2S\s*\(\s*!\s*ringWasRunning\s*\)\s*;)([\s\S]*?)"
     r"(this\s*->\s*feed\s*->\s*start\s*\([^;]*\)\s*;)", r"\3 \1\2"),
]


def selftest():
    """The G7 scan on its own, on synthetic text: a call inside installI2S passes, one elsewhere is
    reported, one in a comment or a string is not a call."""
    ok = True
    good = ("void Audio::installI2S(bool m, bool f) {\n i2s_driver_uninstall(i2s_num);\n"
            " i2s_driver_install(i2s_num, &c, 0, NULL);\n}\n"
            "bool Audio::shutdown() {\n // i2s_driver_uninstall(i2s_num);\n"
            " log_e(\"i2s_driver_uninstall(\");\n i2s_stop(i2s_num);\n}\n")
    if installer_problems({"Audio.cpp": strip_code(good)}):
        print("  SELF-TEST FAIL: G7 reported a call inside installI2S(), a comment or a string")
        ok = False
    bad = good.replace(" i2s_stop(i2s_num);", " i2s_driver_uninstall(i2s_num);")
    if not installer_problems({"Audio.cpp": strip_code(bad)}):
        print("  SELF-TEST FAIL: G7 missed an uninstall in shutdown()")
        ok = False
    if not installer_problems({"Audio.cpp": strip_code(good),
                               "app_gbc.cpp": strip_code("void f() { i2s_driver_install(a, b, 0, 0); }\n")}):
        print("  SELF-TEST FAIL: G7 missed an install in another file")
        ok = False
    # G10: the quit's shutdown on soundStarted, never on soundOn (alone or alongside)
    quit = ("GbcApp::~GbcApp() {\n gbcReleaseEmulator();\n if (%s) {\n  audio->shutdown();\n }\n}\n")
    for cond, holds in (("soundStarted && audio", True), ("audio && soundStarted", True),
                        ("soundOn && audio", False), ("soundStarted && soundOn && audio", False),
                        ("audio", False)):
        if (not quit_shutdown_problems({"app_gbc.cpp": strip_code(quit % cond)})) != holds:
            print(f"  SELF-TEST FAIL: G10 on `{cond}` should {'hold' if holds else 'fail'}")
            ok = False
    if not quit_shutdown_problems({"app_gbc.cpp": strip_code(
            "GbcApp::~GbcApp() {\n if (soundStarted && audio) {\n  x();\n }\n audio->shutdown();\n}\n")}):
        print("  SELF-TEST FAIL: G10 missed an unguarded shutdown() after a guarded block")
        ok = False
    # G4's polarity: `this->audioOn` asserted, `!this->i2sInstalled` negated - not the other way
    g4 = next(c for c in CONTRACTS if c["id"] == "G4-refuse-powered")
    src = ("bool Audio::reseatI2S() {\n if (%s) {\n  return false;\n }\n"
           " this->installI2S(false, true);\n}\n")
    for cond, holds in (("this->audioOn || !this->i2sInstalled", True),
                        ("!this->audioOn || !this->i2sInstalled", False),
                        ("this->audioOn || this->i2sInstalled", False)):
        if (contract_problem(strip_code(src % cond), g4) is None) != holds:
            print(f"  SELF-TEST FAIL: G4 on `{cond}` should {'hold' if holds else 'fail'}")
            ok = False
    return ok


def mutation_test(texts):
    ok = True
    for name, want, pat, repl in MUTATIONS:
        mutated, n = re.subn(pat, repl, texts[name], count=1)
        if n != 1:
            print(f"  SELF-TEST FAILED: the '{want}' guard is no longer where its mutation looks in "
                  f"{name} - update MUTATIONS with the guard")
            ok = False
            continue
        if not any(g.startswith(want) for g in check(dict(texts, **{name: mutated}))):
            print(f"  SELF-TEST FAILED: /{pat}/ -> '{repl}' in {name} did not trip '{want}'")
            ok = False
    if ok:
        print(f"  ok  self-test: each of the {len(MUTATIONS)} mutations of the real source trips "
              "its own contract")
    return ok


def main():
    if not selftest():
        return 1
    files = sorted(list(ROOT.glob("*.cpp")) + list(ROOT.glob("*.h")) + list(ROOT.glob("*.ino")))
    texts = {f.name: strip_code(f.read_text(errors="replace")) for f in files}
    problems = check(texts)
    for p in problems:
        print(f"  CONTRACT BROKEN: {p}")
    if problems:
        return 1
    if not mutation_test(texts):
        return 1
    print(f"  ok  all {len(CONTRACTS)} ring contracts hold, the quit shuts the device down on "
          f"soundStarted, I2S is installed nowhere but {INSTALLER[1]}(), and written or read only behind the "
          f"no-driver refusals - music's sink only through the feed's pass, start and closeRing, each "
          f"guarded (G17-G19) ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

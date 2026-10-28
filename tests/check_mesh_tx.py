#!/usr/bin/env python3
"""check_mesh_tx.py - the LoRa radio never waits for the air on the loop task (0.9.79).

The 0.6-1.5 s 'mesh' LOOP STALL lines were LoRa transmits: MeshPhy::send() keyed the SX1276 and
busy-waited on TxDone for the frame's whole time on air, on the superloop task. The fix is a
queue (mesh_txq.h, host-tested by tests/test_txq.cpp) and a PHY that starts a frame and returns
(MeshPhy::startSend / serviceTx). test_txq proves the queue; it proves NOTHING about the callers,
and every guard that makes the fix safe is an ORDER or a GUARD in mesh_phy.cpp,
meshtastic_service.cpp or WiPhone.ino - files the host suite cannot compile:

  - no blocking send comes back (no MeshPhy::send, no loop polling TX_DONE, no meshPhy.send call);
  - healthCheck() answers true WITHOUT READING while a frame is on the air - without it a 5 s
    health tick inside a transmit (~13 % of them) declares the radio LOST and re-inits it mid-frame;
  - poll() keeps its hands off the IRQ flags mid-frame;
  - serviceTx() reads TxDone BEFORE the deadline (a Game Boy game comes back long after a frame
    finished whole - calling that a timeout fails a receipt for a message that went);
  - txPump() is the FIRST statement of loop(), ahead of the DB save, the health check and poll();
  - only txPump() starts a frame (one start a pass, by construction);
  - the background senders (periodic NodeInfo, the neighbour drip, the position beacon, the replay
    drip) wait for an idle pipeline, and the want_response reply is OWED, not sent from RX;
  - and the four guards that keep 'queued' HONEST (a review removed all four at once in a scratch
    copy and this file still passed): startSend() refuses on a latched RX_DONE before it touches
    the chip (keying TX clears the flags - the old send() lost a received packet that way);
    txPump()'s TIMEOUT path fails the own frame's receipt (else a message that never left stays
    'sent'); MESH RADIO LOST fails everything queued (txDropAll); and `power lora rx` never
    forces RX over a frame on the air (benchSleep's setModeRxContinuous only under !txActive).

Review M1-M3 (2026-09-25) added three more families, because "queued" kept being read as "sent"
one layer further out:

  - M1, QUEUED IS NOT SENT: every own frame's outcome is recorded (meshTxEnqueue notes QUEUED; the
    pump's DONE path SENT; its TIMEOUT path and txDropAll FAILED), and the words are chosen from it
    - "Take it off the mesh" never clears the waypoint id itself (only a retraction reported SENT
    does, in settleMeshOp), sharePin says "Sending", Books' sendMyPlace says "Queued";
    and its follow-up: because the id now stays while a retraction waits, sharePin refuses
    `if (retractionQueued(idx))` BEFORE its settle, and Move here / Rename ask first (else a move
    in that second re-shares the place right behind its retraction); and the Reading menu arms
    its tick on syncTxId (armMenuTick, after buildMenu) so "Queued" becomes "Sent" on the same
    visit without KOSync;
  - M2, THE FRAME ON THE AIR IS FINISHED WHERE loop() CANNOT RUN: txPump() starts with
    txComplete(), which never starts a frame, and a Game Boy game (WiPhone.ino) and the legacy
    upload (app_gbc_xfer.cpp handleUpload) call it - else the chip sits deaf in STANDBY for all of it;
  - M3, BENCH COMMANDS ARE SAFE ON A DAILY PHONE: every serial reboot goes through benchReboot(),
    which runs the four power-off saves first; `meshdb cut`, `power sleep` and `power lora sleep`
    refuse while a frame is on the air or waiting; txCutShort() asks the chip for TX_DONE before it
    calls a frame cut.

Integration review 3 (R3-4) deleted txCutShort()'s `&& !(irq & IRQ_RX_DONE_MASK)` in a scratch copy and
this file stayed green: the M3 contract pinned only "read, then TX_DONE, then txAborted". That half is
what keeps a 0xFF read (the bus floating under a re-init: every flag "set") from calling a CUT frame
SENT. check_cut_short() now pins the whole DONE branch - one read into a variable; TX_DONE set AND
RX_DONE clear, no `||`; txDoneLate and `return false` inside it, txAborted after it; txActive let go
between the read and the branch - and MUTATIONS breaks each part. ⚠ It is the SECOND line today: both
callers are kept off a live frame already (`power lora sleep` refuses while !txPipelineIdle(), pinned
above, and reinit() only follows a failed health check, which healthCheck() never reports mid-frame),
so no phone can reach the branch now; it is pinned for the day one of those gives way.

Like tests/check_call_audio.py this states each contract POSITIVELY and a contract that cannot find
its function fails too. A self-test replays the pre-fix shapes first, so a contract that has
quietly stopped matching fails loudly - and then REMOVES each of those four guards from the real
sources, one at a time (the review's experiment), and requires its contract to trip.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_wifi_restore import function_body, ifs, match_close, strip_code  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent / "WiPhone"


def body(code, name):
    b = function_body(code, name)
    return None if b is None else code[b[0] + 1:b[1]]


def first(text, pat, start=0):
    m = re.compile(pat).search(text, start)
    return m.start() if m else -1


def if_cond(code, if_pos):
    """The condition text of the `if (...)` at if_pos."""
    po = code.index("(", if_pos)
    return code[po + 1:match_close(code, po)]


def else_block(code, block_end):
    """(start, end) of the `else { ... }` straight after the if-block ending at block_end, or None."""
    m = re.compile(r"\s*else\s*\{").match(code, block_end + 1)
    if not m:
        return None
    end = match_close(code, m.end() - 1)
    return None if end < 0 else (m.end() - 1, end)


# resolveAck() on the frame in flight with a NON-ZERO reason (0 is "delivered")
FAIL_IN_FLIGHT = re.compile(r"\bresolveAck\s*\(\s*s_txInFlight\s*\.\s*packetId\s*,\s*(?!0\s*\))[^)\s]")
# an own frame's outcome recorded as FAILED (review M1)
NOTE_FAILED = re.compile(r"\bnoteOwnOutcome\s*\([^;]*\bMESH_TXO_FAILED\b")


def raw_block(raw, code, pat):
    """(open, close) of the `{...}` block that follows the first CODE match of `pat` in the raw
    text (strip_code blanks string literals, so a command's name is found in the raw text at a
    position the stripped text shows is code). None when absent."""
    for m in re.finditer(pat, raw):
        if code[m.start():m.start() + 2] != "if":
            continue
        ob = code.find("{", m.end() - 1)
        cb = match_close(code, ob) if ob >= 0 else -1
        if cb > 0:
            return ob, cb
    return None


IDLE_REFUSAL = re.compile(r"!\s*meshService\s*\.\s*txPipelineIdle\s*\(")


def refuses_before(code, blk, call_rx):
    """Inside block blk: an `if (... !meshService.txPipelineIdle() ...) { ... return ... }` that
    comes before the first match of call_rx. False when either is missing."""
    a, z = blk
    call = call_rx.search(code, a, z)
    if not call:
        return False
    return any(h[0] < call.start() and re.search(r"\breturn\b", code[h[1]:h[2] + 1])
               for h in ifs(code, a, z, IDLE_REFUSAL))


RQ_IDX = re.compile(r"\bretractionQueued\s*\(\s*idx\s*\)")
RQ_SEL = re.compile(r"\bretractionQueued\s*\(\s*pinSel\s*\)")
SETTLE = re.compile(r"\bsettleMeshOp\s*\(")
SHARE_CALL = re.compile(r"\bsharePin\s*\(")
ARM_TICK = re.compile(r"\barmMenuTick\s*\(\s*\)\s*;")


def case_block(code, label, next_label, within=None):
    """(start, end) from `label` to the next `next_label` (or the end of `within`), or None."""
    a0, z0 = within if within else (0, len(code))
    a = code.find(label, a0, z0)
    if a < 0:
        return None
    z = code.find(next_label, a + len(label), z0)
    return a, (z if z >= 0 else z0)


def guarded_before(code, blk, guard_rx, call_rx, need_return=False):
    """Inside blk: an `if (... guard ...)` before the first call_rx, and that call NOT inside the
    guard's own block (it must sit on the other branch, or after a guard that returned)."""
    a, z = blk
    call = call_rx.search(code, a, z)
    if not call:
        return False
    for h in ifs(code, a, z, guard_rx):
        if h[0] < call.start() and not (h[1] <= call.start() <= h[2]) and \
                (not need_return or re.search(r"\breturn\b", code[h[1]:h[2] + 1])):
            return True
    return False


def call_after(code, blk, first_pat, then_rx):
    """Inside blk: then_rx comes after the first match of first_pat."""
    a, z = blk
    f = re.compile(first_pat).search(code, a, z)
    return bool(f) and then_rx.search(code, f.end(), z) is not None


def check(files):
    """files: {name: stripped code}. Returns a list of broken contracts (empty = all hold)."""
    bad = []
    phy = files.get("mesh_phy.cpp", "")
    phyh = files.get("mesh_phy.h", "")
    svc = files.get("meshtastic_service.cpp", "")
    ino = files.get("WiPhone.ino", "")
    # strip_code() blanks string literals, so the phase NAMES are read from the raw text - at a
    # position the stripped text shows is code (offsets are kept), not a comment.
    ino_raw = files.get("WiPhone.ino.raw", "")

    # ── the blocking send stays gone ──────────────────────────────────────────────────────────
    if function_body(phy, "MeshPhy::send") is not None or re.search(r"\bbool\s+send\s*\(", phyh):
        bad.append("MeshPhy::send() is back - a transmit must start and return (startSend)")
    if re.search(r"\bwhile\s*\([^;{]*IRQ_TX_DONE_MASK", phy):
        bad.append("mesh_phy.cpp loops on IRQ_TX_DONE_MASK - that is the busy-wait that stalled the loop")
    if re.search(r"\bmeshPhy\s*\.\s*send\s*\(", svc):
        bad.append("meshtastic_service.cpp calls meshPhy.send() - frames go through the queue")

    # ── healthCheck: true without reading while a frame is on the air ─────────────────────────
    hc = body(phy, "MeshPhy::healthCheck")
    if hc is None:
        bad.append("MeshPhy::healthCheck() not found")
    else:
        guard = [b for b in ifs(hc, 0, len(hc), re.compile(r"(?<![!\w])txActive\b"))
                 if re.search(r"return\s+true", hc[b[1]:b[2] + 1])]
        rd = first(hc, r"readReg\s*\(")
        if not guard or (rd >= 0 and guard[0][0] > rd):
            bad.append("healthCheck() must `if (txActive) return true;` BEFORE its first register "
                       "read - TX/STANDBY mid-frame is not a lost radio")
        # One garbled read is a bus glitch, not a dead radio (five of five 'health FAILED' lines,
        # 2026-09-23..25): the register reads sit inside a loop of at least 3 attempts, and
        # `ready = false` comes only after that loop.
        loop = re.search(r"for\s*\(\s*int\s+(\w+)\s*=\s*0\s*;\s*\1\s*<\s*(\d+)\s*;", hc)
        if not loop or int(loop.group(2)) < 3 or not (loop.start() < rd):
            bad.append("healthCheck() must read the registers inside a retry loop of >= 3 attempts "
                       "- one garbled read on the bit-bang is not a lost radio")
        else:
            lb = hc.index("{", loop.end())
            le = match_close(hc, lb)
            if le < 0 or first(hc, r"\bready\s*=\s*false", le) < 0 or \
                    first(hc[lb:le], r"\bready\s*=\s*false") >= 0:
                bad.append("healthCheck() must set `ready = false` only AFTER its retry loop "
                           "gives up, never inside it")

    # ── poll: hands off the IRQ flags mid-frame ───────────────────────────────────────────────
    pl = body(phy, "MeshPhy::poll")
    if pl is None:
        bad.append("MeshPhy::poll() not found")
    else:
        guard = [b for b in ifs(pl, 0, len(pl), re.compile(r"(?<![!\w])txActive\b"))
                 if re.search(r"return\s+false", pl[b[1]:b[2] + 1])]
        rd = first(pl, r"readReg\s*\(")
        if not guard or (rd >= 0 and guard[0][0] > rd):
            bad.append("poll() must return false while txActive, before it reads the IRQ flags")

    # ── serviceTx: TxDone before the deadline ─────────────────────────────────────────────────
    st = body(phy, "MeshPhy::serviceTx")
    if st is None:
        bad.append("MeshPhy::serviceTx() not found")
    else:
        done = first(st, r"IRQ_TX_DONE_MASK")
        limit = first(st, r"txLimitMs")
        if done < 0 or limit < 0 or done > limit:
            bad.append("serviceTx() must test TxDone BEFORE the txLimitMs deadline")

    # ── startSend: a latched RX_DONE refuses before the chip is touched ───────────────────────
    ss = body(phy, "MeshPhy::startSend")
    if ss is None:
        bad.append("MeshPhy::startSend() not found")
    else:
        guard = [b for b in ifs(ss, 0, len(ss), re.compile(r"IRQ_RX_DONE_MASK"))
                 if re.search(r"readReg\s*\(\s*REG_IRQ_FLAGS\s*\)", if_cond(ss, b[0]))
                 and not re.match(r"\s*!", if_cond(ss, b[0]))
                 and re.search(r"return\s+false", ss[b[1]:b[2] + 1])]
        touch = first(ss, r"\b(?:writeReg|writeFifo|setMode\w*)\s*\(")
        key = first(ss, r"\bwriteReg\s*\(\s*REG_OP_MODE\b")
        if not guard or touch < 0 or key < 0 or guard[0][0] > touch:
            bad.append("startSend() must `if (readReg(REG_IRQ_FLAGS) & IRQ_RX_DONE_MASK) return false;` "
                       "BEFORE it writes the chip - keying TX clears the flags and loses the packet")

    # ── benchSleep: `power lora rx` never forces RX over a frame on the air ───────────────────
    bs = body(phy, "MeshPhy::benchSleep")
    if bs is None:
        bad.append("MeshPhy::benchSleep() not found")
    else:
        idle = [b for b in ifs(bs, 0, len(bs), re.compile(r"!\s*txActive\b"))]
        calls = [m.start() for m in re.finditer(r"\bsetModeRxContinuous\s*\(", bs)]
        if not calls or any(not any(b[1] <= c <= b[2] for b in idle) for c in calls):
            bad.append("benchSleep() must call setModeRxContinuous() only inside `if (!txActive)` - "
                       "mid-frame it truncates the frame on the air")

    # ── the pump: first in loop(), the only starter ───────────────────────────────────────────
    lp = body(svc, "MeshtasticService::loop")
    if lp is None:
        bad.append("MeshtasticService::loop() not found")
    else:
        if not re.match(r"\s*txPump\s*\(\s*\)\s*;", lp):
            bad.append("txPump(); must be the FIRST statement of MeshtasticService::loop()")
        pump = first(lp, r"\btxPump\s*\(")
        for pat, what in [(r"\bsaveDbStep\s*\(", "saveDbStep()"),
                          (r"meshPhy\s*\.\s*healthCheck\s*\(", "the health check"),
                          (r"meshPhy\s*\.\s*poll\s*\(", "poll()")]:
            at = first(lp, pat)
            if at < 0 or pump < 0 or pump > at:
                bad.append(f"txPump() must run before {what} in loop()")
        # background senders wait for an idle pipeline
        for cond, what in [(r"nextNodeInfoMs", "the periodic NodeInfo"),
                           (r"nbrDripMs", "the neighbour drip"),
                           (r"\bposDue\s*&&", "the position beacon"),
                           (r"\bnodeInfoOwed\b", "the owed NodeInfo reply")]:
            hits = ifs(lp, 0, len(lp), re.compile(cond))
            if not any(re.search(r"(?<![!\w])txPipelineIdle\s*\(", lp[h[0]:h[1]]) for h in hits):
                bad.append(f"{what} must wait for txPipelineIdle() in its own condition")
        # the want_response reply is owed, not transmitted from the RX path
        wr = ifs(lp, 0, len(lp), re.compile(r"(?<=\()\s*wantResp\s*$"))
        if not wr:
            bad.append("the `if (wantResp)` NodeInfo-reply branch not found in loop()")
        elif any(re.search(r"announceNodeInfo\s*\(", lp[h[1]:h[2]]) for h in wr) or \
                not any(re.search(r"nodeInfoOwed\s*=\s*true", lp[h[1]:h[2]]) for h in wr):
            bad.append("a want_response NodeInfo must set nodeInfoOwed, not announce from the RX path")
        # MESH RADIO LOST fails everything queued, with a reason (0 would say "delivered")
        lost = ifs(lp, 0, len(lp), re.compile(r"!\s*meshPhy\s*\.\s*healthCheck\s*\("))
        if not any(re.search(r"\btxDropAll\s*\(\s*(?!0\s*\))[^)\s]", lp[h[1]:h[2] + 1]) for h in lost):
            bad.append("the `if (!meshPhy.healthCheck())` (MESH RADIO LOST) branch must call "
                       "txDropAll(<non-zero reason>) - else queued messages sit there looking sent")
    tp = function_body(svc, "MeshtasticService::txPump")
    tc = function_body(svc, "MeshtasticService::txComplete")
    if tp is None:
        bad.append("MeshtasticService::txPump() not found")
    elif tc is None:
        bad.append("MeshtasticService::txComplete() not found - the pump's first half (review M2)")
    else:
        # ── M2: the pump's first half finishes the frame and starts nothing ──────────────────
        if not re.match(r"\s*txComplete\s*\(\s*\)\s*;", svc[tp[0] + 1:tp[1]]):
            bad.append("txComplete(); must be the FIRST statement of txPump() - one copy of TxDone")
        tcb = svc[tc[0] + 1:tc[1]]
        if re.search(r"\b(?:startSend|meshTxPick|meshTxqPop)\s*\(", tcb):
            bad.append("txComplete() starts a frame - it runs where loop() does not (a game, an "
                       "upload), so it must only FINISH one (review M2)")
        # the TIMEOUT path: the else of `if (... MESH_TX_DONE)`, or an `if` on MESH_TX_TIMEOUT alone
        paths, done_paths = [], []
        for h in ifs(tcb, 0, len(tcb), re.compile(r"\bMESH_TX_(?:DONE|TIMEOUT)\b")):
            c = if_cond(tcb, h[0])
            has_done = re.search(r"\bMESH_TX_DONE\b", c)
            has_to = re.search(r"\bMESH_TX_TIMEOUT\b", c)
            if has_done and not has_to:
                done_paths.append((h[1], h[2]))
                e = else_block(tcb, h[2])
                if e:
                    paths.append(e)
            elif has_to and not has_done:
                paths.append((h[1], h[2]))
        own_rx = re.compile(r"(?:==\s*MESH_TXK_OWN|MESH_TXK_OWN\s*==)")
        own = [h for (a, z) in paths for h in ifs(tcb, a, z, own_rx)
               if FAIL_IN_FLIGHT.search(tcb, h[1], h[2] + 1)]
        if not own:
            bad.append("txComplete()'s TIMEOUT path must `if (s_txInFlight.kind == MESH_TXK_OWN) "
                       "resolveAck(s_txInFlight.packetId, <non-zero>)` - else a message that never "
                       "left stays 'sent'")
        # ── M1: queued is not sent - every own frame's outcome is recorded ───────────────────
        failed = [h for (a, z) in paths for h in ifs(tcb, a, z, own_rx)
                  if NOTE_FAILED.search(tcb, h[1], h[2] + 1)]
        if not failed:
            bad.append("txComplete()'s TIMEOUT path must noteOwnOutcome(<id>, MESH_TXO_FAILED) for an "
                       "own frame - else a pin share or retraction that never left reads 'queued' "
                       "forever and the map keeps waiting (review M1)")
        sent = [h for (a, z) in done_paths for h in ifs(tcb, a, z, own_rx)
                if re.search(r"\bnoteOwnOutcome\s*\([^;]*\bMESH_TXO_SENT\b", tcb[h[1]:h[2] + 1])]
        if not sent:
            bad.append("txComplete()'s DONE path must noteOwnOutcome(<id>, MESH_TXO_SENT) for an own "
                       "frame - the only place 'sent' becomes true (review M1)")
    eq = body(svc, "meshTxEnqueue")
    if eq is None or not re.search(r"\bmeshTxoNote\s*\([^;]*\bMESH_TXO_QUEUED\b", eq):
        bad.append("meshTxEnqueue() must meshTxoNote(..., MESH_TXO_QUEUED) an own frame it queues "
                   "- the outcome starts at QUEUED, never at SENT (review M1)")
    da = body(svc, "MeshtasticService::txDropAll")
    if da is None or not NOTE_FAILED.search(da):
        bad.append("txDropAll() must noteOwnOutcome(<id>, MESH_TXO_FAILED) the frames it drops - "
                   "a pin retraction dropped with a dying pack is FAILED, not queued (review M1)")
    for m in re.finditer(r"meshPhy\s*\.\s*startSend\s*\(", svc):
        if tp is None or not (tp[0] < m.start() < tp[1]):
            line = svc.count("\n", 0, m.start()) + 1
            bad.append(f"meshtastic_service.cpp:{line} starts a frame outside txPump()")
    rp = body(svc, "MeshtasticService::replayPump")
    if rp is None:
        bad.append("MeshtasticService::replayPump() not found")
    else:
        idle = [b for b in ifs(rp, 0, len(rp), re.compile(r"!\s*txPipelineIdle\s*\("))
                if re.search(r"\breturn\b", rp[b[1]:b[2] + 1])]
        tx = first(rp, r"meshTxText\s*\(")
        if not idle or tx < 0 or idle[0][0] > tx:
            bad.append("replayPump() must return while !txPipelineIdle(), before it queues a packet")

    # ── WiPhone.ino: the stall line can tell the radio from the screen ────────────────────────
    def phase(name):
        for m in re.finditer(r'loopPhase\s*\(\s*"' + re.escape(name) + r'"\s*\)', ino_raw):
            if ino[m.start():m.start() + 9] == "loopPhase":      # code, not a comment
                return m.start()
        return -1
    a = phase("mesh")
    b = first(ino, r"meshService\s*\.\s*loop\s*\(")
    c = phase("mesh-ui")
    d = first(ino, r"gui\s*\.\s*showMeshPopup\s*\(")
    if not (0 <= a < b < c < d):
        bad.append("WiPhone.ino: loopPhase(\"mesh\") -> meshService.loop() -> loopPhase(\"mesh-ui\") "
                   "-> the popup, in that order")

    # ── M2: where loop() cannot run, the frame on the air is still finished ───────────────────
    game = [h for h in ifs(ino, max(a, 0), b if b > 0 else len(ino),
                           re.compile(r"(?<![!\w])gGbcActive\b"))
            if re.search(r"meshService\s*\.\s*txComplete\s*\(", ino[h[1]:h[2] + 1])]
    if a < 0 or b < 0 or not game:
        bad.append("WiPhone.ino: between loopPhase(\"mesh\") and meshService.loop(), `if (gGbcActive) "
                   "meshService.txComplete();` - a game started mid-frame must not leave the radio "
                   "deaf in STANDBY for the whole game (review M2)")
    xfer = files.get("app_gbc_xfer.cpp", "")
    hu = body(xfer, "handleUpload")
    if hu is None or not re.search(r"meshService\s*\.\s*txComplete\s*\(", hu):
        bad.append("app_gbc_xfer.cpp handleUpload() must call meshService.txComplete() on its "
                   "per-piece yield - the legacy upload owns the loop for the whole file (review M2)")

    # ── M1: the claims. Queued is not sent, where the words are chosen ────────────────────────
    maps = files.get("app_maps.cpp", "")
    maps_raw = files.get("app_maps.cpp.raw", "")
    un = maps.find("case ROW_P_UNSHARE")
    un_end = maps.find("case ROW_P_", un + 10) if un >= 0 else -1
    if un < 0 or un_end < 0:
        bad.append("app_maps.cpp: the `case ROW_P_UNSHARE` block not found")
    else:
        blk = maps[un:un_end]
        if re.search(r"\bsharedId\s*=\s*0\b", blk):
            bad.append("app_maps.cpp: \"Take it off the mesh\" (ROW_P_UNSHARE) clears the pin's "
                       "waypoint id on QUEUED - a retraction that then fails can never be sent again; "
                       "the id goes only when the radio reports it SENT (settleMeshOp, review M1)")
        if not re.search(r"\bbeginMeshOp\s*\(\s*MAP_PINOP_UNSHARE\b", blk):
            bad.append("app_maps.cpp: ROW_P_UNSHARE must beginMeshOp(MAP_PINOP_UNSHARE, ...) so the "
                       "radio's answer decides the id (review M1)")
    sp = function_body(maps, "MapsApp::sharePin")
    if sp is None or not re.search(r"\bbeginMeshOp\s*\(", maps[sp[0]:sp[1]]) or \
            re.search(r'"Shared ', maps_raw[sp[0]:sp[1]]):
        bad.append("app_maps.cpp: sharePin() must beginMeshOp() and say 'Sending' - \"Shared\" is "
                   "said only once the radio reports the frame SENT (review M1)")
    # M1 follow-up: the id is KEPT while a retraction waits, so nothing may re-share over it. The
    # guard is asked BEFORE the settle (which would read the queued retraction as "no word", keep
    # the id, and let the update go out right behind it - the place back on every radio).
    if sp is None or not guarded_before(maps, sp, RQ_IDX, SETTLE, need_return=True):
        bad.append("app_maps.cpp: sharePin() must refuse `if (retractionQueued(idx)) { ... return }` "
                   "before settleMeshOp() - a re-share over a queued retraction puts the place back "
                   "on every radio (review M1 follow-up)")
    mv = case_block(maps, "case ROW_P_MOVE", "case ROW_P_")
    if mv is None or not guarded_before(maps, mv, RQ_SEL, SHARE_CALL):
        bad.append("app_maps.cpp: Move here (ROW_P_MOVE) must ask `if (retractionQueued(pinSel))` "
                   "before it re-shares - moving a pin being taken off the mesh re-shares it")
    mpe = function_body(maps, "MapsApp::processEvent")
    rn = case_block(maps, "case MAPS_RENAME", "case MAPS_", mpe) if mpe else None
    if rn is None or not guarded_before(maps, rn, RQ_SEL, SHARE_CALL):
        bad.append("app_maps.cpp: Rename (MAPS_RENAME) must ask `if (retractionQueued(pinSel))` "
                   "before it re-shares - renaming a pin being taken off the mesh re-shares it")
    books = files.get("app_books.cpp", "")
    books_raw = files.get("app_books.cpp.raw", "")
    sm = function_body(books, "BooksApp::sendMyPlace")
    if sm is None or re.search(r'"Sent', books_raw[sm[0]:sm[1]]) or \
            not re.search(r"\blastQueuedPacketId\s*\(", books[sm[0]:sm[1]]):
        bad.append("app_books.cpp: sendMyPlace() must say 'Queued' and keep lastQueuedPacketId() - "
                   "'Sent' only once the radio reports it (syncNoteSettle, review M1)")
    # M1 follow-up: the Reading menu TICKS while its frame waits - KOSync or not - or "Queued" is
    # all a phone without /books/kosync.txt ever shows on that visit.
    at = function_body(books, "BooksApp::armMenuTick")
    atb = books[at[0]:at[1]] if at else ""
    es = function_body(books, "BooksApp::enterState")
    em = case_block(books, "case BOOKS_MENU:", "case BOOKS_", es) if es else None
    pe = function_body(books, "BooksApp::processEvent")
    sy = case_block(books, "case BOOKS_MENU_SYNC:", "case BOOKS_MENU_", pe) if pe else None
    tick_ok = (
        re.search(r"msAppTimerEventPeriod\s*=\s*syncTxId\s*\?", atb) is not None
        and em is not None and call_after(books, em, r"\bbuildMenu\s*\(", ARM_TICK)
        and sy is not None and call_after(books, sy, r"\bsyncMyPlace\s*\(", ARM_TICK))
    if not tick_ok:
        bad.append("app_books.cpp: the Reading menu must arm its tick on syncTxId - armMenuTick() "
                   "(`msAppTimerEventPeriod = syncTxId ? ...`) after buildMenu() in enterState's "
                   "BOOKS_MENU and after syncMyPlace() - else 'Queued' never becomes 'Sent' on the "
                   "visit without KOSync (review M1 follow-up)")

    # ── M3: bench commands safe to type on a daily phone ──────────────────────────────────────
    ser = files.get("serial_cmd.cpp", "")
    ser_raw = files.get("serial_cmd.cpp.raw", "")
    br = function_body(ser, "benchReboot")
    if br is None:
        bad.append("serial_cmd.cpp: benchReboot() not found - every cable reboot runs the power-off "
                   "saves (review M3)")
    else:
        brb = ser[br[0]:br[1]]
        rs = first(brb, r"\bESP\s*\.\s*restart\s*\(")
        for save in ("booksSaveOpenPosition", "mapsSaveOpenView", "tileFetchPowerOff",
                     "gbcSaveForPowerOff"):
            at = first(brb, r"\b" + save + r"\s*\(\s*\)\s*;")
            if rs < 0 or at < 0 or at > rs:
                bad.append(f"serial_cmd.cpp: benchReboot() must call {save}() before ESP.restart() - "
                           "the same saves as both power-off paths (review M3)")
        for m in re.finditer(r"\bESP\s*\.\s*restart\s*\(", ser):
            if not (br[0] < m.start() < br[1]):
                line = ser.count("\n", 0, m.start()) + 1
                bad.append(f"serial_cmd.cpp:{line} reboots with a bare ESP.restart() - use "
                           "benchReboot() (an open book's page turns, a game, the map's view)")
    cut = raw_block(ser_raw, ser, r'if\s*\(\s*!strcasecmp\s*\(\s*line\s*,\s*"meshdb cut"\s*\)\s*\)')
    if cut is None or not refuses_before(ser, cut, re.compile(r"\bbenchCutSave\s*\(")):
        bad.append("serial_cmd.cpp: `meshdb cut` must refuse `if (!meshService.txPipelineIdle())` "
                   "before benchCutSave() - queued texts would be lost to the reboot looking sent")
    slp = raw_block(ser_raw, ser, r'if\s*\(\s*!strncasecmp\s*\(\s*arg\s*,\s*"sleep"\s*,\s*5\s*\)\s*\)')
    if slp is None or not refuses_before(ser, slp, re.compile(r"\besp_light_sleep_start\s*\(")):
        bad.append("serial_cmd.cpp: `power sleep` must refuse `if (!meshService.txPipelineIdle())` "
                   "before esp_light_sleep_start() - a frame finishing in light sleep leaves the chip "
                   "in STANDBY for the rest of it (review M3)")
    lora = raw_block(ser_raw, ser, r'if\s*\(\s*!strcasecmp\s*\(\s*arg\s*,\s*"lora sleep"\s*\)')
    if lora is None or not refuses_before(ser, lora, re.compile(r"\bbenchSleep\s*\(")):
        bad.append("serial_cmd.cpp: `power lora sleep` must refuse while !txPipelineIdle() before "
                   "benchSleep() - sleep cuts the frame on the air (review M3)")
    cs = body(phy, "MeshPhy::txCutShort")
    if cs is None:
        bad.append("MeshPhy::txCutShort() not found")
    else:
        rd = first(cs, r"readReg\s*\(\s*REG_IRQ_FLAGS\s*\)")
        td = first(cs, r"IRQ_TX_DONE_MASK")
        ab = first(cs, r"\btxAborted\s*=\s*true")
        if rd < 0 or td < 0 or ab < 0 or not (rd < td < ab):
            bad.append("txCutShort() must read REG_IRQ_FLAGS and test IRQ_TX_DONE_MASK before it "
                       "calls the frame cut - a frame that finished whole is SENT (review M3)")
        else:
            bad.extend(check_cut_short(cs))
    return bad


# One read of the flags, kept in a variable: `irq`, as `(irq & MASK)` terms of the DONE branch.
CUT_READ = re.compile(r"\b(?:const\s+)?uint8_t\s+(\w+)\s*=\s*readReg\s*\(\s*REG_IRQ_FLAGS\s*\)\s*;")


def top_level_or(cond):
    """Does `cond` have an `||` outside every parenthesis?"""
    depth = 0
    for i, c in enumerate(cond):
        depth += c == "("
        depth -= c == ")"
        if depth == 0 and cond.startswith("||", i):
            return True
    return False


def check_cut_short(cs):
    """Integration review 3 (R3-4d): txCutShort() reports a frame DONE only on TX_DONE set AND
    RX_DONE clear, and it lets go of the frame (txActive) on both paths. The M3 contract above pins
    only 'read, then TX_DONE, then txAborted', and deleting the RX_DONE half survived it - but that
    half is what keeps a 0xFF read (the SPI bus floating while the rail collapses under a re-init:
    every flag 'set') from reporting a CUT frame as SENT. Mutations in MUTATIONS."""
    bad = []
    rd = CUT_READ.search(cs)
    if rd is None:
        return ["txCutShort() must read the flags ONCE into a variable (`const uint8_t irq = "
                "readReg(REG_IRQ_FLAGS);`) and judge that one read - a second read can disagree"]
    v = re.escape(rd.group(1))
    tx = re.compile(r"\(\s*" + v + r"\s*&\s*IRQ_TX_DONE_MASK\s*\)")
    rx = re.compile(r"\(\s*" + v + r"\s*&\s*IRQ_RX_DONE_MASK\s*\)")
    done = [h for h in ifs(cs, rd.end(), len(cs), tx)
            if re.search(r"\btxDoneLate\s*=\s*true\b", cs[h[1]:h[2] + 1])]
    if not done:
        return ["txCutShort() must set txDoneLate = true only under `if ((irq & IRQ_TX_DONE_MASK) && "
                "!(irq & IRQ_RX_DONE_MASK))` - else a frame that finished whole is never reported "
                "(review 3, R3-4)"]
    h = done[0]
    po = cs.index("(", h[0])
    cond = cs[po + 1:match_close(cs, po)]

    def negated(m):
        return re.search(r"!\s*$", cond[:m.start()]) is not None

    tx_set = [m for m in tx.finditer(cond) if not negated(m)]
    rx_all = list(rx.finditer(cond))
    if not tx_set or not rx_all or not all(negated(m) for m in rx_all) or top_level_or(cond):
        bad.append("txCutShort() must report DONE only when TX_DONE is set AND RX_DONE is clear - "
                   "`(irq & IRQ_TX_DONE_MASK) && !(irq & IRQ_RX_DONE_MASK)`, no `||`: a 0xFF read "
                   "(a floating bus under a re-init) would call a CUT frame sent (review 3, R3-4)")
    blk = cs[h[1]:h[2] + 1]
    if not re.search(r"\breturn\s+false\s*;", blk) or re.search(r"\btxAborted\s*=\s*true\b", blk):
        bad.append("txCutShort()'s DONE branch must `return false` before the cut - falling through "
                   "sets txAborted too, one frame reported twice (review 3, R3-4)")
    ab = re.compile(r"\btxAborted\s*=\s*true\b").search(cs, h[2])
    if ab is None:
        bad.append("txCutShort() must set txAborted = true AFTER the DONE branch - a frame that did not "
                   "finish must be reported cut")
    act = re.compile(r"\btxActive\s*=\s*false\s*;").search(cs, rd.end())
    if act is None or act.start() > h[0]:
        bad.append("txCutShort() must let go of the frame (`txActive = false;`) between the read and "
                   "the DONE branch - on both paths; left true, serviceTx() never reports the DONE or the "
                   "cut just decided (it reports those only with txActive clear) and polls a chip that "
                   "was just put to sleep until its deadline: a TIMEOUT (review 3, R3-4)")
    return bad


# ── self-test: the pre-fix shapes must break the contracts ─────────────────────────────────────

OLD_PHY = """
bool MeshPhy::healthCheck() {
  if (!ready) { return false; }
  uint8_t ver = readReg(REG_VERSION);
  return true;
}
bool MeshPhy::poll(uint8_t* buf) {
  if (!ready || benchSleeping) { return false; }
  uint8_t irq = readReg(REG_IRQ_FLAGS);
  return false;
}
bool MeshPhy::send(const uint8_t* data, uint8_t len) {
  writeReg(REG_OP_MODE, MODE_TX);
  while (!(readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK)) { delay(1); }
  return true;
}
MeshTxState MeshPhy::serviceTx() {
  if ((uint32_t)(now - txStartMs) > txLimitMs) { return MESH_TX_TIMEOUT; }
  if (readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) { return MESH_TX_DONE; }
  return MESH_TX_BUSY;
}
bool MeshPhy::startSend(const uint8_t* data, uint8_t len) {
  if (!ready || txActive) { return false; }
  setModeIdle();
  writeReg(REG_IRQ_FLAGS, 0xFF);
  writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_TX);
  return true;
}
void MeshPhy::benchSleep(bool on) {
  if (on) { writeReg(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP); }
  else { benchSleeping = false; setModeRxContinuous(); }
}
void MeshPhy::txCutShort(const char* why) {
  if (!txActive) { return; }
  txActive = false;
  txAborted = true;
}
"""
OLD_SVC = """
static bool meshTxEnqueue(const uint8_t* pkt, size_t len) {
  return meshTxqPushBack(&s_txq, pkt, len, MESH_TXK_OWN, 0);
}
void MeshtasticService::txPump() {
  const MeshTxState st = meshPhy.serviceTx();
  meshPhy.startSend(a, b);
}
void MeshtasticService::txComplete() {
  const MeshTxState st = meshPhy.serviceTx();
  if (st == MESH_TX_DONE || st == MESH_TX_TIMEOUT) {
    if (s_txInFlight.active) {
      s_txInFlight.active = false;
      if (st == MESH_TX_DONE) {
        if (s_txInFlight.kind == MESH_TXK_OWN) { s_txStats.own++; }
      } else {
        if (s_txInFlight.kind == MESH_TXK_OWN) { s_txStats.timeout++; }
      }
    }
  }
  meshPhy.startSend(a, b);
}
void MeshtasticService::txDropAll(uint8_t err) {
  resolveAck(f->packetId, err);
}
bool MeshtasticService::loop() {
  saveDbStep();
  txPump();
  if (!meshPhy.healthCheck()) { radioState = MESH_RADIO_ERROR; }
  if ((int32_t)(now - nextNodeInfoMs) >= 0) { announceNodeInfo(false); }
  if (nodeInfoOwed) { announceNodeInfo(false); }
  if (nbrPendingMask && (int32_t)(now - nbrDripMs) >= 0) { y(); }
  if (posDue && (uint32_t)(now - s_lastPhyTxMs) >= 1500u) { z(); }
  meshPhy.send(rebroadcast[i].data, rebroadcast[i].len);
  if (!meshPhy.poll(buf)) { return false; }
  if (wantResp) { announceNodeInfo(false); }
  return false;
}
void MeshtasticService::replayPump() {
  meshTxText(a, b, c, ch);
}
void elsewhere() { meshPhy.startSend(a, b); }
"""
OLD_INO = """
    loopPhase("mesh");
    if (!gGbcActive && meshService.loop()) {
      gui.showMeshPopup(title, nm->text);
    }
"""
OLD_XFER = """
static void handleUpload() {
  s_uploadFile.write(up.buf, up.currentSize);
  delay(1);
}
"""
OLD_MAPS = """
bool MapsApp::sharePin(int idx, char* why, size_t whyCap) {
  pins[idx].sharedId = id;
  if (onAir) { snprintf(why, whyCap, "Shared '%s' on %s", pins[idx].name, ch->name); }
  return onAir;
}
void x() {
  switch (sel) {
      case ROW_P_UNSHARE: {
        const bool ok = meshService.unshareWaypoint(pins[pinSel].sharedId, pinChannel(pinSel));
        if (ok) { pins[pinSel].sharedId = 0; savePins(); setNote("Taken off the mesh"); }
        return REDRAW_ALL;
      }
      case ROW_P_CENTRE:
        return REDRAW_ALL;
  }
}
"""
OLD_BOOKS = """
bool BooksApp::sendMyPlace() {
  bool ok = meshService.sendChannelMessage(ch->hash, text);
  snprintf(syncNote, sizeof(syncNote), ok ? "Sent: ch %d, %d%%" : "Radio would not send", a, b);
  return ok;
}
"""
OLD_SERIAL = """
void serialCmd(const char* line, const char* arg) {
  if (!strcasecmp(line, "wifi calreset")) {
    delay(300);
    ESP.restart();
    return;
  }
  if (!strcasecmp(line, "meshdb cut")) {
    const char* why = meshService.benchCutSave();
    delay(300);
    ESP.restart();
    return;
  }
  if (!strcasecmp(arg, "lora sleep") || !strcasecmp(arg, "lora rx")) {
    meshPhy.benchSleep(sleep);
    return;
  }
  if (!strncasecmp(arg, "sleep", 5)) {
    const esp_err_t r = esp_light_sleep_start();
    return;
  }
}
"""


def selftest():
    got = check({"mesh_phy.cpp": strip_code(OLD_PHY), "mesh_phy.h": "bool send(const uint8_t* d);",
                 "meshtastic_service.cpp": strip_code(OLD_SVC), "WiPhone.ino": strip_code(OLD_INO),
                 "WiPhone.ino.raw": OLD_INO, "app_gbc_xfer.cpp": strip_code(OLD_XFER),
                 "app_maps.cpp": strip_code(OLD_MAPS), "app_maps.cpp.raw": OLD_MAPS,
                 "app_books.cpp": strip_code(OLD_BOOKS), "app_books.cpp.raw": OLD_BOOKS,
                 "serial_cmd.cpp": strip_code(OLD_SERIAL), "serial_cmd.cpp.raw": OLD_SERIAL})
    want = ["MeshPhy::send() is back", "loops on IRQ_TX_DONE_MASK", "calls meshPhy.send()",
            "healthCheck() must", "poll() must", "serviceTx() must", "must be the FIRST statement",
            "before saveDbStep()", "the periodic NodeInfo", "the neighbour drip",
            "the position beacon", "the owed NodeInfo reply", "want_response NodeInfo must",
            "starts a frame outside txPump()", "replayPump() must", "WiPhone.ino: loopPhase",
            "startSend() must", "benchSleep() must", "MESH RADIO LOST", "TIMEOUT path must `if",
            "retry loop of >= 3",
            # review M2: the frame on the air is finished where loop() does not run
            "txComplete(); must be the FIRST", "txComplete() starts a frame",
            "meshService.txComplete();` - a game", "handleUpload() must call",
            # review M1: queued is not sent
            "TIMEOUT path must noteOwnOutcome", "DONE path must noteOwnOutcome",
            "meshTxEnqueue() must", "txDropAll() must", "(ROW_P_UNSHARE) clears",
            "ROW_P_UNSHARE must beginMeshOp", "sharePin() must", "sendMyPlace() must",
            # review M1 follow-up: no re-share over a queued retraction; the menu ticks for its frame
            "sharePin() must refuse", "Move here (ROW_P_MOVE) must", "Rename (MAPS_RENAME) must",
            "the Reading menu must arm its tick",
            # review M3: bench commands safe on a daily phone
            "benchReboot() not found", "`meshdb cut` must refuse", "`power sleep` must refuse",
            "`power lora sleep` must refuse", "txCutShort() must read"]
    missing = [w for w in want if not any(w in g for g in got)]
    if missing:
        for w in missing:
            print(f"  SELF-TEST FAILED: the pre-fix shape did not trip '{w}'")
        return False
    print(f"  ok  self-test: the pre-fix shapes break all {len(want)} contract kinds")
    return True


# The review's experiment, kept: each of the four 'queued is honest' guards REMOVED from the real
# source (stripped text, as check() sees it) must trip its own contract. A pattern that no longer
# matches the source fails too - the guard was rewritten, so this mutation needs rewriting with it.
MUTATIONS = [
    ("mesh_phy.cpp", "startSend() must",
     r"if\s*\(\s*readReg\s*\(\s*REG_IRQ_FLAGS\s*\)\s*&\s*IRQ_RX_DONE_MASK\s*\)\s*\{\s*return\s+false\s*;\s*\}",
     ""),
    ("meshtastic_service.cpp", "TIMEOUT path must",
     r"resolveAck\s*\(\s*s_txInFlight\s*\.\s*packetId\s*,\s*3\s*\)\s*;", ""),
    ("meshtastic_service.cpp", "MESH RADIO LOST", r"txDropAll\s*\(\s*4\s*\)\s*;", ""),
    ("mesh_phy.cpp", "benchSleep() must",
     r"if\s*\(\s*!\s*txActive\s*\)\s*\{\s*(setModeRxContinuous\s*\(\s*\)\s*;)\s*\}", r"\1"),
    # the health check's retry loop cut down to one read (the pre-0.9.79 single-read shape)
    ("mesh_phy.cpp", "retry loop of >= 3",
     r"for\s*\(\s*int\s+attempt\s*=\s*0\s*;\s*attempt\s*<\s*3\s*;", "for (int attempt = 0; attempt < 1;"),
    # ── review M1-M3 (2026-09-25) ──
    # a failed own frame no longer recorded: the map would wait on a retraction that never left
    ("meshtastic_service.cpp", "TIMEOUT path must noteOwnOutcome",
     r"noteOwnOutcome\s*\(\s*s_txInFlight\s*\.\s*packetId\s*,\s*MESH_TXO_FAILED\s*\)\s*;", ""),
    # "Take it off the mesh" forgetting the id on QUEUED again (the stranding)
    ("app_maps.cpp", "(ROW_P_UNSHARE) clears",
     r"beginMeshOp\s*\(\s*MAP_PINOP_UNSHARE\s*,[^;]*;", "pins[pinSel].sharedId = 0;"),
    # the game no longer finishing the frame on the air
    ("WiPhone.ino", "meshService.txComplete();` - a game",
     r"if\s*\(\s*gGbcActive\s*\)\s*\{\s*meshService\s*\.\s*txComplete\s*\(\s*\)\s*;\s*\}", ""),
    # a cable reboot that skips the reading position
    ("serial_cmd.cpp", "must call booksSaveOpenPosition()",
     r"booksSaveOpenPosition\s*\(\s*\)\s*;", ""),
    # `meshdb cut` back on a bare restart
    ("serial_cmd.cpp", "bare ESP.restart()", r"benchReboot\s*\(\s*\)\s*;", "ESP.restart();", 2),
    # `meshdb cut` without its pipeline refusal (the first such `if` in the file)
    ("serial_cmd.cpp", "`meshdb cut` must refuse",
     r"if\s*\(\s*!\s*meshService\s*\.\s*txPipelineIdle\s*\(\s*\)\s*\)", "if (false)"),
    # `power sleep` without its pipeline refusal (the second)
    ("serial_cmd.cpp", "`power sleep` must refuse",
     r"if\s*\(\s*!\s*meshService\s*\.\s*txPipelineIdle\s*\(\s*\)\s*\)", "if (false)", 1),
    # the bench cut calling a finished frame cut
    ("mesh_phy.cpp", "txCutShort() must read",
     r"if\s*\(\s*\(\s*irq\s*&\s*IRQ_TX_DONE_MASK\s*\)", "if ((irq & 0)"),
    # ── integration review 3 (R3-4d): the DONE branch's other half, which survived the M3 contract.
    # The RX_DONE test deleted (a 0xFF floating-bus read calls a CUT frame sent), OR-ed, or un-negated;
    # the DONE never recorded; the frame never let go of; the DONE branch falling through to the cut.
    ("mesh_phy.cpp", "must report DONE only when TX_DONE is set AND RX_DONE is clear",
     r"\s*&&\s*!\s*\(\s*irq\s*&\s*IRQ_RX_DONE_MASK\s*\)", ""),
    ("mesh_phy.cpp", "must report DONE only when TX_DONE is set AND RX_DONE is clear",
     r"(\(\s*irq\s*&\s*IRQ_TX_DONE_MASK\s*\))\s*&&(\s*!\s*\(\s*irq\s*&\s*IRQ_RX_DONE_MASK\s*\))", r"\1 ||\2"),
    ("mesh_phy.cpp", "must report DONE only when TX_DONE is set AND RX_DONE is clear",
     r"!\s*(\(\s*irq\s*&\s*IRQ_RX_DONE_MASK\s*\)\s*\))", r"\1"),
    ("mesh_phy.cpp", "must set txDoneLate = true only under", r"\btxDoneLate\s*=\s*true\s*;", ""),
    ("mesh_phy.cpp", "must let go of the frame",
     r"(readReg\s*\(\s*REG_IRQ_FLAGS\s*\)\s*;\s*)txActive\s*=\s*false\s*;", r"\1"),
    ("mesh_phy.cpp", "DONE branch must `return false`",
     r"(txLastAirMs\s*=[^;]*;\s*log_e\s*\([^;]*\)\s*;\s*)return\s+false\s*;", r"\1"),
    # the flags read twice (the DONE test on a second read of the chip)
    ("mesh_phy.cpp", "txCutShort() must set txDoneLate",
     r"if\s*\(\s*\(\s*irq\s*&\s*IRQ_TX_DONE_MASK\s*\)", "if ((readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK)"),
    # ── the M1 follow-up (integration review, 2026-09-25) ──
    # sharePin re-sharing over a still-queued retraction (the place back on every radio)
    ("app_maps.cpp", "sharePin() must refuse",
     r"if\s*\(\s*retractionQueued\s*\(\s*idx\s*\)\s*\)", "if (false)"),
    # Move here without its question (the first retractionQueued(pinSel) in the file)
    ("app_maps.cpp", "Move here (ROW_P_MOVE) must",
     r"if\s*\(\s*retractionQueued\s*\(\s*pinSel\s*\)\s*\)", "if (false)", 0),
    # Rename without it (the third: Move, "Take it off" again, Rename)
    ("app_maps.cpp", "Rename (MAPS_RENAME) must",
     r"if\s*\(\s*retractionQueued\s*\(\s*pinSel\s*\)\s*\)", "if (false)", 2),
    # the Reading menu ticking for KOSync only again ("Queued" for the whole visit)
    ("app_books.cpp", "the Reading menu must arm its tick",
     r"msAppTimerEventPeriod\s*=\s*syncTxId\s*\?", "msAppTimerEventPeriod = false ?"),
    # ...or arming it BEFORE buildMenu() (a settled note would leave the watch running forever)
    ("app_books.cpp", "the Reading menu must arm its tick",
     r"buildMenu\s*\(\s*\)\s*;\s*armMenuTick\s*\(\s*\)\s*;", "armMenuTick(); buildMenu();"),
]


def sub_nth(pat, repl, text, nth):
    """Replace only the nth (0-based) match of pat. Returns (text, 1) or (text, 0)."""
    ms = list(re.finditer(pat, text))
    if len(ms) <= nth:
        return text, 0
    m = ms[nth]
    return text[:m.start()] + m.expand(repl) + text[m.end():], 1


def mutation_test(files):
    ok = True
    for mut in MUTATIONS:
        name, want, pat, repl = mut[:4]
        nth = mut[4] if len(mut) > 4 else 0
        mutated, n = sub_nth(pat, repl, files[name], nth)
        if n != 1:
            print(f"  SELF-TEST FAILED: the '{want}' guard is no longer where this mutation looks "
                  f"in {name} - update MUTATIONS with the guard")
            ok = False
            continue
        got = check(dict(files, **{name: mutated}))
        if not any(want in g for g in got):
            print(f"  SELF-TEST FAILED: removing the guard from {name} did not trip '{want}'")
            ok = False
    if ok:
        print(f"  ok  self-test: removing each of the {len(MUTATIONS)} guards (queued-is-honest, the health "
              "re-read, and review M1-M3's outcome / game / bench guards) from the real source trips its contract")
    return ok


def main():
    if not selftest():
        return 1
    files = {}
    for name in ("mesh_phy.cpp", "mesh_phy.h", "meshtastic_service.cpp", "WiPhone.ino",
                 "app_gbc_xfer.cpp", "app_maps.cpp", "app_books.cpp", "serial_cmd.cpp"):
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
    print("  ok  no blocking transmit; health/poll/serviceTx guards, the pump's order, the "
          "background senders' idle gates, the four 'queued is honest' guards, every own frame's "
          "outcome and the UI that waits on it, the game/upload TX completion and the safe bench "
          "reboots/sleeps all hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""power_bench.py — run THE POWER BENCH (docs/HANDOFF.md, 2026-09-03) as one guided session.

    tools/power_bench.py /dev/cu.usbserial-025A3EAF            # the whole session, ~10 min
    tools/power_bench.py /dev/cu.usbserial-025A3EAF --check    # just print the phone's state

The firmware's serial `power` command (0.9.57) flips one consumer at a time; the USB power
meter reads the result. This script does the serial half so a session is: put the meter
between the cable and the phone, run this, type the meter reading each time it asks. It
sends the commands in the handoff's order, restores every switch it flipped, and prints the
deltas with the handoff's own interpretation, so nothing has to be remembered or recomputed.

⚠ Opening the port RESETS the phone (DTR/RTS are wired to EN/IO0) — that is why the handoff
says to open the terminal FIRST. The script waits for the boot, then for the screen to sleep,
because every reading is "screen asleep, WiFi off, cell full" or it means nothing.

Readings are typed as the meter shows them (USB mA). Only DELTAS mean anything: the charger
IC and the CP2104 ride along in every absolute number. `power lora sleep` is the calibration
— the SX1276 hangs off the same 3.3 V rail, so ~11 mA proves the USB→battery factor is 1:1.
"""
import sys, time, argparse, datetime, os, re

try:
    import serial
except ImportError:
    sys.exit("pyserial is needed: python3 -m pip install pyserial")

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("port")
ap.add_argument("--baud", type=int, default=500000)
ap.add_argument("--wait", type=float, default=15.0, help="seconds to let the phone boot after the port opens")
ap.add_argument("--settle", type=float, default=8.0, help="seconds to let the meter settle after a switch")
ap.add_argument("--sleep-secs", type=int, default=60, help="length of each `power sleep` (read the meter during it)")
ap.add_argument("--screen-timeout", type=float, default=300.0, help="how long to wait for the screen to sleep")
ap.add_argument("--check", action="store_true", help="print the phone's `power` state and exit")
ap.add_argument("--out", default=None, help="TSV to write (default backups/power-bench-<date>-<port>.tsv)")
a = ap.parse_args()


class Phone:
    def __init__(self, port, baud):
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = baud
        self.s.timeout = 0.2
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        self.log = []

    def drain(self, secs, echo=True):
        """Read for `secs`, returning everything, echoing the interesting lines."""
        out = b""
        t0 = time.time()
        while time.time() - t0 < secs:
            chunk = self.s.read(8192)
            if chunk:
                out += chunk
        text = out.decode("utf-8", "replace")
        if echo:
            for ln in text.splitlines():
                if ln.strip() and (ln.startswith("power") or ln.startswith("  ") or "HEALTH" in ln
                                   or ln.startswith("gps") or "back after" in ln):
                    print("   phone> " + ln.rstrip())
        self.log.append(text)
        return text

    def cmd(self, line, secs=1.5):
        self.s.reset_input_buffer()
        self.s.write((line + "\n").encode())
        self.s.flush()
        return self.drain(secs)

    def state(self):
        """Parse the `power` line into a dict."""
        text = self.cmd("power", 1.5)
        m = re.search(r"power: (.*)", text)
        if not m:
            return {}
        return dict(kv.split("=", 1) for kv in m.group(1).split() if "=" in kv)


def ask(prompt):
    """A meter reading in mA; 's' skips this step, 'q' quits (switches are restored first)."""
    while True:
        raw = input("   " + prompt + " [mA, or s=skip, q=quit]: ").strip().lower()
        if raw in ("s", "skip"):
            return None
        if raw in ("q", "quit"):
            raise KeyboardInterrupt
        try:
            return float(raw.replace("ma", "").strip())
        except ValueError:
            print("   a number, please (what the meter shows, in mA)")


print(f"opening {a.port} @ {a.baud} — this REBOOTS the phone; waiting {a.wait:.0f} s for the boot")
ph = Phone(a.port, a.baud)
ph.drain(a.wait, echo=False)
st = ph.state()
if not st:
    sys.exit("no `power:` line came back — is this 0.9.57, and is the baud right?")
print("phone: " + "  ".join(f"{k}={v}" for k, v in st.items()))
if a.check:
    sys.exit(0)

problems = []
if st.get("wifi") != "OFF":
    problems.append("WiFi is ON — switch it off first (menu > WiFi: off); `power sleep` refuses otherwise")
if st.get("audio") == "ON":
    problems.append("audio is ON — stop the music first")
if st.get("gbc") not in (None, "0"):
    problems.append("the Game Boy is running — quit it first")
if st.get("lora") != "rx":
    problems.append(f"LoRa is '{st.get('lora')}' — the radio calibration step needs it in rx")
if problems:
    for p in problems:
        print("✗ " + p)
    sys.exit(1)

# Cell full? The HEALTH line carries soc/est/v/chg once a minute; the meter reading is only the
# board if the charger has tapered. We do not block on it — we print it so it goes in the record.
print("\nwaiting for the screen to sleep (triple-tap the screen to hurry it; the reading must be scr=0)")
t0 = time.time()
health = ""
while True:
    text = ph.cmd("power", 1.5)
    m = re.search(r"screen=(\d+)", text)
    hm = re.search(r"HEALTH .*", "\n".join(ph.log[-6:]))
    if hm:
        health = hm.group(0)
    if m and m.group(1) == "0":
        break
    if time.time() - t0 > a.screen_timeout:
        sys.exit("the screen never slept — check Settings > Screen (sleep timeout) or triple-tap")
    time.sleep(8)
print("screen asleep." + (f"  last health: {health}" if health else ""))
cpu = re.search(r"cpu=(\d+)MHz", text)
if cpu and cpu.group(1) != "80":
    print(f"⚠ cpu={cpu.group(1)}MHz with the screen asleep — the CPU gate has not dropped to 80 MHz; "
          "wait a minute and re-run, or the I0 baseline will include the clock")
print("\nput the meter between the cable and the phone if it is not already; let it settle ~30 s.")
input("   press Enter when the meter is inline and steady...")

rows = []   # (label, reading, restore-note)


def step(label, on_cmds, off_cmds, wait, prompt):
    """Flip on_cmds, wait, ask for the reading, flip off_cmds. Always restores."""
    print(f"\n[{label}]")
    try:
        for c in on_cmds:
            ph.cmd(c, 1.5)
        if wait:
            print(f"   settling {wait:.0f} s ...")
            ph.drain(wait, echo=True)
        val = ask(prompt)
    finally:
        for c in off_cmds:
            ph.cmd(c, 1.5)
    rows.append((label, val))
    return val


def sleep_step(label, pre_cmds, post_cmds):
    """`power sleep N`: the reading has to be taken DURING the sleep, so ask while it is asleep,
    then wait for the wake line before touching the phone again (typed serial is lost meanwhile)."""
    print(f"\n[{label}]")
    val = None
    try:
        for c in pre_cmds:
            ph.cmd(c, 1.5)
        ph.s.reset_input_buffer()
        ph.s.write(f"power sleep {a.sleep_secs}\n".encode())
        ph.s.flush()
        t0 = time.time()
        ph.drain(3.0, echo=True)
        print(f"   the phone is light-sleeping for {a.sleep_secs} s — read the meter NOW, while it is asleep")
        val = ask("reading during the sleep")
        left = a.sleep_secs + 3 - (time.time() - t0)
        if left > 0:
            print(f"   waiting {left:.0f} s for it to wake ...")
            text = ph.drain(left, echo=True)
        text = ph.drain(2.0, echo=True)
        if "back after" not in "".join(ph.log[-3:]):
            print("   ⚠ no 'back after' line seen — if the phone is unresponsive, press a key (keypad wakes it)")
    finally:
        for c in post_cmds:
            ph.cmd(c, 1.5)
    rows.append((label, val))
    return val


try:
    I0 = step("I0 baseline: on, screen asleep, WiFi off", [], [], 0,
              "I0 — the steady reading now")
    I1 = step("I1 LoRa asleep (the calibration: expect ~11 mA less)", ["power lora sleep"], ["power lora rx"],
              a.settle, "I1 — with the radio asleep")
    I2 = step("I2 panel controller asleep (DISPOFF+SLPIN)", ["power lcd sleep"], ["power lcd wake"],
              a.settle, "I2 — with the panel asleep")
    I3 = step("I3 I2S stopped", ["power i2s stop"], ["power i2s start"],
              a.settle, "I3 — with I2S stopped")
    I4 = sleep_step("I4 ESP32 light sleep (everything except the core)", [], [])
    I5 = sleep_step("I5 the floor: LoRa asleep + light sleep", ["power lora sleep"], ["power lora rx"])
    I6 = step("I6 240 MHz (gps on = the clock step alone on phone 1)", ["gps on"], ["gps off"],
              a.settle, "I6 — at 240 MHz")
    print("\n[I_off  the whole board: hold END until the phone powers OFF, then read the meter]")
    print("   (the 3.3 V rail drops to 0 V; what is left is the charger IC + CP2104 alone)")
    Ioff = ask("I_off — with the phone OFF")
    rows.append(("I_off phone powered off", Ioff))
except KeyboardInterrupt:
    print("\nstopped — every switch flipped so far was restored")

# ---------------------------------------------------------------- the table
def d(x, y):
    return None if x is None or y is None else x - y


print("\n" + "=" * 72)
print("POWER BENCH  " + datetime.datetime.now().strftime("%Y-%m-%d %H:%M") + f"  {a.port}")
print(f"start state: " + "  ".join(f"{k}={v}" for k, v in st.items()))
if health:
    print("health:      " + health)
print("-" * 72)
for label, val in rows:
    print(f"{label:<58} {'' if val is None else f'{val:7.1f}'}")
print("-" * 72)
vals = dict((r[0].split()[0], r[1]) for r in rows)
I0 = vals.get("I0"); I1 = vals.get("I1"); I2 = vals.get("I2"); I3 = vals.get("I3")
I4 = vals.get("I4"); I5 = vals.get("I5"); I6 = vals.get("I6"); Ioff = vals.get("I_off")
deltas = [
    ("board idle       = I0 − I_off", d(I0, Ioff),
     "40-50 → the gap is the CELL's health; 80-100 → something on the board eats 30-50 mA"),
    ("SX1276 RX        = I0 − I1", d(I0, I1),
     "~11 → USB≈battery 1:1 (LDO rail); ~9.5-10 → a switcher, divide deltas by ~1.15; 0 → method wrong, stop"),
    ("panel controller = I0 − I2", d(I0, I2), "1-8 expected; the SLPIN lever if it is big"),
    ("I2S + DMA        = I0 − I3", d(I0, I3), "1-3 expected"),
    ("ESP32 core       = I0 − I4", d(I0, I4), "the MOST any CPU-sleep work could ever save"),
    ("the floor        = I5", I5, "charger overhead + panel + SD + regulators: where the rest of the gap lives"),
    ("80→240 MHz       = I6 − I0", d(I6, I0), "the cost of the clock alone (the 08-22 downclock never measured it)"),
]
for name, v, note in deltas:
    print(f"{name:<32} {'   --' if v is None else f'{v:6.1f}'} mA   {note}")
print("=" * 72)

out = a.out or os.path.join("backups", "power-bench-" + datetime.datetime.now().strftime("%Y-%m-%d-%H%M")
                            + "-" + os.path.basename(a.port).replace("cu.usbserial-", "") + ".tsv")
os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
with open(out, "w") as f:
    f.write("# power bench " + datetime.datetime.now().isoformat(timespec="minutes") + " " + a.port + "\n")
    f.write("# start: " + "  ".join(f"{k}={v}" for k, v in st.items()) + "\n")
    if health:
        f.write("# " + health + "\n")
    f.write("reading\tmA\n")
    for label, val in rows:
        f.write(f"{label}\t{'' if val is None else val}\n")
    f.write("delta\tmA\n")
    for name, v, _ in deltas:
        f.write(f"{name}\t{'' if v is None else round(v, 1)}\n")
print(f"written: {out}   (backups/ is gitignored — quote the deltas in the handoff)")

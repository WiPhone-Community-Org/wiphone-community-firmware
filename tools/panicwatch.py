"""Serial capture that CANNOT be killed by a transient port problem.

⚠ The previous version died the moment anything else touched the port — pyserial raises
"device reports readiness to read but returned no data (multiple access on port?)" — and it
took the crash we were waiting for with it. Any exception now means reopen and carry on;
losing a few bytes beats losing the backtrace.

⚠ NOTHING ELSE MAY OPEN THIS PORT while this runs. Route commands through /tmp/wiphone.cmd
instead: write a line to that file and this process types it for you.
"""
import serial, time, subprocess, re, os, sys, glob

ELF  = '/Users/{username}/wiphone/.pio/build/wiphone/firmware.elf'
A2L  = '/Users/{username}/.platformio/packages/toolchain-xtensa32/bin/xtensa-esp32-elf-addr2line'

# THERE ARE TWO PHONES NOW (2026-08-22). Phone 1 is 025A3EAF, phone 2 is 025A3F65 —
# adjacent serials from the same batch, so read them carefully. Pass a port (or a bare
# serial-number fragment) as argv[1]; with no argument, take the only usbserial device
# present, and refuse to guess when two are plugged in. The log and cmd files are named
# per port so two watchers can run side by side without eating each other's commands.
def _pick_port(arg):
    if arg and arg.startswith('/dev/'):
        return arg
    cands = sorted(glob.glob('/dev/cu.usbserial-*'))
    if arg:
        m = [c for c in cands if arg.lower() in c.lower()]
        if len(m) == 1:
            return m[0]
        raise SystemExit('no unique port matching %r in %s' % (arg, cands))
    if len(cands) == 1:
        return cands[0]
    raise SystemExit('need a port argument — found %s' % (cands or 'none'))

PORT = _pick_port(sys.argv[1] if len(sys.argv) > 1 else None)
_tag = PORT.rsplit('-', 1)[-1]
LOG  = '/tmp/wiphone-serial-%s.log' % _tag
CMD  = '/tmp/wiphone-%s.cmd' % _tag
# Phone 1 keeps the historic paths, so every note and script that says
# /tmp/wiphone.cmd still works.
if _tag == '025A3EAF':
    LOG, CMD = '/tmp/wiphone-serial.log', '/tmp/wiphone.cmd'

f = open(LOG, 'a', buffering=1)
def note(msg):
    print(msg, flush=True); f.write(msg + '\n')

note('=== watcher started %s ===' % time.strftime('%H:%M:%S'))
buf = b''
s = None
while True:
    try:
        if s is None:
            s = serial.Serial(PORT, 500000, timeout=1)
            note('=== port opened %s ===' % time.strftime('%H:%M:%S'))
        # A queued command? Type it, then delete the file.
        if os.path.exists(CMD):
            try:
                cmd = open(CMD).read().strip()
                os.remove(CMD)
                if cmd:
                    # Wake the UART first: with the phone idling (DFS, 80 MHz) the first
                    # bytes of a write get mangled — 2026-08-20, 'pos' arrived as 40 chars
                    # of junk while a command 3 s later was clean. A sacrificial newline
                    # plus a pause absorbs the wake; the phone ignores empty lines.
                    s.write(b'\n'); s.flush()
                    time.sleep(0.25)
                    s.write((cmd + '\n').encode()); s.flush()
                    note('=== sent: %s ===' % cmd)
            except Exception as e:
                note('=== cmd failed: %s ===' % e)
        d = s.read(4096)
        if not d:
            continue
        buf += d
    except Exception as e:
        note('=== serial hiccup, reopening: %s ===' % e)
        try:
            if s: s.close()
        except Exception:
            pass
        s = None
        time.sleep(2)
        continue

    while b'\n' in buf:
        line, buf = buf.split(b'\n', 1)
        t = line.decode('utf-8', 'replace').rstrip()
        f.write(t + '\n')
        if any(k in t for k in ('Backtrace:', 'Guru Meditation', 'reset_reason=4', 'assert')):
            note('*** %s' % t)
        if 'Backtrace:' in t:
            addrs = re.findall(r'0x4[0-9a-fA-F]{7}', t)
            if addrs:
                try:
                    out = subprocess.run([A2L, '-pfiaC', '-e', ELF] + addrs,
                                         capture_output=True, text=True, timeout=90).stdout
                    note('--- DECODED BACKTRACE ---\n' + out)
                except Exception as e:
                    note('addr2line failed: %s' % e)

"""One command: files onto the WiPhone — no hands on the phone, no browser.

Wraps the raw chunked transport (chunkup.py's header says why NOT `curl -F`)
with the bench's serial bridge, so the whole flow is:

    python3 tools/wiphone_send.py --app books file1.epub file2.epub

  * finds the phone: --ip wins; else the serial bridge is asked (`up` prints
    the URL); else wiphone.local is tried
  * starts the uploader itself over the bridge (`up on books`) when a
    panicwatch bridge is running and the server is off
  * pushes every file over ONE keep-alive connection to :8081 — 16 KB pieces,
    CRC32 per piece, resuming from whatever the card already holds
  * verifies each file by re-asking the phone how many bytes it holds
  * --off stops the server afterwards (default leaves it; it stops itself
    after 10 idle minutes anyway)

Exit 0 only if every file verified. Works without the bridge too: start the
server from the phone (any upload screen, or `up on books` by hand) and pass
--ip <addr>.

⚠ The app chooses the folder (books=/books photos=/photos roms=/roms), and an
upload server already running for a DIFFERENT folder is restarted for the one
asked for. A plain re-run is cheap: files the phone already holds are skipped
by the resume query.
"""
import argparse
import glob
import http.client
import os
import re
import socket
import sys
import time
import urllib.parse
import zlib

PIECE = 16384          # the raw port's piece size; in-flight is TCP-window-bounded anyway
RAW_PORT = 8081


# ── the serial bridge (tools/panicwatch.py) ──────────────────────────────────────────
def bridge_paths(tag=None):
    """(cmd_file, log_file) for a running bridge, or (None, None)."""
    cands = []
    if tag:
        cands = [('/tmp/wiphone-%s.cmd' % tag, '/tmp/wiphone-serial-%s.log' % tag)]
        if tag == '025A3EAF':
            cands = [('/tmp/wiphone.cmd', '/tmp/wiphone-serial.log')]
    else:
        # phone 1 keeps the historic names; other bridges are per-port
        cands = [('/tmp/wiphone.cmd', '/tmp/wiphone-serial.log')]
        for log in glob.glob('/tmp/wiphone-serial-*.log'):
            t = log[len('/tmp/wiphone-serial-'):-len('.log')]
            cands.append(('/tmp/wiphone-%s.cmd' % t, log))
    for cmd, log in cands:
        # a live bridge keeps its log fresh; a stale one is yesterday's
        if os.path.exists(log) and time.time() - os.path.getmtime(log) < 300:
            return cmd, log
    return None, None


def bridge_say(cmd_file, log_file, command, wait=4.0):
    """Send one command through the bridge and return the log lines it produced."""
    start = os.path.getsize(log_file)
    with open(cmd_file, 'w') as f:
        f.write(command + '\n')
    deadline = time.time() + wait
    marker = '=== sent: %s ===' % command
    while time.time() < deadline:
        time.sleep(0.3)
        with open(log_file, 'rb') as f:
            f.seek(start)
            tail = f.read().decode(errors='replace')
        if marker in tail and tail.rstrip().rsplit(marker, 1)[1].count('\n') >= 1:
            return tail.rsplit(marker, 1)[1]
    return ''


def phone_ip_via_bridge(cmd_file, log_file, app):
    out = bridge_say(cmd_file, log_file, 'up')
    m = re.search(r'uploader: ON\s+http://([0-9.]+)/', out)
    wanted = {'books': '/books', 'photos': '/photos', 'roms': '/roms', 'maps': '/maps',
              'keys': '/API Keys'}[app]
    if m and wanted.split('/')[-1] in out:
        return m.group(1)
    if m:
        # serving a different folder — restart for the right one
        bridge_say(cmd_file, log_file, 'up off')
        time.sleep(1)
        m = None
    if not m:
        cmd = 'up on' if app == 'roms' else 'up on %s' % app
        out = bridge_say(cmd_file, log_file, cmd, wait=8.0)
        m = re.search(r'uploader: ON\s+http://([0-9.]+)/', out)
        if not m:
            m2 = re.search(r'Low memory[^"\n]*', out)
            raise SystemExit('phone refused to start the uploader%s' %
                             (': ' + m2.group(0) if m2 else ' (see %s)' % log_file))
    return m.group(1)


# ── the transport (same wire protocol as the page's fast path) ───────────────────────
def held_bytes(conn, name):
    conn.request('GET', '/chunk?name=%s' % urllib.parse.quote(name))
    r = conn.getresponse()
    body = r.read().decode(errors='replace').strip()
    try:
        return int(body)
    except ValueError:
        return 0


def push_file(ip, path, verbose=True, name=None, replace=False):
    """Returns (ok, message). One keep-alive connection; resumes; verifies.
    `name` is what the phone will call it - the basename, or (tree mode) the path relative
    to the tree's parent, forward slashes, which the maps uploader turns into folders.
    `replace` sends from byte 0 regardless of what the phone holds: resume-by-size cannot
    tell a re-converted tile from the old one (every tile is exactly 131072 bytes)."""
    if name is None:
        name = os.path.basename(path)
    size = os.path.getsize(path)
    tries = 0
    off = None
    conn = None
    t0 = time.time()
    with open(path, 'rb') as f:
        while True:
            try:
                if conn is None:
                    conn = http.client.HTTPConnection(ip, RAW_PORT, timeout=30)
                    off = 0 if replace else held_bytes(conn, name)
                    if off > size:
                        off = 0        # a different file wearing our name; start over
                if off >= size:
                    break
                f.seek(off)
                piece = f.read(min(PIECE, size - off))
                crc = zlib.crc32(piece) & 0xFFFFFFFF
                url = '/chunk?name=%s&off=%d&crc=%08x&last=%d' % (
                    urllib.parse.quote(name), off, crc, 1 if off + len(piece) >= size else 0)
                conn.request('POST', url, body=piece,
                             headers={'Content-Type': 'text/plain'})
                r = conn.getresponse()
                body = r.read().decode(errors='replace').strip()
                if r.status == 200:
                    tries = 0
                    m = re.search(r'(\d+)', body)
                    got = int(m.group(1)) if m else -1
                    off = got if off < got <= size else off + len(piece)
                    if verbose:
                        pct = 100 * off // size
                        sys.stdout.write('\r  %s: %d%% ' % (name, pct))
                        sys.stdout.flush()
                elif r.status == 409:
                    m = re.search(r'(\d+)', body)
                    if m and int(m.group(1)) <= size:
                        off = int(m.group(1))
                    tries += 1
                    if tries > 24:
                        return False, 'kept resyncing at %d' % off
                    time.sleep(0.15)
                elif r.status == 507:
                    # SD write hiccup — transient under sustained load (measured:
                    # 3 of 12 files on 2026-08-26), and the page JS retries it
                    # too. The held-progress query is the stall guard: a card
                    # that stops advancing gets 24 tries, not forever.
                    tries += 1
                    if tries > 24:
                        return False, 'SD kept refusing writes at %d' % off
                    time.sleep(min(0.5 * tries, 5.0))
                    off = held_bytes(conn, name)
                    if off > size:
                        off = 0
                else:
                    return False, 'refused: %d %s' % (r.status, body[:60])
            except (http.client.HTTPException, OSError):
                if conn is not None:
                    conn.close()
                conn = None
                tries += 1
                if tries > 24:
                    return False, 'unreachable after %d tries (off=%s)' % (tries - 1, off)
                time.sleep(min(0.5 * tries, 5.0))
    # the honest check: what does the phone say it holds NOW?
    try:
        final = held_bytes(conn, name)
    finally:
        conn.close()
    dt = time.time() - t0
    if final != size:
        return False, 'SIZE MISMATCH: phone holds %d of %d' % (final, size)
    rate = size / dt / 1024 if dt > 0 else 0
    return True, 'ok — %d B in %.1f s (%.0f KB/s)' % (size, dt, rate)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('files', nargs='*')
    ap.add_argument('--app', choices=('books', 'photos', 'roms', 'maps', 'keys'), default='books',
                    help='which folder the phone puts them in (default books)')
    ap.add_argument('--tree', help='send every file under this folder, keeping the relative '
                                   'path (maps: a converted <area> folder -> /maps/<area>/...)')
    ap.add_argument('--replace', action='store_true',
                    help='send every file from byte 0 even if the phone holds one of the same '
                         'size (a re-converted tile tree: every tile is the same size); '
                         'always on for --app keys')
    ap.add_argument('--ip', help='phone address; otherwise bridge, then wiphone.local')
    ap.add_argument('--port-tag', help='bridge tag when two phones are attached '
                                       '(e.g. 025A3F65)')
    ap.add_argument('--off', action='store_true', help='stop the uploader afterwards')
    args = ap.parse_args()

    missing = [p for p in args.files if not os.path.isfile(p)]
    if missing:
        raise SystemExit('not a file: ' + ', '.join(missing))
    # (path, name-on-the-phone) pairs. A tree is sent under its own folder name so
    # `--tree out/home` lands at /maps/home/<z>/<x>/<y>.565 - the area the phone lists.
    jobs = [(p, os.path.basename(p)) for p in args.files]
    if args.tree:
        if args.app != 'maps':
            raise SystemExit('--tree is for --app maps')
        root = os.path.abspath(args.tree.rstrip('/'))
        if not os.path.isdir(root):
            raise SystemExit('not a folder: ' + args.tree)
        area = os.path.basename(root)
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(d for d in dirnames if not d.startswith('.'))
            for fn in sorted(filenames):
                if fn.startswith('.'):
                    continue                       # Finder sidecars; the phone refuses them anyway
                rel = os.path.relpath(os.path.join(dirpath, fn), root).replace(os.sep, '/')
                jobs.append((os.path.join(dirpath, fn), area + '/' + rel))
    if not jobs:
        raise SystemExit('nothing to send')

    cmd_file = log_file = None
    ip = args.ip
    if not ip:
        cmd_file, log_file = bridge_paths(args.port_tag)
        if cmd_file:
            ip = phone_ip_via_bridge(cmd_file, log_file, args.app)
            print('phone at %s (via serial bridge)' % ip)
        else:
            try:
                ip = socket.gethostbyname('wiphone.local')
                print('phone at %s (wiphone.local) — make sure the %s uploader is ON'
                      % (ip, args.app))
            except OSError:
                raise SystemExit('no --ip, no serial bridge, wiphone.local not '
                                 'resolving. Start tools/panicwatch.py or pass --ip.')

    # A key file is always sent whole: resume-by-size would skip a NEW key of the same length
    # (every AIza key is 39 characters) and print "verified" with the old key still on the card,
    # or append a longer key's tail to the old one.
    replace = args.replace or args.app == 'keys'
    fails = 0
    done = 0
    t_all = time.time()
    for p, name in jobs:
        ok, msg = push_file(ip, p, verbose=not args.tree, name=name, replace=replace)
        done += 1
        if args.tree:
            # hundreds of files: one line per failure, a running count otherwise
            if not ok:
                print('\r  %s: %s        ' % (name, msg))
            else:
                sys.stdout.write('\r  %d/%d sent  (%.0f s)   ' % (done, len(jobs), time.time() - t_all))
                sys.stdout.flush()
        else:
            print('\r  %s: %s        ' % (name, msg))
        if not ok:
            fails += 1
    if args.tree:
        print()
    if args.off and cmd_file:
        bridge_say(cmd_file, log_file, 'up off')
        print('uploader stopped')
    if fails:
        raise SystemExit('%d file(s) FAILED' % fails)
    print('all %d file(s) verified on the phone' % len(jobs))


if __name__ == '__main__':
    main()

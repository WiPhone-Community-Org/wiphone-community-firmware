"""Upload files to a WiPhone the way the browser does — the CHUNKED transport.

⚠ USE THIS, NOT `curl -F`. The phone serves two upload paths and they are not
equivalent:

  /chunk on port 8081   pieces of 16 KB, body straight to PSRAM, ONE long-lived
                        connection. Per-request heap cost is zero, so it rides
                        ABOVE the low-heap breaker. This is what the page's own
                        JavaScript uses after it probes the port.
  /upload               legacy whole-file multipart. Exists for browsers with no
                        fetch(). ⚠ curl -F lands here by default.

MEASURED 2026-08-25, 13 photos of 12-51 KB onto two phones:
  * via /upload : phone 1 took 0 of 13; phone 2 took 6 then the breaker tripped
                  and refused the rest. Each file cost ~1.5 KB of the largest
                  free block, and the breaker closes the listener at 6144 —
                  which a browser reports as "site cannot be loaded".
  * via /chunk  : 13 of 13 on both, ~0.6 s each, nothing tripped.

It also RESUMES: the GET returns how many bytes the phone already holds, so a
re-run costs nothing for files that are already there and picks up a partial
where it stopped.

  python3 tools/chunkup.py <ip> <file1.jpg,file2.jpg,...>

Start the receiving end first — `up on photos` (or `up on books` / `up on`) on
the serial console, or the Files app's "Upload into this folder".
"""
import http.client, zlib, os, sys, time
PIECE=16384    # what the page uses on the raw port; 4096 is the same-origin fallback's size
def upload(ip, path):
    name=os.path.basename(path); data=open(path,'rb').read()
    c=http.client.HTTPConnection(ip, 8081, timeout=30)
    # resume point
    c.request('GET','/chunk?name=%s'%name); r=c.getresponse(); held=r.read().decode().strip()
    try: off=int(held)
    except: off=0
    if off>=len(data):
        c.close(); return 'already complete (%d B)'%off
    while off < len(data):
        end=min(off+PIECE, len(data)); piece=data[off:end]
        crc=zlib.crc32(piece) & 0xFFFFFFFF
        url='/chunk?name=%s&off=%d&crc=%08x&last=%d' % (name, off, crc, 1 if end>=len(data) else 0)
        c.request('POST', url, body=piece, headers={'Content-Type':'text/plain','Content-Length':str(len(piece))})
        r=c.getresponse(); body=r.read().decode(errors='replace').strip()
        if r.status!=200:
            c.close(); return 'FAIL at off=%d: %d %s' % (off, r.status, body[:40])
        off=end
    c.close(); return 'ok (%d B)'%len(data)

if len(sys.argv) < 3:
    raise SystemExit("usage: chunkup.py <ip> <file> [file ...]   (or a comma-separated list)")
ip = sys.argv[1]
paths = []
for a in sys.argv[2:]:
    paths += [x for x in a.split(',') if x.strip()]
missing = [p for p in paths if not os.path.isfile(p)]
if missing:
    raise SystemExit("not a file: " + ", ".join(missing))
fails = 0
for path in paths:
    t0 = time.time()
    try:
        res = upload(ip, path)
    except Exception as e:
        res = 'EXC %s: %s' % (type(e).__name__, e)
    if not res.startswith(('ok', 'already')):
        fails += 1
    print("  %-30s %-28s %.1fs" % (os.path.basename(path), res, time.time() - t0))
    sys.stdout.flush()
    time.sleep(0.5)
sys.exit(1 if fails else 0)      # so a script can tell whether the batch landed

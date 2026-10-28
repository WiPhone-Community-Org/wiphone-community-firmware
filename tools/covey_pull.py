#!/usr/bin/env python3
"""covey_pull.py - COVEY's DOWNLOADED map tiles into the tile pool, and what it will not take.

    python3 tools/covey_pull.py pull              # what `cardday.sh covey pull` runs
    python3 tools/covey_pull.py pull --dry-run    # manifests and counts only: nothing moves
    python3 tools/covey_pull.py report            # COVEY's streamed-only places (`cardday.sh status`)

COVEY keeps two trees of tiles (covey-ui's covey_ui/tilestore.py):

    /root/covey-tiles/<src>/z/x/y.png            DOWNLOADED on purpose - what the pool takes
    /root/covey-tiles-streamed/<src>/z/x/y.png   STREAMED while panning - never taken

A streamed tile is there because the map happened to be looked at there, and it may expire when
COVEY's card fills. Pooling it would put a patchwork on the phones that nobody chose, so this
reads ONLY /root/covey-tiles/<src>: usgs-topo, usgs-img, otm, and any other folder whose name
the phone could use as a map area. It never takes the root-level digit folders (the legacy
'local' tree that fetch_tiles.py wrote) or a folder named *-streamed, and it never names the
streamed tree in any command it sends.

Before anything moves, it prints COVEY's own list of streamed-only places (its tilestore
--places), so a spot that exists only because it was streamed can be downloaded properly on
COVEY before the phones are updated. A place whose ground the master already holds (a phone's
own download, the Mac's fetch) is listed apart: the phones get it anyway. WITHOUT THAT REPORT
THE PULL REFUSES (exit 2): a COVEY that cannot give it is most likely one whose streamed tiles
still sit in the downloaded tree, and pulling them would put them on both phones as if chosen.
`--without-report` overrides that, for when the cut-over is known to be done.

NEVER OVERWRITES A MASTER TILE, AND NEVER POOLS A CUT ONE. Tiles land in a staging folder,
<master>/.incoming/<src>, are checked there, and only then go into the master with os.link(),
which fails when the name exists. So "the master's copy wins" is one atomic system call, not a
check followed by a write. The check is COVEY's size, more than 0 bytes, AND the file's own end
(a PNG's IEND chunk, a JPEG's FFD9): ⚠ when a tar stream stops mid-tile, bsdtar still extends
that tile to the size in its header with ZEROS, so a cut tile is exactly COVEY's size. The tile
bsdtar was writing when a stream stopped is also dropped by name, and whatever an earlier,
interrupted pull left in staging is dropped unread. A zero-byte tile on COVEY is never pulled;
it is listed in <master>/covey_zero_bytes.txt instead, and deleting it on COVEY stays a
deliberate act. The only files this tool ever deletes are its own staged copies.

THE PASS TEST is a set difference: every non-empty tile in COVEY's manifest must be in the
master afterwards, non-empty. Not a count - the master is a union and legitimately holds tiles
COVEY lacks until the next `cardday.sh covey push`. An EMPTY master file where COVEY has a good
tile fails it: the pull cannot replace it, and the phones would get no tile there.

Transfer: one tar stream per zoom when there are BULK_MIN or more tiles to fetch (about 40-50
files/s on this link, against about 13 for openrsync), under a deadline worked out from the
bytes; then up to RSYNC_PASSES `rsync --files-from` passes for what is still missing. macOS has
no `timeout` command, which is why this is Python.

--local and --remote-root exist for tests/check_covey_pull.py: the "remote" is a directory on
this Mac and the same tar/rsync commands run through sh, without ssh or sudo.
"""
import argparse
import fcntl
import math
import os
import re
import shlex
import stat
import string
import subprocess
import sys
import tempfile
import time

REMOTE_ROOT = "/root/covey-tiles"            # covey-ui runs as root: its tiles are root's
REMOTE_PREFS = "/root/.covey/prefs.json"
REMOTE_WAYPOINTS = "/root/.covey/waypoints.json"
BULK_MIN = 500          # tiles at one zoom that are worth a tar stream
RSYNC_PASSES = 6        # gap-fill passes after the tar stream (or instead of it, for a few)
TAR_BPS = 150 * 1000    # bytes/s the tar deadline assumes: pull_covey's measured floor, 2026-09-21
RSYNC_BPS = 100 * 1000
REPORT_TIMEOUT_S = 120
TILE_RE = re.compile(r"^(\d+)/(\d+)/(\d+)\.png$")
AREA_OK = set(string.ascii_letters + string.digits + "-_.")
# `tar xv` names each entry on stderr as it STARTS it ("x 15/1/2.png"), and appends an error to
# that same line when the entry fails ("x 15/1/2.pngtar: Write error").
TAR_X_RE = re.compile(r"^x (?:\./)?(\d+/\d+/\d+\.png)")
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
PNG_END = b"\x00\x00\x00\x00IEND\xaeB`\x82"     # the IEND chunk: length 0, type, CRC
JPEG_MAGIC = b"\xff\xd8\xff"
JPEG_END = b"\xff\xd9"                              # EOI
KM_PER_DEG = 111.0      # tilestore's KM_PER_DEG_TILEDL: the figure its half_x/y_km were made with
COVER_MAX = 500000      # tiles one place's master check may look at (a 20 km place at z17: ~40,000)

HEAD = "Will NOT reach the phones unless downloaded on COVEY first:"
HEAD_HAVE = "Streamed on COVEY, already in the master (the phones get it):"
WHERE = "on COVEY: Map › DL › Streamed-only"
WITHOUT = "python3 %s pull --without-report" % os.path.abspath(__file__)


class PullError(Exception):
    pass


def say(msg):
    """cardday.sh's `say` format; cardday tees this into cardday.log."""
    print("%s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def note(msg):
    print(msg, flush=True)


def num(v):
    try:
        return "{:,}".format(int(v))
    except (TypeError, ValueError):
        return str(v)


def gb(n):
    return "%.1f GB" % (n / 1e9)


def mb(n):
    return "%.1f MB" % (n / 1e6)


def last_line(text):
    lines = [l for l in (text or "").splitlines() if l.strip()]
    return lines[-1].strip() if lines else "no message"


def zoom_of(rel):
    return int(rel.split("/", 1)[0])


def source_ok(name):
    """May <name>, a folder in COVEY's downloaded root, be pulled as a map source?

    Not a dot folder, not a zoom number (the legacy 'local' tree keeps z/x/y at the root), not
    *-streamed (a COVEY_TILES given with a trailing slash once made tile_root()+"-streamed" a
    folder INSIDE the downloaded root), and a name the phone can use: the source folder becomes
    the card's /maps/<area>, and convert_tiles.py refuses the whole push for a name the
    firmware's mapAreaNameOk() would skip in silence."""
    if not name or name[0] == "." or name.isdigit() or name.endswith("-streamed"):
        return False
    return all(c in AREA_OK for c in name) and len(name.encode("utf-8")) <= 31


def run(argv, timeout, stdin=None):
    """-> (returncode, stdout, stderr). returncode is None when the deadline killed it."""
    try:
        p = subprocess.run(argv, stdin=stdin if stdin is not None else subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "", "timed out after %ds" % timeout
    return (p.returncode, p.stdout.decode("utf-8", "replace"),
            p.stderr.decode("utf-8", "replace"))


def tar_x_suspects(log, clean):
    """`tar xv`'s stderr -> ([rel] that must not be trusted, [error texts]).

    Suspect: every entry bsdtar appended an error to, and - when the pass did not end cleanly -
    the LAST entry it started, which is the one the stream stopped in."""
    suspects, errors, last = [], [], None
    for line in log.splitlines():
        m = TAR_X_RE.match(line)
        if not m:
            if line.strip() and "Error exit delayed" not in line:
                errors.append(line.strip().lstrip(": "))
            continue
        last = m.group(1)
        tail = line[m.end():].strip()
        if tail:
            suspects.append(last)
            errors.append("%s: %s" % (last, tail.lstrip(": ")))
    if not clean and last is not None and last not in suspects:
        suspects.append(last)
    return suspects, errors


def write_list(rels):
    fd, path = tempfile.mkstemp(prefix="covey_pull.", suffix=".lst")
    with os.fdopen(fd, "w") as f:
        f.write("".join(r + "\n" for r in rels))
    return path


# ---------------------------------------------------------------- the two ends

class Remote:
    """COVEY, over ssh. Every read goes through sudo: /root is root's."""
    sudo = "sudo "

    def __init__(self, host, root=REMOTE_ROOT, connect_timeout=10, places_cmd=None):
        root = os.path.normpath(root)
        if root.endswith("-streamed"):
            raise SystemExit("refusing %s: that is COVEY's STREAMED tree, which is never pooled"
                             % root)
        self.host = host
        self.root = root
        self.ssh_opts = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=%d" % connect_timeout,
                         "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=4"]
        self.places_cmd = places_cmd or self.default_places_cmd()

    def where(self):
        return "%s:%s" % (self.host, self.root)

    def argv(self, cmd):
        return ["ssh"] + self.ssh_opts + [self.host, cmd]

    def src_dir(self, src):
        return self.root + "/" + src

    # The command strings. Each names self.root or one source folder under it, and nothing else.
    def list_cmd(self):
        return "%sfind %s -mindepth 1 -maxdepth 1 -type d" % (self.sudo, shlex.quote(self.root))

    def manifest_cmd(self, src):
        # -name '*.png' leaves out .part files and rsync's temp names; %P is the path under src.
        return ("%sfind %s -type f -name '*.png' -printf '%%P\\t%%s\\n'"
                % (self.sudo, shlex.quote(self.src_dir(src))))

    def tar_cmd(self, src):
        return "%star cf - -C %s -T -" % (self.sudo, shlex.quote(self.src_dir(src)))

    def rsync_argv(self, src, listfile, dest):
        return (["rsync", "-rt", "-i", "--ignore-existing", "--min-size=1", "--timeout=60",
                 "-e", "ssh " + " ".join(self.ssh_opts), "--rsync-path=sudo rsync",
                 "--files-from=" + listfile, "%s:%s/" % (self.host, self.src_dir(src)),
                 dest + "/"])

    def default_places_cmd(self):
        # COVEY_PREFS and COVEY_WAYPOINTS explicitly: under sudo, HOME is not a thing to trust,
        # and the report names a place after the nearest waypoint.
        return ("cd covey-ui && sudo env COVEY_TILES=%s COVEY_PREFS=%s COVEY_WAYPOINTS=%s "
                "python3 -m covey_ui.tilestore --places"
                % (shlex.quote(self.root), REMOTE_PREFS, REMOTE_WAYPOINTS))

    def unreachable(self, rc):
        return rc == 255                  # ssh's own failure, not the remote command's

    # The operations.
    def list_sources(self):
        rc, out, err = run(self.argv(self.list_cmd()), 120)
        if rc != 0:
            raise PullError("could not list %s: %s" % (self.where(), last_line(err)))
        return sorted(os.path.basename(l.rstrip("/")) for l in out.splitlines() if l.strip())

    def manifest(self, src):
        rc, out, err = run(self.argv(self.manifest_cmd(src)), 900)
        if rc != 0:
            raise PullError("could not list %s/%s: %s" % (self.where(), src, last_line(err)))
        return out

    def places(self, timeout=REPORT_TIMEOUT_S):
        return run(self.argv(self.places_cmd), timeout)

    def fetch_tar(self, src, rels, dest, nbytes):
        """One tar stream of <rels> into <dest>. -> (ok, why, suspects).

        ⚠ A STREAM THAT STOPS MID-TILE LEAVES THAT TILE AT FULL SIZE. libarchive finishes an
        entry even when its data ran out, and ftruncate()s the file UP to the size in the tar
        header: a zero-filled tail at exactly COVEY's size, which a size check accepts (COVEY
        switched off or out of WiFi mid-pull, the deadline kill, the Mac's disk filling). So the
        extract runs verbose: bsdtar prints `x <path>` as it STARTS each entry and finishes an
        entry before starting the next, so when the pass does not end cleanly the last one it
        named is the one it was writing. That tile, and any entry it reported an error on, come
        back as suspects, and verify() drops them whatever their size."""
        deadline = nbytes / TAR_BPS + 120
        listfile = write_list(rels)
        try:
            with open(listfile) as lst, tempfile.TemporaryFile() as err1, \
                    tempfile.TemporaryFile() as err2:
                p1 = subprocess.Popen(self.argv(self.tar_cmd(src)), stdin=lst,
                                      stdout=subprocess.PIPE, stderr=err1)
                # -k: never overwrite. Staging holds only copies already checked good.
                p2 = subprocess.Popen(["tar", "xkvf", "-", "-C", dest], stdin=p1.stdout,
                                      stdout=subprocess.DEVNULL, stderr=err2)
                p1.stdout.close()
                why = ""
                try:
                    p2.wait(timeout=deadline)
                    p1.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    # ⚠ Kill only the LOCAL end. The remote tar dies of SIGPIPE when ssh goes.
                    # Never a remote `pkill -f 'tar cf -'`: that pattern is in the killing
                    # shell's own command line (the old pull_covey.py did exactly that).
                    for p in (p1, p2):
                        if p.poll() is None:
                            p.kill()
                    p1.wait()
                    p2.wait()
                    why = "stopped at the %ds deadline" % deadline
                clean = not why and p1.returncode == 0 and p2.returncode == 0
                err2.seek(0)
                suspects, errors = tar_x_suspects(err2.read().decode("utf-8", "replace"), clean)
                if clean:
                    return True, "", suspects
                if not why:
                    err1.seek(0)
                    why = "tar exit %s/%s: %s" % (
                        p1.returncode, p2.returncode,
                        last_line(err1.read().decode("utf-8", "replace") + "\n"
                                  + "\n".join(errors)))
                return False, why, suspects
        finally:
            os.unlink(listfile)

    def fetch_rsync(self, src, rels, dest, nbytes):
        """One rsync --files-from pass. -> (ok, why, suspects). rsync writes each file under a
        temporary name and renames it only when whole, so it names no suspects."""
        deadline = nbytes / RSYNC_BPS + 300
        listfile = write_list(rels)
        try:
            rc, _, err = run(self.rsync_argv(src, listfile, dest), deadline)
        finally:
            os.unlink(listfile)
        if rc == 0:
            return True, "", []
        return False, ("stopped at the %ds deadline" % deadline if rc is None
                       else "rsync exit %d: %s" % (rc, last_line(err))), []


class LocalRemote(Remote):
    """For tests: the "remote" is a directory on this Mac, and the same command strings run
    through sh without ssh or sudo. Only the manifest is walked here instead, because the
    Mac's BSD find has no -printf; it produces the same '<path>\\t<size>' lines."""
    sudo = ""

    def __init__(self, root, places_cmd=None):
        Remote.__init__(self, "local", root,
                        places_cmd=places_cmd or "echo 'no places command given' >&2; exit 1")

    def where(self):
        return self.root

    def argv(self, cmd):
        return ["sh", "-c", cmd]

    def unreachable(self, rc):
        return False

    def manifest(self, src):
        top = self.src_dir(src)
        lines = []
        for d, _, files in os.walk(top):
            for f in files:
                if f.endswith(".png"):
                    p = os.path.join(d, f)
                    st = os.lstat(p)
                    if stat.S_ISREG(st.st_mode):
                        lines.append("%s\t%d" % (os.path.relpath(p, top), st.st_size))
        return "".join(l + "\n" for l in lines)

    def rsync_argv(self, src, listfile, dest):
        return ["rsync", "-rt", "-i", "--ignore-existing", "--min-size=1", "--timeout=60",
                "--files-from=" + listfile, self.src_dir(src) + "/", dest + "/"]


# ---------------------------------------------------------------- COVEY's streamed-only report

def parse_fields(rest):
    """'src=otm tiles=513 ... name=near Elk Camp' -> dict. name= is last and takes the rest
    of the line (a place name has spaces); every other field is one key=value token."""
    d = {}
    rest = rest.strip()
    while rest:
        if rest.startswith("name="):
            d["name"] = rest[5:].strip()
            break
        tok, _, rest = rest.partition(" ")
        k, eq, v = tok.partition("=")
        if eq:
            d[k] = v
        rest = rest.lstrip()
    return d


def parse_report(text):
    """COVEY's `tilestore --places` lines (ANCHORS / PLACE / OVERVIEW / DISK) -> a dict.
    Lines of any other kind are ignored, so the report can grow without breaking this."""
    rep = {"places": [], "overview": [], "disk": None, "anchors": None}
    for line in text.splitlines():
        kind, _, rest = line.strip().partition(" ")
        if kind == "PLACE":
            rep["places"].append(parse_fields(rest))
        elif kind == "OVERVIEW":
            rep["overview"].append(parse_fields(rest))
        elif kind == "DISK":
            rep["disk"] = parse_fields(rest)
        elif kind == "ANCHORS":
            rep["anchors"] = parse_fields(rest)
    return rep


def zoom_range(zmin, zmax):
    if zmin is None or zmax is None or zmin == zmax:
        return "z%s" % (zmax if zmax is not None else zmin)
    return "z%s-%s" % (zmin, zmax)


def lon2x(lon, z):
    return (lon + 180.0) / 360.0 * (1 << z)


def lat2y(lat, z):
    r = math.radians(lat)
    return (1.0 - math.log(math.tan(r) + 1.0 / math.cos(r)) / math.pi) / 2.0 * (1 << z)


def place_rect(p):
    """A PLACE's tile rectangle at its deepest level -> (z, x0, x1, y0, y1), end-exclusive, or
    None when the line has no extent (a COVEY from before half_x_km/half_y_km) or a bad one.

    COVEY's centre and half-extents come from the bounding box of the place's tiles at zmax:
    lat, lon = the middle of its tile EDGES in degrees, half_y_km = half its height x 111,
    half_x_km = half its width x 111 x max(0.2, cos lat) (tilestore._place_record). Inverting
    that gives the edges back, off only by the printed rounding (lat/lon to 1 m, half_* to 5 m):
    a few hundredths of a z17 tile. So each edge is ROUNDED to the nearest tile line - floor and
    ceil would let one rounding metre add a whole row of tiles nobody streamed."""
    try:
        z = int(p["zmax"])
        lat, lon = float(p["lat"]), float(p["lon"])
        hx, hy = float(p["half_x_km"]), float(p["half_y_km"])
    except (KeyError, TypeError, ValueError):
        return None
    if not (0 <= z <= 24 and -85.0 < lat < 85.0 and -180.0 <= lon <= 180.0
            and 0.0 <= hx < 1000.0 and 0.0 <= hy < 1000.0):
        return None
    dlat = hy / KM_PER_DEG
    dlon = hx / (KM_PER_DEG * max(0.2, math.cos(math.radians(lat))))
    n = 1 << z
    try:
        y0 = int(round(lat2y(min(lat + dlat, 85.05), z)))
        y1 = int(round(lat2y(max(lat - dlat, -85.05), z)))
    except (ValueError, ZeroDivisionError):
        return None
    x0 = int(round(lon2x(lon - dlon, z)))
    x1 = int(round(lon2x(lon + dlon, z)))
    x0, y0 = max(0, min(n - 1, x0)), max(0, min(n - 1, y0))
    x1, y1 = max(x0 + 1, min(n, x1)), max(y0 + 1, min(n, y1))
    return z, x0, x1, y0, y1


def place_cover(p, master):
    """-> (tiles the master has, tiles in the place's rectangle, zmax), or None when the line
    carries no extent, names no usable source, or no master was given. A tile counts only when
    <master>/<src>/<zmax>/<x>/<y>.png exists and is not empty (an empty file is not a tile)."""
    src = p.get("src", "")
    rect = place_rect(p)
    if not master or not rect or not source_ok(src):
        return None
    z, x0, x1, y0, y1 = rect
    total = (x1 - x0) * (y1 - y0)
    if total > COVER_MAX:
        return None
    have = 0
    for x in range(x0, x1):
        col = os.path.join(master, src, str(z), str(x))
        try:
            names = set(os.listdir(col))
        except OSError:
            continue
        for y in range(y0, y1):
            name = "%d.png" % y
            if name in names:
                try:
                    have += os.stat(os.path.join(col, name)).st_size > 0
                except OSError:
                    pass
    return have, total, z


def place_row(p, cover=None, where=True):
    name = p.get("name") or "%s, %s" % (p.get("lat", "?"), p.get("lon", "?"))
    part = p.get("part", "")
    if "/" in part:
        k, n = part.split("/", 1)
        name += " (part %s of %s)" % (k, n)
    extra = ""
    if cover:
        have, total, z = cover
        extra = (" (master has all %s z%d tiles there)" % (num(total), z) if have == total
                 else " (master has %s of %s z%d tiles there)" % (num(have), num(total), z))
    return "%s: %s: %s tiles %s, streamed %s%s%s" % (
        p.get("src", "?"), name, num(p.get("tiles")),
        zoom_range(p.get("zmin"), p.get("zmax")), p.get("last", "?"), extra,
        " → " + WHERE if where else "")


def report_rows(rep, master=None):
    """A parsed report -> the lines a person reads. tests/check_covey_pull.py pins it.

    With a master, each place's deepest-level rectangle is looked up in it: a place whose every
    tile there is already in the master is listed apart, under HEAD_HAVE - a phone's own
    download or the Mac's fetch pooled that ground, and re-downloading it on COVEY would cost
    hours of OTM's throttle for tiles the phone push already delivers. The rest stay under
    HEAD, with how much of each the master has."""
    rows, need, have = [], [], []
    for p in rep["places"]:
        cover = place_cover(p, master)
        if cover and cover[0] == cover[1]:
            have.append("  " + place_row(p, cover, where=False))
        else:
            need.append("  " + place_row(p, cover))
    for o in rep["overview"]:
        need.append("  %s: also %s overview tiles from zoomed-out panning (not a place)"
                    % (o.get("src", "?"), num(o.get("tiles"))))
    if need:
        rows += [HEAD] + need
    if have:
        rows += [HEAD_HAVE] + have
    if not rows:
        rows.append("Nothing on COVEY is streamed-only: every tile it holds is a downloaded one.")
    d = rep["disk"]
    if d:
        try:
            free = gb(int(d.get("free_bytes", "")))
        except ValueError:
            free = "? GB"
        rows.append("COVEY's card: %s %% used, %s free; streamed tiles expire past %s %%"
                    % (d.get("used_pct", "?"), free, d.get("expire_at_pct", "?")))
        if d.get("keeping") == "no":
            rows.append("⚠ COVEY's card is past %s %% full: what it streams now is shown there "
                        "but NOT kept" % d.get("expire_at_pct", "?"))
    return rows


def streamed_counts(rep):
    """{src: streamed-only tiles} over places and overview."""
    out = {}
    for p in rep["places"] + rep["overview"]:
        try:
            out[p.get("src", "?")] = out.get(p.get("src", "?"), 0) + int(p.get("tiles", 0))
        except ValueError:
            pass
    return out


def report_problem(out, err):
    """Why a report failed, in COVEY's own words. tilestore prints its refusals and errors on
    STDOUT (`REFUSE reason=roots-overlap ...`, `ERROR <traceback>`), after its ANCHORS line;
    stderr holds ssh's, sudo's and Python's own complaints."""
    for line in (out or "").splitlines():
        line = line.strip()
        if line.startswith(("REFUSE", "ERROR")):
            return line if len(line) <= 300 else line[:40] + " … " + line[-240:]
    return last_line(err) if (err or "").strip() else last_line(out)


def fetch_report(remote, timeout=REPORT_TIMEOUT_S):
    """-> ('ok', parsed) | ('unreachable', why) | ('failed', why)."""
    rc, out, err = remote.places(timeout)
    if rc is None:
        return "failed", "no answer in %ds" % timeout
    if remote.unreachable(rc):
        return "unreachable", last_line(err)
    if rc != 0:
        return "failed", report_problem(out, err)
    rep = parse_report(out)
    if rep["disk"] is None:
        return "failed", "its report had no DISK line: %s" % last_line(out)
    return "ok", rep


# ---------------------------------------------------------------- the pull

def parse_manifest(text):
    """'<z>/<x>/<y>.png\\t<size>' lines -> ({rel: size}, ignored). Anything that is not
    z/x/y.png (an AppleDouble ._123.png, a stray file) is counted and left alone."""
    sizes, ignored = {}, 0
    for line in text.splitlines():
        rel, tab, size = line.rpartition("\t")
        if not tab or not TILE_RE.match(rel) or not size.isdigit():
            ignored += 1 if line.strip() else 0
            continue
        sizes[rel] = int(size)
    return sizes, ignored


def local_tiles(top):
    """-> ({rel: size} of every z/x/y.png under top, [rel] of .part and dot files)."""
    tiles, junk = {}, []
    if not os.path.isdir(top):
        return tiles, junk
    for d, dirs, files in os.walk(top):
        for f in files:
            p = os.path.join(d, f)
            rel = os.path.relpath(p, top).replace(os.sep, "/")
            if f.startswith(".") or f.endswith(".part"):
                junk.append(rel)
            elif TILE_RE.match(rel):
                try:
                    tiles[rel] = os.lstat(p).st_size
                except OSError:
                    pass
    return tiles, junk


def drop_staged(stage, path):
    """Delete one file from STAGING - the only thing this tool ever deletes."""
    rs = os.path.realpath(stage)
    rp = os.path.join(os.path.realpath(os.path.dirname(path)), os.path.basename(path))
    if rp == rs or os.path.commonpath([rp, rs]) != rs:
        raise PullError("refusing to delete %s: it is not in the staging folder %s" % (path, stage))
    os.unlink(path)


def ends_whole(path):
    """Does the file start like a PNG and end with its IEND chunk, or start like a JPEG and end
    with FFD9? A tile cut mid-stream and zero-padded by bsdtar to its full size fails this
    whatever its size (its tail is zeros), and so does a file that is neither. Every one of the
    136,584 tiles in ~/tiles-master passed it on 2026-09-23 (PNG and JPEG-under-.png alike)."""
    try:
        with open(path, "rb") as f:
            head = f.read(8)
            f.seek(-len(PNG_END), os.SEEK_END)      # OSError when shorter than that
            tail = f.read(len(PNG_END))
    except OSError:
        return False
    if head == PNG_MAGIC:
        return tail == PNG_END
    if head.startswith(JPEG_MAGIC):
        return tail.endswith(JPEG_END)
    return False


def staged_good(stage, rel, sizes):
    """Is the staged copy of rel a whole tile: a regular file of COVEY's size, more than 0,
    that ENDS like a tile? -> None when it is not there at all.

    ⚠ Size alone is not enough: a tile cut off mid-stream is exactly COVEY's size (see
    Remote.fetch_tar), with zeros where the rest of it should be."""
    p = os.path.join(stage, rel)
    try:
        st = os.lstat(p)
    except OSError:
        return None                      # not there
    return (stat.S_ISREG(st.st_mode) and st.st_size > 0 and st.st_size == sizes.get(rel, -1)
            and ends_whole(p))


def clear_staging(stage, dry_run):
    """Drop everything an earlier, interrupted pull left in staging. -> number dropped.

    ⚠ NOT re-checked and kept: the pull that left it was stopped, and the tile its tar was
    writing when it stopped sits there at full size, padded with zeros. Nothing about a
    leftover proves it whole, and dropping one costs a single re-fetch."""
    dropped = 0
    if not os.path.isdir(stage):
        return dropped
    for d, _, files in os.walk(stage):
        for f in files:
            dropped += 1
            if not dry_run:
                drop_staged(stage, os.path.join(d, f))
    return dropped


def verify(stage, rels, sizes, suspects=()):
    """After a transfer: -> (rels that landed whole, rels dropped, rels dropped that were
    COVEY's size but did not end like a tile although no stopped stream explains it). A short,
    cut or suspect copy is deleted from staging so the next pass fetches it again; a suspect
    (the tile a stopped tar stream was writing) goes whatever it looks like. Only <rels> are
    looked at: a suspect outside them is ignored."""
    landed, dropped, unended = set(), [], []
    suspects = set(suspects)
    for rel in rels:
        good = staged_good(stage, rel, sizes)
        if good and rel not in suspects:
            landed.add(rel)
        elif good is not None:
            p = os.path.join(stage, rel)
            if not good and rel not in suspects and os.lstat(p).st_size == sizes.get(rel, -1):
                unended.append(rel)
            drop_staged(stage, p)
            dropped.append(rel)
    return landed, dropped, unended


def transfer(remote, src, rels, stage, sizes, bulk_min):
    """Fetch <rels> (one zoom) into staging until all are there or the passes run out.
    -> (set of rels staged good, [rels still missing whose last arrival was COVEY's size but
    did not end like a tile, with no stopped stream to explain it: COVEY's own copy may be
    damaged, and re-running will not help])."""
    got, unended = set(), set()
    pending = list(rels)
    method = "tar" if len(pending) >= bulk_min else "rsync"
    for _ in range((1 if method == "tar" else 0) + RSYNC_PASSES):
        if not pending:
            break
        z = zoom_of(pending[0])
        t0 = time.time()
        nbytes = sum(sizes[r] for r in pending)
        fetch = remote.fetch_tar if method == "tar" else remote.fetch_rsync
        ok, why, suspects = fetch(src, pending, stage, nbytes)
        landed, dropped, cut = verify(stage, pending, sizes, suspects)
        got |= landed
        unended = (unended - set(dropped) - landed) | set(cut)
        pending = [r for r in pending if r not in landed]
        note("    %s z%d: %s asked by %s, %s landed whole%s%s (%ds)" % (
            src, z, num(len(landed) + len(pending)), method, num(len(landed)),
            ", %d arrived short or cut off (asked again)" % len(dropped) if dropped else "",
            "" if ok else " [%s]" % why, time.time() - t0))
        # Gap-fill is rsync. If an rsync pass brings nothing at all, the next pass is a tar
        # stream instead: the Mac's openrsync --files-from was proven against a LOCAL sender
        # only (2026-09-23), never against COVEY's rsync, and tar -T is the other road in.
        method = "tar" if (method == "rsync" and not landed) else "rsync"
    return got, sorted(unended & set(pending))


def promote(stage, master_src, rels):
    """Staged -> master with os.link: EEXIST means the master already holds that name, and
    the master's copy wins. -> (added rels, kept rels)."""
    added, kept = [], []
    for rel in sorted(rels, key=lambda r: (zoom_of(r), r)):
        sp = os.path.join(stage, rel)
        mp = os.path.join(master_src, rel)
        os.makedirs(os.path.dirname(mp), exist_ok=True)
        try:
            os.link(sp, mp)
            added.append(rel)
        except FileExistsError:
            kept.append(rel)
        drop_staged(stage, sp)
    return added, kept


def tidy(top):
    """Remove the empty folders staging leaves behind; never a file."""
    if not os.path.isdir(top):
        return
    for d, _, _ in sorted(os.walk(top), key=lambda w: -len(w[0])):
        try:
            os.rmdir(d)
        except OSError:
            pass


def pull_source(remote, src, master, work, bulk_min, dry_run):
    """-> dict(ok, added=[rels], zero=[rels])."""
    t0 = time.time()
    text = remote.manifest(src)
    with open(os.path.join(work, src + ".remote.tsv"), "w") as f:
        f.write(text)
    sizes, ignored = parse_manifest(text)
    zero = sorted(r for r, n in sizes.items() if n == 0)
    nonempty = {r for r, n in sizes.items() if n > 0}
    master_src = os.path.join(master, src)
    stage = os.path.join(master, ".incoming", src)
    have, _ = local_tiles(master_src)
    dropped = clear_staging(stage, dry_run)
    differ = sorted(r for r in nonempty if have.get(r, 0) > 0 and have[r] != sizes[r])
    # An EMPTY master file under a good COVEY tile is not fetched: os.link cannot replace it,
    # so the copy would only be dropped again. The pass test below fails on it instead.
    todo = sorted((r for r in nonempty if r not in have), key=lambda r: (zoom_of(r), r))
    say("  %s: COVEY has %s downloaded (%d zero-byte, not pulled)%s; %s to fetch (%s)" % (
        src, num(len(nonempty)), len(zero),
        "; %s other file(s), not z/x/y.png, left alone" % num(ignored) if ignored else "",
        num(len(todo)), mb(sum(sizes[r] for r in todo))))
    if differ:
        say("  %s: %d differ in size from COVEY's copy; the master's copies are kept (e.g. %s)"
            % (src, len(differ), ", ".join(differ[:3])))
    if dropped:
        say("  %s: %d staged cop%s left by an earlier, interrupted pull %s dropped unread "
            "(fetched again)" % (src, dropped, "y" if dropped == 1 else "ies",
                                 "would be" if dry_run else "were"))
    if dry_run:
        return {"ok": True, "added": [], "zero": zero, "todo": len(todo)}

    added, kept, unended = [], [], []
    for z in sorted({zoom_of(r) for r in todo}):
        want = [r for r in todo if zoom_of(r) == z]
        os.makedirs(stage, exist_ok=True)
        got, cut = transfer(remote, src, want, stage, sizes, bulk_min)
        unended += cut
        a, k = promote(stage, master_src, got)
        added += a
        kept += k
    # Anything still in staging is not a whole tile of this manifest (an rsync temp name, a
    # short copy the last pass left): staging ends empty.
    clear_staging(stage, dry_run=False)
    tidy(stage)

    after, junk = local_tiles(master_src)
    missing = sorted(r for r in nonempty if after.get(r, 0) == 0)
    shadowed = [r for r in missing if r in after]          # an empty master file stands there
    absent = [r for r in missing if r not in after]
    empty = sorted(r for r, n in after.items() if n == 0)
    per_z = {}
    for r in added:
        per_z[zoom_of(r)] = per_z.get(zoom_of(r), 0) + 1
    say("  %s: master %s -> %s (+%s%s) in %ds" % (
        src, num(len(have)), num(len(after)), num(len(added)),
        (": " + ", ".join("z%d %s" % (z, num(n)) for z, n in sorted(per_z.items())))
        if per_z else "", time.time() - t0))
    if kept:
        say("  %s: %d were already in the master by the time they went in; the master's "
            "copies were kept" % (src, len(kept)))
    say("  %s: %d on COVEY missing from the master; %d zero-byte in the master"
        % (src, len(missing), len(empty)))
    if absent:
        say("  ⚠ %s: NOT COMPLETE - re-run to fetch the rest (e.g. %s)"
            % (src, ", ".join(absent[:5])))
    if unended:
        say("  ⚠ %s: %d arrived at COVEY's size but do not end like a PNG or JPEG, and no stopped "
            "stream explains it - COVEY's own cop%s may be damaged: look there (e.g. %s/%s)"
            % (src, len(unended), "y" if len(unended) == 1 else "ies", src, unended[0]))
    if shadowed:
        say("  ⚠ %s: NOT COMPLETE - %d EMPTY file%s in the master stand%s where COVEY has a good "
            "tile, and a pull never overwrites a master file: delete %s and re-run (%s)"
            % (src, len(shadowed), "" if len(shadowed) == 1 else "s",
               "s" if len(shadowed) == 1 else "", "it" if len(shadowed) == 1 else "them",
               ", ".join("%s/%s/%s" % (master, src, r) for r in shadowed[:5])
               + (" and %d more" % (len(shadowed) - 5) if len(shadowed) > 5 else "")))
    other_empty = [r for r in empty if r not in nonempty]
    if other_empty:
        say("  ⚠ %s: %d empty file%s in the master that COVEY has no tile for (not tiles; the "
            "phones get nothing there) (e.g. %s/%s)"
            % (src, len(other_empty), "" if len(other_empty) == 1 else "s", src, other_empty[0]))
    if junk:
        say("  ⚠ %s: %d .part or dot files in the master (e.g. %s/%s)"
            % (src, len(junk), src, junk[0]))
    return {"ok": not missing, "added": added, "zero": zero, "todo": 0}


def print_rows(rows):
    """The report as pull() logs it: the headings timestamped, the rest under them."""
    for i, row in enumerate(rows):
        if i == 0 or row in (HEAD, HEAD_HAVE):
            say(row)
        else:
            note("  " + row)


def pull(remote, master, bulk_min=BULK_MIN, dry_run=False, without_report=False):
    """-> exit code: 0 every source passed, 1 something is missing, 2 could not start (COVEY
    not reachable, no streamed-only report and no without_report, another pull running)."""
    master = os.path.abspath(os.path.expanduser(master))
    if not os.path.isdir(master):
        say("no master at %s" % master)
        return 2
    work = os.path.join(master, ".covey_pull")
    os.makedirs(work, exist_ok=True)
    lock = open(os.path.join(work, "lock"), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        say("another covey pull is already running on %s - not starting a second" % master)
        return 2

    # 1. What will NOT be pooled, before anything moves: the time to act on it is now.
    status, rep = fetch_report(remote)
    counts = None
    if status == "ok":
        print_rows(report_rows(rep, master))
        counts = streamed_counts(rep)
    elif status == "unreachable":
        say("COVEY is not reachable: %s" % rep)
        return 2
    else:
        # ⚠ REFUSE. The COVEY that cannot give this report is, above all, the one whose covey-ui
        # predates the streamed tree - and there the streamed tiles are still IN the downloaded
        # tree, so a pull would put them on both phones as if chosen, and the master never
        # forgets (nothing deletes from it).
        say("⚠ COVEY gave no streamed-only report: %s" % rep)
        note("  Without it this pull cannot tell COVEY's streamed tiles from its downloaded ones:")
        note("  until COVEY's streamed/downloaded cut-over is done, its streamed tiles sit IN")
        note("  %s/<src> and would reach both phones as if chosen." % remote.root)
        if without_report:
            note("  --without-report: carrying on, on your word that the cut-over is done.")
        elif dry_run:
            note("  Dry run: counting anyway. A real pull REFUSES here unless --without-report.")
        else:
            note("  Do the cut-over on COVEY first (or fix what the line above says). If you KNOW")
            note("  it is done and only the report is failing:  %s" % WITHOUT)
            say("=== PULL covey REFUSED: no streamed-only report from COVEY; nothing moved ===")
            return 2

    # 2. The sources.
    try:
        names = remote.list_sources()
    except PullError as e:
        say(str(e))
        return 2
    srcs = [n for n in names if source_ok(n)]
    left = sorted((n for n in names if not source_ok(n)),
                  key=lambda n: (not n.isdigit(), int(n) if n.isdigit() else 0, n))
    say("sources in %s: %s%s" % (remote.where(), ", ".join(srcs) or "none",
                                 "  (left alone: %s)" % ", ".join(left) if left else ""))

    ok, added, zero, would = True, [], [], 0
    for src in srcs:
        try:
            r = pull_source(remote, src, master, work, bulk_min, dry_run)
        except (PullError, OSError) as e:
            say("  ⚠ %s: %s" % (src, e))
            ok = False
            continue
        ok = ok and r["ok"]
        added += [src + "/" + a for a in r["added"]]
        zero += [src + "/" + z for z in r["zero"]]
        would += r["todo"]
    tidy(os.path.join(master, ".incoming"))

    if not dry_run:
        with open(os.path.join(master, "covey_zero_bytes.txt"), "w") as f:
            f.write("".join(z + "\n" for z in zero))
        if added:
            with open(os.path.join(master, "covey_pulled_%s.txt" % time.strftime("%Y-%m-%d")),
                      "a") as f:
                f.write("".join(a + "\n" for a in added))
    if counts is None:
        so = "streamed-only on COVEY: unknown, no report"
    else:
        so = "streamed-only on COVEY, not pulled: " + (
            " / ".join("%s %s" % (s, num(n)) for s, n in sorted(counts.items())) or "none")
    say("=== PULL covey %s (%s)%s ===" % (
        "dry run: would bring %s tiles into the master" % num(would) if dry_run
        else "done: +%s tiles into the master" % num(len(added)), so,
        "" if ok else " - NOT COMPLETE, see the ⚠ lines"))
    return 0 if ok else 1


def report(remote, master=None):
    """`cardday.sh status`'s COVEY block. Best effort: never fails. With a master, places whose
    ground it already holds are listed apart (report_rows)."""
    status, rep = fetch_report(remote)
    if status == "unreachable":
        note("COVEY (%s): not reachable - %s" % (remote.host, rep))
        return 0
    if status == "failed":
        note("COVEY (%s): reachable, but gave no streamed-only report: %s" % (remote.host, rep))
        note("  `cardday.sh covey pull` REFUSES until it does (COVEY's streamed/downloaded "
             "cut-over first)")
        return 0
    note("COVEY (%s):" % remote.host)
    for row in report_rows(rep, master):
        note("  " + row)
    return 0


def main(argv=None):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="COVEY's downloaded tiles into the tile pool.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("what", choices=("pull", "report"))
    ap.add_argument("--master", default=os.environ.get("CARDDAY_MASTER", "~/tiles-master"))
    ap.add_argument("--host", default=os.environ.get("CARDDAY_COVEY_HOST", "covey"))
    ap.add_argument("--remote-root", default=REMOTE_ROOT,
                    help="COVEY's DOWNLOADED tile root (default %s)" % REMOTE_ROOT)
    ap.add_argument("--dry-run", action="store_true", help="manifests and counts; nothing moves")
    ap.add_argument("--without-report", action="store_true",
                    help="pull even when COVEY gives no streamed-only report: ONLY once its "
                         "streamed/downloaded cut-over is known to be done (cardday.sh never "
                         "passes this)")
    ap.add_argument("--bulk-min", type=int, default=BULK_MIN, help=argparse.SUPPRESS)
    ap.add_argument("--local", action="store_true", help=argparse.SUPPRESS)       # tests
    ap.add_argument("--places-cmd", default=None, help=argparse.SUPPRESS)          # tests
    a = ap.parse_args(argv)
    if a.local:
        remote = LocalRemote(a.remote_root, places_cmd=a.places_cmd)
    else:
        remote = Remote(a.host, a.remote_root, connect_timeout=5 if a.what == "report" else 10,
                        places_cmd=a.places_cmd)
    if a.what == "report":
        master = os.path.abspath(os.path.expanduser(a.master))
        return report(remote, master if os.path.isdir(master) else None)
    return pull(remote, a.master, bulk_min=a.bulk_min, dry_run=a.dry_run,
                without_report=a.without_report)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""nmea_phone_stub.py - a stand-in for a phone's NMEA app, for the WiPhone's Phone location bench.

    python3 tools/nmea_phone_stub.py                    # :10110, RMC+GGA at 1 Hz, 9 sats, HDOP 0.9
    python3 tools/nmea_phone_stub.py --port 11123 --ios # GPS 2 IP's port, GGA with no sats/HDOP
    python3 tools/nmea_phone_stub.py --sats 3           # a 2D fix: kept, drawn grey, never sent
    python3 tools/nmea_phone_stub.py --burst 30         # gpsdRelay's stale queue on every connect

Point a phone at it with `phonepos at <this Mac's LAN IP>` on the serial console (RAM only; the
phone otherwise asks its network's GATEWAY, which a Mac on the home network is not). Then
`phonepos` and `pos` on the phone say what it made of the stream. Fixes from a `phonepos at`
address are BENCH fixes on the phone: drawn on its map, never sent by the beacon's schedule,
and `pos now` with one is logged INJECTED and refused on a public channel.

WHAT IT SENDS: checksummed $..RMC and $..GGA every --rate seconds, stamped with this Mac's UTC
clock (the phone drops a sentence more than 5 s from a trusted clock - keep the Mac on NTP), at a
DUMMY position (40.0, -100.0 by default: a field in Kansas; never put a real one in a capture).

  --sats N / --hdop X   GGA's satellites and HDOP (3 or 12.0 = a poor fix the phone refuses to send)
  --quality Q           GGA's fix quality (0 none, 1 GPS, 6 dead reckoning: refused as "estimate")
  --status V            RMC says V (no fix) and GGA quality 0 with no position
  --burst N             on each connect, N lines from 60 s ago first (gpsdRelay keeps one 30-line
                        queue for every client; the phone's 2 s settle and time check drop them)
  --no-crlf             no line endings at all (the phone feeds a '\\n' before every '$')
  --stall S             go silent for S seconds, 5 s into each connection (the phone gives up a
                        silent link after 10 s and reconnects)
  --warm S              "no fix yet" (RMC V, GGA quality 0) for the first S seconds of each
                        connection, then the fix: NH Companion turning the phone's GPS on when a
                        device connects. The X4's Locate Me waits up to 30 s for this (8 s before)
  --talker GP|GN        the talker id of the real sentences
  --generated           gpsdRelay's GENERATED sentences (its mock-location path): $GNGGA with 12
                        satellites always and the ACCURACY IN METRES where HDOP goes (--acc), and
                        coordinates through its ddToDDMM - Kotlin's Double.toString, so minutes
                        under 0.001 come out in E notation and are then CUT to 6 characters:
                        "6.0E-5" -> "06.0E-500", which the phone's strict reader refuses; but
                        "5.999999998E-5" -> "05.999999", a coordinate ~11 km off that parses
                        (try --lat 40.000001). The phone takes a phone fix only when it agrees
                        with the one before it (self_pos.h, selfPhoneAgrees), which catches a
                        moving or noisy run of those; a stationary one stays a known hazard of
                        generated sentences - tell people to relay the receiver's own NMEA.
  --acc M               --generated's accuracy in metres (default 3.9)
  --ios                 GPS 2 IP's shape: GGA with EMPTY satellites and HDOP; the phone takes it
                        only because an 'A' RMC of the same second vouches for it

Standard library only. The Mac's own address is printed at start so `phonepos at` can be typed;
nothing else about the network is.
"""
import argparse
import decimal
import socket
import threading
import time


def checksum(body):
    c = 0
    for ch in body:
        c ^= ord(ch)
    return "%02X" % c


def sentence(body, crlf):
    return "$%s*%s%s" % (body, checksum(body), "\r\n" if crlf else "")


def ddmm(deg, is_lat):
    """Plain NMEA ddmm.mmmmm / dddmm.mmmmm and the hemisphere letter."""
    hemi = ("N" if deg >= 0 else "S") if is_lat else ("E" if deg >= 0 else "W")
    a = abs(deg)
    d = int(a)
    m = (a - d) * 60.0
    width = 2 if is_lat else 3
    return "%0*d%08.5f" % (width, d, m), hemi


def kotlin_double_str(x):
    """Kotlin/Java Double.toString: decimal for 1e-3 <= |x| < 1e7, else d.dddE-n."""
    if x == 0.0:
        return "0.0"
    if 1e-3 <= abs(x) < 1e7:
        s = repr(x)
        if "e" in s or "E" in s:
            s = format(decimal.Decimal(s), "f")
        return s if "." in s else s + ".0"
    sign, digits, exp = decimal.Decimal(repr(x)).as_tuple()
    alld = "".join(str(d) for d in digits)
    e = exp + len(alld) - 1                      # from ALL the digits, before the zeros go
    digits = alld.rstrip("0") or "0"
    mant = digits[0] + "." + (digits[1:] or "0")
    return ("-" if sign else "") + mant + "E" + str(e)


def gpsdrelay_ddmm(deg, is_lat):
    """gpsdRelay's ddToDDMM, ported: minutes.toString().split('.'), padded and cut to 6."""
    degrees = int(deg)
    minutes = abs((deg - degrees) * 60)
    parts = kotlin_double_str(minutes).split(".")
    ip = parts[0].rjust(2, "0")
    fp = (parts[1] if len(parts) > 1 else "").ljust(6, "0")[:6]
    if is_lat:
        hemi = "N" if deg > 0 else "S"
    else:
        hemi = "E" if deg > 0 else "W"
    return str(abs(degrees)).rjust(2 if is_lat else 3, "0") + ip + "." + fp, hemi


def epoch_lines(a, t, crlf):
    """The sentences for one epoch at UTC time t."""
    g = time.gmtime(t)
    hms = time.strftime("%H%M%S", g) + ".00"
    dmy = time.strftime("%d%m%y", g)
    out = []
    if a.generated:
        la, ns = gpsdrelay_ddmm(a.lat, True)
        lo, ew = gpsdrelay_ddmm(a.lon, False)
        out.append(sentence("GNGGA,%s,%s,%s,%s,%s,1,12,%.1f,%.1f,M,0,M,," %
                            (hms, la, ns, lo, ew, a.acc, a.alt), crlf))
        out.append(sentence("GPRMC,%s,A,%s,%s,%s,%s,0.0,0.0,%s,,,A,V" %
                            (hms, la, ns, lo, ew, dmy), crlf))
        return out
    la, ns = ddmm(a.lat, True)
    lo, ew = ddmm(a.lon, False)
    tk = a.talker
    if a.status == "V":
        out.append(sentence("%sRMC,%s,V,,,,,,,%s,,,N" % (tk, hms, dmy), crlf))
        out.append(sentence("%sGGA,%s,,,,,0,%02d,,,M,,M,," % (tk, hms, a.sats), crlf))
        return out
    out.append(sentence("%sRMC,%s,A,%s,%s,%s,%s,0.02,,%s,,,A" % (tk, hms, la, ns, lo, ew, dmy), crlf))
    if a.ios:
        sats, hdop = "", ""
    else:
        sats, hdop = "%02d" % a.sats, "%.1f" % a.hdop
    out.append(sentence("%sGGA,%s,%s,%s,%s,%s,%d,%s,%s,%.1f,M,-21.3,M,," %
                        (tk, hms, la, ns, lo, ew, a.quality, sats, hdop, a.alt), crlf))
    return out


def stamp():
    return time.strftime("%H:%M:%S")


def serve(conn, peer, a):
    crlf = not a.no_crlf
    sent = 0
    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if a.burst:
            # gpsdRelay's queue: lines that were waiting before this client connected.
            old = time.time() - 60
            lines = []
            while len(lines) < a.burst:
                lines += epoch_lines(a, old, crlf)
                old += 1
            conn.sendall("".join(lines[:a.burst]).encode())
            sent += a.burst
        t0 = time.time()
        stalled = False
        cold = argparse.Namespace(**dict(vars(a), status="V")) if a.warm else None
        while True:
            if a.stall and not stalled and time.time() - t0 >= 5:
                print("%s  %s: stalling %.0f s" % (stamp(), peer, a.stall), flush=True)
                time.sleep(a.stall)
                stalled = True
            now = time.time()
            for ln in epoch_lines(cold if cold and now - t0 < a.warm else a, now, crlf):
                conn.sendall(ln.encode())
                sent += 1
            time.sleep(a.rate)
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass
    finally:
        conn.close()
        print("%s  %s: gone after %d sentences" % (stamp(), peer, sent), flush=True)


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))      # TEST-NET-1: nothing is sent, it only picks a route
        return s.getsockname()[0]
    except OSError:
        return "?"
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=10110)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--lat", type=float, default=40.0)
    ap.add_argument("--lon", type=float, default=-100.0)
    ap.add_argument("--alt", type=float, default=500.0)
    ap.add_argument("--sats", type=int, default=9)
    ap.add_argument("--hdop", type=float, default=0.9)
    ap.add_argument("--quality", type=int, default=1)
    ap.add_argument("--status", choices=["A", "V"], default="A")
    ap.add_argument("--burst", type=int, default=0)
    ap.add_argument("--no-crlf", action="store_true")
    ap.add_argument("--stall", type=float, default=0.0)
    ap.add_argument("--warm", type=float, default=0.0)
    ap.add_argument("--talker", choices=["GP", "GN"], default="GP")
    ap.add_argument("--rate", type=float, default=1.0)
    ap.add_argument("--generated", action="store_true")
    ap.add_argument("--acc", type=float, default=3.9)
    ap.add_argument("--ios", action="store_true")
    a = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.bind, a.port))
    srv.listen(4)
    print("NMEA stub on %s:%d - on the phone: phonepos at %s%s" %
          (a.bind, a.port, lan_ip(), "" if a.port in (10110, 11123) else ":%d" % a.port), flush=True)
    for ln in epoch_lines(a, time.time(), True):
        print("  sample: " + ln.strip(), flush=True)
    while True:
        conn, (host, port) = srv.accept()
        peer = "%s:%d" % (host, port)
        print("%s  %s: connected" % (stamp(), peer), flush=True)
        threading.Thread(target=serve, args=(conn, peer, a), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass

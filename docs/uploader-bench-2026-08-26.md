# Uploader instability — overnight bench notes, 2026-08-26 night

{user}'s ask (verbatim intent): the WiFi uploaders are "unstable and unusable (ram leaking
and pages not wanting to load on browsers)"; goal is to upload files to the phone
WITHOUT opening the case, "somehow and conveniently, it doesn't have to be this method",
without panic boots / lost connections / failed page loads. Fix OR alternative.

## Reproduced in minutes, on phone 1 (0.9.27, up 3.3 h, WiFi 192.168.1.57, SIP registered)

All via the panicwatch bridge (NO reset on port open — panicwatch does not toggle DTR;
uptime survived attach). Server started headless with `up on books`.

1. **Starting the server alone costs largest 14792 → 12688 (−2.1 KB).**
2. **Ten rapid page GETs (curl, 829-byte page): 2 succeeded, 8 connection-refused.**
   The breaker had PAUSED (freed) the server: `XFER: PAUSED (largest 3180)`.
   → TWO requests drove largest 12688 → 3180. Per-request transient ≈ 4–5 KB plus
   TCP teardown debris, on a phone that idles at largest ≈ 12–15 K with SIP up.
3. **Even PACED requests (one per 8 s) each dip largest to ~3–4 K and trip the
   breaker**, whose backoff escalates 90 s → 360 s → 600 s. A request landing inside a
   pause is refused. A browser fires 3–6 requests per visit (page, favicon, XHRs) —
   so nearly EVERY visit trips it. This IS "pages not wanting to load".
4. **heap min-ever hit 264 B** during ten curl page loads. No crash (dropped-frame
   design), but that is the margin the whole stack lives on.
5. **`blocks used` climbed 451 → 498 over ~14 requests** — per-connection debris
   (TIME_WAIT pcbs, 120 s tail) exactly as the 08-21 hardware verdict measured. Not a
   true leak (recovers), but on this heap it is indistinguishable from one in use.

## Conclusion (matches the 08-21 verdict; now confirmed at the PAGE level too)

The WebServer's PER-REQUEST cost is the killer — not the chunk protocol, not payload
size (829-byte page!). Pacing cannot save it. The agreed direction stands and extends:
the raw single-connection server must carry the PAGE too, not just /chunk, or the
browser trips the breaker before the first piece flies.

## Plan being executed tonight

1. `xfer_raw`: hand-rolled minimal HTTP/1.1 KEEP-ALIVE server, ONE client at a time,
   on :80 — serves the page, /chunk (stop-and-wait pieces into the PSRAM block),
   /fetch (the proven full-speed pull), /log. No String per request, no per-piece
   connections (keep-alive kills TIME_WAIT churn by construction), non-blocking state
   machine pumped from the main loop, 2 s dead-client deadline, piece cap 32 K.
   Legacy WebServer path stays reachable (`up on legacy`) for native-form/no-JS.
2. Page JS rewritten on the same wire protocol + the five OPEN 08-21 review findings
   fixed (batch-abandon, 4xx retried 24x, fetch-less dead page, SD-fail loop counter,
   zero-byte accounting).
3. `tools/wiphone_send.py` — Mac CLI: one command, speaks the same keep-alive chunk
   protocol; the convenient no-browser path.
4. Acceptance per `docs/upload-redesign-brief.md`: 3 consecutive 4-book batches, zero
   breaker trips, then a real browser run.

Raw serial evidence: /tmp/wiphone-serial.log (Mac-side, this session).

---

## RESULTS (written 2026-08-27 morning; plan above executed with one big change)

The plan's step 1 assumed the raw endpoint had to be BUILT — it already existed (4ff2a18,
:8081, acceptance-passed 08-21). What was actually broken was everything AROUND it: the
page, favicon, and probe still rode the WebServer, whose ~10 KB/connection transient
tripped the breaker on nearly every visit. So 0.9.28 moved the front door instead:

| step | result |
|---|---|
| page hammer, 20 rapid GETs | 19-20/20 answered (baseline 2/10); the stragglers are the single-slot takeover race under curl rapid-fire |
| 3× 4-book batches (~52 MB) | 12/12 byte-verified, 48-69 KB/s, ZERO breaker events, zero panics, no wedge |
| resumed tails | 156-249 KB/s effective |
| SD | three 507s across 12 files, all recovered by retry — watch this card |
| post-batch tail | largest pinned 3-5 K for minutes; SIP could not re-register until heap recovered; one DEAF-RADIO episode (scan: 0 networks, AP provably on air) cured only by reboot |
| real browser | STILL OWED (sandbox refused LAN HTTP; Chrome extension signed out; {user} opened the page 08-27 morning) |

Breaker as shipped: instant trip < 3072 (PHY guard), 1.5 s sustained for the 6144 line,
raw traffic inhibits, resume recovery-only at ≥ 10240, mDNS begins once and never ends.

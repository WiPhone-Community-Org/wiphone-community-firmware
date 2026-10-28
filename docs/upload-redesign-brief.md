# Upload server redesign — cold-start brief

**Written 2026-08-20 evening, at the end of the day the old design's true failure mode was
finally measured. {user} has approved the token spend for a proper multi-agent redesign.
Read this + the "upload saga" section of `docs/HANDOFF.md` and you need nothing else.**

## The measured problem (do not re-derive; it cost a day)

The phone runs its WiFi stack on **~25 KB of free internal RAM** (SIP + mesh + GUI own the
rest; PSRAM is plentiful but WiFi RX buffers are DMA = internal-only). On a **fast link**
(home LAN 3 ms RTT, or phone-to-phone hotspot) a push-mode HTTP upload lets TCP open wide
and the radio driver floods internal heap with RX buffers *below the application*:
**measured heap 14 KB → 868 bytes within seconds** of one 5 MB multipart POST. The slow
work-hotspot uplink was accidental flow control — that is the ONLY reason uploads ever
"worked". Symptoms users see: page loads once per boot, uploads retry-till-fail, listener
death, mute stack (no ping), and at ~3 KB largest, the historic `operator new` → abort.

**Pull mode already wins**: `/fetch` (paste-a-link → `downloadTo`) had the phone read at
its own pace — **3.9 MB in 8.5 s (~460 KB/s), heap intact**. The radio is excellent; the
architecture of PUSH is the whole problem.

## What is already in place (flashed + committed, `0c063e7`..`0c169d0`)

- STA-mode transfer server no longer blocks WiFi reconnect/auto-switch (softAP-only gates).
- Low-heap circuit breaker v4: pause frees the server outright (delete + MDNS.end), resume
  on heap recovery OR timer w/ backoff; mid-upload it defers to backpressure (trips < 3 KB).
- Backpressure in `handleUpload`: heap tight ⇒ stop consuming ⇒ TCP window closes.
  Push now *survives* fast links but **crawls** (1.5 KB/s observed). Not a solution — a net.
- Per-upload heap telemetry (one log_e per file/abort); serial `up on books`.
- Known wart: v4 timer-resume into sub-threshold heap serves ~3 s slivers (flaps); a pause
  landing right after `handleFetch` can eat the HTTP response after a download SUCCEEDED.

## The redesign (agreed direction)

**Chunked stop-and-wait push** as the default drag-and-drop path:
- Page JS slices each File (`File.slice`) into pieces (start ~16–32 KB; tune by test),
  sends ONE piece at a time, each awaiting the phone's ack before the next departs.
  One piece in flight ⇒ the radio can never be handed more than one piece's burst.
- Server: append-mode writes via the existing PSRAM block buffer; pieces addressed by
  `name + offset (+ last flag)`; idempotent (re-sent piece overwrites at offset);
  resume = client asks the server how many bytes it holds (`GET /chunk?name=`) and
  continues from there. Consider a per-piece CRC32 (JS-side is cheap) verified before
  append — flaky links are the norm here.
- Keep: `/fetch` pull path (bench + link UX), breaker + backpressure as safety nets that
  should never fire again, telemetry, multi-file queue + per-file progress + auto-retry
  in the page, the softAP fallback, `up on books`.
- UX bar: drag-and-drop from a bare MOBILE browser (no HTTPS, no exotic APIs — File.slice
  + fetch/XHR only), progress that shows piece-level retries honestly.

⚠ Implementation trap scouted ahead: this framework is the OLD arduino-esp32 core (IDF 3.x
era). Its `WebServer` may lack raw-body upload (`HTTPRaw`); if so, per-piece multipart
through the existing upload-callback path is fine (bounded at piece size). Do NOT accept
whole-body-in-a-String handlers (`server.arg("plain")` buffers the entire body internal).
The WebServer library lives in the platformio framework package — read it before designing.

## First deliverable before any design: a `heap` serial command

`heap_caps_get_info` internal vs PSRAM (free/largest/minimum), plus stack high-water marks
for the main tasks. Two mysteries need it: (a) some boots idle at largest ≈ 9–10 KB vs
16 KB on others (6 KB unexplained variance); (b) the push path's per-request transient
(7–15 KB) has never been itemized. Measure BEFORE redesigning; the numbers may move the
piece-size choice.

## Test protocol (the corpus is on disk, overwrite-safe)

The four Dominion Civil War EPUBs (3–5.5 MB each) live in this session's uploads dir and
on COVEY (`/home/covey/books/`); uploading them AGAIN to the WiPhone is idempotent (clean
names already in `/books`). Serve them from the Mac (`python3 -m http.server`) for pull
tests; push tests via curl multipart or a headless browser against the page's own JS.
**Acceptance bar: three consecutive 4-book batches over the fast LAN at full speed,
zero breaker trips, internal-heap floor never below 10 KB, then the same from a REAL
mobile browser. Then update the README's upload claims (audited today — keep them true).**

## Files

`WiPhone/app_gbc_xfer.cpp` (server + page HTML/JS + breaker + backpressure) ·
`app_books.cpp` / `app_music.cpp` / `app_files.h` users of XferConfig · `serial_cmd.cpp`
(`up on books`, telemetry conventions) · framework `WebServer` library (read-only) ·
`tools/panicwatch.py` + `/tmp/wiphone.cmd` (the serial bridge; phone at
`/dev/cu.usbserial-025A3EAF`, 500000 baud).

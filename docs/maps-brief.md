# Maps on the WiPhone — the analysis before the build (2026-09-18)

**Status: the analysis that came first — the build followed it the same night.** Everything
under §9's Phase 0–2 was built and proven on WiPhone 2 by the morning of 2026-09-19 (see
`docs/HANDOFF.md`'s top block and `docs/maps.md`); §10's decisions were taken as recommended.
Kept as the record of what was known before any code was written. Written the evening of
2026-09-18 from PR #1
(`origin/claude/wiphone-maps-app-lzry49`, "0.9.64: Maps"), COVEY's map, the framework
sources, and a bench session on WiPhone 2. Every number marked **measured** was read off the
phone or the wire tonight; everything else says *estimate*.

## Verdict up front

1. **PR #1 is a sound offline viewer and merges as a pure fast-forward** (8 commits on top of
   `eef0e03`, `git merge-tree` clean). It has never met a phone. **It also has a byte-order bug
   that would paint every tile in the wrong colours — measured tonight, see §4 — and the same
   bug is live in the shipped Photos/Books image paths.** One-line fix; keep the file format.
2. **{user}'s ask is mostly a NEW download subsystem plus a rebinding.** The PR downloads
   nothing (tiles are converted on the Mac), shares pins to an automatically chosen channel,
   binds zoom to `*`/`#`, has no F-key handling, no "go to coordinates", no download menu.
3. **USGS from the phone is blocked by TLS, not by decoding.** USGS is HTTPS-only and the
   phone's TLS **failed tonight** (`SSL - Memory allocation failed`, internal heap min-ever
   2,676 B). The cause is fixed in the precompiled framework (`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`,
   two 16.7 KB record buffers from a heap whose largest block is 25.7 KB), and the handshake
   also runs on the caller's stack while the loop task's floor is 408 bytes. **There is one
   escape hatch — a runtime PSRAM allocator for mbedTLS on a dedicated task — and it is
   UNPROVEN.** Proving it is Phase 0. If it fails, OpenTopoMap (plain HTTP, measured working)
   is the on-phone source and USGS stays on the Mac converter or a LAN relay.
4. **Decoding is not the problem the PR thought it was.** USGS Topo and Imagery tiles are
   baseline *colour* JPEG (measured, ~120 samples), which the ROM TJpgDec this firmware already
   drives for Photos decodes today; OpenTopoMap is 8-bit palette PNG, and the ROM `tinfl`
   inflater the EPUB parser already uses covers it with ~300 lines of in-repo PNG glue.
5. **RAM is the design constraint, and it is manageable if nothing new touches internal RAM:**
   the PR's working set is 768 KB of PSRAM; the downloader's buffers all fit in PSRAM; the one
   unavoidable internal cost is a task stack (~8–10 KB) if TLS is in play.

## 1. What PR #1 is, and the gap to the ask

| {user}'s ask | PR today | Gap |
|---|---|---|
| Download maps to SD from the phone (USGS topo etc.) | **Absent.** `docs/maps.md`: "this repo does not fetch them"; `tools/convert_tiles.py` on the Mac | New subsystem (§3) |
| Every COVEY feature unless unreasonable | ~⅔ present or portable (§6) | GO-to-coordinates, channel picker, download screen, ruler, follow-me |
| Arrows pan | Present (arrows + 2/4/6/8, press-run accelerator) | — |
| Top user buttons zoom in/out | **Absent** — `*`/`1` out, `#`/`3` in; no F-key handled | Bind F1/F2; win over the music transport (§5) |
| OK sets a pin | **Absent** — `5` drops a pin; OK opens pin-options or the menu | Split OK from SELECT (§5) |
| OK on a pin edits/renames | Present (pin within 14 px: Rename / Move / Share / Unshare / Centre / Delete) | — |
| Pins share to a CHOSEN channel | **Partial** — `announceChannel()` = first non-default-PSK channel, else public `channels[0]`; no choice, no channel stored with the pin | Picker + channel name on the pin (§7) |
| Top-left SELECT opens a menu (download / go to pin / coords / node) | **Partial** — SELECT == OK (`LOGIC_BUTTON_OK`, `Hardware.h:212`); menu has Pins / Places / Nodes / Centre on me / Area / Rescan / Keys / Back — no Download, no coordinates | Ordering fix + two rows + a coordinate entry screen |
| One user button centres on me | **Absent** — `0` and a menu row | Bind F3 (assumed, see §10) |
| View saved across exit/restart | **Half** — NVS save in the destructor only; **both power-off paths pull the latch before any destructor runs** (`WiPhone.ino:3549-3557`, `:3603-3612`) | `mapsSaveOpenView()` hook like `booksSaveOpenPosition()` |
| No RAM crashes | Unproven; design is PSRAM-only (+72 B internal static, measured by the author) | Bench (§8) |

Also in the PR and worth keeping: areas (`/maps/<area>/z/x/y.565`), stale-node greying, label
collision avoidance, serial `maps` / `maps goto` (a cable cannot press keys), 32 KB-per-tick
SD reads, host tests (110 checks) and `check_convert_tiles.py`.

## 2. Measured tonight on WiPhone 2 (0.9.63, up 22 h, on `SmithWifi`)

| What | Number |
|---|---|
| Internal heap idle | free **27,972** / largest **25,716** / min-ever 19,860; PSRAM 3.46 MB free |
| Loop-task stack floor | **408 bytes** of 8,192 — any deeper call path is an overflow, not a heap failure |
| HTTPS `/fetch` of a USGS tile | **FAILED**: `ssl_client.cpp:36 start_ssl_client():207 (-32512) SSL - Memory allocation failed`; min-ever fell to **2,676**; largest stayed fragmented at 19,160 for the rest of the session |
| Plain-HTTP `/fetch`, OpenTopoMap PNG (30 KB) | works; **~1.8 s/tile**, server-bound (0.94 s from the Mac too) |
| Plain-HTTP `/fetch`, 24 KB tile from the Mac on the LAN | **~0.30 s/tile** steady (first three 1.4–2.3 s), of which **~255 ms is a LOOP STALL** (`'gbc-xfer'`) |
| Heap across 12 LAN pulls | flat: 21,576 → 21,568 (no per-pull leak) |
| USGS endpoints | `http://` → **301 to https** for every basemap service; TLS 1.2 ECDHE-RSA-AES128-GCM, 2-cert chain, keep-alive honoured, cached to **z16** (z17+ 404) |
| USGS tile format | **baseline colour JPEG**, mean 20–29 KB per tile z11–z16 (service says `MIXED`; no PNG seen in ~120 samples, but edges/no-data can be PNG or a 572-byte HTML 404 — sniff magic) |
| OpenTopoMap | `http://{a,b,c}.tile.opentopomap.org` **200 over plain HTTP**, 256×256 **8-bit palette PNG** non-interlaced, 17–55 KB, CC-BY-SA, "mass downloads discouraged" |
| **Byte-order test** (§4) | a BMP of pure red / pure green / mid-grey rendered by Photos as **(0,28,197) / (230,0,57) / (16,16,32)** — exactly RGB565 with the two bytes exchanged |
| Framework facts | `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`, `CONFIG_SPIRAM_USE_MALLOC` unset (plain `malloc` never returns PSRAM), `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384`, no asymmetric/dynamic buffers in this mbedTLS 2.16.7; `esp_config.h` defines `MBEDTLS_PLATFORM_MEMORY` and `libmbedtls.a` exports `mbedtls_platform_set_calloc_free`; `CONFIG_ARDUINO_LOOP_STACK_SIZE=8192` is baked into `sdkconfig.h` |

Left on phone 2's card from the bench: `/photos/usgs_tile.jpg` (a real USGS tile, decodes fine
in Photos), `/photos/download.jpg` (actually an OpenTopoMap PNG — Photos will refuse it) and
`/photos/swaptest.bmp` (the test card). Delete or keep; harmless.

## 3. The download subsystem — the design, problem by problem

### 3a. TLS (the gate for USGS)

- `mbedtls_ssl_setup()` callocs two `13+16+48+256+16384 = 16,717`-byte I/O buffers from
  internal RAM before the handshake starts. 33.4 KB against a 25.7 KB largest block: that is the
  measured `-32512`. Max-fragment-length cannot shrink them in this mbedTLS (compile-time size),
  and `ssl_client.cpp` never negotiates it anyway.
- **The hook:** `mbedtls_platform_set_calloc_free(psram_calloc, free)` installed once before
  the first `WiFiClientSecure`. Every mbedTLS allocation (buffers, contexts, cert parse) then
  lands in PSRAM. It is global; nothing else in the firmware allocates through mbedTLS
  (`mesh_crypto` uses stack AES contexts; hashes are stack contexts), hardware AES is CPU-fed
  (no DMA), MPI/SHA are software — so PSRAM buffers are functionally safe, just slower.
  **Unproven on this phone.**
- **The stack:** the handshake runs on the caller's stack; the loop task has 408 B to spare,
  and its 8 KB is baked into `sdkconfig.h`. So TLS needs **a dedicated task with a ~10 KB
  INTERNAL stack** (IDF 3.3 cannot put task stacks in PSRAM), allocated once with
  `xTaskCreateStatic` and never freed (the GBC rule: deleting a task defers the free to the
  starved idle task). That ~10 KB is the real internal-RAM price of USGS-on-phone, and it
  must be measured against the RX-flood floors from the upload saga (14 KB → 868 B).
- **Keep-alive is mandatory:** one `HTTPClient`/`WiFiClientSecure` held across the whole area
  (`_reuse` defaults true; USGS honours it). A per-tile handshake is 1–3 s of software
  ECDHE-RSA out of PSRAM and a heap transient each time.
- **Fallbacks, in order:** (1) OpenTopoMap over plain HTTP, direct from the phone, measured
  working; (2) a LAN relay — COVEY already caches the same three sources at
  `~/covey-tiles/<source>/z/x/y.png` and the Mac pulled a tile at 0.30 s; (3) the PR's
  `convert_tiles.py` path.

### 3b. Where the work runs (the superloop)

- Everything shares one task; the stall detector fires at 250 ms; keys come from an I²C FIFO
  only `loop()` drains; the LoRa PHY is polled from `loop()` at 10 ms; SIP/RTP are pumped from
  `loop()`. Tonight's 24 KB pull froze it for 255 ms *per tile* on the existing blocking path.
- **Recommendation: a single downloader task** (internal static stack, priority
  `tskIDLE_PRIORITY+1`, core 1, blocking socket reads with timeouts, `vTaskDelay` per tile)
  that does fetch → decode → write, with the loop task only drawing progress. This is the only
  shape that can host TLS at all.
- **It may write the SD itself.** A reader's fear that a second task would corrupt the shared
  SPI bus was **refuted from the sources**: `TFT_eSPI.cpp:36-37` force-defines
  `SUPPORT_TRANSACTIONS` on ESP32 (the HAL mutex is toggled), `sd_diskio.cpp` wraps every sector
  in `AcquireSPI`, and FatFs is built `FF_FS_REENTRANT=1`. The cost is *latency* (a tile write
  holds the bus; the loop's next TFT call waits), not corruption. Batch each tile as one
  128 KB write to a temp name then `SD.rename()` so a power-off never leaves a wrong-length file.
- **Rules from this tree the task must obey:** pause while `sipNeedsFullSpeed()` or music is
  sounding; pause on `!wifiState.isConnected()` and resume itself (reconnect is only gated in
  softAP mode — do not add a gate); add itself to the DFS busy predicate (else 80 MHz once the
  screen sleeps at 30 s) and hold the screen awake (`xferHoldAwake` pattern) — a locked phone
  eats every key including cancel; feed nothing to the loop WDT (it is the loop's own).
- A tick-driven state machine on `APP_TIMER_EVENT` (the PR's own `tileLoadStep` idiom) is
  viable **only for plain HTTP** and costs no stack; it is the fallback design if the TLS
  experiment fails and OpenTopoMap/relay is the source.
- Note the PR's `APP_TIMER` is re-routed to `callApp` during a call, so a timer-driven
  downloader pauses itself for a call for free; a task must check.

### 3c. Decoding to the PR's `.565`

- **Keep the PR's raw format** (256×256 RGB565 little-endian, exactly 131,072 B, length is
  the whole check). It keeps the proven viewer untouched, keeps `convert_tiles.py` and its
  test valid, and a phone-side writer that fills a native `uint16_t[65536]` with `color565()`
  and `write()`s it matches the Python **byte-for-byte** (Xtensa is little-endian). The price is
  8× card space and a 128 KB write per tile (~0.28 s at the measured 460 KB/s floor).
- **JPEG (USGS):** ROM TJpgDec (`rom/tjpgd.h`, `jd_prepare`/`jd_decomp`) with a private
  input/output callback pair via the JDEC's `device` pointer — not `display::load_jpg_at`,
  whose globals and 4 KB *internal* pool a wallpaper reload would race. `ps_malloc` the 4 KB
  pool and the ~30 KB compressed tile; write straight into a PSRAM `uint16_t[65536]`. Guards:
  `JDR_FMT3` (greyscale/progressive) → skip and count, width/height must be 256 (a 512 px tile
  would overflow the buffer), clip in the callback anyway. `jd_decomp` is not resumable:
  **one uninterruptible ~80–200 ms stall per tile (estimate — instrument it on the first run)**.
- **PNG (OpenTopoMap, USGS edges):** in-repo, ~300 lines, host-testable against Pillow like
  `check_convert_tiles.py`: chunk walk, IDAT concatenation, strip the 2-byte zlib header and
  4-byte Adler *ourselves* and call ROM `tinfl_decompress` exactly as `epub_parse.cpp:99-113`
  does (raw deflate, `TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF`, ~11 KB state in PSRAM) — the
  zlib-header flag is unproven on this ROM; the five filters; colour types 0/2/3/4/6 at 8-bit,
  palette 1/2/4/8-bit; refuse interlaced and 16-bit with a logged reason and a counted skip.
  Peak transient ~430 KB of PSRAM. No vendored library (PNGdec/pngle/lodepng all bring a
  second inflater and an allocator to redirect).
- **Sniff, never trust the URL:** `FF D8 FF` / `89 50 4E 47` / anything else (HTML 404) →
  count as skipped and blacklist for the run.

### 3d. Budget (Web Mercator, ~47 N, COVEY's ±radius square, z11 base)

| Radius | z11–15 tiles | on card (.565) | USGS JPEG in | est. minutes: LAN relay / USGS keep-alive / OTM |
|---|---|---|---|---|
| 2 km | ~55–67 | 7–9 MB | ~1.5 MB | 0.5 / 0.5 / 2 |
| 5 km | ~245 | 32 MB | ~5 MB | 1.2 / 2 / 7 |
| 10 km | ~865 | 113 MB | ~18 MB | 4.5 / 6.5 / 26 |
| 20 km | ~3,260 | 427 MB | ~67 MB | 16 / 24 / 98 |

z16 quadruples every column (20 km z11–16 = 12.7 k tiles, 1.6 GB, 1.5–6.5 h) and for USGS Topo
it is the z15 cartography drawn larger (same contours, same labels — checked). **Default the
picker to 5 km / z15, cap at 10 km / z15, offer z16 only with the honest estimate on screen.**
The deepest zoom is ~73 % of any pyramid, so dropping z11–12 saves ~2.5 % — not worth a knob.
Minutes are per-tile costs derived from tonight's measurements (0.30 / ~0.45 / 1.8 s) plus an
unmeasured ~0.1 s decode and ~0.28 s write; the USGS column assumes keep-alive works.

## 4. The byte-order bug — measured, pre-existing, and inherited by the PR

`TFT_eSprite` stores every pixel **byte-swapped** (`drawPixel`, `pushColor`, `fillRect` all
do `color>>8 | color<<8` on store) so that `pushSprite` can stream the buffer to the ST7789
raw, MSB first. `TFT_eSprite::pushImage(uint16_t*)` stores the word **unchanged** unless
`setSwapBytes(true)` was called — and the firmware never calls it anywhere. So any path that
pushes native `color565()` words through `pushImage` reaches the glass with its two bytes
exchanged: pure red `F800` → `00F8` → blue.

- **Measured tonight** on 0.9.63: a 24-bit BMP with red / green / 50 % grey bands opened in
  Photos rendered **blue / red-pink / near-black** — `(0,28,197)`, `(230,0,57)`, `(16,16,32)`
  are exactly the swapped values. (`readPixel` unswaps, so the serial screenshot shows what
  the panel shows; the JPEG wallpaper behind it, which goes through `pushColor`, is correct.)
- **Live in shipped code:** `app_photos.cpp:141-143` (BMP), `:161-166` (greyscale JPEG),
  `app_books.cpp:893-903` (greyscale book images). The 08-25 Photos run only ever looked at a
  colour JPEG, which takes the correct path. Nobody looked at a grey one.
- **The PR:** `app_maps.cpp:964-967` forces `setSwapBytes(false)` on both the panel and the
  sprite with a comment asserting a sprite stores little-endian. It does not. Every tile would
  render byte-swapped.
- **Fix:** `setSwapBytes(true)` on the sprite (and the panel, for the no-sprite fallback) around
  the blit, restored after, exactly as `app_gbc.cpp:311` restores it; keep `.565` files
  little-endian so the Mac converter, its test, and the phone-side writer all agree. Photos and
  Books get the same one-liner. **Do not "fix" the converter to big-endian** — that diverges
  from native `uint16_t` writes.

## 5. Keys

- Physical side buttons, top to bottom: **F1, F2, F3, F4** (README, `app_gbc.cpp:1139`). So
  "top user buttons zoom" = F1 / F2. "The one user button" is ambiguous — **assume F3**
  (the third), F4 unused, unless {user} says otherwise.
- **Music claims them first.** `WiPhone.ino:3000-3018` swallows F1 and F2 whenever a track is
  merely *loaded* (paused counts) and F3/F4 while it is sounding, before any app sees the key
  — the same silent "dead hardware" symptom the keypad self-test already documents. The GBC
  has the precedent: `!gGbcActive` exempts it. **Maps gets a `gMapsActive` exemption** for
  F1–F3 while open (set in ctor, cleared in dtor, and in the F2 release block at `:2708`);
  keypad `*`/`#`/`0` stay as aliases. (Pre-existing and separate: F1 with no track loaded
  during a call starts a track under the call, HANDOFF:5210.)
- **SELECT vs OK.** `LOGIC_BUTTON_OK` is OK‖CALL‖SELECT; `app_music.cpp:405-412` already has
  a dead `== WIPHONE_KEY_SELECT` branch from testing the macro first. Maps must test
  `event == WIPHONE_KEY_SELECT` (menu) **before** `LOGIC_BUTTON_OK`, then `WIPHONE_KEY_OK`
  (pin under crosshair → options, else drop pin → rename); decide CALL explicitly (suggest:
  nothing). Do not narrow the shared macro (`Hardware.h:214-226` house rule). Footer becomes
  "Menu / Back"; the help screen and `tests/check_menu_keys.py` follow.
- Proposed map: arrows/2-4-6-8 pan · F1 in / F2 out (`#`/`*` aliases) · F3 centre on me (`0`) ·
  OK pin / pin options (`5`) · SELECT menu · 7/9 prev/next pin · BACK/END exit.

## 6. COVEY parity

Portable as-is or already in the PR: pan with acceleration, zoom, one-shot centre-on-me, view
memory, pin drop/rename/move/share/unshare/delete, node markers with age (PR greys stale ones —
better than COVEY), own-position marker incl. stale-fix grey and declared-pin white, Pins /
Places / Nodes lists, first-open seed chain, offline "no tiles" honesty.

Needs a keypad redesign (build these): **GO picker** (nodes and pins nearest-first with
distance/compass, sections; the PR lists by identity only), **Go to coordinates** (a DD text
field — `47.4964, -121.7261` — the multi-format `geoparse` port is optional; no clipboard on
the phone), **channel picker on share** (§7), **download screen** (source / radius / depth /
estimate / DOWNLOAD-STOP / progress, in-app modal for v1), **ruler** (anchor at crosshair,
"pan to measure", one menu row — cheap, the bearing/haversine helpers exist), **follow-me
latch** (cheap: re-centre on a fresh fix until a pan; lock-loss must not move the view),
**N / F toggles** as menu rows.

Unreasonable or meaningless on a 240×320 keypad phone: pinch/drag/long-press gestures and
their hygiene, the full waypoint editor's description field (180 chars on T9), icon glyph
set (no font), "stream when online" while browsing (a decode per pan on the loop task —
maybe later, not v1), 400-tile decoded cache (PSRAM says 6), background download that
outlives the app (v2 — the transfer server's 10-minute self-stopping job is the precedent).

## 7. Other findings that change the build

- **Channel choice — reuse, don't invent.** Main already has the keypad picker with the
  right rules: `MeshtasticApp::buildPosChannel()` (lists every channel, tags PUBLIC with a
  press-again confirm, re-resolves by index at press time, persists the choice by **name** via
  `setPosChannelName`/`getPosChannel`). A shared pin (`expire=0`, persists on every radio) is
  the beacon case, not the one-shot case the PR's `announceChannel()` was written for. Store the
  channel **name** on `MapPin` and in `pins.txt` so rename re-sends and "Take it off the mesh"
  go out on the channel the pin was shared on; default the highlighted row to the beacon
  channel. `sendWaypointOn()` already takes a `MeshChannel*`.
- **Save on power-off.** Add `mapsSaveOpenView()` beside `booksSaveOpenPosition()` at both
  pre-`powerOff()` call sites; plus a debounced save a few seconds after the crosshair stops.
  Not per pan (NVS wear).
- **"Centre on me" trusts a bad fix.** `selfPosition()` uses `getGpsFix()` with no quality
  gate, while `resolveReference()` requires `meshPosFixUsable()` (sats ≥ 4, HDOP ≤ 10 — the
  "twenty kilometres wrong" note). Gate it the same way; fall through to the pin with a note.
- **Mesh news may not repaint the map:** `WiPhone.ino:4207-4209` discards the result of the
  `NEW_MESSAGE_EVENT` it forwards (the mirror path at `:2587` uses it). OR it into a redraw.
- **Waypoint table is 8 slots** and every shared pin takes one; a 9th evicts the oldest place
  somebody else shared. Cap shared pins with a note, or plan the DB migration.
- **Holed areas:** the viewer session-blacklists any wrong-length file; the downloader's skip
  test must be `size == 131072`, and the pre-walk should delete short files and count them
  "repaired". Check `SD.totalBytes()-usedBytes()` against the estimate before starting.
- **Battery:** 900 mAh cell, hard power-off at 3.30 V with no unwind, WiFi-active draw at
  240 MHz unmeasured (typically +100–150 mA). Gate area downloads on USB or a battery floor,
  and measure once with `power` during a 5 km pull.
- **Attribution:** OpenTopoMap is CC-BY-SA and asks for a credit line; USGS requests one.
  One row per source on the help screen and the download footer.
- **Loop stack floor 408 B** is a pre-existing hazard independent of maps; the downloader
  must add nothing to that stack (all state in PSRAM structs, pointers only on the stack), and
  the PR's `char path[160]` + `MapBlit b[12]` frames deserve a high-water-mark check on the
  first run.

## 8. RAM budget

**PSRAM (3.46 MB free):** viewer 6 × 128 KB = 768 KB (app open only); downloader: 128 KB
output tile + 32 KB compressed tile + 11 KB tinfl state (+ up to 262 KB inflate buffer for an
RGBA PNG) + 4 KB TJpgDec pool + mbedTLS ~50 KB via the hook. Under 1.3 MB peak. Fine.

**Internal (27.9 KB free / 25.7 KB largest at idle, 19 KB after one failed TLS):** the PR adds
72 B static. The downloader adds a task stack (~10 KB, permanent) **only if TLS is used**,
`WiFiClient`'s 1.4 KB RX buffer, `HTTPClient` Strings (< 1 KB — never `getString()`), and
`File` handles. Guards: one connection in flight; start gate largest ≥ ~12 KB after the stack
exists; pause the fetcher below ~6 KB with the breaker's hysteresis; log heap/largest per
tile the way XFER does and watch for a walk-down across a batch. **The tick-driven plain-HTTP
design costs no stack at all** — the internal-RAM argument for it is real.

## 9. Build plan (phases, each with an acceptance bar)

**Phase 0 — prove the three things nothing else can be designed around (bench, ~half a day).**
(a) Flash PR #1 as-is to phone 2 with a Mac-converted 5 km area, fix the swap byte (§4) with a
pure-red tile that must show red, pan for a session and read the stall log; instrument
`jd_decomp` and the loop stack high-water mark. (b) The TLS experiment: install the PSRAM
calloc hook, pull one USGS tile from a task with a 10 KB static internal stack, log
heap/largest/min-ever before/during/after and the handshake time, then a heap-flat 50-tile
keep-alive run; confirm WiFi rejoin and (on phone 1) SIP register still work. (c) One
`shot`-verified Photos/Books grey-image fix (same one-liner). **Bar:** red is red; TLS verdict
is a number, not an opinion; loop stack floor known after a decode.

**Phase 1 — the rebinding and the small asks (a session).** F1/F2/F3 with `gMapsActive`;
SELECT/OK split; Download and Go-to-coordinates rows; coordinate entry screen; channel picker
on share reusing `buildPosChannel()`, channel name on the pin; `mapsSaveOpenView()` pre-latch
hook; fix-quality gate; NEW_MESSAGE redraw. **Bar:** panel-proven with a track loaded and
paused, on both phones, host suite green.

**Phase 2 — the download engine, source-agnostic (1–2 sessions).** Task (or tick machine if
Phase 0b failed), sniff-decode-write pipeline, temp+rename, skip-by-size, pause rules,
progress screen with STOP, estimate before start, USB/battery gate, attribution rows. First
source: whichever Phase 0b allows (USGS keep-alive, else OpenTopoMap). **Bar:** three
consecutive 5 km areas, zero reboots, internal largest never below 10 KB, every tile exactly
131,072 B, stall log clean between tiles, then a 10 km area on the phone hotspot.

**Phase 3 — parity extras.** GO picker nearest-first, ruler, follow-me latch, N/F toggles,
PNG decoder (needed for OpenTopoMap and USGS edges) with a Pillow pixel test.

## 10. Decisions {user} owns

1. **Which side button is "the one"** for centre-on-me — F3 (third) is assumed. And Maps
   taking F1–F3 from music while the map is open (as the Game Boy does) — assumed yes.
2. **If the TLS experiment fails:** OpenTopoMap direct + USGS via the Mac converter, or a LAN
   relay (COVEY/Mac serving USGS over plain HTTP), or neither (phone-only or nothing)?
3. **Card format:** keep raw `.565` (recommended — proven viewer, cheap card, one write per
   tile) with 5 km/z15 default and 10 km cap, or store compressed and decode on view?
4. **Channel on share:** per-share picker defaulting to the beacon channel, public needs
   press-again (recommended), or a one-time Maps setting?
5. **Downloads:** in-app modal with STOP, on USB or above a battery floor, for v1 (recommended);
   background-while-you-read is v2.

Method note for the record: three of tonight's load-bearing facts (TLS failure, the 255 ms
stall, the byte swap) came from measuring on the phone; the reading agents found the mechanism
for each, and one confident code-reading risk (bus corruption from a second task) was refuted
from the sources. Nothing in the PR should be trusted on hardware until Phase 0 runs.

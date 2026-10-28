#!/usr/bin/env bash
# Build and run the host-side tests.
#
# These compile the PHONE'S OWN source files with the Mac's compiler. That is the point:
# a pass proves the bytes that ship are the bytes COVEY expects. Nothing here needs
# PlatformIO, an ESP32, or the phone to be plugged in.
#
#   ./tests/run_tests.sh
#
# To regenerate the interop vectors after a change to COVEY's booksync.py:
#   python3 tools/gen_booksync_vectors.py \
#       --booksync ../New_phone_project/.../covey_ui/booksync.py \
#       --out tests/vectors_booksync.h
set -euo pipefail

cd "$(dirname "$0")/.."
OUT=".pio/hosttests"
mkdir -p "$OUT"

CXX="${CXX:-c++}"
CC="${CC:-cc}"
FLAGS=(-std=c++11 -O1 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined)
# Vendored C (helix) is built with the C compiler and its own flags. It MUST NOT go
# through the C++ front end: C++11 narrowing rules reject helix's constant tables, which
# are full of values above INT_MAX written as plain integers. Perfectly legal C.
#
# ASan yes, UBSan no. helix is fixed-point DSP and shifts negative values left all over
# dct32.c and imdct.c, which UBSan reports and which is exactly what the algorithm means
# to do. Leaving it on buried a passing run in hundreds of lines of noise about
# third-party code that is not ours to change. ASan is kept because a buffer overrun in a
# decoder fed untrusted files is a real thing worth catching.
CFLAGS=(-std=c99 -O1 -g -fsanitize=address -w)

fail=0
for src in tests/test_*.cpp; do
  name="$(basename "$src" .cpp)"
  # Every WiPhone source the tests need must be free of Arduino headers; that constraint is
  # what keeps this suite possible, so a link error here is a design warning, not a nuisance.
  extra=()
  csrc=()
  case "$name" in
    test_booksync) deps=(WiPhone/booksync.cpp WiPhone/book_hash.cpp) ;;
    test_epub)     deps=(WiPhone/epub_parse.cpp WiPhone/book_hash.cpp); extra=(-lz) ;;
    test_bookstore) deps=(WiPhone/bookstore.cpp WiPhone/booksync.cpp WiPhone/book_hash.cpp) ;;
    test_layout)   deps=(WiPhone/book_layout.cpp) ;;
    test_inbox)    deps=(WiPhone/booksync_inbox.cpp WiPhone/booksync.cpp WiPhone/book_hash.cpp) ;;
    # KOSync (KOReader's sync protocol): the vectors from tools/gen_kosync_vectors.py (partial
    # MD5, CrossPoint's byte-weighted spine and its inverse), the pure protocol module, the
    # inbound chain into the booksync inbox, AND the before/after proof that no saved place
    # moves (tests/golden_positions.h, generated once from the pre-KOSync parser).
    test_kosync)   deps=(WiPhone/kosync.cpp WiPhone/epub_parse.cpp WiPhone/book_hash.cpp
                         WiPhone/booksync.cpp WiPhone/booksync_inbox.cpp WiPhone/bookstore.cpp)
                   extra=(-lz) ;;
    test_jpeg)     deps=(WiPhone/jpeg_grey.cpp) ;;
    test_music)    deps=(WiPhone/music_lib.cpp WiPhone/wav_reader.cpp) ;;
    test_sms_mirror) deps=(WiPhone/sms_mirror.cpp) ;;
    # The PKC DM crypto: the shipping mesh_pki.cpp plus the vendored donna and
    # tiny-AES beneath it (C, so csrc), against vectors from Python cryptography
    # playing the RAK's side of the exchange.
    test_pki)      deps=(WiPhone/mesh_pki.cpp WiPhone/book_hash.cpp)
                   csrc=(WiPhone/src/crypto/curve25519_donna.c WiPhone/src/crypto/tiny_aes.c) ;;
    # Position/Waypoint payloads + the distance/bearing math (python-checked), and since 0.9.82
    # location_source/precision_bits both ways and the firmware's cell cut (meshPosBuildEx,
    # meshPosParseEx, meshPosTruncate; regenerate with tools/gen_pos_vectors.py).
    test_pos)      deps=(WiPhone/mesh_pos.cpp) ;;
    # Which chat messages survive a reboot: newest-per-conversation, capped.
    test_retain)   deps=(WiPhone/mesh_retain.cpp) ;;
    # The woods plate's GPS: NMEA assembly, checksums, fixed-point coords.
    test_nmea)     deps=(WiPhone/nmea.cpp) ;;
    # Where this phone is (0.9.82, self_pos.cpp): which source to believe (GPS > the hotspot
    # phone's GPS > a Wi-Fi location), the 120 s / 30 s bars, bench-injected fixes never sent by
    # the schedule - and the phone-over-WiFi stream through the shipping NMEA reader: one GGA
    # judged with nothing inherited, the RMC cross-check, the settle and time-of-day stale guards
    # across midnight, the '\n' before every '$', the port search and its back-off, the status
    # words, two fixes in a row agreeing (gpsdRelay's generated coordinates can be km off and
    # still parse). The Wi-Fi slot's own rules: its clock is the scans (12 min shown after the
    # last confirming one, gone after), sent only with "map + mesh", after two agreeing scans
    # 60 s apart and a confirming scan within 60 s, as a 17-bit cell stamped with that scan's
    # UTC; the cell kept while the fix stays put; a precise beacon of the last hour wins.
    # The woods plate's UART (selfGpsApply, the 2026-10-10 review): only a sentence that itself
    # carries a measured fix stamps the GPS slot - a run of no-fix GGAs with the RMCs lost no longer
    # re-stamps the last position as now (reproduced against the old rule: 22 checks failed); the
    # M100's RMC+GGA at 1 Hz, a lost fix going OLD on the bars, the cold-start RMC kept POOR, and
    # (review 2026-10-11) a POOR fix kept POOR when its GGAs go missing or come back empty.
    # Reads meshtastic_service.h to hold the bars equal.
    test_selfpos)  deps=(WiPhone/self_pos.cpp WiPhone/mesh_pos.cpp WiPhone/nmea.cpp) ;;
    # Wi-Fi location (0.9.82, geoloc.cpp): the X4 e-reader's beaconDB vectors ported onto
    # json_read.cpp (the request bodies with the IP fallback off twice and no SSID, the privacy
    # filters, the -85 dBm floor, halves dealt by device, the parser, the two-half agreement and
    # the moved access point it catches) - no IP lookup to test, none exists - and this phone's
    # rules: the scan snapshot, when a look runs (every gate, the 10/2-minute gaps, 3 attempts
    # an hour, dry looks), later scans confirming or dropping a fix, the fold into the real
    # self_pos NET slot end to end down to the beacon's bytes, the auto-switcher's scan mode
    # (never its disconnect machinery for a geo scan), the row's words, the bench's canned answers
    # (`netloc answer`: stamped now, never sent, the schedule untouched) and the three pinned roots
    # (ISRG X1 + X2 + Root YE, by SHA-256). And what two agreeing halves cannot see: the 300 m/s
    # jump from this boot's last GPS/phone fix, a scan that lands while the look is out, a fix
    # HELD when the WiFi link drops or changes, an expired fix cleared (no millis()-wrap revival),
    # when a scan is asked for, and a look abandoned when the setting is switched off.
    test_geoloc)   deps=(WiPhone/geoloc.cpp WiPhone/json_read.cpp WiPhone/self_pos.cpp WiPhone/mesh_pos.cpp
                         WiPhone/nmea.cpp WiPhone/book_hash.cpp) ;;
    # Who may set the clock: GPS time (fed through the shipping NMEA reader) and mesh time —
    # a 'V' sentence refused, two consecutive readings, a fresh NTP never overridden, and the
    # mesh only into an unknown clock (two nodes on a public channel, one on a private one).
    test_clocksrc) deps=(WiPhone/clock_source.cpp WiPhone/nmea.cpp) ;;
    # Sunrise/sunset/civil twilight - the old test_sun's implementation-independent checks
    # (almanac anchors, equinox/solstice day lengths, the longitude shift, ordering, polar night),
    # moved onto astro.cpp when sun_times.cpp was retired (0.9.80: one source of legal light).
    test_sun_astro) deps=(WiPhone/astro.cpp) ;;
    # The Almanac (0.9.80): sun/moon/phases/seasons/solunar against PyEphem, WMM2025 against NOAA's
    # own test values, UTM/MGRS against utm+mgrs - one vector file, tests/vectors_almanac.h, that
    # COVEY's Python port is tested against too (tools/gen_almanac_vectors.py).
    test_almanac)  deps=(WiPhone/astro.cpp) ;;
    test_wmm)      deps=(WiPhone/wmm.cpp) ;;
    # The time zone grid (tz_grid.cpp + its generated tz_grid_data.h, ODbL data from
    # timezone-boundary-builder): the lookup, the US/Canada rule from first principles, every
    # DST change 2026-2030 of every zone against zoneinfo, the named places against the clock
    # zoneinfo gives the polygon they are in, the content hash COVEY's tzgrid.bin also carries
    # (book_hash.cpp's SHA-256). Regenerate with tools/gen_tz_grid.py; COVEY's covey_ui/tzgrid.py
    # runs the same vectors.
    test_tzgrid)   deps=(WiPhone/tz_grid.cpp WiPhone/book_hash.cpp) ;;
    # The clock's time zone (tz_auto.cpp, 0.9.82): the setting, Auto's decision through the real
    # self_pos slots (two looks 2 min apart, never a bench/poor/old fix, never a Wi-Fi fix however
    # live, never a border, a phone
    # fix - rated or not - only 10 km from any other clock, checked against brute-force sampling of
    # the grid), the offset at every zoneinfo vector instant, reboot memory, the manual override and
    # the way back to Auto (never an old zone), the plate's GPS through its UART gate (a stale
    # no-fix GGA run no longer holds the old zone over the phone's live fix), and every Settings
    # line measured with the firmware's Akrobat_Bold16.
    test_tzauto)   deps=(WiPhone/tz_auto.cpp WiPhone/tz_grid.cpp WiPhone/self_pos.cpp WiPhone/mesh_pos.cpp
                         WiPhone/nmea.cpp) ;;
    test_geogrid)  deps=(WiPhone/geo_grid.cpp) ;;
    # What the Almanac SAYS (almanac_lines.cpp): every screen's rows, the inward rounding of legal
    # light, the countdown, the US DST reminder - and every row measured against the phone's own
    # Akrobat_Bold20 (fonts.h) over six places, two years of days, both rules and both units.
    test_almanac_lines) deps=(WiPhone/almanac_lines.cpp WiPhone/astro.cpp WiPhone/units.cpp WiPhone/mesh_pos.cpp
                              WiPhone/geo_grid.cpp WiPhone/wmm.cpp WiPhone/weather_lines.cpp WiPhone/weather.cpp
                              WiPhone/json_read.cpp WiPhone/tz_grid.cpp) ;;
    # Elevation tiles (sampling, the z13 -> z10 fallback, terrarium decode) and the metric/US
    # formatters; map_tiles.cpp and mesh_pos.cpp are linked as the ORACLES they must equal.
    test_elev)     deps=(WiPhone/elev_tiles.cpp WiPhone/map_tiles.cpp) ;;
    # The map's altitude words (elev_text.cpp): every wording in metric and US, and the rows
    # measured with the firmware's own Akrobat_Bold16 glyph table (fonts.h) against the strip.
    test_elevtext) deps=(WiPhone/elev_text.cpp WiPhone/units.cpp) ;;
    test_units)    deps=(WiPhone/units.cpp WiPhone/map_tiles.cpp WiPhone/mesh_pos.cpp) ;;
    # The map: Web Mercator, the slippy-tile grid, the blit rectangles, and the pins
    # file. Pure arithmetic on purpose — a map that is one tile out looks fine on a
    # 240x320 screen, so this is the only place that error can be caught.
    test_maptiles) deps=(WiPhone/map_tiles.cpp WiPhone/map_pins.cpp) ;;
    test_tilepng)  deps=(WiPhone/tile_png.cpp WiPhone/tile_decode.cpp WiPhone/elev_tiles.cpp); extra=(-lz) ;;
    # The map download's arithmetic and decisions (tile_plan.cpp): tile counts, 64-bit bytes,
    # the Detail row's depths, the centre-out block order, the time, and the auto-resume table.
    test_tileplan) deps=(WiPhone/tile_plan.cpp WiPhone/map_tiles.cpp WiPhone/units.cpp) ;;
    # Voltage -> SOC off phone 1's recorded discharge (tests/fixtures/p1_discharge_2026-09-03.tsv).
    # Scores the table AND the CW2015's own number against time-linear truth.
    test_battery)  deps=(WiPhone/battery_curve.cpp) ;;
    # Why the WiFi dropped: the core's reason->status/rejoin predicates (how an old health.log's
    # wifi=5 is read), the IDF reason/err names, the event ring the loop drains, the HEALTH field.
    test_wifidiag) deps=(WiPhone/wifi_diag.cpp) ;;
    # Header-only: the CPU clock gate's decisions (cpu_clock_policy.h) - never a PLL re-lock
    # under a running radio, checked for every state, and the 2026-09-25 bench day replayed.
    test_cpuclock) deps=() ;;
    # Header-only: may the WiFi station come back, and how (wifi_policy.h) - never over the
    # owner's "off", never under a game or a live hotspot, all 256 input states swept.
    test_wifi_policy) deps=() ;;
    # Header-only: a call's RTP clocks (rtp_watch.h) - far-end silence counted PER CALL (the old
    # boot-long rule, reproduced, ends the second call of a boot at connect) and the hot-mic
    # backstop that releases an RTP session no live call owns.
    test_rtpwatch) deps=() ;;
    # The mesh-history replay wire format vs vectors generated from COVEY's
    # replay.py (regenerate with tools/gen_replay_vectors.py after changes).
    test_replay)   deps=(WiPhone/replay_proto.cpp) ;;
    # NeighborInfo encoding vs the real protobuf runtime's bytes
    # (regenerate with tools/gen_neighbor_vectors.py).
    test_neighbor) deps=(WiPhone/neighbor_info.cpp) ;;
    # The Data/User protobuf codec and the on-air header, vs bytes from the real
    # protobuf runtime (regenerate with tools/gen_wire_vectors.py after an upstream bump).
    test_wire)     deps=(WiPhone/mesh_wire.cpp WiPhone/mesh_hash.cpp) ;;
    # LoRa time-on-air at the phone's registers (hand-worked AN1200.13 references) and the
    # text budget of one frame, measured with the shipping meshBuildData — the numbers
    # MeshPhy::send's timeout and the compose cap are built from.
    test_airtime)  deps=(WiPhone/mesh_airtime.cpp WiPhone/mesh_wire.cpp) ;;
    # The radio's send queue (mesh_txq.cpp): FIFO own frames with ACKs ahead of them, relays only
    # when due and cancelled on a duplicate, one start per pass. It replaced a transmit that held
    # the superloop for the frame's whole time on air - the 0.6-1.5 s 'mesh' STALL lines.
    test_txq)      deps=(WiPhone/mesh_txq.cpp) ;;
    # Is a leftover /meshdb.tmp or /meshfav.tmp WHOLE? Boot renames one over a missing database
    # (a save cut between its remove and its rename) - only if its length is exactly what its
    # own header's counts say.
    test_dbfile)   deps=(WiPhone/mesh_dbfile.cpp) ;;
    # Compiles the REAL helix decoder so a pass proves the bytes that ship are the bytes
    # that decode. helix is C and is listed in csrc, not deps — see CFLAGS above.
    test_mp3)      deps=(WiPhone/mp3_stream.cpp)
                   csrc=(WiPhone/src/audio/helix-mp3/*.c) ;;
    # The music feed (music_feed.cpp) with the real helix and Mp3Stream, driven by simulated
    # main-loop passes against a model of IDF 3.3's I2S DMA: the 0.9.78 feed reproduced (8.6
    # "gaps"/s with no dropout at 10 ms passes, real dropouts at 50 ms), the new one clean at
    # every pass length and stall the old one failed, and the drop counter checked against the
    # model's ground truth. Synthetic streams, so it needs no fixture (the real-track resume
    # tests use tests/fixtures/mp3/track.mp3 when it is there).
    test_musicfeed) deps=(WiPhone/music_feed.cpp WiPhone/mp3_stream.cpp WiPhone/wav_reader.cpp)
                   csrc=(WiPhone/src/audio/helix-mp3/*.c) ;;
    # Header-only: the Game Boy cartridge arithmetic (gnuboy/gb_romsize.h). gnuboy.c itself
    # cannot be compiled here (esp_heap_caps.h, hw.h, cpu.h, sound.h, lcd.h), which is why the
    # arithmetic lives in a header both it and this suite include.
    test_gbrom)    deps=() ;;
    # Header-only: the scrolling selected menu row (WiPhone/menu_marquee.h) — its phase clock
    # and the UTF-8 glyph stepping. The drawing itself needs the LCD and stays on the phone.
    test_marquee)  deps=() ;;
    # Header-only: a display-only menu note broken into rows that fit (WiPhone/menu_wrap.h).
    test_wrap)     deps=() ;;
    # Header-only: where the Now playing title breaks into its two lines (WiPhone/music_title.h).
    # The loop it replaced stopped one character short and split EVERY title (2026-09-26).
    test_music_title) deps=() ;;
    # Header-only: the Files app's folder copy/move/delete path questions (files_paths.h).
    test_filepaths) deps=() ;;
    # Menu > AI's pure half (gemini.cpp): the key file (a superset of the T-Deck's), the request
    # body, Google's answers on fixtures in the shapes measured live (200, MAX_TOKENS with no text,
    # thought parts, 400/404/503, the free tier's 429 per DAY and per MINUTE, blockReason, \u
    # escapes), the chunked body, the screen's ASCII, the model ladder, the saved chat and its
    # topic breaks - and the fake key never in any text built. Fake keys only, no network. Since
    # 2026-10-11: today's date in the system instruction (only from a trusted clock, a zone named
    # like an offset said as "local time"), NO tool ever sent or named, the Wikipedia background in
    # the question's own turn only, the "Wikipedia:" sources line shown and cut off as context,
    # lookup=off, and the ladder's rule 0 (MALFORMED_FUNCTION_CALL with no text: once more). Round 3:
    # rule 6, "Retry if it times out" (a timeout or a lost connection: once more on the model, then
    # the fallback once - never a daily limit booked, never a loop, the setting off = as before), its
    # menu row measured in Akrobat 20, and Key info's line CUT rather than emptied when it is long.
    # Then (shared with COVEY and the X4): every 5xx is busy (500/502/504 the 503 ladder), and the
    # fold table one character at a time (geminiFoldNext - the keywords' and relevance's fold).
    test_gemini)   deps=(WiPhone/gemini.cpp WiPhone/json_read.cpp) ;;
    # Menu > AI's Wikipedia lookup (wiki_lookup.cpp, 2026-10-11): the question's keywords (the
    # stopwords, digits kept, contractions to their stems, a follow-up borrowing the topic's newest
    # question that named something), relevance (a keyword in a title or description, whole words)
    # and wikiCompose's all-or-nothing (no sources line, no background), the one URL,
    # Wikipedia's response on fixtures in the shapes measured live (pages in no order - "index" is
    # the rank - "" descriptions, no "query" when nothing matched, errors, cut bodies), the
    # background block and its caps (a sentence end, UTF-8 whole), the sources line, the log line's
    # reasons, and the round trip with gemini.cpp: background in the question's turn only, never in
    # the history or the saved chat. Then THE SHARED FOLD (gemini.cpp's table before the keywords
    # and the relevance match, every other non-[a-z0-9'] a separator: "navier–stokes" is two
    # words), the contraction before the 40-letter limit, and the vectors all three devices share.
    # No network.
    test_wiki)     deps=(WiPhone/wiki_lookup.cpp WiPhone/gemini.cpp WiPhone/json_read.cpp) ;;
    # The Almanac's weather (0.9.81): weather.cpp (the URLs, the Open-Meteo and NWS parsers on the
    # fixtures in tests/fixtures/wx - public points only, the alert bodies synthetic - the streaming
    # alert counter, the cache file, the fold, the fetch policy) and weather_lines.cpp (every row,
    # measured against Akrobat_Bold20 with all 111 NWS event names), json_read's numbers and null,
    # the curve order (tls_curves.h) and the pinned roots' SHA-256 (isrg_roots.h). No network.
    test_weather)  deps=(WiPhone/weather.cpp WiPhone/weather_lines.cpp WiPhone/json_read.cpp WiPhone/gemini.cpp
                         WiPhone/almanac_lines.cpp WiPhone/astro.cpp WiPhone/units.cpp WiPhone/mesh_pos.cpp
                         WiPhone/geo_grid.cpp WiPhone/wmm.cpp WiPhone/book_hash.cpp WiPhone/tz_grid.cpp) ;;
    # Header-only: the notification pop's stop timer (notify_timing.h) against the shipping
    # pop_pcm[] bytes. WiPhone.ino cannot be compiled here, same reason as above. Review 3 (A1): the
    # ring's lead too, with a model of IDF 3.3's TX ring - the old 360 ms stop erased the chirp.
    test_notify)   deps=() ;;
    *)             deps=() ;;
  esac
  echo "building $name"
  objs=()
  if [ "${#csrc[@]}" -gt 0 ]; then
    cobj="$OUT/$name-c"
    mkdir -p "$cobj"
    for c in ${csrc[@]+"${csrc[@]}"}; do
      o="$cobj/$(basename "$c" .c).o"
      "$CC" "${CFLAGS[@]}" -I"$(dirname "$c")" -c "$c" -o "$o"
      objs+=("$o")
    done
  fi
  # ${deps[@]+...}: macOS bash 3.2 + set -u calls an EMPTY array unbound; the
  # guard is the same one csrc/objs/extra already wear. First hit by test_chunk,
  # the first header-only suite (its whole subject compiles from the .h).
  "$CXX" "${FLAGS[@]}" -o "$OUT/$name" "$src" ${deps[@]+"${deps[@]}"} ${objs[@]+"${objs[@]}"} ${extra[@]+"${extra[@]}"}
  if ! "$OUT/$name"; then
    fail=1
  fi
done

# ── SOURCE GUARD: no menu row may be added with a key of 0 ────────────────────────────────
# See tests/check_menu_keys.py. It replaced a one-line `git grep` that would NOT have caught
# the bug it was written for: the original fault passed a NAMED CONSTANT (ROW_INERT = 0), and
# the grep needed a literal zero on the same line as `addOption(`.
echo "checking for menu rows with a key of 0"
if ! python3 tests/check_menu_keys.py; then
  fail=1
fi

# ── SOURCE GUARD: nothing but Networks.cpp brings the WiFi station back by itself ─────────
# See tests/check_wifi_restore.py. Every site that gives the radio back goes through
# wifiRestoreStation(); a bare esp_wifi_start()/WiFi.reconnect()/WiFi.begin()/WiFi.mode(WIFI_STA)
# elsewhere is how WiFi came back on behind "off" after every Game Boy game (0.9.79).
echo "checking that WiFi is only given back through wifiRestoreStation()"
if ! python3 tests/check_wifi_restore.py; then
  fail=1
fi

# ── SOURCE GUARD: the call-audio guards of 0.9.79 stay where they are ─────────────────────
# See tests/check_call_audio.py. test_rtpwatch proves rtp_watch.h, but the fixes that USE it -
# newCall()'s per-call begin, END's gui.inCall() gate and its one way out of CallState::Error,
# F1/F2 kept off a call, the ring's pop-then-music-then-loudspeaker order, the loop yield held
# under a pop, the backstop held for the motor, Settings > Audio through music's stash, the
# **202## mic egg compiled out - live in files this suite cannot compile. A review reverted six
# of them in a scratch copy and the suite stayed green; each is a positive contract now.
echo "checking the call-audio guards (END, F1/F2, ring order, RTP silence, hot-mic backstop)"
if ! python3 tests/check_call_audio.py; then
  fail=1
fi

# ── SOURCE GUARD: a Game Boy game's exit puts the I2S ring back where the heap wants it ───
# See tests/check_gbc_ring.py. The game (re)installs the ring with its own 32 KB already in the heap,
# and best fit put it on top of them: at quit they freed below it and the phone's largest block
# stayed cut in two (63,716 -> 32,816). ~GbcApp now reinstalls it after the release and the
# shutdown, before WiFi; the order and Audio::reseatI2S()'s guards live in files this suite cannot
# compile, and I2S is installed nowhere but Audio::installI2S() (whose cache the reseat relies on).
# Also: the quit shuts the device down on soundStarted (a starved game leaves it on), and start() /
# shutdown() never reach IDF 3.3's NULL-dereferencing i2s_start()/i2s_stop() with no driver. Review 3
# (A2/A6): nor do turnOn(), Audio::loop(), a pop, the ring or a call's stream (a reinstall can fail with
# the device ON), and a ring installed with the device off is left stopped.
echo "checking the Game Boy's I2S reseat at quit (release, shutdown, reseat, WiFi; one installer; no-driver guards on every writer; a ring installed with the device off is stopped)"
if ! python3 tests/check_gbc_ring.py; then
  fail=1
fi

# ── SOURCE GUARD: the LoRa radio never waits for the air on the loop task ─────────────────
# See tests/check_mesh_tx.py. test_txq proves the queue; the guards that make it safe - no blocking
# send, healthCheck() true mid-frame, TxDone before the deadline, txPump() first in loop(), the
# background senders' idle gates - live in files this suite cannot compile. Since review M1-M3 it
# also pins "queued is not sent" where the UI words are chosen, the TX completion a game or the
# legacy upload runs, and the bench commands that must be safe to type on a daily phone.
echo "checking the mesh transmit guards (no blocking send, pump first, health mid-frame, queued != sent, safe bench)"
if ! python3 tests/check_mesh_tx.py; then
  fail=1
fi

# ── SOURCE GUARD: the KOSync home client never waits on a name; Books' re-reads keep warnings ─
# See tests/check_kosync_glue.py. test_kosync proves the pure half (the mDNS bytes, the lookup
# and fallback rules); both bugs 0.9.79 fixed were in the GLUE - resolveDomain() at the start of
# every home job, and the window's warnings cleared by the re-read Books makes on every entry.
# A review put both back in a scratch copy and the suite stayed green.
echo "checking the KOSync glue (no blocking lookup, only 'kosync reload' clears on asking)"
if ! python3 tests/check_kosync_glue.py; then
  fail=1
fi

# ── SOURCE GUARD: no sync carrier writes, applies, locates or names a reading position ────
# See tests/check_positions_write.py. The hard rule of every sync (docs/booksync-simple-plan.md): a
# carrier ends at the card, and only "Go there" on it - BooksApp::applyPending() - writes a place.
# golden_positions.h proves the parser; this proves who may call the writer, on comment-blanked
# source (kosync*.h mention positions.cbs on purpose). A new carrier file goes on its CARRIERS list.
echo "checking that only the sync card's OK writes a synced position (no carrier reaches the writer)"
if ! python3 tests/check_positions_write.py; then
  fail=1
fi

# ── SOURCE GUARD: the Now playing title is redrawn only on a track change, over cleared strips ──
# See tests/check_music_title.py. "Pulse" painted over "Space Explorer" after the side button's next
# (0.9.79, {user}'s first listening pass): the two title lines had no clear under the 1 Hz refresh.
echo "checking the Now playing title (drawn on a track change only, both strips cleared first)"
if ! python3 tests/check_music_title.py; then
  fail=1
fi

# ── SOURCE GUARD: the web flasher's two parts never cover nvs ────────────────────────────
# See tests/check_webflasher_split.py. Through 0.9.78 the one merged image, written from offset
# 0, erased every updater's NVS (mesh key, booksync passcode, KOSync memo, WiFi calibration).
echo "checking the web flasher split (nvs in neither part, the page flashes every part)"
if ! python3 tests/check_webflasher_split.py; then
  fail=1
fi

# ── SOURCE GUARD: the SIP message store never keeps a MESH clock's time (review SA-3) ─────
# See tests/check_msg_clock.py. test_clocksrc proves clockMsgFinal/clockMeshCorrS; the glue that
# makes a mesh stamp provisional - msgStamp() at every load and own stamp, the trusted-only
# finalise AFTER the sentinel repair and re-stamping only through clockMsgRestamp, the "tm" marks,
# the reload armed until NTP/GPS, the offset kept before the overwrite - lives in Storage.cpp,
# GUI.cpp, WiPhone.ino and clock.cpp.
echo "checking the message store's mesh-clock stamps (provisional until NTP/GPS)"
if ! python3 tests/check_msg_clock.py; then
  fail=1
fi

# ── SOURCE GUARD: HEALTH out of loop()'s frame; WiFi link lines gated to the card (F1, F3) ─
# See tests/check_health_log.py. The loop-task stack margin and the health.log's retention are
# both properties of WHERE code sits (a noinline helper; every card write asking WifiCardGate),
# which no host test can see.
echo "checking the HEALTH helper and the WiFi card gate"
if ! python3 tests/check_health_log.py; then
  fail=1
fi

# ── SOURCE GUARD: the 0.9.80 review's fixes that live in device files (2026-09-27) ────────
# See tests/check_almanac_review.py. test_almanac_lines/test_elevtext/test_tileplan/test_filepaths
# prove the pure halves; this pins where they are USED - the GPS motion taken only from an RMC, the
# Time offset save recorded for the DST reminder, ONE countdown for the Sun screen and `sun`, the
# resume's space check and the whole-layer record, `maps dl`'s parsed tail, the case-blind Files
# guard, the ruler's coarse climb and the Follow me sample in its own tick; and the second review's:
# the same-place tolerance (day slots, the Sun screen's cache), one thing a timer pass (a slice or
# its repaint, the gap counted from the slice's end, the band push), the poor fix refused / named,
# Follow me's one card read a fix, the scroll kept, the measured titles, the shared units row. And the
# weather's (0.9.81): no card, HTTP or TLS call in a key handler, a build or an entry (the cache and the
# fetch on the timer, Refresh only queues), Back's table grown, the loop's hooks on netRequestActive(),
# the fold waiting out a game, no insecure client, no URL or coordinate in a log line.
echo "checking the Almanac/altitude review fixes and the weather's in the device files (GPS motion, DST record, one countdown, no I/O in key handlers, ...)"
if ! python3 tests/check_almanac_review.py; then
  fail=1
fi

# ── SOURCE GUARD: the hotspot phone's GPS keeps its rules (0.9.82) ────────────────────────
# See tests/check_phonepos_review.py. test_selfpos proves self_pos.cpp; this pins the glue: no IP
# geolocation anywhere under WiPhone/, phone_pos.cpp never sets the clock or touches the woods
# plate's reader and never waits (MSG_DONTWAIT, O_NONBLOCK before the connect, a {0,0} select, no
# CPU hold), no coordinate/MAC/SSID in its log lines, the beacon's location_source/precision_bits,
# a bench fix never sent by the schedule nor to a public channel, serial `phonepos auto` refused
# while reporting goes to a public channel, and the real 2026-08-25 fix kept out of the tree.
echo "checking the phone-location glue (no IP geolocation, never the clock, never waits, no coordinates in logs, bench fixes stay home)"
if ! python3 tests/check_phonepos_review.py; then
  fail=1
fi

# ── SOURCE GUARD: the clock's time zone keeps its rules (0.9.82) ───────────────────────────
# See tests/check_tz_glue.py. test_tzauto proves tz_auto.cpp; this pins the glue: no SPIFFS write
# from the loop (the INI only when a person types an offset), Auto's own question over the slots
# and never under a game, never a bench fix, the boot order after the INI, the notice only to an
# awake screen, no coordinate past 2 decimals on the console, the ODbL credit on the screen.
echo "checking the time zone glue (no SPIFFS in the loop, never a bench fix, the boot order, the credit)"
if ! python3 tests/check_tz_glue.py; then
  fail=1
fi

# ── SOURCE GUARD: Wi-Fi location keeps its rules (0.9.82) ──────────────────────────────────
# See tests/check_geoloc_review.py. test_geoloc proves geoloc.cpp and test_selfpos the Wi-Fi slot;
# this pins the glue: no IP geolocation host and the IP fallback off twice ("considerIp":false and
# "ipf":false), no SSID in a body, Content-Type JSON, a generic User-Agent, ISRG X1 + X2 + Root YE
# pinned, the look inside netRequestActive() with the PSRAM hook first, no MAC/SSID/coordinate in
# its logs or `netloc`, the cable never choosing "map + mesh", the auto-switcher's copy before
# scanDelete and no hop/disconnect from a Wi-Fi location scan, the beacon sending the cell with the
# confirming scan's time and the precise-fix-wins rule, the time zone never taking a Wi-Fi fix,
# no automatic look beside the music; and (the 2026-10-10 review) "off" reaching a look already
# out, no scan beside a handshake nor a look beside a scan, a geo ask never a FULL switcher round,
# the jump anchor and the latest scan handed to the fold, the joined AP asked about again when a
# look starts, and "map + mesh" on the air only once the My node screen is left.
echo "checking the Wi-Fi location glue (no IP geolocation, the request, the roots, no MACs in logs, the switcher, the beacon, off, the guards)"
if ! python3 tests/check_geoloc_review.py; then
  fail=1
fi

# ── SOURCE GUARD: Menu > AI's current facts and patience (2026-10-11) ────────────────────
# See tests/check_ai_glue.py. test_gemini and test_wiki prove the pure halves; this pins the glue:
# 150 s past HTTPClient's uint16 through the client's own first-byte wait (armed only once the
# request is written, Cancel AND a call honoured, asleep between slices, stop() at the deadline,
# never the virtual connected()), the date from the TRUSTED clock built per request, the Wikipedia
# lookup (one GET, never retried, keywords only, the policy User-Agent, X1 + X2 + Root YE, 24 KB,
# a 20 s budget, lookup=off honoured, before the ladder's loop) and its background in the
# question's turn only, the sources line only with background, the answer scrubbed. Round 3: the
# "Retry if it times out" setting (NVS, absent = on) reaching the ladder with each attempt's `net`,
# a Cancel never retried, the lookup's walk back and wikiCompose, the heap bar before every Gemini
# handshake ("reboot first"), the budget's clamp right before the request and the client's reading
# guard after the first byte. And the key's home: the uploader (app_gbc_xfer.cpp) never stats,
# reads, resumes or lists a file in "/API Keys" (or a gemini.txt) - it writes them, fresh.
echo "checking the AI glue (150 s wait, a call ends it, the trusted date, the Wikipedia lookup, the timeout retry, the write-only key folder)"
if ! python3 tests/check_ai_glue.py; then
  fail=1
fi

# ── THE MAC-SIDE TILE CONVERTER, against what the phone actually reads ────────────────────
# tools/convert_tiles.py runs on a computer this suite will never see, and two of the three
# things it can get wrong are SILENT on the phone: a byte-swapped RGB565 tile still draws a
# recognisable map in wrong colours, and the sips/BMP fallback has three chances to be subtly
# off. See tests/check_convert_tiles.py. Pillow is optional.
echo "checking the tile converter against the firmware's pixel format"
if ! python3 tests/check_convert_tiles.py; then
  fail=1
fi

# ── THE COVEY PULL, against a fake COVEY in a temp dir ─────────────────────────────────────
# tools/covey_pull.py feeds the pool the phones are built from; the two ways it can go wrong
# silently are overwriting a master tile and pooling COVEY's STREAMED tiles as if they were
# downloaded. See tests/check_covey_pull.py (no ssh, no network).
echo "checking the COVEY pull"
if ! python3 tests/check_covey_pull.py; then
  fail=1
fi

exit "$fail"

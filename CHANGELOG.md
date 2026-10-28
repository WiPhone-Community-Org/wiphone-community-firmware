# Changelog

## 0.9.82-NH (unreleased) - map tiles from the Mac over WiFi, the phone's location from a hotspot, automatic time zones, Wi-Fi location, AI answers that know today

### AI: today's date, a Wikipedia lookup, longer waits, retry if it times out (0.9.82-NH)

{user} (2026-10-11): COVEY's Gemini said Tomb Raider: Legacy of Atlantis was "rumoured, may come out in 2025", and his Witcher 3
questions timed out. Measured that day on his free-tier key:
- Google Search grounding isn't free on the current models.
- The free url_context tool hung on Flash.
- Free Flash often takes 30-90 s and answers 503.

What changed:
- **The system instruction starts with today's date and time** (only from a trusted clock), and says the model's own knowledge
  is older than that. It never mentions a tool: doing so made Flash return MALFORMED_FUNCTION_CALL with no text.
- **Wikipedia background.** Before each question the phone makes ONE request to Wikipedia with the question's keywords only
  (stopwords and contractions removed, a short follow-up borrows the topic's last real question). It hands Gemini the intros of
  the top two relevant articles as labelled background, and shows "Wikipedia: ..." under the answer. If anything fails, the
  question goes ahead without it. `lookup=off` in gemini.txt turns it off. The pinned ISRG roots cover en.wikipedia.org.
  COVEY and the X4 Pro e-reader use byte-identical rules.
- **Longer waits.** The wait for Gemini's reply is now up to 150 s, in 200 ms slices that honour Cancel and an incoming call.
  HTTPClient's own 65 s ceiling (a uint16) is gone.
- **Retry if it times out** (AI menu, or `ai retry on|off`; on by default). With no answer in time, the question is asked once
  more, then once to Flash-Lite. Every HTTP 5xx is now "busy". MALFORMED_FUNCTION_CALL is retried once.
- **Key file privacy.** The uploader's resume check no longer says whether a key file exists or how long it is.
- **Proven 2026-10-11 on phone 1:** "has the new tomb raider legacy of atlantis come out yet" -> Wikipedia 2 pages in 5.6 s,
  Flash answered in 11.6 s total ("No ... an upcoming game revealed at The Game Awards 2025"). Internal heap min-ever dipped
  to 8.5 KB across the two handshakes: watch it. Both phones flashed.

{user}: the phones should get new OpenTopoMap tiles *"over WiFi from the Mac"* rather than by
opening them for the SD card.

- **`maps dlurl <template> [area]`**: the custom source (source 3, a plain-HTTP relay) can write
  into a real map's folder - `otm` fills the phone's OpenTopoMap map. Left out, the area is
  `custom`, as before. `elev` (any case) and any name ending in `.` are refused: on the FAT card
  both would open the altitude layer's folder or another map's under a different spelling. Only the
  folder changes: the OpenTopoMap z17 interval stays with OpenTopoMap's servers, and a custom job
  is still never saved for resuming. The form and status call it `Custom -> otm`, and `maps dl`
  says which relay and which map the run used (`custom source <template> into /maps/otm`).
- The Download form's time for the custom source is 1.2 s a tile (measured), not 0.6 (a guess).
- **`tools/tile_relay.py`**: serves the Mac's tile pool read-only on the home network (only
  `/<source>/<z>/<x>/<y>.png`; the Wi-Fi address only). Measured on phone 1: a z17 tile 124 ms from
  the Mac against 1,220 ms from OpenTopoMap; a 20 km z17 area at ~1.2 s a tile, roughly half the
  time it takes from OpenTopoMap (docs/maps.md, "Without opening a phone").
- The weather's User-Agent is built from the firmware version (it read "0.9.81" whatever ran).
- **`mv </from> </to>`** on the serial console renames one file in place (a FAT rename: nothing
  is copied or deleted). Quote paths that have spaces. It never overwrites, refuses folders, and
  waits until Books, Music, Files, Photos and the Game Boy are closed. It is for giving a book the
  same file name it has on another reader: KOSync's second document id is the md5 of the file
  name. Reading places are kept, because the position store keys on the book's epub ids.

**Phone location: a phone's GPS over its hotspot** (phone 1 has no GPS plate):

- **Meshtastic > My node > Phone location: auto** (new, on by default on a new phone). When the WiPhone has
  joined a phone's hotspot and that phone runs a GPS-sharing app, the WiPhone uses the phone's
  GPS as its own position. The apps: **gpsdRelay** (Android, F-Droid) or **Share GPS** (Android)
  set to port 10110, relaying the receiver's own NMEA; **GPS 2 IP** (iPhone) on its own port
  11123. Nothing is typed: the WiPhone asks the network's gateway (on a hotspot, the phone) on
  those ports, remembers the one that answers, and on a network where nothing answers (a home
  router) asks a few times and then once every 5 minutes. The line under the row says what is
  happening: "this network: fix, 9 sats", "no app found on this network", "no fix (indoors?)",
  "fix found, checking it", "fixes jump about: not used". `phonepos port <n>` adds a port of
  your own. **No IP-based location is used anywhere.**
- **Upgrading:** a phone whose position reporting was armed (an interval and a channel) while
  "GPS is off" held it, or aimed at a public channel, comes up with Phone location **off** - so
  the first hotspot does not start a beacon nobody re-confirmed. My node turns it on.
- **Who it trusts (read this):** whatever answers on the joined network's gateway. On your own
  hotspot that is your phone; on a hotel's, a campground's or a boat's network (marine NMEA
  gateways use 10110 too) it can be anything, and a device there serving well-formed NMEA can
  set the map's "me (phone)", the distances, the Almanac's place and - with reporting armed and
  no live fix from the plate - the position the beacon sends as a phone GPS fix. It can never
  set the clock or outrank the plate's own fix, and nothing is stored. Switch Phone location
  off on networks you do not control. (The first design paired it to one hotspot name; zero
  configuration traded that away, and a per-network confirm is the known next step.)
- **What counts as a fix:** the position, fix quality, satellites and HDOP all come from one GGA
  sentence (nothing is carried over from an older one). No fix, dead reckoning, manual and
  simulated positions are refused. Four satellites and HDOP 10 or better are needed to send it,
  as for the GPS. When an app leaves satellites and HDOP empty (GPS 2 IP can), an RMC of the same
  second has to say the fix is valid, and such a fix is **shown but never sent**: iOS builds it
  from CoreLocation, which can be a Wi-Fi/cell guess, or kilometres off on purpose with Precise
  Location off (docs/phone-location.md's GPS 2 IP step turns that on). GPS 2 IP fills the fields in instead:
  every GGA says 8 satellites and HDOP 0.9, whatever the fix is worth (its own NMEA docs). So that
  exact pair is read as "not said" at once on GPS 2 IP's port 11123, and after five GGAs running
  on any other port. The line says "iPhone app gives no accuracy: shown, not sent". The first
  2 s after each connect are ignored (gpsdRelay
  replays up to 30 old lines), and with a trusted clock a sentence more than 5 s off is dropped.
  Two fixes in a row must agree: gpsdRelay's *generated* sentences can print a coordinate
  kilometres off that still looks valid. They may be up to 10 s apart (the dead-link limit), so
  an app sending every 5-8 s works too. The phone's data never sets the clock.
- **Where it shows:** the map's "me (phone)" (grey "me (phone), N m ago" once it is old), the
  distances, the Almanac ("At phone GPS") and its weather, the Nodes self row ("here - phone
  fix, 9 sats") and Status ("From: phone (9 sats)"). The Almanac's fallback takes the newer
  *usable* of the two fixes, so a fresh poor phone fix indoors never hides a good older GPS one. The woods plate's GPS still comes first
  when both have a fix. Places > **Here (live fix)** (was "Here (GPS)") measures from either,
  never from the pin.
- **Position reporting** works from Phone location too: the Position and Send to rows show when
  either source is on, so phone 1 can switch reporting on from the screen at last. A beacon now
  says where its fix came from (Meshtastic `location_source`: the woods plate's GPS "internal",
  a phone's "external", full precision): 20 bytes, 559 ms on air (was 15 B, 518 ms). The pin
  announce is unchanged. A poor or old fix no longer spends the owed slot; it waits for a good one.
- **Other nodes' approximate positions:** a node that sends a reduced precision (stock channels
  can cut a position to a ~2.9 km cell) is drawn on the map with a dotted ring of that size, and
  its distance has a "~" in front (Nodes, the map's lists; `pos` says "approx +-N m").
- **Serial bench:** `pos` lists each source and why it is or is not used. `pos inject
  gps|phone <lat> <lon> [sats hdop]` and `pos inject clear` set a bench fix (RAM only, never
  sent by the schedule; `pos now` with one logs INJECTED and sends it as Meshtastic MANUAL with
  no precision, so no receiver takes it for a measurement). `phonepos` (state, port, bytes,
  sentences, bad checksums, stale lines, last fix age, connects), `phonepos auto|off`,
  `phonepos port <n>`, `phonepos at <ip>[:port]` (a Mac standing in for the phone; its fixes
  are bench fixes).
- **The cable never starts a location going out on a public channel:** `pos every <n>`, `gps
  on` and `phonepos auto` are refused while reporting is aimed at a public (or vanished)
  channel, and `pos now` never sends to a public channel at all - a public channel confirmed
  once survives "Position: off" on the screen, and a line on the cable is not that consent. **`tools/nmea_phone_stub.py`** is that Mac side: RMC+GGA at
  a dummy position, with switches for poor fixes, the stale burst, no line endings, a stall,
  gpsdRelay's generated sentences and GPS 2 IP's empty fields.
- **Privacy:** the beacon's log line and the first-fix line give coordinates to 2 decimals
  (about 1 km) now, and the phone-location lines give none (no SSID or address either). The
  console's `almanac` (at a measured place: no UTM/MGRS, 2 decimals; a typed place stays exact),
  `almanac bench`, the Almanac's day log and `elev`'s "you" line are rounded the same way. The
  real measured position that sat in two code comments is gone from the tree (git history
  still has it).
- **Phone 2's GPS plate: an old position no longer comes back as "live"** (found by the
  phone-location review). When the receiver's RMC sentences went missing (lost to line noise, or
  switched off), every "no fix" GGA after them stamped the last position, with the last
  satellite count and HDOP, as a fix taken that second - for as long as GGAs kept coming. The
  map, the Almanac, the position beacon and Auto time zone all took that old place as where you
  are, and it outranked a live fix from the phone's hotspot. Now only a sentence that itself
  carries a measured position counts: an RMC that says "valid" in a real measuring mode, or a
  GGA with a GPS fix quality and its own coordinates. Dead-reckoned, manual and simulated
  positions never count. A lost fix keeps its own time and goes old on schedule (30 s for
  sending, 2 minutes on screen). Satellites and HDOP come from the sentence itself (an RMC, which
  has none, borrows the GGA of the second before), and the altitude from the GGA that made the
  fix. An older rating is carried only when it is a bad one: a fix rated poor (fewer than 4
  satellites or HDOP over 10) stays poor until a GGA says better, even when GGAs go missing or
  leave their fields empty, and the Almanac judges "last GPS" by that same rating, not by the
  last sentence's satellite count. A receiver that sends only GGA
  sentences now gets a fix. With the plate's normal RMC + GGA every second nothing changes, and
  a lost fix still turns grey at once when the receiver reports 0 satellites. Host tests:
  `test_selfpos` (the stale re-stamp reproduced against the old rule: 22 checks failed, 0 now),
  `test_tzauto` (the old zone kept over the phone's live fix), `test_nmea`.


**Time zones: the clock follows a MEASURED position** (until now the clock was a fixed typed
offset with no DST rules: carried across the country it kept the old zone's time, and the
Almanac's sunrise, sunset and legal light came out hours off):

- **Settings > Time zone** (was "Time offset"). The first row: **Auto** (the default, the upgrade
  included), a **named zone** - Pacific, Mountain, Arizona, Central, Eastern, Atlantic,
  Newfoundland, Alaska, Hawaii, Saskatchewan, each with its own daylight-saving rule, so the
  clock changes by itself - or **Custom** (the typed offset, no DST: the old screen). The typed
  offset is the row below it; Custom uses it, and Auto uses it until a zone is found, so an
  upgraded phone reads exactly what it did until a measured fix says where it is. Under a ruler,
  what is in force now: `Eastern, UTC-4` / `From phone GPS` (`Set by hand`, `Typed offset, no
  DST`), the next change (`DST ends Sun Nov 1 (UTC-5)`), and Auto's last look (`Auto: near a time
  zone line`, `Auto: checking Central`). Save applies at once, no reboot: the status bar, the
  Almanac, the weather and message times all read the clock's offset when they draw.
- **What Auto trusts (the owner's rule: a wrong answer is worse than none - when unsure, change
  nothing).** Only a live fix by the beacon's 30 s bar - the woods plate's GPS first, then the
  hotspot phone's - with 4+ satellites and HDOP 10 or better when the fix says. **Never a bench
  fix** (`pos inject`, `phonepos at`), never the internet address (there is no IP location
  anywhere in the firmware), never a Wi-Fi guess. The zone comes from the zone map
  (timezone-boundary-builder 2026d, (c) OpenStreetMap contributors, ODbL 1.0 - credited on the
  screen): no answer within a map cell (at least 5.6 km north-south) of a change of clock, at sea
  or outside North America and the Caribbean. The same new zone must be seen on two looks at
  least 2 minutes apart (every 30 s; anything else between them starts over), so two separate
  fixes agree before anything changes. A zone with the same clock as the one in use (Detroit and
  Windsor) is not a change.
- **A phone's fix counts, but only well inside a zone.** GPS 2 IP gives no accuracy, and even a
  phone fix with good numbers can be kilometres off (GPS 2 IP's stand-in 8 satellites / HDOP 0.9
  off its own port; gpsdRelay's generated sentences, 2-17 km off near every whole arc-minute). A
  town-sized error does not change a time zone, but near a line it could. So a phone fix, from
  any app, sets the zone only when nothing within 10 km has another clock (measured: 15 km also
  refused Honolulu, San Juan and Bermuda, where the edge of territorial waters is near; 10 km
  still refuses Knox IN, the Hopi villages, Victoria BC, Atikokan and Dawson Creek). The woods
  plate's own GPS is metres off and only has to be clear of the line itself. Not covered, and
  written down as accepted: gpsdRelay with generation on (the setup says to turn it off), the
  phone standing still within ~2 m of a whole arc-minute, 12-17 km from a line.
- **Back to Auto never brings back an old zone.** While the setting is a named zone or Custom,
  Auto looks at nothing, so the zone it found before says where the phone *was*. Choosing Auto
  again starts from what the clock already shows: from a named zone, that zone (`Set by hand,
  Auto checks it`, its DST still running); from Custom, the typed offset. The clock does not move
  at the Save; two looks that find another clock change it, with the banner.
- **Told once, kept across a reboot:** a zone change shows a banner when the screen is next awake
  (`Time zone: Eastern` / `Now UTC-4, from phone GPS`; never over a game). The zone is saved by
  name in NVS, so daylight saving keeps changing at the right second after a reboot with no fix.
  The offset is re-computed once a second and the moment the clock is set; a DST zone waits for
  the clock (nothing shows a time before it).
- **Who it trusts, again:** Phone location's warning applies here too - a device on a hotel's or
  a boat's network that serves well-formed NMEA can set the zone (only to a real zone at the
  place it claims, after two looks, and the banner says so). Choose a named zone on networks you
  do not control.
- **The Almanac:** with a zone (named or Auto), daylight saving is the zone's own rule, exact to
  the second (Newfoundland's -3:30 too, which the old offset guess could not do), and the
  reminders say it happens by itself: `Clocks go back 1 h Sun Nov 1 - the phone changes by
  itself`, `Clocks went back 1 h at 2 AM today - the phone changed by itself`. Its Settings row
  reads `Daylight saving: automatic` (`none here` for Arizona and Hawaii); the old "US daylight
  saving" switch still applies to a typed offset. With a typed offset everything is as before.
- **Where things live:** NVS `wptime` (mode, named zone, typed offset, Auto's zone by IANA name).
  configs.ini's `[time] zone` keeps the TYPED offset and is written only when you type one (never
  from the loop: SPIFFS is ~1.6 s to open) - an older firmware flashed over this one comes up on
  the typed offset, as before; a typed offset changed under another firmware wins at the next boot.
  NVS remembers what it last saw in the file, so if the file write fails the typed offset is kept
  in NVS and still wins at the next boot (the file is not mistaken for a newer edit).
- **Serial:** `tz` (setting, zone, offset, source, Auto's last look and why, the next change - never
  a coordinate), `tz auto`, `tz custom [offset]`, `tz <zone>` (pacific ... saskatchewan), `tz test
  <lat> <lon>` (the lookup alone: zone, border or not covered, the offset, whether a phone fix
  there would count; changes nothing), `tz forget`. `clock` names the zone too.
- **Size:** the zone map is 81.5 KB of flash once used (firmware 46.9 % of the app partition);
  RAM unchanged.
- Host tests: `test_tzauto` (5,048 checks: the decision through the real position slots, the 10 km
  margin against brute-force sampling and for rated phone fixes too, every zoneinfo offset of
  2026-2030 through a named and an Auto zone, reboot memory, the manual override and the way back
  to Auto, every Settings line measured in the firmware's font), `test_almanac_lines` +21 (a zoned
  clock), and `tests/check_tz_glue.py` (the glue - no SPIFFS in the loop, back to Auto, the look's
  own millis(), a failed INI write never undone at boot, the soft key's word - with 22 mutations
  caught).

**Wi-Fi location: beaconDB, believed only when two lookups agree** ({user}'s rule: own GPS, then the
phone's GPS over its hotspot, then a Wi-Fi location only when it is genuinely local and accurate,
otherwise nothing; never an internet-address guess; a wrong location is worse than none):

- **Meshtastic > My node > Wi-Fi location: off | map only | map + mesh** (NVS `wpmesh/netloc`,
  **off** by default). Its line says what turning it on does: `if on: sends Wi-Fi IDs to
  beaconDB`. On, it says what came of it: `+/-60 m, 3 min ago, map only`, `not enough known
  Wi-Fi here`, `too few Wi-Fi networks (2 of 4)`, `lookups disagree: not used`, `used while Maps
  or Almanac is open`. The serial console may set only `off` or `map`: **map + mesh** puts a
  position on other people's maps and is chosen on the screen.
- **What is sent:** to `api.beacondb.net`, the BSSIDs and signal strengths of up to 20 access
  points per request - no SSIDs, a generic `WiPhone-NH` User-Agent with no version, Content-Type
  JSON, `"considerIp":false` and `"fallbacks":{"ipf":false}` (beaconDB's own IP fallback off
  twice), and any answer carrying a `fallback` key is refused. Never sent: hidden and
  `_nomap`/`_optout` networks, locally administered (randomised, most phone hotspots), group,
  all-zero and VRRP addresses, the joined network when it is a phone's hotspot (a location app
  answered on it) or locally administered, and anything under -85 dBm. TLS is pinned to ISRG
  Root X1 + X2 + the self-signed Root YE (beaconDB serves leaf <- YE1 <- Root YE <- X2 <- X1;
  Root YE second-sourced by its key like Root YR - `isrg_roots.h`).
- **The two-half agreement (the review's item 1):** beaconDB's answer is a weighted average of
  the access points it knows, and its accuracy is how far each is usually heard - one access
  point that moved drags the answer kilometres while it still reads ~50 m. So at least 4
  separate devices (a router's several BSSIDs count once) at -85 dBm or stronger are dealt into
  two halves by signal rank and asked about separately; both must be HTTP 200 with no fallback,
  each 1-100 m, and within the larger claim of each other. The result is the midpoint at
  max(50 m, either claim, their distance). The X4 Pro e-reader runs the same protocol.
- **When it looks:** no live GPS or phone fix; the position wanted (Maps or the Almanac on screen
  in the last 10 minutes, a beacon armed with map + mesh, or `netloc now`); the access points
  changed, never looked on this network, or the fix ran out; 10 minutes since the last look (2
  for `netloc now`), at most 3 an hour; the weather's gates (no download, uploader, sync window,
  call, game, Files job); no music playing (`netloc now` pauses it). A look needs a scan of this
  network under 2 minutes old: it asks the auto-switcher for one, at most every 2 minutes, through
  the same heap gate and skips as its own scans.
- **The auto-switcher (the review's item 8):** a Wi-Fi location scan with auto-switch **off** is a
  scan and nothing else - no hop, never the disconnected branch (its `WiFi.disconnect()` and the
  deaf-radio bounce), and its result does not count as the switcher's join evidence. While a
  phone's NMEA stream is live on the hotspot, auto-switch does not hop away from it. A scan in
  flight is always polled to its end and a round only pending is called off, so `scanBusy()` can
  never hold the rejoin loop off. The snapshot (BSSID, RSSI, flags) is copied with
  `WiFi.getScanInfoByIndex()` before `scanDelete()`; nothing is copied while the setting is off.
- **Freshness (the review's item 2):** a Wi-Fi fix is true only while its access points are still
  heard. Every later scan that still hears 2/3 of its devices confirms it; one that does not
  drops it at once. Shown 12 minutes after the last confirming scan, then gone (no grey "last
  Wi-Fi"); a scan is asked for at 10.5 minutes if the switcher's own is late.
- **On the mesh (map + mesh only):** after two agreeing scans at least 60 s apart, and only when a
  confirming scan finished within the 60 s before the send - an owed report asks for that scan
  and sends on it. Sent as a 17-bit cell (~182 m, the point moved to the cell's middle) with no
  `location_source`, field 4 = the confirming scan's trusted UTC. The last cell is kept while a
  new fix lies within the accuracy of the one that produced it (no flip-flop at a cell edge), and
  no Wi-Fi report goes out while a GPS/phone report of the last hour lies within its accuracy +
  50 m (a precise dot is never replaced by a coarse cell). The public-channel rules are as
  before. The time zone never takes a Wi-Fi fix.
- **Where it shows:** the map's `me +/-60 m Wi-Fi` with a dotted ring at that size (`0` centres:
  "Centred on the Wi-Fi location, +/-60 m"), distances (`From: Wi-Fi +/-60 m`; Places > Here
  includes it, named with its size), the Almanac (`At Wi-Fi +/-60 m`, `Source: Wi-Fi +/-60 m`)
  and its weather, the Nodes self row (`here - Wi-Fi +/-60 m (beaconDB)`), `pos`.
- **Serial:** `netloc` (the setting, the last scan's counts, the last look's verdict and accuracy,
  the fix, each half's HTTP code and handshake time - never a MAC or a coordinate), `netloc
  off|map`, `netloc now`, `netloc answer <A> <B>` (two canned halves through the same rule; the
  fix is a bench fix, never sent by the schedule).
- **Code:** `geoloc.{h,cpp}` (pure: the filters, the bodies, the parser on `json_read.h`, the
  agreement, when to look, the freshness), the Wi-Fi slot's rules in `self_pos.{h,cpp}`,
  `geoloc_net.{h,cpp}` (the glue: a third job kind on the shared HTTPS worker, inside
  `netRequestActive()`), the auto-switcher's modes in `Networks.cpp`. PSRAM for the snapshot and
  the request bodies (~3.9 KB).
- **What two agreeing halves cannot see (the second review: 12 findings, two of them the same, all fixed):**
  - *The jump check* (the first review's item 1 kept it as a backstop): routers that moved
    together agree perfectly, at the old address. An answer that could not be reached from this
    boot's newest GPS/phone fix of the last 6 h at 300 m/s (beyond its accuracy + 50 m) is refused:
    `far from your last fix: not used`. A bench fold is held to bench fixes too (`pos inject`).
  - *A scan during the lookup:* a look queued behind a weather fetch can wait ~40 s; a scan that
    finished meanwhile and no longer hears 2/3 of the look's devices refuses the answer (`moved
    during the lookup`); one that still hears them confirms it.
  - *The WiFi link:* a drop or a new network takes the fix off the map at once; the next scan
    brings it back or drops it (`WiFi changed: waiting for a scan`). With Maps or the Almanac open
    a live fix is re-checked by a scan every 2 minutes, so a fix carried off on a hotspot that
    stays connected goes within ~2 minutes, not 10.
  - *Expired is gone:* a fix 12 minutes past its last confirming scan is cleared, not left to age
    (its age is `millis()`-based and would read fresh again after 49.7 days on a charger).
  - *The phone hotspot's BSSID:* "its gateway served NMEA" is now sticky for the association
    (it was the probe's remembered port, which three refused reconnects clear), and the joined
    access point is asked about again when a look starts; a look waits 15 s after a join so the
    phone's app can answer first.
  - *Off means off:* switching it off reaches a look that is queued or out - the worker checks
    before each half, the request in flight is cancelled, and nothing is kept (a half that went out
    still counts against the hour's cap). The request bodies are wiped after every fold.
  - *map + mesh starts when the My node screen is left* (`mesh starts when you leave here`): the
    row's way from map only to off passes through it, and an owed report could send a Wi-Fi cell
    between the two taps.
  - *Scans:* a Wi-Fi location scan never starts while an AI question, a weather fetch or a look is
    connecting (a scan's allocation inside a handshake's heap dip is the 2026-08-28 reboot), and a
    look never starts while a scan is running. With auto-switch on, a scan Wi-Fi location asks for
    is a scan only - no hop and no `/networks.ini` reload (a second or more of SPIFFS on the loop
    task per ask); the switcher's own 10-minute round still evaluates on time.
  - *Names:* the map's height line and its lists name a Wi-Fi "you" with its size (`35m below
    Wi-Fi +/-60 m`, `nearest first, from Wi-Fi +/-60 m`); `maps` prints the saved view to 2
    decimals.
- Host tests: `test_geoloc` (278 checks: the e-reader's beaconDB vectors minus anything
  IP, the schedule, the bench fold, the jump check, a scan during the look, the held fix, expiry,
  when a scan is asked for, a cancelled look, the three roots by SHA-256, every row line measured
  in the firmware's font), `test_selfpos` (the Wi-Fi slot), `test_tzauto` (a live Wi-Fi fix
  refused), `test_almanac_lines` (the Wi-Fi place), and `tests/check_geoloc_review.py` (the glue:
  no IP lookup, the request, the roots, no MAC in a log, the cable never choosing mesh, the
  switcher, the beacon's cell and time, the music, off reaching the worker, no scan beside a
  handshake, the fold's guard, map + mesh committed on leaving the screen - 13 contracts, 36
  mutations caught).

## 0.9.81-NH (2026-10-04) - Menu > AI (Gemini), the API Keys folder, weather on the Almanac

{user}: *"I'd like to add a similar ai terminal to my wiphones ... I'd want the API to be inputable
for other users in a sort of easy way"*, *"Can you make an API key folder on each device in root"*,
then *"I prefer some thinking and then just a longer answer limit"* and *"I'll let you decide how ai
chat history should be saved or reset"*. Modelled on the Gemini app of the T-UI T-Deck firmware
(same `gemini.txt`), without its shortcuts: the key in the URL, no certificate check, the error
body thrown away, one `strstr` for the answer, sticky fallbacks, no history.

- **Menu > AI** (new, with its own icon): ask Gemini from the keypad (predictive text, 500
  characters; Ask = the left soft key or Call, End cancels, OK never sends a half-typed question).
  The answer is paged like the Files viewer, with the model that wrote it under it. A status line
  says the stage and the seconds; Back leaves and the question carries on.
- **The key is the owner's own**, in `gemini.txt` (`key=...`, optional `model=`, `thinking=`,
  `fallback=`) in **`/API Keys`**, which the phone makes at boot (or `/gemini.txt`, where the T-Deck
  keeps it). Read on every open and before every question; a failed card read keeps the last good
  key rather than becoming "no key". Key info explains aistudio.google.com and offers "Add key file
  over WiFi" (the uploader into `/API Keys`, with the warning that the phone's own hotspot is open).
  Also `up on keys` + `tools/wiphone_send.py --app keys` (over WiFi; a key file is always sent whole).
- **A key on the wrong line is never a model name**: `model=` and `fallback=` take only
  `[a-z0-9][a-z0-9.-]*` (an optional `models/` first) that is not the key itself. Anything else is
  dropped when the file is read - the default model is used and Key info / `ai` say "model= line
  ignored (not a model name)" - so it is never sent in the URL, drawn or printed.
- **Thinking, bounded**: `gemini-flash-latest` with `thinkingBudget` 1024 inside `maxOutputTokens`
  4096 (measured: with a small cap the thoughts ate the whole budget and the answer was empty).
  `gemini-flash-lite-latest` rejects thinkingConfig (400) and is never sent one.
- **The free tier's daily limit** (20 Flash answers a day, measured): a 429 naming a PerDay quota
  is not retried - the phone asks Flash-Lite at once and remembers "Flash used up" until Google's
  limit lifts (`/ai/state.txt`, from Google's own retryDelay), so later questions go straight
  there. The notice gives Google's deadline in local time ("using Flash-Lite until about 17:00",
  "tomorrow" when it is), "for about 4 h" without a trusted clock, "for now" when Google gave no
  time - never a time the phone made up. A
  per-minute 429 is waited out once; a 503 is retried twice 2.5 s apart, then Flash-Lite with its
  own two. Every new question starts on the configured model again. Each answer records its model.
- **A question gets the core** (measured on phone 1, 2026-10-03): the TLS handshake to Google is
  ~4.5 s of software ECC on the AI worker, and Google resets a handshake that takes over ~10 s
  (`start_ssl_client: -80`). With the 1-tick main loop sharing the core it took 5.6-10 s and failed
  3 of 3 with the screen on; now the loop sleeps 10 ms a pass while a question is in flight -
  3 of 3 answered, 3.9-7.6 s. A playing track **pauses** for the question ({user}: *"make music turn
  off when doing ai"*): with music decoding beside it the handshake still failed; Play in Music
  carries on where it stopped.
- **The chat is saved** (`/ai/chat.txt`: the last 20 exchanges or 16 KB, whichever is smaller; a
  file that is not whole loads as an empty chat, never half of one). Only the **current topic** is
  sent: the last 4 exchanges since a topic break - automatic after two hours (on an NTP or GPS clock), or Menu > New topic.
  Failed questions stay in the transcript, marked, and are never sent. Menu > Clear chat deletes it.
- **TLS with Google's roots pinned** (GTS Root R1 + R4, built in; no override file), the key in
  an `x-goog-api-key` header, the chunked answer de-chunked into PSRAM, on a persistent worker of
  its own with mbedTLS in PSRAM (tile_fetch's hook, now exported). Refused - with the reason on
  screen - without WiFi, during a call or the minute after, a game, a map download, the uploader,
  a sync window, a Files folder job, or with the internal heap under the handshake's bar. The
  request holds the CPU at full speed (160 MHz with WiFi on).
- **The key is never shown**: the Files viewer refuses anything in an `API Keys` folder (and any
  `gemini.txt`), Books never lists `gemini.txt`, `tools/card_clone.sh` leaves keys off a cloned card,
  and the serial `ai` prints the key's length only. Google's error messages are kept (shortened) and
  scrubbed of the key - a 400 is never just called a bad key.
- Serial: `ai`, `ai ask <question>`, `ai last`, `ai topic`, `ai clear`, `ai reload`, `open ai`,
  `up on keys`. Host tests: `tests/test_gemini.cpp` (the key file, the body, Google's answers in the
  shapes measured live, the chunked body, the ladder with the daily limit, the saved chat and its
  topics; fake keys only).

### Weather on the Almanac (0.9.81-NH)

{user}: *"weather info from a free source on the almanacs of each device?"* -> (Open-Meteo + NWS
alerts, cached for offline) -> *"Yes and yes! I like where you are thinking!"* One spec for both
Almanacs (2026-10-03); COVEY gets the same screen from the same rules.

- **Almanac → Weather...** (a new screen, key 107; Refresh / Back): alerts first, then now, the
  next day of hours (every 3 h, past ones dropped), seven days labelled by their MIDPOINT on the
  phone's clock (`daily.time` is the place's midnight - a phone an hour behind it would show
  yesterday's high), the as-of line and the credit. *Now* comes from Open-Meteo's `current` only
  within an hour of the fetch; then it is the hourly entry, labelled *Forecast for 14:00*. Past
  the last day: *No forecast after Fri Oct 9 - refresh on Wi-Fi*. A warning when the place's
  offset is not the phone's (*Local time here is UTC-7 - check the clock setting*); no clock is
  ever set from a response. TODAY carries the most severe alert in force near the top (in its
  colour) and a summary row over the entries: *Weather: 52F, rain 60% this afternoon* within an
  hour of the fetch, *Forecast: ...* after it (*Forecast 12 km away: ...* from elsewhere). A fetch of
  unknown time is never *Now*; alerts held without a forecast carry their own *Alerts as of*.
- **The alerts' four answers**, never a bare "No alerts": *No alerts at 07:10* (an empty 200 - an
  offshore marine zone answers that too); *Alerts: US only (NWS)* (the 400 "out of bounds", and
  only that 400); *Alerts not checked (no answer)* keeping the last ones with their own as-of; and
  *Alerts: 9+ (too many to list)* when a body outgrows the 64 KB buffer - an empty answer is ~230 B,
  so that size proves alerts exist, and a streaming counter that sees every byte gives the number.
  `&status=actual` (a "Test" is never a warning); urgency "Past" and cancellations dropped; most
  severe first; shown until `ends` (`expires` is the MESSAGE's - it can fall before onset). Alert
  rows are coloured on every wrapped row: yellow (Moderate, Minor) through MenuWidget's second
  style, red (Severe, Extreme) through a row of its own.
- **One canonical cache**, `/wx/weather.txt` (`wiphone-wx 1` ... `end`, SI units; tmp + rename;
  a file it cannot read set aside as `weather.bad`): the rounded point and the place source, the
  fetch time (the trusted clock, else the HTTP Date header, else NWS's `updated` - never Open-Meteo's
  `current.time`, a 15-minute model slot), each host's status, 48 hours, 7 days and the alerts.
  The forecast is replaced only when Open-Meteo's body parses (a captive portal's HTML 200 never
  does); an alerts failure keeps the old alerts - unless the place moved over 5 km. The alerts
  keep their OWN point (at the end of the `alerts` line; a file without it loads with the
  forecast's): a fetch 127 km on whose forecast failed but whose alerts answered holds THAT
  place's alerts beside the old forecast, and each says its own distance (*Alert 127 km away:
  ...*, *Alerts for a place 127 km from here*, *Forecast for a place ...*).
- **When**: on opening the Almanac (and Weather) with WiFi up, once the day's core has landed, when
  the forecast is over an hour old or over 5 km away; 10 minutes after a FAILED try (stamped before
  the request) before the next automatic one; never automatically while music plays. **Refresh**
  fetches now and pauses the music, as an AI question does. The card read and the fetch run on the
  Almanac's timer - the key only queues - and the result is folded on the loop (`wxLoopTick`) even if
  the Almanac was left, never while a game owns the SPI bus.
- **One HTTPS worker for the AI and the weather** (`https_worker.cpp`): the AI's worker generalised
  into job kinds on one 8 KB stack and one request helper (DNS first, a FRESH WiFiClientSecure per
  host, the body de-chunked into a shared 64 KB PSRAM sink and read to its end). The AI's job is
  the AI's as it was - its ladder, key wipe, music pause and gates; the two queue behind each other,
  never two handshakes at once (a question asked during a weather fetch waits "Waiting for the
  weather fetch..." and the worker checks aiAsk's gates and the heap bar when its turn comes). The weather's GETs
  connect FIRST - after `http.begin()`, which would otherwise stop the connected client and make
  `sendRequest` handshake again - so each host costs ONE handshake, timed alone (`wx`); they
  re-check the 10 KB / 14 KB internal heap bar before EACH handshake, and the worker asks aiAsk's
  gates again before each host (a job queued behind the AI can meet a call or a download).
  Serial `wx` refuses while a game owns the card. `netRequestActive()` replaces `aiRequestActive()` in the loop's
  busy, hardBusy (160 MHz) and 10-tick sleep.
- **P-256 first in every ClientHello** (`-Wl,--wrap=mbedtls_ecp_grp_id_list`, `tls_curves.cpp`):
  mbedTLS 2.16 offered P-521 first, and api.open-meteo.com takes the client's first curve - a
  software P-521 ECDHE is ~6-9x a P-256 one. The wrap lists exactly the 11 curves of the linked
  table (secp256r1, secp384r1, secp521r1, then the rest): this build's ClientHello writer returns
  MBEDTLS_ERR_SSL_BAD_CONFIG on any id it does not know (read off ssl_cli.o), which would fail EVERY
  handshake. Global: the AI (Google picks P-256 anyway), the map downloader and OTA see it too.
- **Roots**: ISRG Root X1 (moved out of ota.cpp into `isrg_roots.h`, the same bytes, re-hashed
  96:BC:EC...DF:08:C6) + the self-signed **ISRG Root YR** (E5:7B:7E...A8:6F), second-sourced by its
  key: it equals the X1-signed Root YR cross certificate both hosts serve today. Both hosts serve
  leaf <- YR1/YR2 <- Root YR (cross by X1); Root YR covers the day they drop the cross-sign. No
  override file. The test suite re-hashes both PEMs.
- **Units**: US now also means F, mph, inHg and inches: `unitsFmtTemp` ("52F", never "-0"),
  `unitsFmtWind` (whole), `unitsFmtPressure`, `unitsFmtPrecip` ("<0.1 mm" for a trace); the shared
  Settings / Maps row reads **"Units: US (ft, mi, mph, F)"**.
- **json_read.cpp**: gemini.cpp's no-allocation reader, moved unchanged, plus numbers and `null`.
- **Privacy**: no URL or coordinate is ever logged - one line a fetch (`weather: ok 200/200 4.1 s`,
  `weather: failed tls`); `wx` prints the rounded point the cache holds.
- Serial: `wx` (cache age, place source, rounded point, each host's code / bytes / handshake ms,
  the worker's stack floor, the heap floors, the policy), `wx fetch` (Refresh's path), `wx show`
  (the screen's rows through the same builder, colours tagged), `wx clear`, `open weather`.
- Host tests: `tests/test_weather.cpp` (224 checks - the parsers on fixtures captured at public
  points only (Seattle, offshore 45,-130) with the echoed point scrubbed and SYNTHETIC alert bodies,
  nulls, a chunked body byte by byte, the 400 out of bounds, an empty 200, a Test filtered, a null
  `ends`, a 14.7 KB two-alert body, an HTML 200, an overflow; the midpoint across the Nov 1 change
  and a phone-zone/place-zone mismatch; the cache round trip and every corrupt file; the fetch policy
  over thousands of frames; every row measured against Akrobat Bold 20 with all 111 NWS event names;
  the curve order; the roots' SHA-256), `test_units` (the new formatters), and
  `tests/check_almanac_review.py` (no card, HTTP or TLS call in the Almanac's key handlers, builds
  or entries; the loop's hooks on netRequestActive; no insecure client; no URL in a log).

## 0.9.80-NH (2026-10-01) - the Almanac, altitude on the map, metric/US units, elevation downloads

{user}: *"expand that app ... moon cycle, date, altitude and so on (and whatever else you can think of)"*,
*"altitude measurement on map center ... relative to where I am"*, *"also put this on covey too as an app"*.
COVEY got the same app the same day (its D-164) - both devices compute from ONE spec, `docs/almanac.md`,
and are tested against ONE vector file, `tests/vectors_almanac.{h,json}` (PyEphem, NOAA's WMM2025 test
values, utm + mgrs), and COVEY's `tools/almanac_crosscheck.py` proves its Python port gives the phone's
C++ answers to the second.

- **Menu > Almanac** (new, with its own icon). TODAY: the legal-light countdown, the legal window, sun,
  moon phase, the solunar rating and the next period, the place. SUN: legal light, sunrise, solar noon,
  sunset, civil twilight, day length and its change, the sun's altitude/azimuth now. MOON: rise, overhead,
  set, underfoot, phase, illumination, age, the next four quarters, the moon now. SOLUNAR: J. A. Knight's
  major/minor periods and a 0-4 rating (labelled as folk tables). POSITION: source and age, lat/lon,
  degrees + minutes, UTM, MGRS, ground elevation from the tiles, GPS altitude, speed and course (the NMEA
  reader now parses RMC speed/course), satellites/HDOP, **magnetic declination** (NOAA WMM2025, valid to
  2030 - North Bend 14.8 E). DATE: day of year, ISO week, the clock's source, the US clock-change reminder,
  the next equinox/solstice. Left/Right = another day (+-365).
- **Legal light defaults to Washington's big-game rule** (sunrise - 30 min to sunset + 30 min); civil
  twilight is a setting. At 47 N civil twilight runs 31-41 min (31 at the equinoxes, 36 in December, 41 in
  June) - always earlier AND later than the law. Legal times round INWARD (first light up, last light down) so the minute shown is always legal. The
  Meshtastic "Sun & legal light" screen and the serial `sun` use the same rule and code (sun_times.cpp is
  gone; its independent checks moved to `test_sun_astro`).
- **Days past a US clock change are shown in the offset they will have** ("Times in UTC-8 (after the Nov 1
  change)"): stepping from Oct 27 to Nov 7 used to show legal light ending an hour late. New setting "US
  daylight saving: yes / no (HI, AZ)". The phone's clock itself is unchanged (a fixed offset).
- **Place:** GPS / pin / chosen waypoint as before, then the last GPS fix of this boot, then the map's saved
  view - each labelled.
- **Cost, measured on phone 2 (160 MHz):** the day's tables are ~154 ms of software-double maths, now cut
  into slices of <= ~28 ms on the app timer (it was one 148 ms block in the key handler plus a 174 ms
  rebuild - a 334 ms LOOP STALL); the minute tick rebuilds in ~10 ms from saved results.
- **Altitude on the map:** a strip row "Elev 1,352ft, 394ft above you" - the ground under the crosshair
  against the ground under you (GPS / pin), both from elevation tiles, never the GPS's own altitude. The
  ruler says the climb. Menu: "Altitude: ON/off". Tiles: `/maps/elev/<z>/<x>/<y>.elv` (int16 metres; z13
  ~13 m + a coarse z10 layer; `elev` is a RESERVED name, never a map area), built on the Mac by
  `tools/make_elev_tiles.py` from the AWS Open Data Terrain Tiles (USGS 3DEP / SRTM; ~1 m against USGS's
  own point service on flat ground). `tools/cardday.sh` copies them (step 4).
- **The phone downloads elevation too:** "Elevation too: ON" on the download form fetches the area's
  terrain tiles over HTTPS with the map tiles (proven on phone 2: Mount Rainier's summit reads 4,391.2 m,
  identical to the Mac-built tile; true height 4,392 m).
- **Units: metric / US** (Maps menu and Almanac settings; `units` over the cable): scale bar, distances,
  heights, speed. Metric output is byte-identical to before.
- Serial: `almanac [lat,lon] [+-N]`, `almanac cost`, `almanac bench`, `elev`, `elev <lat> <lon>`,
  `units`, `maps dl ... [elev 1|0]`, `open almanac`.
- Fixed on the way: the downloader's HTTP client wrote into freed memory once per run (it closed a TLS
  client after deleting it).

## 0.9.79-NH (2026-09-26) - reading-position sync over KOSync (Xteink X4, KOReader, COVEY), music rebuilt, the web installer keeps your settings

**The version is `0.9.79-NH` from here on** ({user}: "add something to differentiate it from the default
firmware"): the `-NH` suffix shows on the splash (with a second line, "Custom firmware by {user} H."),
in Settings, in `ver` and in the installer's manifest. A suffix, not a prefix, because the updater's
`compVersions()` reads `%u.%u.%u` and ignores what follows — a prefix would parse as 0.0.0 and make
every server version look newer. Never `rc` or `db` in the tag: `compVersions()` gives those letters
meanings.

{user}: *"push 'sync location' on the device I was just reading and have it ready when I open it
on another device"* — and, the one hard rule: *"we can't let it screw up the position people are
at in their own wiphones after they update"*.

**Off unless `/books/kosync.txt` exists.** Without it, nothing changes: "Sync my place" is the
LoRa booksync send it always was. With it (key=value lines; `#` comments go on their OWN lines):

```
# The one account all your devices share.
user=<your KOSync user>
# 12+ characters, kept only as its MD5. Or key=<32 hex> instead.
password=<12+ characters>
# Optional: a KOSync server on your WiFi (COVEY: 8088); http only. A name (covey.local, or
# just covey) follows COVEY to a new address; an IP address works too.
home=covey.local:8088
# Optional: default "WiPhone-" + Sync settings > This device. Give EACH phone its own.
device=WiPhone-Sam
# Optional: closing a book opens a window + sends home (only if the place moved).
auto=on
# Optional: opening a book off WiFi opens a 60 s window.
open_window=on
# Optional: the sync hotspot is WPA2 with this password (8-63 characters).
hotspot_pass=<8-63 characters>
```

A comment at the END of a line (`auto=on   # ...`) is cut off on `user`, `home`, `device`, `auto`
and `open_window`; on `password`, `key` and `hotspot_pass` it is REFUSED and the reason shown
(`password= has ' #' - put comments on their own line`), because `#` is a legal password
character and a guessed-wrong MD5 is a 401 on every exchange with nothing saying why. A `#` with
no space before it (`pass#word`) is part of the value.

- **"Sync my place" opens a 5-minute sync WINDOW**: the phone serves KOSync for that one book,
  on its own open hotspot **WiPhone-Books (http://192.168.4.1)** when it is not on WiFi, or on
  its WiFi address when it is (and then also sends the place to `home=`). Point an X4's KOReader
  Sync at `http://192.168.4.1` with the same user/password. The window closes 10 s after a PUT
  for the book, 120 s after a GET (a stock X4 may be waiting for you to choose before it sends
  anything), or at the deadline. The reader menu shows `Window open, 5 min left - WiPhone-Books
  192.168.4.1`, then `Sent 61% to home` / `Picked up by CrossPoint`. The window does NOT keep
  the screen on or the CPU at 240 MHz — it serves with the screen dark, at 80 MHz — and it ends
  when a Game Boy game starts or Settings > WiFi is opened (and cannot open under either).
- **A place sent TO the phone arrives as the ordinary sync card** ("CrossPoint says: they are at
  61% (48% here)" — the X4's own number, then this reader's for the same place) with its undo,
  arming and backwards warning. It is parked in the booksync inbox as a locally-signed record,
  so none of that code knows KOSync exists, and a new KOSync offer for a book REPLACES that
  book's previous one, so other books' LoRa positions are never pushed out of the inbox.
- **Opening a book on WiFi with `home=` asks the server** (partial MD5 first, then the file-name
  id) and offers, as the card, the newest place from ANOTHER device that is more than 0.001
  away, has not been offered already (a new PUT is offered again; a declined one is not), and
  is not provably older than your last real MOVE in that book (a page turn, a jump, a card
  taken — not a save or a close). With either clock unknown it is offered. A book opened before
  the WiFi came up is asked about once it does. The last move and the last offer per book are
  kept in NVS (`kosync`/`memo`, 16 books, with what home last had from this phone); `positions.cbs` and the LoRa record are untouched.
- **"Sync my place" on WiFi READS home first**: a newer place from another device is offered
  instead of being overwritten ("Home: CrossPoint is at 61% - offered; yours NOT sent over
  it"); anything else, or pressing again after declining it, sends yours. **An automatic push
  on close goes out only if the place moved** since the book was opened, or differs from the
  last place home had from this phone (kept in NVS, so a place read off WiFi, or a push that
  gave up, is still sent by the next close at home — even one with no page turned), so an
  open-and-close no longer puts a stale place over the X4's. Each phone is known by its
  MAC-derived `device_id`; the name is only the fallback for a record with no id, so two
  phones given one `device=` still see each other's places.
- **The home client never blocks the loop** (its connect is polled, where it used to sleep up to
  600 ms), and a push or pull that gets no answer is tried 3 times, 1 s and 4 s apart, before
  "no answer ... (3 tries)". A window on the phone's WiFi address closes when that WiFi goes
  (5 s grace — a new ask during a blip keeps it; switching WiFi off closes it at once) or the
  address changes, so the next ask brings up the hotspot. A window opening by itself waits for
  the station only while a join is under way or the station was up in the last 10 s (a blip or
  a roam at home no longer puts the phone on its hotspot; no more 2 s freeze on every close out
  of range), and closing one no longer restarts a station its owner had disabled.
- **`home=` can be a NAME** (`covey.local`, or just `covey`), so a new DHCP lease for COVEY no
  longer breaks sync silently. It is looked up with the phone's own one-shot mDNS query (a DNS
  name: lwIP's resolver), polled, never waited on. An ANSWERED name, or a fallback COVEY then
  answers at, is reused for the rest of the WiFi join; a name nobody answers is asked again by
  the next home job (2 s in the background - on a WiFi without COVEY, 3 multicasts per book open
  or close). Unanswered, it falls back to the last address that name had on the same WiFi (NVS
  `kosync`/`home`, one record: an answer for another name or on another WiFi replaces it). A
  `.local` name used to never resolve (this core's `mdns_query_a()` takes the bare label, so
  "covey.local" went out as ONE label) and blocked the loop ~0.5 s on every book open and close.
  `kosync` shows `home covey.local -> 192.168.1.55 on 'SmithWifi' - this join`. The window's `!`
  warnings now also clear on `kosync reload` or an edit of kosync.txt (the re-read on each Books
  entry keeps them). `tests/check_kosync_glue.py` pins the glue the host suite cannot compile:
  no blocking lookup in the home client, and only `kosync reload` clears the warnings by asking.
- **What the X4 would not tell you is on the phone's screen**: a place PUT for a DIFFERENT book
  (`! A place for a DIFFERENT book arrived (id 0c9a1f7e..) - not the same file here?`) and
  wrong-password requests, in the reader menu and Sync settings. `kosync.txt` and
  `smsmirror.txt` are no longer listed as books. A book is refused, with the reason, when the
  X4 would count it differently: over 512 manifest items/itemrefs, or a `.txt` too big to be
  shown whole (it used to send 0 % and apply a peer's place to the cut text). Serial:
  `kosync sync` (the read-then-offer-or-send path).
- **Review round (KS-1..3):** the ask on book open is judged against your last page turn AS OF
  THE ASK, so a page turned while it is on its way no longer hides the X4's newer place (only
  "Sync my place" uses the latest turn); a place counts as declined only once you ANSWER its
  card (one lost to a restart or a full inbox is offered again, and "Sync my place" sends
  nothing while one waits on the card); and the X4 fork's second PUT, under its file-name id,
  no longer shows "! A place for a DIFFERENT book" beside a sync that worked. The push on close
  obeys the same rule: closed while the pull on open was still on its way, it no longer PUTs your
  place over the X4's the pull has just parked (`Home: another device's place waits for your
  answer - yours NOT sent`); the next close after you answer the card sends it.
  `tools/kosync_home_stub.py` is a slow bench home server for these races.
- **Integration review, WiFi (R1, R2):** the core's auto-reconnect is held off while a sync
  window's or the uploader's hotspot is up (a late NO_AP_FOUND could turn it into AP+STA, the
  station hunting beside the hotspot) and after WiFi is switched off (it could restart the radio
  behind "off"); a game and a hotspot hold it without undoing each other. `power sleep` refuses
  on the radio's real state. The loop's join retry no longer re-begins over the join a window
  close or a game exit started in the same pass, and restored joins get the 14 dBm TX cap.
  A hotspot that comes up with the station already restarted beside it (the core's handler can be
  past its check when the hold is taken) switches the station off and says so on serial.
- The phone only ever HOSTS: it never scans for or joins another device's hotspot.
- **A KOReader that pulls from the window gets "No progress found", never page 1 (2026-09-26,
  the plan's §5.1).** The phone has no XPointer to offer, so its reply carried `percentage` and
  `"progress":""` — and KOReader's kosync plugin takes exactly that body to `GotoXPointer("")`,
  a crengine null pointer, and lands the book on PAGE 1 (a manual pull applies without asking).
  The record now goes only to a reader that says it can take a percentage: the X4 fork sends
  `X-BookSync: 1`, stock CrossPoint an `Authorization: Basic` header beside its `x-auth-*` pair;
  everyone else (KOReader sends `x-auth-*` only) is answered `{}` for the book — "No progress
  found" on a manual pull, nothing on an automatic one. (Readest sends Basic INSTEAD of
  `x-auth-*` and is 401'd by the auth check before the marker matters: nobody serves it on
  0.9.79; the plan's §5.5 "any account" is where that changes.) A KOReader PUT to the window is unchanged (parked,
  the card). The window says so on the screen (`! A reader that can't take a percentage asked
  (x1) - told 'No progress found' (KOReader?)`, cleared with the window's other warnings) and
  once per window on serial; the close line counts them as `no-percentage=`. Auth still comes
  first (a wrong key is a 401 with or without the marker), `/users/create` stays 402, an unknown
  document stays `{}` for everyone, and the phone's own PUT body to `home=` is untouched.
  Host-tested in `tests/test_kosync.cpp` (`kosyncClientTakesPercentage`, the window's answers).
  **The phone's own home client sends the marker too** (review, 2026-09-26): `X-BookSync: 1` on
  its GET and its PUT to `home=` (`KOSYNC_MARKER_LINE`; one client, the same on both verbs, as
  the fork does). COVEY D-163 answers `{}` to an UNMARKED GET when the stored record has no
  XPointer — and every record a phone wrote is one — so a marker-less phone would read `{}` for
  its own place, decide "this book is not on the server yet" and PUT an older place over the
  newer. 🛑 **DEPLOY ORDER: flash both phones with this build BEFORE COVEY D-163 goes live** — a
  D-163 COVEY facing marker-less phones takes their stale PUTs. Pinned in `tests/test_kosync.cpp`:
  the request bytes, and the phone's own GET and PUT pass `kosyncClientTakesPercentage` beside a
  valid `x-auth-*` pair. **Proven on hardware 2026-09-26** (phone 2, a window on its WiFi
  address): an x-auth-only GET → `{}`, the same GET with `X-BookSync: 1` or `Authorization: Basic`
  → the record, `no-percentage=2` on the close line; and against COVEY D-163, deployed after both
  phones were flashed: the phone's own pull read its record back, an unmarked curl got `{}`.
- **"Sync my place" asks home FIRST; the window opens after (2026-09-26, the plan's §5.2).** On
  WiFi with `home=` the press used to open the window and queue the push behind it. A window on
  the phone's WiFi address (all of 0.9.79) leaves the station alone, but the coming
  hotspot-always rule takes the station down under the push, which then dies "not on WiFi any
  more" — so the order is flipped now, while it costs nothing: the home job is queued first, the
  reader menu says `Asking home first - the window opens after` (and `Window waits for home -
  opens after its answer` until it does), and the window opens from the loop when the job has its
  verdict — sent, offered as a card, gave up — or after 25 s (`KOSYNC_WINDOW_AFTER_HOME_MAX_MS`,
  which covers the longest verdict that matters: a DNS name's 6 s give-up plus three 3 s
  connects 1 s and 4 s apart, "did not answer (3 tries)", = 20 s). Off WiFi, with no `home=`, or
  when the home job could not start (its note is kept), the window opens at once as before. The
  deferred open follows every deferred window's rules (none during a call, under a game or
  Settings > WiFi). Serial `kosync` says `window waits for home (N s of at most 25 s)`. Pinned:
  `kosyncWindowWaitsForHome` in `tests/test_kosync.cpp`; the order, the two release paths and
  the bound in `tests/check_kosync_glue.py` (contract 12 — the old order trips it). **Proven on
  hardware 2026-09-26** (phone 2, the reader menu's Sync my place, home = COVEY): `waits for the
  home job (at most 25 s)` → `home answered the push - opening the window ... now` → `window OPEN
  300s`. ⚠ Serial `kosync sync` is the home half only (`kosyncSyncHome`) and opens no window.
- **The sync hotspot is OPEN unless `hotspot_pass=` is set.** With 8-63 plain-ASCII characters
  (spaces inside are fine; spaces at either end are trimmed) the window's `WiPhone-Books` is
  WPA2-PSK. A stock CrossPoint then needs that password typed once when it joins the hotspot;
  {user}'s fork has a matching "Peer Wi-Fi Password" setting. An invalid line keeps the hotspot
  OPEN and says why, in `kosync` and in Sync settings (`hotspot: open - hotspot_pass ignored:
  8-63 characters` / `... plain ASCII only`). The window note reads `WiPhone-Books (password)
  192.168.4.1` so whoever joins knows to ask. The password itself is never shown, printed or
  logged: everything says only `hotspot: WPA2` or `hotspot: open`. A window that finds the Books
  uploader already hosting `WiPhone-Books` rides it as it is (`hotspot: open (uploader's)`); an
  uploader started while a protected window holds it rides it protected, and its screen then
  says "it has a password" instead of "no password". (Like any WiFi password on this phone, the
  ESP32 WiFi driver keeps it in its own NVS config.)

**No saved place moves.** The reading spine, `epubFraction`/`epubLocate`, `/books/positions.cbs`
and the LoRa record are untouched. KOSync's percentage is CrossPoint's byte-weighted one over
CrossPoint's own spine (linear="no" items and spine images included, %XX-decoded paths), captured
in the zip walk the reader already made, and computed only on the way out / converted back only
on the way in. Proven: `tests/golden_positions.h` was generated from the 0.9.78 parser and the
current one reproduces it byte for byte (and did the same across 27 real EPUBs, {user}'s library
included). And who may CALL the writer is a source contract (`tests/check_positions_write.py`,
the plan's §5.3, in run_tests.sh): no carrier file — the LoRa record and inbox, the KOSync
protocol, window and home client, the LoRa receive that parks (`meshtastic_service.cpp`) and the
:80 pump that carries every KOSync request (`app_gbc_xfer.cpp`) — references `savePosition(`,
`booksSaveOpenPosition(`, `applyPending(`, `epubLocate(`, `store->put(`, `BOOKS_POS_FILE` or
`positions.cbs`; `app_books.cpp` has ONE `store->put(` (inside `savePosition`) and ONE
`applyPending()` call (the sync card's OK, after the arming guard; "Go to X's place" only enters
the card), and `epubLocate(` only inside `applyPending()`. The guard checks the real sources
FIRST and prints their `CONTRACT BROKEN file:line`; its self-test then judges each planted
spelling as what it ADDS (review, 2026-09-26: the first version ran the self-test first, on
absolute presence, and a real `savePosition(true)` in `kosync_sync.cpp` came out as three
`SELF-TEST FAILED` lines and never the file:line).

**The window is KOSync-only on the raw port-80 pump** — no upload page, no `/chunk`, no
WebServer: an open hotspot that opens itself on a book close must not offer an SD-card writer,
and the raw pump costs ~zero heap per request. It shares a running uploader's network (same
hotspot name or the same WiFi). `xferStop()` now stops the uploader only, so leaving Books, Music,
Games or Files no longer kills a window; the softAP WiFi gates use `xferServing()` (uploader OR
window), the busy/240 MHz predicate stays the uploader's (`xferOn()`). One slow client cannot hold
the single raw slot: a request's headers, and a whole KOSync request, must finish within 8 s of
their first byte. KOSync's per-book data is an OPTIONAL allocation: if it cannot be had the book
opens exactly as before and is simply not syncable. Serial: `kosync` (status), `kosync open [secs]`, `close`,
`push`, `pull`, `reload`; a `KOSYNC window ... heap= largest=` line every 15 s while one is open.
Host suite: `tests/test_kosync.cpp` reproduces every number in `tests/vectors_kosync.h`.

### Music: mono, half rate, half a second of ring, and a gap counter that tells the truth (0.9.79 dev, 2026-09-25)

**{user}'s listening pass (2026-09-26): "Music works good! Passed all the tests"** — and one photo with
two bugs in the Now playing title, both fixed:
- **Titles overlapped after the side button's next** ("Pulse" painted over "Space Explorer"): the two
  title lines were redrawn on every 1 Hz pass with no clear — every OTHER line of that screen clears
  its strip first. The title is now drawn only when the track changes, over both strips cleared
  (`titleShown`), and "Nothing playing" clears the whole panel. Pinned by `tests/check_music_title.py`.
- **Every title lost its last character to a second line** ("My_Name_I" / "s"; the stray letter in
  the photo was the "e" of "Pulse"): the pixel-width probe ran `k < len` and never measured the whole
  title, so a title that fit still split. The measuring is `WiPhone/music_title.h` now
  (`musicTitleFit`/`musicTitleBreak`, host-tested in `tests/test_music_title.cpp`).
- **A notification chirp no longer stops the music** ("it stopped playing the song after the
  chirp"). The pop's cut was kept as a pause at the exact place — 0.9.79's fix for "every mesh
  message restarted the song at 0:00" — but then waited for F1. The pop's own teardown now carries
  the track on (`musicPlayerResumeAfterPop()`), after its `restore()` and with `meshPopPlaying`
  already down (or `startTrack()`'s finish-the-pop backstop tore it down a second time — seen on
  the first bench). Only a track that was PLAYING when the pop started: a pause the user made
  stays paused through a chirp (`s_pausedByStop`), and a call that cut the pop takes its own
  branch. Proven on phone 2: chirp over *Pulse* → `the track the pop cut carries on at its place`,
  {user}: "Works on this one!"; paused by hand, chirp → still paused. Three contracts and three
  mutations in `tests/check_call_audio.py`.

{user}: music *"has always been bad"*; *"Lower audio quality and having to take time to load in-between
tracks are things I can live with."* Four faults, measured on both phones and replayed on the Mac
against the real decoder (`WiPhone/music_feed.h` has the numbers):
- **The loudspeaker played garbage** since the first MP3 commit: stereo PCM went to I2S as 1152 mono
  samples, so half of every 26 ms frame played an octave down and the other half never played.
- **Real dropouts** whenever a main-loop pass outlasted the ~70 ms the old ring let the writer get
  ahead (a map frame, a Files slice, a repaint and anything else). `LOOP STALL` only sees > 250 ms.
- **`gaps` was arithmetic**: 512-byte reads against 627-byte 192 kbps frames made 8.6 short passes a
  second, each a "gap" (167 in 19 s on 0.9.78), with no dropout at all — and it read 0 through real ones.
- **A resume re-fed its first frame** (helix's MAINDATA_UNDERFLOW taken for "need bytes", helix never
  reset): a loud burst on 2-33 of every 500 resumes, and ~1% played the rest of the track at the wrong
  speed off a false sync that decoded cleanly.

Now: **music plays MONO, and 44.1/48 kHz files at HALF RATE** (22.05/24 kHz, through a 23-tap
half-band filter: flat to 8 kHz), on **its own I2S ring — 24 x 512 samples, TX only, 24 KB of
internal RAM (8 KB less than the install the phone boots with), 534 ms deep** against ~70 ms. Each
pass decodes whole frames (4 KB card reads; never a pass ended for lack of bytes) until the ring
refuses, 4 frames at most (8 after a stall). The format is believed only when two decoded frames in a
row agree; a resume steps over the frame with no bit reservoir, resets helix and drops the overlap
transient, and returns to two frames before the one that was PLAYING (it was the read position). A
pop, the ring or a call that cuts a track now leaves it PAUSED at its place (F1 carries on; it used to
restart at 0:00); the devices still yield exactly as before. The jack no longer reopens the track: the
route moves live. **The codec's tone** is now adaptive bass boost, no treble shelf, no de-emphasis
(linear +9 dB clipped inside the DAC at any volume, and the shelf would have cut from 1.8 kHz at
22.05 kHz) — for everything, calls included; `audio tone old` puts 0.9.78's back for an A/B.
- **Now Playing says `drops:N buf:X.Xs`**: `drops` = the ring CERTAINLY ran dry (never invented).
  Serial **`music`** adds `minLead` (lowest lower bound on what the ring held; > 0 = certainly no
  gap), `lead`, `maxGap` (the loop's longest unfed stretch), work per pass, reads; `music reset`
  zeroes them; `music swap on|off` A/Bs the ESP32 mono pair swap by ear. `audio` prints them too.
- A gap of ~0.1-0.2 s between tracks (up to ~0.6 s for the first after a pop, call or game: a fresh
  ring plays its own silence once). Stereo and 44.1 kHz are what was traded.
- Host suite: `tests/test_musicfeed.cpp` — the 0.9.78 feed reproduced (8.6 gaps/s with 0 true
  dropouts at 10 ms passes; 4.8 true dropouts/s at 50 ms) against the new one (0 at 5-80 ms passes and
  through 100/300/450 ms stalls), the counter checked against the model's ground truth, reservoir and
  false-sync resumes, bit-exact resumes on the real-track fixture. `tests/check_call_audio.py` pins
  the new guards (music's install is music's alone; the mic reinstalls RX; the ring is closed on stop).
- **Review round (same day, before any flash):** the catch-up mark is **80%**, not 40% (at a map pan's
  ~77 ms passes the ring settled at 40% and a 300 ms stall on top dropped out: 4-5 times in 180 s);
  a **card read that fails mid-track** is retried on a new handle and then stops the track as "Card
  read failed" at its place (it was taken for the end of the file: the rest skipped, a pulled card
  walked the queue); music refuses when its **I2S install fails** (IDF would have dereferenced NULL);
  `setSampleRate()` marks the ring's queue **stale** so the pop, the ringtone and a call get a clean
  ring again (the 2nd pop after a music session played late, scrambled and cut); **a pop in flight is
  finished before a track, a game or the mic apps take the device** (F1 inside a chirp was undone
  300 ms later; a game started under a chirp got the pre-pop 22.05 kHz mono back); a **WAV's first
  unit** no longer inverts the mono pair swap for the whole track. ⚠ **The codec tone change reaches
  calls, the ringtone and the pop too, and has not been heard: the release waits on {user}'s
  `audio tone clean|old|flat` A/B on music AND a ringtone/call.**

### The 0.6-1.5 s 'mesh' freezes were LoRa transmits, not the database save (0.9.79 dev, 2026-09-25)

`MeshPhy::send()` waited out each frame's time on air on the loop task (0.5-2.2 s, ~20 an hour, mostly
flood relays); every `STALL ... mesh` value sits on the airtime lattice. Now the chip is started and left
to transmit, and an 8-frame queue feeds it one frame a pass (ACKs, then our own frames, then due relays;
a relay somebody else sent first is cancelled; beacons wait for an idle radio). **"Sent" now means
queued**: a frame that fails to leave marks its message failed, a full queue says "radio busy, try
again", and the `MESH ANNOUNCE/POSITION/WAYPOINT` lines say `QUEUED`. The stall line splits `mesh` /
`mesh-ui` / `mesh-post`. Serial: `radio` (with the longest the chip sat deaf in STANDBY after a frame
before a pass put it back in RX); `power` shows `lora=tx`. Host suite: `tests/test_txq.cpp`,
`tests/check_mesh_tx.py` (which also pins the four guards that keep "queued" honest, and removes each
from the real source to prove its contract trips).
- **A save cut between its remove and its rename no longer loses the chat history.** It left only
  `/meshdb.tmp`, which boot ignored (the stale SPIFFS copy, or nothing, loaded instead) and the next save
  truncated. Boot now renames a WHOLE temp file into place (its length must be exactly what its header's
  counts say — `MESH DB: RECOVERED ...`); the star list gets the same. Host suite: `tests/test_dbfile.cpp`.
  Bench: serial `meshdb cut` leaves exactly that state (the real file set aside as `/meshdb.cut`) and reboots.
  It never deletes `/meshdb.cut`: a leftover one refuses the next run (after a failed recovery it IS the
  database), and it runs on the card only, where `rm` and a computer can reach that file.
- **The card save goes 16 KB a pass** (~66 KB was one ~0.1 s write), and the close/remove/rename gets a
  pass of its own on both filesystems; SPIFFS still writes its image whole (an erase cannot be chunked
  around). The finish happens on the filesystem the save started on, even if the card is pulled mid-save.
- **Queued is not sent, everywhere it was said** (review M1-M3). Own frames are now QUEUED -> SENT/FAILED;
  the map's share/unshare/delete, Books' "Sync my place" and the pin/beacon rows wait for that answer (a
  retraction's waypoint id is kept until it has really left). A game or legacy upload started mid-frame no
  longer leaves the radio deaf. `meshdb cut`/`wifi calreset`/`wifi restore` run the power-off saves first;
  `meshdb cut` and both `power` sleeps refuse while a frame is on the air or waiting.
  Follow-up: moving or renaming a pin while "Take it off the mesh" is still queued changes it here only
  (it used to re-share right behind the retraction), and the Reading menu ticks while its frame waits,
  so "Queued" becomes "Sent" on the same visit without KOSync.

### Calls and music stop fighting over the codec; the SIP audit's leftovers (0.9.79 dev, 2026-09-25)

Both phones register now (`sip=1`), which made the 2026-08-15 audit's "unreachable" findings reachable.
- **F1/F2 during a call do nothing** (they started a track over the call: the caller went silent and the
  mic went out garbled or dropped), and `Audio::playMusic()` refuses with "In a call" while RTP is armed.
- **END hangs up a call and nothing else.** It set HangUp from every screen: that stopped music and lost its
  place, zeroed the volumes, and — with no WiFi or no SIP account — left HangUp stuck, so music could not
  play again until a reboot. **END in CallState::Error (the proxy was unreachable at init: COVEY's
  no-internet hotspot, a captive portal, a DNS miss) now retries SIP once per press** — the only way out
  of Error used to be the old END-everywhere's accidental HangUp -> HungUp -> Idle walk (`SIP: END in
  Error - one re-init attempt` at log_e; needs an account and WiFi).
- **The ring and the answered call use the call levels**, not the music level (the ringtone stole
  `playback` before the music pause could see it). A pop still playing is finished before a ring or a
  call's audio starts, so its restore() no longer lands on top of them.
- **No more 0/0/0 dB after every call end and END press** (never-written `restore*` globals, removed).
  ⚠ The ring now follows Settings > Audio's loudspeaker level instead of the accidental 0 dB maximum.
- **Settings > Audio with music playing** shows and saves the CALL levels (into music's stash, applied
  when music lets go) instead of writing under the track and being overwritten by it.
- **The second call of a boot is no longer ended at connect**: RTP silence is counted per call, and a
  call ends after 120 s of continuous silence (`CALL: no RTP from the far end` at log_e).
- **Hot-mic backstop**: an RTP session armed with no live call is shut down within 3 s (`AUDIO: RTP
  session armed with no live call`). The audio easter eggs (**202## streamed the mic to a hardcoded LAN
  address) are compiled out unless `-DAUDIO_DEBUG_EGGS`.
- Serial: `audio` gains an `rtp:` line (mic/stream/port, armed/owned, `vol=`, `callvol=`) and prints line
  by line; `audio orphan` arms a receive-only session with no call to watch the backstop fire. Host suite:
  `tests/test_rtpwatch.cpp`.
- Review round: the loop's music yield waits for a pop in flight (dialling inside a pop's 360 ms left the
  call at the music level and route); the backstop is held while the motor is driven or a pop plays
  (shutdown() on the shared I2C bus can leave the motor running); F1 while a session is armed keeps the
  paused track and its place ("In a call") instead of unloading it; opening Settings > Audio with an
  unreadable configs.ini no longer writes a three-key file over every other setting (only Save writes).
  `tests/check_call_audio.py` pins every guard above in the source (a review reverted six of them and the
  suite stayed green), and `test_rtpwatch` shows a strike left at hang-up ending the next call at
  connect without Audio::newCall()'s per-call begin.
- Integration review SA-1/SA-2: **the call screen's UP/DOWN moves only the level you are listening on**
  (it moved all three, so two presses on the earpiece left every later ring 12 dB quieter), and the ring
  and a call apply the stored levels (`AUDIO: ring: the codec held ... - applied` when something left
  its level behind); a game puts back the levels its F1/F2 moved. **Outgoing calls start on the
  EARPIECE** (they played on the loudspeaker the ring's code picked), and the Loud Spkr key is reset
  every call. 12 more contracts in `check_call_audio.py`.
  Its verification: **connect no longer reads configs.ini** (a 0.5-1.6 s SPIFFS open with the RTP port
  still shut could lose the far end's "Hello?"; the dial or the ring already applied those levels), and
  the checker now pins the stored key, the store itself and the Loud Spkr key's record (54 -> 56).
- Integration review 3 (audio): **the message chirp can be heard again** — a freshly installed I2S ring
  plays itself once (512 ms) before the first written sample reaches the speaker, and the pop's 360 ms
  teardown zeroed the chirp first; the stop now waits for the ring's lead (`NOTIFY: pop start took ...
  (flash, fresh ring, lead 512 ms, stop at 872 ms)`). **In-call UP/DOWN no longer loads and stores
  configs.ini per press** (1.5-5 s of silence both ways); it applies at once and is written once after
  the call (`AUDIO: after the call: the call screen's level saved`). The press finishes a pop first and
  steps the CALL's route, into music's stash while a track holds the codec. A pop's restore() re-reads
  the jack (an unplug inside a pop sent the ring to an empty jack), a ring installed with the device off
  is left stopped, the mic apps finish a pop first, and a failed I2S reinstall with the device on is
  refused by every writer instead of pa{user}ing. `test_notify` models IDF's TX ring.
  Its verification's repair: the chirp now waits one trip (872 ms stop) on a RUNNING ring too — a
  pop arriving just after the last one's teardown found the queue drained and lost the chirp at 360 —
  and the SPIFFS fallback takes no lead (its open runs the first trip; the 792 ms stop replayed its
  second attack). The model's queue is IDF's real depth, one short of the ring (objdump).
- Integration review 3 (R3-4, tests only - no firmware change): hand mutations the source guards let
  through now trip them. The music place kept at a pop (`s_resumePos = pos`, and its path from
  ceasePlayback to the resume: `check_call_audio.py`'s first real-source mutations), N1's 60 s cap and
  the terms of `joinInProgress()`, `wifiMarkLink()`'s noinline (without it loop()'s frame is 288 B, not
  240 - objdump) and three more helpers' with a rule for every static loop helper, `txCutShort()`'s RX_DONE
  half, and the I2S writers reachable only behind the no-driver refusals (`check_gbc_ring.py` G17).
  Its verification's repair: **a stopped track no longer tops up an I2S ring that failed to reinstall**
  (`ceasePlayback()`'s `closeRing()` only with a driver; F1 inside the Recorder, then Record on an
  exhausted heap, then F1 again was a LoadProhibited panic), pinned as G18/G19 with the feed's three
  writers at one site each; the place's middle link (`musicTakeStopPlace()`, `musicFilePos()`); the
  noinline rule file-wide (`applyStoredCallVolumes()` and `healthLogTrim()` gained the attribute; loop()
  still 240 B); the KOSync join-wait constants themselves (cap 120 s or less).

### WiFi drops explain themselves; a station wedged mid-connect now cures itself (0.9.79 dev, 2026-09-25)

Phone 2 lost WiFi twice on 2026-09-24 (HEALTH `wifi=5` then `wifi=1`, a serial `wifi scan` of -2,
only `wifi bounce` brought it back), and nothing on the phone could say why: the disconnect REASON
was thrown away (the core's own `Reason:` line is log_w, compiled out).

- **`WIFI LOST reason=200 BEACON_TIMEOUT ap=<bssid> ch=6 rssi=-63 (sampled 1400ms before)`** on the
  drop that ends a link, and **`WIFI JOIN ap=... ch=... rssi=... (same|OTHER AP) after 94s down: N
  disconnects (last 201 NO_AP_FOUND), N core rejoins, N refused-scan-start runs`** on the rejoin —
  both on serial AND in `/health.log`. In between, every disconnect prints (`WIFI disc reason=...`),
  with a run of one reason — the core's auto-reconnect storm — counted after two lines and summarised
  once a minute. Recorded on the WiFi event task with plain stores only (no log, no allocation, no
  card); printed by the loop.
- **HEALTH gains ` wdis=<disconnects>/<last reason> cr=<answered by the core's own begin()>
  ssf=<runs of refused scan starts>/<last esp_err hex>`.**
- **Serial `wifi why` (or `wifi`)**: status, the driver's STA config (SSID; never the password), the
  AP/channel/RSSI, a count per reason, the scan-start refusals, the auto-switcher's counters, and the
  last 15 WiFi events. **`wifi scan` now says WHICH -2**: refused at the start (with the esp_err —
  `ESP_ERR_WIFI_STATE` = mid-connect) or started and never finished. **`wifi log on|off`**: the WiFi
  driver's own state lines (RAM only, bench only).
- 🛑 **FIX: a round of refused scan starts now counts toward the deaf-radio self-bounce.** `_dryScans`
  only counted scans that COMPLETED, so a station held mid-connect (every start refused while the
  core's capless auto-reconnect keeps it "connecting") could never reach the bounce written for that
  state. Two refused rounds and the next one restarts the radio (~1.5 min after the drop).
- **FIX: the auto-switcher's `MARK scan-pre` is taken once per round, not once per attempt** — a round
  of refused starts retried every loop pass and wrote a health.log line (an SD open/append/close) for
  each one.

### Phone 1's WiFi drop: the CPU gate no longer re-locks the PLL under a running radio (0.9.79 dev, 2026-09-25)

🛑 **The cause, measured on phone 1:** the gate's 240→80 MHz drop straight after WiFi traffic broke WiFi
4 of 4 (0 of 3 held at 240; 0 of 20 with an idle radio). On the ESP32, 240 runs off PLL 480 and 80/160
off PLL 320 (TRM Table 7.2-2; there is no 480/6), so `setCpuFrequencyMhz` re-locked the BBPLL under the
radio every time (TRM 7.2.5 requires WiFi to be in low power first). **New CPU clock module
(`cpu_clock.cpp`):** while the radio runs, the clock stays on one PLL, with **160 MHz for work and 80
idle** (a divider write, the switch IDF's own DFS uses). With WiFi off it is **240/80 as before**, and the
Game Boy still gets 240. `esp_wifi_start/stop` are link-wrapped so every radio start first moves the
clock off PLL 480. There are no APB callbacks, so the GPS-UART deadlock cannot happen. ⚠ **With WiFi on,
"full speed" is now 160 MHz.** Serial: `cpu`, `cpu method old|new` (A/B, RAM only), `cpu cycle <n> <ms>`.
HEALTH gains ` pll=<MHz> csw=<same-PLL>/<re-locks radio off>/<re-locks radio ON>`. The last number must
stay 0. Host suite: `tests/test_cpuclock.cpp`.

Review round: `cpu method old` is refused while the GPS reader is on, and `gps on` is refused under
method old. Either order would bring back the APB-callback deadlock that the GPS reader can cause on
phone 2. A Game Boy game now switches the core's WiFi auto-reconnect off for its duration: a
NO_AP_FOUND still queued at game start could restart the radio under the emulator, and that restart
would have dropped the game to 160 MHz. The pre-radio clock move is now decided inside the lock, so a
racing gate pass can no longer leave the radio on PLL 480 with idle held at 240.

### WiFi no longer comes back on behind "off" (0.9.79 dev, 2026-09-25)

🛑 **After every Game Boy game, a phone with no saved network (or after Disconnect or Forget) started
its station and joined an ERASED config, and nothing ever quieted it until the next reboot.** The exit
asked only the global off switch, and that phone is "disabled" without being "off". It was one of a
class: every site that gives the radio back asked its own subset of the owner's state. **Now they all
go through `wifiRestoreStation()`** (decision in `wifi_policy.h`, host suite `tests/test_wifi_policy.cpp`,
all 256 states), which logs `WIFI restore (<who>): JOIN|UP_IDLE|OFF|LEAVE [...]`. Also fixed:
- the loop's join retry ran DURING a game (`WiFi.begin()` under the emulator);
- serial `up on` brought up a hotspot mid-game, and a game left a headless uploader "up" over a dead radio;
- Settings > WiFi could scan and join under a headless uploader's hotspot (the chip-panic combination);
- both Settings "WiFi on" controls used a bare `esp_wifi_start()`, which restarts the driver's LAST mode:
  a soft-AP after a sync window closed with WiFi off;
- leaving the WiFi list, and booting with the saved network Disconnected, left the station up idle;
- in a 5+ min dry spell a restore now leaves the join to the loop's gated retry.
`wifi scan`/`wifi bounce` refuse under a game or a hotspot (bounce also with WiFi off); a scan with WiFi
off puts the radio back off. `wifi why` shows the last restore. **Connect with WiFi off now switches
WiFi ON (persisted) - once the join has actually STARTED.** (Review: switching on first meant a Connect
on an unsaved or blank SSID failed yet left WiFi on, and the loop joined the OLD network 20 s later;
a refused join now leaves the Disconnected flags alone too.) The edit screen's Save refuses a blank
SSID (it made an empty network the PREFERRED one), and a saved name typed into the blank edit screen
no longer crashes Connect on a missing button. A restore right after a hotspot or a game rejoins at
once instead of treating the join those killed as still in flight (`wifiJoinInFlight`).
`tests/check_wifi_restore.py` fails the suite on a bare station start - or ANY `WiFi.begin(` - outside
Networks.cpp, and (review: deleting a guard spells nothing banned) states 13 contracts positively:
the retry's `wifiStationWanted()` gate, xferStart's game refusal, each restore call site and its order.

### The clock from GPS, and (carefully) from the mesh (0.9.79 dev, 2026-09-25)

{user}: *"for wiphones, are we able to also do gps time as long as the gps is enabled?"* Until now the
clock was NTP or nothing, so a phone off WiFi since boot did not know the date however long its GPS
had had a fix. The rules are all in `clock_source.{h,cpp}` (host suite `tests/test_clocksrc.cpp`).

- **GPS time**: with `gps on`, an RMC with status `A` and its OWN full date and time sets the clock —
  once a SECOND consecutive reading agrees (next second, and its seconds match the millis() between
  them within 1 s), in a year from 2026 to 2079 (a GPS week-rollover date lands in 2006 and is
  refused; so is a receiver's two-digit DEFAULT year 80..99 — `060180` is the 1980 GPS epoch, which
  2000 + yy reads as 2080 — the same ceiling as COVEY's `gpstime.py`). It sets an unset clock, replaces a mesh-set one, and corrects a clock more than 2 s out —
  but **never while NTP has set it in the last 30 minutes**: NTP stays in charge when it is present
  (a disagreement is logged, at most every 10 min). A `V` (no-fix) sentence's time is NOT used even
  when it looks complete: u-blox prints RTC / first-satellite time before a fix. Modes `E` (dead
  reckoning), `M` (manual) and `S` (simulator) are refused too.
- **Mesh time, for a phone without GPS** (phone 1): a received Position's `time` fills an UNSET
  clock only — one packet on a private channel, or two DIFFERENT nodes agreeing within 60 s on a
  public one (LongFast is anyone's). It never overrides anything, and NTP or GPS replaces it.
  ⚠ It CAN be spoofed, ahead as well as behind, even on a private channel: channel crypto is
  AES-CTR with no MAC, so a recorded Position's time bytes can be bit-flipped and replayed without
  the key. What bounds that is that a mesh clock is never trusted (below), not these rules.
- **Known vs trusted**: a mesh clock is shown (clock, message times, `sun`, which warns) but NOT
  trusted — KOSync, waypoint expiry, the position beacon's time, booksync's turnedAt, the replay
  ring's stamps and the mirrored-text buzz's "is it recent" all treat it as unknown, so nothing any
  other device sees changes, nothing is deleted and no real arrival is hushed on its word. GPS is trusted like NTP (KOSync's "provably older" uses it). This phone never
  puts a mesh-derived time back on the air.
- **FIX (latent): `minuteTick` read "the loop's `now` is before the last set" as the millis() wrap
  and added ~49.7 days.** NTP's thread could land a set in that window (microseconds wide); the new
  sets make it matter more, so a negative gap now counts as nothing passed.
- **Serial `clock`** (or `time`): the time, its source and age, NTP freshness, the last GPS and mesh
  verdicts, and gps-minus-clock in ms; `gps` gains a `gps: time -> ...` line. **HEALTH gains
  ` clk=<none|ntp|gps|mesh>/<minutes since set>`**, last on the line after `pll=`/`csw=`; the line
  is now ~232 of its 255 bytes, so a clk= that would not fit is dropped whole, never cut to a
  different number (a line with no clk= at all ran out of room). Log lines: `CLOCK: set from GPS -> ...`,
  `CLOCK: set from the MESH -> ...`, `CLOCK: NTP set the clock (it was gps; moved N ms)`.
- **Review round (SA-3, F1, F3):** a text stamped from a MESH clock (a SIP arrival, one composed here,
  or the unknown-time repair) is PROVISIONAL (`tm`); the first load under NTP/GPS moves it by exactly
  what that set moved the clock (`MSG: finalised ...`; an older boot's stays unless in the future),
  and the forced message reload stays armed until the clock is trusted (`clock` shows it;
  `tests/check_msg_clock.py`). The HEALTH line is built in a noinline helper: loop()'s own frame
  640 -> 240 B (560 on main), under the picture-page chain (`tests/check_health_log.py`). `WIFI
  LOST/JOIN` and `MARK assoc/disassoc` reach health.log at most one pair a minute, the rest
  serial-only and counted (`WIFI card: N LOST + M JOIN ...`; `wifi why`), which keeps the log's
  ~9 h window (not ~16: HEALTH lines are ~232 B now).
- **Misc repair round:** an earlier boot's mesh-stamped texts still in the future at the first
  NTP/GPS load are re-stamped in the order they came (the newest gets the latest stamp,
  `clockMsgRestamp`), not reversed, and below this boot's unknown-time repairs. Before this, the
  newest-first partition was walked with ascending stamps, which reversed that thread for good.
  The WiFi card gate no longer reads an event stamped later in the loop pass as ~49 days old, so a
  held `WIFI card:` count is no longer written early.


**Found on the 2026-09-25 hardware pass:**
- 🛑 **The web flasher no longer erases the phone's NVS.** Through 0.9.78 the one merged image was
  written from offset 0 with the nvs partition (0x9000) padded to 0xFF inside it: every browser
  install wiped the mesh identity key (other nodes then flag a key MISMATCH and DMs fail), the
  booksync passcode and name, and the KOSync memo. `tools/make_webflasher.sh` now stages two parts
  cut around nvs (`wiphone-boot.bin` @0x1000, `wiphone-app.bin` @0xe000); proven on both phones
  (key, KOSync id and mute unchanged across the install). The cut is `tools/webflasher_split.py`
  (reads nvs/otadata from the table INSIDE the image; `tests/check_webflasher_split.py` pins it),
  and the installer page now flashes EVERY part of the manifest and refuses one that would cover
  nvs (the range read from the partition table in the parts, 0x9000-0xe000 as the fallback) -
  "Stopped: this installer build would overwrite the phone's settings - nothing was written".
  **Release day, 2026-09-26 13:20 — the page's reset step was wrong and is fixed:** after a good
  write it called `loader.after()`, which esptool-js 0.4.5 does not have, so the page said
  "Failed: loader.after is not a function" and left the phone dark in the bootloader with the new
  firmware already on it ({user}'s first browser install; phone 1 came up on a hard reset with its
  mesh key and KOSync id intact). Now `loader.hardReset()`; a reset that fails can no longer turn
  an install into "Failed" (the message says: installed, unplug and replug); and esptool-js is
  **served from this site** (`webflasher/esptool-js-0.4.5.bundle.js`, `THIRD_PARTY.md`) instead
  of a CDN, so `tests/check_webflasher_split.py` proves every `loader.*(` / `transport.*(` the page
  calls exists in the bytes actually served. A mock could not catch this — it copied the page's
  assumption — which is why the guard reads the served library.
  **The live installer was republished on 2026-09-26 as 0.9.78 in two parts** cut from the very
  image that had been live (byte-identical slices; the merged image is gone from gh-pages), so
  updaters stop losing their settings before 0.9.79 ships. Proven: phone 1 flashed from the LIVE
  parts booted `0.9.78` with its mesh key, node keys and mute intact. ⚠ Anyone who updated through
  the old page has ALREADY lost the booksync passcode (Books > Sync settings shows "Passcode:
  (none)"; set it again) and other nodes flag a key MISMATCH until they `pki forget` this phone.
- **`pki forget <!node>`** re-trusts ONE node's key after it re-keyed, instead of "Clear nodes"
  (which wipes every node, name, key and star).
- **One garbled radio register read is no longer "MESH RADIO LOST"** (all five on record were
  single-bit bus glitches; RADIO LOST now fails queued messages). Three reads must agree; glitches
  are counted in `radio`.
- **A full reading-position store evicts the book saved longest ago**, not the smallest time stamp
  (0 for every save without a trusted clock, so last night's book in the woods went first).
- **An NTP answer must be a real one** (from the server asked, mode 4, synchronised, sane date);
  a stray datagram could set a trusted clock in 2036. Message-store repairs are stamped in UTC.
- **A KOSync ask waits for a WiFi join in progress** (review N1): a push queued at a book close
  before a Game Boy game was dropped "not on WiFi any more" on the first pass after the game.
  Integration review 3 completed it: the wait is for ONE join (opening Games ends it, so a wait
  begun before a game, or back-to-back games, no longer makes the rejoin after look 60 s old), and
  a job already waiting to retry, about to connect or looking up home goes back to waiting with its
  ask kept instead of "gave up" (`KOSYNC home push: off WiFi waiting to retry - a game has the radio;
  the ask waits and starts over`); a connect is no longer tried on a station not yet rejoined.

### A Game Boy game no longer leaves the internal heap split for the rest of the boot (0.9.79 dev, 2026-09-25)

After a game, free RAM came back but the **largest block stayed at 32,816 (phone 1) / 28,732
(phone 2) against ~64 KB before** — the number that predicts this phone's panics. The game installs
its I2S ring (8 x 4 KB of DMA RAM) with its own stacks, VRAM and audio buffer (32 KB) already at the
bottom of the big free block, and IDF 3.3's best-fit heap put the ring straight on top of them; at
quit they freed BELOW it. (Phone 1, to the byte: the hole after quit is exactly those four blocks,
32,768 + 4 x 12 B of heap poisoning; phone 2's music install moving the ring away gave 64,316 back,
so nothing else was left there.) **The game's exit now reinstalls the ring once the emulator's RAM is
freed and the codec is off** (`Audio::reseatI2S()`, one uninstall-then-install, before WiFi comes
back), so it goes back into the hole the old ring left. `GBC: I2S ring reinstalled ... largest A -> B`
says what it did. `tests/check_gbc_ring.py` pins the order (release, shutdown, reseat, WiFi), the
refusal under a powered codec, the fresh default install, and I2S installed nowhere but installI2S().

Review of the reseat, three repairs:
- **A game whose sound starved now also turns the device off at quit.** After 60 short
  `i2s_write`s in a row, the emu thread clears `soundOn` and leaves the codec ON. The quit used to
  skip `shutdown()` on that path, so the reseat refused it ("LEFT where it is"), the heap stayed
  split, and the codec stayed powered until the idle watchdog, which a lit screen holds off. The quit
  now asks `soundStarted` (did the game turn the device on) instead of `soundOn`.
- **No I2S driver no longer means a panic at the next start.** A failed install leaves no driver,
  because the old ring is freed first. That includes the reseat's own install, however unlikely. IDF
  3.3's `i2s_start()`/`i2s_stop()` dereference NULL, and `playMusic()` starts the device BEFORE it
  installs its ring. `Audio::start()` now puts the default back once and refuses if even that fails,
  and `shutdown()` stops I2S only with a driver installed. The quit line says `reinstall FAILED`
  rather than confusing it with having nothing to move.
- **What the quit line reads:** `~32,816 -> ~L` for a game that set the ring up at start.
  `~L -> ~L` for a second game that found it matching; that reinstall has nothing to fix and is
  harmless, and it hands the next start a fresh queue. `X -> X` well below L means something else
  was left in the big block.

## 0.9.78 (2026-09-23) - OpenTopoMap's z17, and the most detailed tile there is, on all three devices

{user}, after looking at OpenTopoMap one level deeper: *"So it seems like open topo has a native
z17 tile set. For all three devices, can we make that a downloadable option?"* Then, on what
a map should do where its tiles run out: *"Actually in general on all devices, if the more
detailed zoom levels don't exist, don't just let the higher zooms go blank, just stretch the
most detailed available tile and say what tile is being stretched and by how much"*. And, on
the 5 km cap proposed for z17: *"I say allow up to [20]km but throttle the download a bit. I'm
thinking overnight downloads or multi day downloads even pre-trips"*.

**First, a correction to 0.9.77.** Its entry says COVEY "streams the real z17 tile and there is
nothing to stretch". **That was true for OpenTopoMap only.** Both USGS services answer **404 at
z17 and z18** (measured 2026-09-23), so over USGS there has never been a z17 to stream. Worse,
each of those 404s set a 20 s streaming backoff for COVEY's whole map, z16 parents included,
and the chip read "offline" on a good connection (fixed below). OpenTopoMap's z17 is real: house
numbers are visible. Its z18 answers 200 with a placeholder that reads *"max zoom layer = 17"*,
so no device ever asks for OpenTopoMap z18.

**The map now draws every tile position from the deepest level on the card there.** Nothing is
left grey while a shallower level exists. The walk is:
- the position's own tile;
- else its parent's quarter, stretched 2x;
- else its grandparent's sixteenth, stretched 4x;
- and so on, down to the area's shallowest level or 8 levels up, whichever comes first
  (`MAP_ANCESTOR_MAX_D`; past 8 levels a position would be smaller than one ancestor pixel).

A grey square now means there is no tile at ANY level. A real tile is never stretched.

0.9.77's zoom step one level past the deepest tiles is the same walk, starting one level up.
So there is one drawing path, computed at the view zoom and exact at every depth:
- `mapAncestorSrc()` finds the ancestor, the source rectangle, and the phase of a piece that
  starts part-way through an ancestor pixel;
- `mapAncestorRow()` widens one ancestor row, which is then pushed once per screen row it
  covers.

`MapsApp::overZoom()`/`tileZoom()` and `mapOverzoomView()` are gone. As before, the view lives at
the zoom you chose, so the scale bar, the pins and the distances stay exact.

**It says what it stretched, and by how much.** The status chip lists every stretched level in
view with its factor, **deepest level first**, so each end of the range sits beside its own
factor: `z15 tiles stretched 4x`, or `z16-14 stretched 2-8x` (`z16-14 x2-8` when the full text
will not fit). Written the other way round, `z14-16 stretched 2-8x` reads as z14 at 2x.
- **The corner star** (`z16*`) now means ANY position on screen is stretched, not only "one
  level past the tiles". The `z16*` part is never trimmed.
- **The one-shot note** on the zoom step past the deepest level is unchanged from 0.9.77.
- **A tile the card holds but cannot read** (wrong length, or unreadable twice) is still drawn
  from an ancestor, but the chip says **`card read error`** instead. A failing card in the
  woods must not pass for ordinary stretching.
- **The Help screen** has new rows for the star and for `grey square  no tile at any level`.

**How the tiles load.** For each position the loader wants the finest level that is neither in
memory nor known to be missing. It reads a coarser level only once every finer one is known to
be missing, and meanwhile draws the finest level that is ready (counted as stretched while it
is shown).
- A miss now redraws at once. That also fixes the old stall at an area's edge, which waited for
  a keypress.
- The miss cache holds 64 entries and evicts the entry hit longest ago, so a hole on screen is
  never evicted.
- While a download is writing into the area on screen, the map checks it every 10 s. It then
  forgets the misses at the levels the download wrote, so a fresh tile replaces its stretched
  parent within 10 s.
- The ruler's dashed line no longer overflows int32 when its anchor is far away.

🛑 **Caught before it shipped:** a `const` row pointer selects TFT_eSprite's PROGMEM
`pushImage` overload. That overload inverts the byte swap and does not clip, so every tile
would have drawn in swapped colours. The pointers are non-const now, and a comment says why.

**Downloads: the Detail row offers z17 where the source has it.** That means OpenTopoMap (and
the serial custom relay). USGS still stops at z16, and the press steps only through the depths
the source has, so there is no dead press that shows z16 twice. The depth shown is the depth
used; the depth you picked stays in NVS. Radii stay 2/5/10/20 km at every depth. The form opens
on the source of the map you are looking at.

**The estimate gives a time, and no longer wraps.** For OpenTopoMap at 20 km to z17 it reads
`50451 tiles, 6.2 GB, up to about 36 h` (phone 1's crosshair; the count moves a little with
where you stand). The old 32-bit card total saturated at ~4 GB. "Up to" is there because tiles
already on the card are skipped in milliseconds. Past 4 h the form adds `Keep it on USB: about
36 h of downloading`. The time takes z11-16 at 4.0 s a tile and OpenTopoMap's z17 at 2.1 s,
the 2.0 s interval plus 0.1 s, because phone 2 measured ~2.07 s a z17 tile. While a job
runs, `About N left` comes from the mean time of the last 200 tiles fetched.

**Throttled for OpenTopoMap:**
- **At least 2.0 s between the starts of two OpenTopoMap z17 requests.** At z17 that interval
  IS the pace: its tiles came in faster than that, and a real 2 km run on phone 2 measured
  2,068 ms a tile (z11-16 take ~3.5-4 s). Shallower levels keep OpenTopoMap's 0.6 s pause.
- **429/502/503/504** retry the same tile after 60 s, doubling to 15 min
  (`Server busy - trying again in N min`), and double the pace for the rest of the run (cap
  10 s). These never count as failures.
- **z18 is never requested.** OpenTopoMap's source ceiling is z17.

**Built for multi-day runs:**
- **The job is kept in NVS:** source, the WiFi network it started on, centre, radius, depth
  and a cursor. It is saved at most every 500 tiles, every 5 min, and at the end of each
  level. Custom-source jobs are not kept.
- **A stopped job resumes by itself** under one host-tested decision, `tileResumeCheck()` in
  `tile_plan.cpp`. It needs:
  - the same WiFi network, up for 60 s;
  - USB, or a battery at 3.8 V or more;
  - no call for a minute, no game, no Files folder job, and a card present.
- **Stops and cool-downs.** A WiFi drop longer than 20 s stops the job, because the WiFi
  rescue needs the RAM; it resumes 60 s after WiFi is back. A call or a game longer than
  10 min also stops it. Off USB below 3.75 V a running job stops and waits for USB. After a
  failure stop it cools down for 10, 20 and 40 min, then retries every 60 min. **After 26
  failure stops or refused starts in a row without a new tile (about a day of cool-downs) it
  gives up and says so.** Days spent waiting at a gate, on another network or off USB, are
  not retries and do not count. A card that keeps refusing writes never resumes by itself.
- **Crash strikes.** Three boot resumes that neither ended in a recorded stop nor wrote 50 new
  tiles stop the resuming, and the form shows `Stopped resuming: the phone restarted 3 times
  during this download`. Switching the phone off (the held button or the low-battery cut) and
  a restart from the menu save the live cursor and record a clean stop, so they never count
  as a strike.
- **Another network.** On a network other than the one it started on, the job waits
  (`Paused: resumes on <network>, where it started`); the network on a trip is often a phone's
  hotspot, on somebody's data plan. The form offers **Resume on this network**. A job waiting
  for any other reason (a give-up, a cool-down) gets **Resume it now**. A refused resume says
  why on the form.
- **The order is centre-out.** Within each level the tiles go centre-out in 16x16 blocks, so a
  run cut short still holds the middle of the area. At the end, one automatic second pass
  retries the tiles that did not land.
- **Stop takes two presses within 3 s.** The first press says `Stopping forgets it: N of M
  done, and it will not resume`. A Stop forgets the job at once.
- **Card space.** A resume checks card space only for the tiles at or past the cursor that
  it has not written, with max(256 MB, 2 % of the card) spare. If there is not enough room,
  the job is KEPT but stops resuming: `Stopped: no room on the card for the rest - free some,
  then Resume`.
- **What the form shows.** While a job runs or waits, the form shows THAT job read-only, and
  Start is not offered. A new Start would replace a job that may be days old.

A PNG in which every pixel is transparent counts as "no tile" and is not written (`tileDecode`
returns OK/BLANK/ERROR). ⚠ That check does NOT catch OpenTopoMap's real z18 answer, which is
opaque text on a transparent background. The guard there is that nothing asks for z18.

The Files app refuses to delete or move a folder at or above the `/maps/<src>` of a download
that is running or waiting to resume. `maps dl` prints the depth actually used and the resume
state. `maps dl stop` says the job is forgotten.

**COVEY** (covey-ui, its decisions D-159 and D-160):
- **Per-source zoom ceilings.** OpenTopoMap has tiles to z17 and the view goes to z18; USGS has
  tiles to z16 and the view goes to z17.
- **The same nearest-ancestor stretch**, named on the info chip: `z18 · OpenTopo · z17
  stretched 2x`, or `· z16-14 stretched 2-8x`.
- **A 404 now rests that one tile for 10 min.** It no longer switches the whole map to
  "offline".
- **Streamed tiles have their own tree.** Tiles streamed while panning go to
  `/root/covey-tiles-streamed`, beside the downloaded `/root/covey-tiles`. On the map they are
  washed amber, and a chip reads `N streamed tiles in view`. **Map Downloads → Streamed-only**
  lists the places COVEY has only because it streamed them, each with **Download this area
  properly**.
- **Streamed tiles may expire when the card nears full.** This is the first time COVEY deletes
  map tiles, and it never deletes a downloaded one. Before it deletes a streamed tile because
  a downloaded copy exists, that copy must start AND end like a tile (a PNG's IEND, a JPEG's
  FFD9): a copy cut short keeps its good header.
- **Area downloads** get OpenTopoMap z17 and the same 2.0 s interval. They resume after a
  restart on the same WiFi network, and make a second pass over failed tiles.
- **Deployed 2026-09-24 00:26** (covey-ui `131feb3`), with a one-off cut-over. 749 tiles had
  reached COVEY since the 2026-09-22 card-day push. 478 of them were a deliberate OpenTopoMap
  download made on COVEY that same evening, and were kept as downloaded. The other 271 were
  streamed browsing, and were moved into the streamed tree.

**The Mac tools** (`tools/`):
- **`cardday.sh covey pull`** (new; `tools/covey_pull.py`): COVEY's DOWNLOADED tiles into
  `~/tiles-master`, never its streamed ones.
  - Tiles are staged in `<master>/.incoming`. Each is checked against COVEY's own manifest
    (same size, more than 0 bytes) AND against its own end marker (a PNG's IEND chunk, a JPEG's
    FFD9), then linked into the master with `os.link`. A link cannot replace an existing file,
    so **a master tile is never overwritten**. 🛑 The end check is the review's critical
    catch: when a tar stream stops mid-tile, bsdtar still pads that tile to its full size with
    zeros, so a size check alone pooled a corrupt tile. The tile bsdtar was writing when a
    stream stopped is also dropped by name.
  - The pass test is a set difference. An empty master file where COVEY has a good tile fails
    it.
  - Zero-byte tiles on COVEY are never pulled; they are listed in `covey_zero_bytes.txt`.
  - Before anything moves, it prints COVEY's own streamed-only places under *"Will NOT reach
    the phones unless downloaded on COVEY first:"*, with how much of each the master already
    has; a place the master already covers is listed apart.
  - **It refuses (exit 2) when COVEY cannot give that report.** A COVEY that cannot is most
    likely one whose streamed tiles still sit in the downloaded tree. `--without-report`
    overrides it, once the cut-over is known to be done.
- **Card-day order:** `status` the evening before → `covey pull` → the phone pulls → the phone
  pushes → `covey push`.
- **`cardday.sh covey push`** (renamed from `covey`) writes only into COVEY's downloaded tree.
  It uses `rsync -rt`, counts only non-empty tiles, and skips a source that would leave COVEY
  under 4 GiB free.
- **`cardday.sh status`** adds a COVEY block when COVEY answers: its streamed-only places and
  disk. Otherwise it prints "not reachable".
- **The phone push** writes z0-16 for every source first, then z17, each after a free-space
  check. It never writes z18. A card that fills up loses only z17, never the USGS sources that
  sort after `otm`.
- **Fixes in the existing tools:**
  - `set -o pipefail`: a failed step behind `| tee` never stopped the run;
  - a message whose backticks ran as a command;
  - `convert_tiles.py` now exits 1 when it stops early (a full card) and refuses a fully
    transparent tile;
  - `tiles_565_to_png.py` fills a 0-byte master PNG instead of keeping it;
  - `card_clone.sh`'s unquoted `find` pattern.

Host tests: `test_maptiles` 130 → 179. It now includes every stretched piece compared pixel by
pixel with a directly scaled reference, a parent painted as four coloured quadrants at d = 1..3
(the plausible-looking failure is still a stretched tile showing its neighbour's ground), and a
z17 → z18 round trip. Twelve deliberate breakages of `map_tiles.cpp` each turn it red.
- `test_tileplan` is new, with 140 checks: tile counts (20 km to z17 at 47.5° N = 51,084), bytes
  past 4 GiB, the Detail depths, the block order, the time, and the resume table including the
  `millis()` wrap and the 26-retry give-up.
- `test_tilepng` 42 → 52 (BLANK).
- `check_convert_tiles.py` 22 → 35.
- `check_covey_pull.py` is new, with 83 checks. It runs against a fake COVEY in a temp dir, and
  `run_tests.sh` runs it now.

**Reviewed before release** by six adversarial reviewers (phone view, phone downloads, COVEY's
store, COVEY's UI, the Mac tools, fidelity to {user}'s asks), with a verifier on every finding.
All 30 findings were fixed. The ones that mattered:
- **The map, locked (major):** a tile that finished loading while the phone was LOCKED was
  read again 40 times a second until unlock. The redraw went to the clock, so the map's "want"
  still named the finished tile. A tile already in memory is now never read again, and the
  25 ms load tick stands down when a load ends.
- **The form (major):** in the second after a run ended, the form could show a waiting
  multi-day job as "no job", with Start under the cursor. One press would have replaced it. It
  now settles a run that just ended before drawing.
- **The give-up (major):** "24 h without a new tile" also counted days spent waiting at a gate,
  so a job that had waited on another network gave up on its first failure after. It is now
  26 retries in a row.
- Also: a power-off or menu restart looked like a crash strike; a resume's space check counted
  tiles the job had skipped as still to write, and a full card dropped the job; a `maps dl
  stop` in the first loop pass after a boot forgot nothing; a job writing a level deeper than
  the map had scanned was not shown until a rescan.
- **COVEY (two major):** a tile whose body did not decode was fetched again on every frame,
  and pass 2 could retry one tile the server always refuses forever. **The Mac tools (one
  critical):** the bsdtar padding above.

Bench, reviewed build (`firmware 0.9.78, built Sep 24 2026 00:04:42`, 2,663,296 bytes, RAM
27.1 %), flashed to both phones (esptool hash verified). Both were left muted; a held `#`
unmutes.
- **The stretch, phone 2:** at z16 over a spot where the pool has z13 but nothing deeper, the
  corner read `z16* otm` and the chip `z13 tiles stretched 8x`. At that tile's corner at z15 it
  read `z13-12 stretched 4-8x`, and the roads lined up across the seam.
- **A real OpenTopoMap z17 download, phone 2:** 2 km, 567 tiles (555 new, 12 already there), 0
  failed, 1,243 s (21 min). z17 went at 2,068 ms a tile and z11-16 at ~3.5-4 s. The internal
  largest block never fell below 49,616 B (the heap's min-ever since boot: 38,868 B), and the
  worker's stack floor was 4,572 of 8,192 B. The saved job cleared itself at the end.
- **Over it:** z17 is REAL (`z17 otm`, no star, house numbers readable). z18 reads `z18* otm`
  and `z17 tiles stretched 2x`, with a 20 m scale bar and the one-shot "Past the tiles" note.
- **Crash resume, phone 2:** an esptool reset in the middle of a 5 km z17 job. At boot the job
  waited to resume, then `resumed the otm download at z16 #1 (pass 1, 217 written, strike 1)`.
  The strike cleared after 50 new tiles. A reflash in the middle of the run later resumed `at
  z17 #399 (pass 1, 923 written, strike 1)`. That job was left running on phone 2.
- **Phone 1:** `z17* usgs-topo` with `z16 tiles stretched 2x`. The Download form for
  OpenTopoMap 20 km to z17 read `50451 tiles, 6.2 GB, up to about 36 h` and `Keep it on USB:
  about 36 h of downloading`. USGS Topo's Detail stops at z16.
- **COVEY:** deployed and cut over, as above. Its Mac suite and its device suite are both 72/72.

Not yet exercised on hardware: a resume after a WiFi drop, a double Stop, and a battery stop
off USB.

## 0.9.77 (2026-09-22) - one level past the tiles, on all three devices

{user}, with the 132,826-tile pool freshly on both phones and COVEY: *"for all 3 devices, can we
allow them to zoom in one level more (maybe add a little bit of text saying its zooming past
the tile level or something of the sort) on maps? i totally get that it will get fuzzy, but
would be usefull regardless."*

**The view may now sit one level deeper than the deepest tiles the area has**
(`MAP_OVERZOOM_MAX`, map_tiles.h), with those tiles stretched 2x — each tile pixel drawn as a
2x2 block. 🔑 **The VIEW still lives at the zoom the user chose**, which is what keeps the
scale bar (50 m where z16 read 100 m), the pin projection, the crosshair readout and the
distance lines exact; only the tile FETCH is pulled back to the level that exists
(`MapsApp::overZoom()` / `tileZoom()` — everything that touches a tile uses the second,
everything that is geometry uses the first, and mixing them up draws a map that is wrong by a
factor of two without looking it).

**It says so in three places**, because a fuzzy screen otherwise reads as a bad tile or a dirty
lens: the corner chip gains a star (`z17* usgs-img`), the status chip reads `z16 tiles
stretched 2x`, and the step that crosses the line notes *"Past the tiles: z16 stretched 2x.
Same detail, bigger - and fuzzy."* once — not every frame. A further press now says *"z17 is
as close as this map goes, tiles and all"*. The stretched level survives a power-off, and it
travels between areas but only ever one level past that area's own tiles (z17 over a z13 area
would be a 16x smear).

**The arithmetic is its own tested function.** `mapOverzoomView()` pulls the centre back
(rounded, so in-and-out lands on the same ground) and divides the viewport, **rounded UP** so
the last row and column of magnified blocks reach the screen edge instead of leaving a strip
that reads as a missing tile. `test_maptiles` +7: the identity at over 0, the halving of both
centre and viewport, the odd-viewport round-up, the odd-centre rounding, a z16 -> z17 -> tile
view round trip landing on exactly the ground it left, and the clamp past the cap. The push
itself widens one source row into a 480-byte stack buffer and pushes it twice.

**COVEY too** (`covey_ui/mapview.py`): `ZMAX` 16 -> 17, and `TileCache.get_stretched()` returns
the ancestor tile scaled to size and **cropped to the right quadrant** — the info chip then
reads `z17 · USGS topo · z16 stretched 2x`. Online it streams the real z17 tile and there is
nothing to stretch. New `tests/test_map_overzoom.py`, 17 checks, built around the failure that
would look plausible: a stretched tile showing its NEIGHBOUR's ground. A parent painted as
four coloured quadrants proves each of the four children takes its own, corner to corner.

Proven on hardware, both phones: aerial z16 -> z17* and topo z16 -> z17*, a further press
refused, panning at z17, zooming back out to the same coordinates and the 100 m bar, and the
stretched zoom still there after a reflash and reboot.

## 0.9.76 (2026-09-21) - hold '#' to mute

{user}: *"maybe that we can make holding the pound key mute and unmute. It already has an icon on
it that suggests muting. Obviously this will only work when you're not entering text or when
that key isn't being used for something else"* — and, on the clock's shortcut: *"I'm not sure
if it automatically tries to bring up the dialer when the button is held. But maybe we can
stop that behavior if it does."* It did; it doesn't now.

**Hold `#` for half a second** and the master mute of 0.9.75 flips, wherever `#` is nobody's:
the clock, every menu and list, Books, Music, the Game Boy picker and a running game. A
70 ms buzz says it took (the one confirmation a mute can give), the crossed speaker appears
in the header — and on the clock face, which draws its own icon row and had never shown it.
Where `#` IS somebody's, nothing changes: any focused text field (the dialer's number, a
plain field where a tap is shift, a predictive field where the hold is still the capital),
and the map, where it is zoom in. The gate is the same one every hold uses — screen on,
unlocked — and the hold rides through the keypad chip's release-blip like the others.

**The press is swallowed, unlike the capital's.** On the clock and in the menus a tap of `#`
opens the dialer with a `#` in it (or Messages when the envelope shows); keys are dispatched
on key-down, so the old shape would have opened the dialer and THEN muted. While the mute
hold is armed the press is owed instead of delivered: released early, it is the tap it
always was and goes to the same `processEvent`, ~100 ms late; held, it is the mute and the
tap is forgiven. The F2 music key is decided on release the same way.

🛑 **The first time it fired inside a game it wedged the phone.** Phone 2, uCity running,
`key hold # 800`: the mute flipped, the buzz started — and `loopTask` never came back
(task watchdog; the motor ran until the phone was reset, which {user} heard from the next
room). The icon refresh asked for `REDRAW_HEADER`, and the GUI pushed the header rows to
the LCD under the emulator's blit task, which owns the SPI bus from the other core. The
request now stays away during a game, and `GUI::redrawScreen` refuses header and footer
pushes while `gGbcActive` from any source — the pause menu's own `REDRAW_SCREEN` still
reaches the app, which paints the LCD itself with the blit task parked. Re-run: two holds
in the running game, unmute then mute, `buzz off after 70 ms` both times, loop alive.

**Bench: `key hold <key> [ms [blip]]`** — `maps hold` for every key. The hold trackers ask
`uiKeyDownOrBlip()`, which now answers from the bench too, so the F2, held-digit and `#`
holds can all be driven from the cable. The mute icon on the clock: `GUI::drawMuteIcon` in
`ClockApp::redrawScreen`. Settings → Mute all sounds gains the line "Or hold # where nothing
is typed." ⚠ Injected keys do not reach a RUNNING game (known since 0.9.75: `key end` never
paused it); `gbc autosave` parks it and an esptool reset is the way out from the cable.

Proven on both phones, from the cable: clock (mute, no dialer; a tap still opens the dialer
with `#`), the dialer (types `#`, no mute), a submenu (mute, stays put; tap → dialer), the
Phonebook list (unmute), Compose's To: field and its T9 body (no mute; the hold in T9 stepped
the mode back as before), the map (zoom z15→z16, no mute), a running game (unmute, mute).

## tools (2026-09-21) - pooling the tiles of three devices; the converter is 40x faster

{user}: *"All three of my devices (both wiphones and covey) each have random tiles from
different downloads. Can you pull the files from each and combine them all to get the max
tile coverage and put them back on the devices?"* No firmware in this one.

**The numbers first, because they chose the method.** COVEY's cache: 124,091 PNGs (usgs-topo
55k, usgs-img 40k, otm 29k, z8-16). Phone 1: 26,025 raw tiles; phone 2: more. The union is
~127,000 tiles = **15.5 GB per phone** in the phone's 128 KB-per-tile format — three days each
over the WiFi uploader (48-69 KB/s measured 2026-08-26), and nothing serves a file OFF the
phone over WiFi anyway (`/log` is the one GET, at 60-78 KB/s). Under an hour onto a card in
the Mac's reader. So the phones' halves wait for their cards; everything else runs now.

- **`tools/convert_tiles.py`: ~300 tiles/s** (was ~7): the per-pixel Python loop packs a tile
  in ~150 ms, numpy in ~1 ms, and `--jobs N` (default: one per core) converts several at
  once. Same bytes — `tests/check_convert_tiles.py` holds the numpy path to the loop. The
  workers read and write their own tiles; `.mbtiles` rows are handed over as bytes since
  SQLite does not cross processes.
- **`tools/tiles_565_to_png.py`** — the way back: a phone area folder → `<z>/<x>/<y>.png`,
  keeping any PNG already there (an original beats a tile that has been through 5-6-5). Bit
  replication on the way up, so white stays white. `--jpeg` for imagery: COVEY's usgs-img
  files are JPEG bytes under `.png` names (its downloader keeps what the server sent), and a
  photo as true-colour PNG is 105 KB against 42 KB at JPEG q92 (measured; ≤2 LSB of 5/6 bits
  after a round trip, half the pixels identical). The check now round-trips
  565 → PNG → 565 through both scripts: a tile that goes phone → master → other phone passes
  through both, and one bit of disagreement is a map in the wrong colours on the second phone.
- **`tools/card_clone.sh pull|push`** — the whole card to a folder and back: rsync with the
  FAT32-appropriate flags, macOS's `._`/`.Spotlight`/`.fseventsd` litter excluded both ways,
  Spotlight told not to index the card, `dot_clean` after, and a file count on both sides so a
  copy that stopped early cannot pass for a finished one. Refuses a destination outside
  `/Volumes`. Never deletes.
- `docs/maps.md`: *Pooling the tiles of several devices* — the four commands, in order.

Bench: a 200 MB FAT32 disk image (`hdiutil`) — clone both ways byte-identical, 751 tiles
converted straight onto it in 4.8 s, zero sidecars. COVEY's cache is pulled with a tar stream
over SSH per zoom folder with an rsync pass to fill anything a stalled stream dropped: its
WiFi link measured 185-450 KB/s (a Pi 3A+ at 54-66 % signal; NetworkManager's WiFi power save
turned off on the connection, which did not change it), so 2.5 GB is ~90 minutes.

## 0.9.75 (2026-09-21) - a master mute; the Game Boy leaves the phone its RAM

{user}, at work: *"You could always implement a master 'mute' button within the settings. Just
make sure it also works for the Gameboy emulator before trying... Up to you though."*

**Settings → Mute all sounds.** One ON/MUTED choice, applied the moment it changes and stored
(`configs.ini [audio] mute`), read back at boot. It is done AT THE CODEC — the WM8750's DAC
soft-mute plus both output pairs at their floor, and the loudspeaker amplifier left off —
whenever the LOUDSPEAKER is the route, which is what the ring, the message chirp, the mesh pop,
music and the Game Boy all use and the one output the room hears. A call answered to the
earpiece, and anything on headphones, plays as normal: nobody else can hear those, and a muted
phone that could not take a call would be a phone left unmuted. Every volume write in `Audio`
goes through one gate (`applyVolume`), so no app's volume change can undo it (the codec's own
`setVolume()` clears the mute bit as it goes; the gate writes it back). The volumes the apps
set are kept underneath, so unmuting is exact. The vibrate motor is not sound and is not
touched — "Vibrate only" still vibrates. A crossed speaker sits in the header while it is on. Serial `mute on|off` is the same path from
the cable, and `audio` reports `muted=`. Bench, phone 2: `mute on`, a game launched and left
running with the codec powered and `muted=YES`; the setting survived a reboot.

**The Game Boy under the new memory layout (0.9.74 gave the phone its Bluetooth RAM at
boot).** First game, `gbc`: `speed=101% wram=internal vram=internal` — and the in-game HEALTH
line read **4.8 KB free, largest 2.3 KB**, where every earlier session on both phones had sat at
35-38 KB. The 30 KB is gnuboy's 32 KB work RAM: with a 67 KB block on offer it went internal,
where before — no 32 KB contiguous block after WiFi went off — it had been landing in PSRAM all
along, at 98-100 % speed. A phone with 2 KB to spare while the blit task writes 180 KB save
states through FatFs is a phone waiting to crash. `gb_alloc_pref_internal` (gnuboy/hw.c) now
takes internal RAM only when **20 KB stays free after it**; VRAM (16 KB) still fits, WRAM goes
to PSRAM as it always had. Measured: `speed=98% wram=PSRAM vram=internal`, in-game heap
37.9 KB / largest 35.6 KB — the old numbers back, with the phone knowing why this time.

**Why the reserve survived (the review's find).** arduino-esp32 (1.0.6 here, not the 1.0.5 the
notes said) releases the Bluetooth DRAM itself in `initArduino()`, before `setup()` — unless
`btInUse()` returns true. The weak default returns false; the strong `btInUse(){ return true; }`
lives in `esp32-hal-bt.c`, and that file is linked in by ANY reference to `btStop()`. The one
reference in this firmware was `Networks::disable()`'s `btStop(); // ... leave it off to save
power` — a no-op on a controller that is never started. One dead line cost every phone 40 KB of
internal RAM for as long as it has existed (ELF: `btInUse` compiled to `movi a2,1; retw`; the
only `btStop` call was that line). Removed, together with the `esp_bt_controller_disable()`
beside it; `btInUse` is a weak symbol in the ELF again and the core does the release. The
`setup()` block stays as the belt to the framework's braces and logs which of the two did it.

**A game gives the RAM back when it ends.** `~GbcApp` unloaded the ROM and left the two task
stacks (12 KB), VRAM (16 KB) and the audio buffer (4 KB) — and WRAM, when it had landed
internal — resident until reboot; only the "Transfer ROMs" row ever called
`gbcReleaseEmulator()`. So the first game of a boot cost the phone 32-64 KB of internal RAM for
the day. The destructor releases everything now (the tasks are already deleted by then), and
the next launch re-allocates as the first launch of every boot always has. Measured on
phone 2: idle 69.0 KB → in game 37.7 KB (WRAM in PSRAM, VRAM internal, 100 %) → after Quit
**68.2 KB, largest 67.4 KB**. The floor in `gb_alloc_pref_internal` tests the largest free
block as well as the total, per the review: a 16 KB VRAM out of the only 20 KB block would
leave the phone's largest at 4 KB.

**A tile over 96 KB is a tile now.** Phone 1's restarted run showed its remaining failures
were `tile larger than 96 KB` — USGS aerial tiles past the downloader's body buffer, which
failed the same tiles on every run (grey squares that no Start could fill). The buffer is
PSRAM, so the cap is 256 KB now. The card retry, measured on the same run: **9 refused
writes, all 9 written on the second try, none lost** (the earlier run had lost 103 of 2,722).

**README:** the SD card to buy, with the measurement behind it ({user}: "Others have asked").

## 0.9.74 (2026-09-21) - the phone gets its Bluetooth RAM back; the map downloader says its numbers

{user}: *"When downloading maps on phone 1 (not phone 2), the downloads get very slow and the
'waiting for memory' is always being displayed... Then eventually it stops due to ram problems
it says. Can we make sure the maps downloader is designed for the worst case ram condition?
Also, can you check if phone one has something going on that is taking extra ram?"*

**What phone 1 had.** `maps dl` on the cable: `RUNNING (waiting for RAM) | 0/12864 done, 5303
skipped ... 0 KB down in 351 s | last: no RAM for a new connection`. The downloader's
handshake gate wants 14 KB of internal RAM free before it opens a TLS connection (a handshake
takes ~9 KB for a second, and the WiFi stack aborts the phone when it cannot get a few KB);
phone 1 with Maps open sits at **13.2-13.7 KB** and never reached it, so the run failed a
tile every 30 s and stopped after twelve, having said only "waiting for memory". Phone 2 in
the same state has 15.7 KB. The 2 KB between them is **SIP**: phone 1 is registered with a
SIP server (`sip=1`; 3,402 `SIP REGISTER -> sending` lines in its log against 57 on phone 2,
which sits at `sip=0`), and a minute after boot on the same firmware phone 1 has 25,960 B
free to phone 2's 28,228. Nothing is wrong with phone 1; it is the phone with the account.

**What every phone had, and never used: 56 KB of internal RAM reserved for Bluetooth.** The
SDK is built with Bluetooth on (`CONFIG_BT_RESERVE_DRAM 0xdb5c`), the linker starts the app's
RAM above that window, and nothing in this firmware has ever started Bluetooth. The Game Boy
has reclaimed it on its first launch since the emulator landed — that is how a game finds room
for its stacks and VRAM — and every phone that has run a game has then run the rest of its day
with the memory in the heap. Everything else ran in the 16-28 KB left over: every "the internal
heap is only ~16 KB" note in this repo, the fragmentation crashes, the downloader's bar.
`setup()` now calls `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` first thing and logs
what it got: **`BOOT: BT reserve released: internal free 171600 -> 211924`**. Idle, with
WiFi and SIP up: **phone 2 heap 28,228 -> 68,816, largest block 24,704 -> 67,564; phone 1
26,000 -> 66,320 / 66,076.** A 20-tile HTTPS download on phone 2 bottomed out at **48 KB free**
(phone 1's run this morning: 5.3 KB). What comes back is ~40 KB, not 56 (five regions:
6,192 + 25,480 + 6,688 + 2,412 + 52 bytes; the other 16 KB of the window were never withheld
— heap from boot, per the SOC's reserved-memory table), added at the HEAD of the allocator's
list — so they absorb the boot-time allocations made after them, and
the main region's ~67 KB block stays whole for a TLS handshake or any other large request
(the review corrected my first description, which had the order backwards; the 67 KB
"largest" is the proof). ⚠ No Bluetooth can start in a boot after this — which has been true
of every phone that ran a game, and nothing here asks for it. **0.9.75 found WHY the reserve
survived at all — see there.**

**The downloader, for the worst case.** The bar stays (it is what keeps a handshake from
being a reboot) and is now exactly 14 KB / 10 KB (it was 14,000 / 10,000 bytes, which printed
as "13" and "9" against a reading of "13.6" — the review caught the screen contradicting
itself), but the wait now says what is short: the Download screen adds `Needs 14.0 KB free
(has 13.6) and a 10.0 KB block (has 10.5)` under "Waiting for memory", the failure text
carries the same numbers (`no RAM to connect: 13.6/14.0 KB free, 10.5/10.0 KB block` — sized
to fit the 96-byte error field, which the first wording did not), and a run that is refused
three connections in a row for want of RAM (90 s, its own counter — a network failure is not
the same run of luck) stops with them rather than failing twelve tiles over six minutes. `maps dl` prints the
numbers while waiting and a `job:` line with the run's spec — source, centre, radius, zoom —
which is also how {user}'s run was restarted on the cable after the flash: its centre recovered
from the tile columns already on the card (`ls /maps/usgs-img/16`), `12864 tiles` back to the
tile, 5,303 skipped in four minutes, then downloading with 53 KB free.

**Phone 1's card refuses one write in eight.** Watching the restarted run: 35 tiles "written on
the second try" and 7 lost outright in the first 311 — `card refused the write` is a short
`f.write()`, the SD layer's 500 ms allowance for a card to come out of busy running out on a
card that pauses longer for its own housekeeping (phone 2's card did it 4 times in 12,853). A
tile lost there had already crossed the network. The write is now tried again after 200 ms,
then 1 s, then 3 s — long enough for any card's pause — with the pixels still in PSRAM, so
nothing is fetched twice; only a card that refuses for five seconds fails the tile. The
retries are counted: `maps dl` says `card busy N times`, and the Download screen's last-run
summary adds "The card was busy N times (each write waited and went through)" — a few is
normal, one in eight is a card to replace before the woods.

**Also:** `gbc` reports `wram=internal|PSRAM vram=internal|PSRAM` — where gnuboy's work RAM
landed — for checking the emulator still gets its fast RAM under the new layout (to be run
when a game can be started; the bench phones must stay silent today).

## 0.9.73 (2026-09-20) - the map's hold-to-scroll no longer stops under a held thumb

{user}: *"So the hold to scroll through maps isn't reliable. It works for one to 3 or 4 jumps
before it just stops... Ideas?"* — then, on the fix: *"Worked."* / *"Tried another way, worked
too. Both were a little choppy but not bad."*

**The chip emits a release under a held finger.** The raw keypad trace (`keys raw`), taken
while {user} held RIGHT on phone 2:

```
+58ms  0x5C P     RIGHT held: a re-report every ~58 ms...
+124ms 0x00 r     ...two beats of silence, then a RELEASE (key code 0)...
+4ms   0x5C P     ...and RIGHT pressed again 4 ms later, beats resuming.
```

Nothing left the key. It happened at the first re-report slot (123-125 ms after the press,
three holds running — the sweep never started) and mid-hold (2-4 repeats in, four more); the
counters showed 22 heartbeat gaps over 100 ms in a few minutes of holding. The firmware
attributes a code-0 release to the one key that is down (0.9.3x's fix for "every release is
CALL"), which cleared `uiKeyDown`; the map's 50 ms poll read "up" and dropped its hold; and
the re-press 4 ms later was killed as contact bounce (`KEY_BOUNCE_MS` = 25 — correctly: a menu
must not double-step), so no keypress ever told the map the finger was still there. Hence
"stuck": a thumb on the key, a map that had stopped.

**The fix is in the map, not the keypad path** ({user}'s rule from 0.9.66: "for everything but
maps, the d-pad works well"). `MAP_PAN_HOLD_BLIP_MS` = 40: a release younger than that is not
a finger coming off — the poll keeps the hold (`panHoldBlipMs`) and looks again next tick,
and the same arrow arriving as a keypress inside the window (a re-press just past the bounce
filter) is the hold continuing, not a tap: no nudge, no restart of the 400 ms delay. Humans
cannot re-tap a key inside 80 ms (measured 2026-08-22), the blip is 4 ms; 40 sits between, so
fast tapping is untouched. WiPhone.ino gains one read-only accessor for it,
`uiKeyUpAgeMs(mask)` — how long ago the key's last release was decoded.

**And the sweep moves by time now, not by tick — at twice the frame rate.** The repeats were
asked for every 100 ms and came every 125-170 (a map redraw and a tile read between them run
long), each moving the same distance late — the choppiness, and a sweep ~25 % slower than
designed. `mapPanHoldMove(run, dt, &carry)` (map_tiles.h, `test_maptiles` +7) turns the curve
into a speed — 24/48/72/96 px per 100 ms, `run` counted from when the repeats began — and
moves each frame the distance its own interval warrants, sub-pixel remainder carried; a tick
over 250 ms counts as 250, so a stalled loop cannot teleport the view. Then the frame itself,
timed on the phone (107 ms): 15 ms of it filled the whole viewport black before the tiles
covered it again (skipped when the blits cover the band), ~15 ms pushed the header and footer
rows that never change under a held arrow (`WiPhoneApp::drewInsideBand()` — the map says yes
on its view, and `GUI::redrawScreen` pushes only the band between them; the header's own
repaints still push the header), and the "at least 100 ms since the last repeat" gate on a
50 ms timer let every other tick through — one frame per 180 ms from a phone that draws one
in 77. Every tick moves now. Measured on phone 2 from the cable, 3 s holds: **14 frames →
29-31 frames, 2,215-2,231 px (the curve says 2,208)**; a frame is ~77 ms: ~18 ms copying tile
rows into the page (PSRAM to PSRAM — swapping the bytes once at load and memcpy-ing was tried
and saved nothing), ~10 ms markers and labels, ~45 ms pushing 240x250 pixels down a 40 MHz
SPI bus. Faster than that means DMA or a faster bus; not tonight.

**Reviewed (4 lenses, 15 findings; the verifiers ran out of Fable credit after the first, so
the rest were checked by hand).** Real and fixed: (1) the blip is judged on DRAIN-time
stamps — the re-press comes 4 ms after the release but is read on the next pass, and a pass
that went on to read a 32 KB tile piece (25-60 ms) delivered it 25+ ms "late": past the bounce
filter, so dispatched as a keypress, past the blip window, so taken for a fresh tap — nudge,
snap, a 400 ms restart. The tick that sees a fresh release now does nothing else that pass
(the tile waits one tick), which keeps the re-press inside `KEY_BOUNCE_MS`, where the keypad
path absorbs it silently; on the bench the ride-throughs went from "51ms release blip" to
"24ms". (2) The same chip blip hits the loop's OTHER hold trackers: F2 for the music player
(a blip mid-tap = `Next` twice, mid-hold = a spurious `Next` before `Previous`), a held digit
(typed twice on a long hold), a held `#` — all three now use `uiKeyDownOrBlip()`, "down, or
released under 40 ms ago inside a hold". (3) `maps hold` armed the bench override BEFORE
injecting the key, so a full key buffer left a phantom hold armed against the next real tap
(inject first now); a non-numeric `[blip]` ran as a plain hold and said so (an error now); the
usage line still showed the old shape. (4) `keys raw` moved its cut from 192 bytes to
keypadTrace's 1,400 (64 entries with `+more (poll)` tags are ~1,800): it prints one entry per
call now, no big buffer at all. (5) Comments: the heartbeat has measured 58 ms (today) and
109 ms (2026-08-22) — both the chip's; only the 350 ms sweep depends on it and is clear of
both. Rejected: a human re-tap swallowed as a continuation needs the tap under ~120 ms after
the lift AND the release drained late — the tap is honoured as a nudge otherwise, and the
window is the same 40 ms the ride-through needs. Noted, not done: a blip on a single held
Game Boy direction can drop one frame of input (1 in 4 blips; measure with `keys` in a game
first); phone 1's heartbeat has not been traced (`keys raw` while holding an arrow).

**The bench:** `maps hold <dir> <ms> [blip]` fakes the chip's release-and-re-press at `blip`
ms into a hold (`hold continues past a 24ms release blip (6 repeats in)` / `hold ended after
31 repeats, 3040ms, 2231 px` are the log lines). Seen on phone 2 by hand, three sessions:
`hold continues past a 49ms release blip (3 repeats in)`, `...51ms... (4 repeats in)` — real
ones, ridden through; {user}: "Worked." / "worked too" / "Did it a bit better" (that last on the
by-time build before the frame-rate work).

## 0.9.72 (2026-09-20) - folders in the file browser

{user}: *"On the file browser, I realize there is no way to select a folder and copy/paste/
delete. Found it out when trying to delete 'home' in the maps folder. Can we add this? Some
way of selecting a folder."*

**How a folder is selected.** OK on a folder row still enters it (browsing stays one press a
level). Inside any folder but the root there is now a **`[ This folder... ]`** row, and it
opens Copy / Move / Delete for the folder you are standing in. Copy and Move mark the
clipboard exactly as a file does (`[ Paste "home/" here ]` appears in whatever folder you walk
to); a folder **Move is one rename** — FAT moves a whole tree across folders in an instant;
a folder **Copy** and a folder **Delete** are tree walks, so they are JOBS.

**A job runs a slice at a time.** `stepJob()` works for 80 ms per tick of the app timer
(30 ms), then hands the loop back — the keypad, WiFi, the mesh and the music keep running
underneath, and **Back stops it where it is** with a note that says what got done ("Stopped:
120 files deleted, the rest still there"). The walk keeps one open directory handle per
level (at most six; each costs ~700 B of internal heap) so the next entry is one readdir,
not a rescan; an entry is unlinked as it is read (safe on FatFs: it is marked deleted in
place, and the directory's read position does not move); a directory is removed when its
handle runs dry. Delete goes through POSIX `unlink`/`rmdir` rather than `SD.remove()`, which
is a stat, an open and a close before the unlink — 25 s became 16 s for the same 271 files.
A copy streams 32 KB per pass with both handles held open, and a stopped copy leaves no
half-file. The screen is pushed at most four times a second (a push is 30 ms of the same
SPI bus), and while music plays the slice is 35 ms and the DMA is fed between operations —
the I2S buffers hold ~90 ms.

**Delete asks, and counts while you read.** "Delete home/?" — Cancel first, then "Yes,
delete all of it", and under them a line that fills in as a COUNT job walks the tree:
`271 files, 38 folders, 33 MB`; a subtree it could not read makes the number a stated floor.
OK on that line does nothing (a note is read, not chosen).

**What is refused, and why** (`files_paths.h`, host-tested by `test_filepaths`, 22 checks):
pasting a folder into itself — the ONLY thing that stops it: FatFs `f_rename` has no
ancestor check, so `/maps` renamed to `/maps/home/maps` would re-parent the tree under
itself, unreachable; deleting or moving the area a running map download is writing into
(the worker outlives the Maps app and would recreate folders under the delete; "stop it
first" instead); and a delete at the card's root, which is simply not offered. The clipboard
is cleared when the tree it points into goes.

**Reviewed by 20 agents, 3 lenses + refute-or-confirm.** Real and fixed: the download race
above; a failed job start that left the confirm's OK bound to the browse menu's `[..]` key
(the confirm's keys are 101/102 now, and a failed start returns to browsing); a job's
"Stopped" note and screen-hold on a start that never began; the confirm rebuilt every tick
(now only when the numbers move); `bytes` in 32 bits (a map area can pass 4 GB); the
first error kept rather than the last (on a delete every later one is "folder not empty",
up to the root); an unreadable directory entry read as the end of the directory (errno
tells them apart); the audio DMA under the slice.

**Seen on phone 2:** a 5-file scratch tree copied, moved (rename) and deleted, the delete's
confirm counting `5 files, 4 folders, 19 KB`; `/maps/home` (271 tiles, 33 MB) copied to
`/home` in 3 min 19 s, and `/home` deleted in 16 s with no LOOP STALL from the job; paste
into itself refused. Serial `open files` opens the browser for the bench.

## 0.9.71 (2026-09-20) - where a map's tiles are

{user}: *"When toggling through maps, what is the 'home' map? I don't have any tiles for it and
there are no tiles available on the downloader. Can we figure that out?"*

What it is: `home` is the area from the first maps release — the tiles converted on the Mac
with `tools/convert_tiles.py` from COVEY's tile tree and copied to `/maps/home/`, z11-15,
covering about 47.28-47.52 N, 121.99-121.64 W (North Bend and the Snoqualmie valley; the pins
are inside it). From the hunt area east of Stampede Pass that is 35 km away, so switching onto
it there shows grey — and the downloader has no `home` source because it only writes its own
three areas. Nothing was wrong; the phone just could not say any of that.

**Now it can.** `scanAreaExtent()` reads each area's bounding box once when Maps opens — the x
folders at its coarsest zoom and the y files in the first and last of them, three small
listings per area — and:

- switching onto a map whose tiles are elsewhere (the side button or the menu) says
  `Map: home (z11-15) - its tiles are 35km NW of here` on the strip (two rows, since 0.9.70);
- the **Map area** list ends every row with `here` or the distance and bearing to that map's
  tiles from the crosshair, and a note under it says what the numbers are;
- serial `maps` prints each area's box in degrees and its centre.

A box, not a census: an L-shaped download reads as its enclosing rectangle, which is what
"where is this map" wants. Seen on phone 2: from the hunt area, `home * z11-15 35km NW` and
the other three `here`; from the pins, all four `here`.

## 0.9.70 (2026-09-20) - two quick ones on the map

{user}: *"If I don't have a GPS fix and try to center on myself, the message gets cut off due to
lack of text wrapping. Can we make the last user button cycle through map types? Let's also
update the controls guide."*

**The strip's message wraps.** The bottom strip was two rows — the scale bar and one line
for a message — and "No fix yet, and no pin of your own to fall back on" is 50 characters
where the row holds about 30. The message row now takes a second row when it needs one
(`noteRows`, through `menu_wrap.h` — the same breaking the menus' notes use), the strip grows
upward over the map for it and shrinks back at the next key; the no-map page does the same.
Seen on phone 1: `Nothing of your own - centred on / hunt camp maybe` on two rows.

**The fourth side button cycles the map types** — the same switch as Menu → Map area, one
press at a time, round and round; the ground under the crosshair stays put and the strip says
`Map: usgs-img (z11-13)`. With one map on the card it says so instead. The main loop already
handed the map all four side buttons while it is open, so nothing changed for music. Seen on
phone 1: topo → aerial → topo.

**The controls guide** (Menu → What the buttons do...) draws the fourth side button as
`map type` — eight characters, because that row shares its line with the D-pad's "arrows
scroll" caption and "next map type" ran under it — and the text page gains "4th side button:
next map type". docs/maps.md and the README have the row.

## 0.9.69 (2026-09-20) - the snap is a landing, not a catch

{user}, the same morning: *"Can we make the snap to pins/locations less aggressive? I still want
to be able to scroll around them, but if I just land near one while scrolling (as near as I
possibly can) then I want it to snap. It needs to feel intentional... You could make it more
sensitive by making each d-pad click move less since it already will accelerate as you hold
it."* Yes to all of it.

**What was aggressive.** Yesterday's snap did two things a tap could not get around: a marker
anywhere in a tap's 24 px path (plus 14 px either side) stopped the map on it, and a tap from
a marker jumped to the next marker anywhere on screen in a 45° cone — so from a pin, an arrow
could not simply move 24 px away if another marker lay that way. And taps sped up (24 → 48 →
72 → 96 px within 400 ms of each other), so the size of a tap depended on its timing — the
opposite of "as near as I possibly can".

**What it is now** (`panOnce` / `snapLanding`, app_maps.cpp; the numbers in map_tiles.h):
- **A tap is a 12 px nudge, always.** Only a HOLD climbs the accelerator (24 → 48 → 72 → 96 px
  at the 100 ms repeat, after 400 ms), and a hold's repeats never snap.
- **The map snaps only when the crosshair LANDS within 10 px of a marker** — after a tap, or
  where a hold stops (the release check). Nothing in the path of a move matters.
- The two numbers are tied, and `test_maptiles` now pins both relations: the radius is
  UNDER the nudge, so the tap after a snap steps clear (12 > 10 — that is "scroll around
  them"); and it is at least nudge/2 × √2, so taps on the two axes can put the crosshair
  within 8.5 px of any marker ("as near as I possibly can" is always near enough).
- The arrow-walk from marker to marker is gone; `7` / `9` still walk the pins in order, and
  the menu's three lists (nearest-first) go to any place or node.

🛑 **And a bug the new rule exposed, caught on phone 2 before this shipped:** a tap off Pin 1
"landed" on a node an ocean away. `snapNear` squared the marker's offset from the crosshair in
a `long` — 32 bits on this chip — and a marker across the world has view coordinates in the
millions, so the product wrapped and a garbage distance passed the radius test. The pin
picker (`mapPinPickNearest`) has guarded against exactly this since 0.9.64, with a comment
saying why; the snap's copy of the arithmetic did not. Yesterday's `snapUnder` had the same
wrap and could read "on a marker" while on nothing — which then walked the arrows to the next
marker in the cone: some of the "aggressive" snapping was this. Now the axis test comes first,
before anything is squared. Phone 2, after: `9` onto Pin 1, a tap right steps 12 px clear, a
tap left lands and says `Pin 1 (1 of 3)`.

Seen on phone 1 at z14 (a pin dropped for the bench, removed after; every position read off
the strip's coordinates): from 29.5 px east of the pin, one tap left lands 17.5 px off — no
snap — and the second lands 5.5 px off and the strip says `Pin 1 (1 of 1)`; a tap right then
steps 12 px clear, no snap; a 900 ms hold sweeps 156 px straight through the pin (ends 84 px
past it, never stopped); a hold that ends 8 px short of the pin snaps onto it when it is let
go (`Pin 1 (1 of 2)`), and one that ends on a second pin takes that one (`Pin 2 (2 of 2)`).

## 0.9.68 (2026-09-20) - the download screen says the whole sentence

{user}, the morning after 0.9.67: *"On the maps download screen, there is still text that can
get cut off, especially the status of the previous download attempt and the file size and so
on."* And, from phone 1's screen: *"something about card refused... idk what that means."*

**Every note on that screen wraps.** `12853 tiles, 1606 MB, about 257 min`, `Last run: 5517
new, 7332 already had, 4 failed, 7013 s`, an error handed up from the fetcher — these are the
lines whose length the numbers decide, and each was one row ending in `..` with the part that
mattered behind it. The selected-row marquee of 0.9.65 is no help: a note is read, not selected.
`MenuWidget::addNoteWrapped()` breaks a note at the last space that fits its font and width
into as many rows as it needs; the breaking is `menu_wrap.h`, header-only, with a host suite
(`test_wrap`, 42 checks: word boundaries, a word wider than the row is cut and never looped
on, UTF-8 boundaries, the last word kept). The Maps app's other variable notes — the pin
sheet's channel line, the delete confirmation's name, the lists' "nearest first" — wrap too.
Seen on phone 2: the estimate on two rows, the OpenTopoMap credit on three, `Last run: 0 new,
6 already had, 0 failed, 0 s` on two.

**"card refused the write" is named for what it is.** The fetcher's last error is shown as
`Last problem: z16 10714/23006: card refused the write` with, when anything failed, `Start
again fetches only the missing tiles` under it. The error itself is a short `f.write()` on one
tile — a busy timeout in the SD layer on a card that is otherwise fine; phone 2's 12,853-tile
run lost four tiles to it, each of which had already crossed the network. **A refused write is
now tried once more** (200 ms later) before the tile counts as failed, with a `TILES: … written
on the second try` line when the retry rescued it; ten refusals in a row still stop the run
(a full or pulled card).

## 0.9.67 (2026-09-19) - the Game Boy picks up where you left it

{user}, the evening of 0.9.66: *"Can you make the default for the Gameboy emulator to save state
upon exit and open last state upon entrance? That's how covey does it. I still want to be able to
clear state, from the in game 'end button menu' if I want and manually save state from the same
menu. I'd also like the default screen to be 'fill' instead of '1:1'."*

**Quit writes your place; the next launch of that game loads it, silently.** Two files per game
under `/gbc/` now, the way RetroArch keeps `<rom>.state.auto` beside its numbered slots (which is
what COVEY runs with `savestate_auto_save` / `savestate_auto_load`):

- `<rom>-<cartid>.auto` — the RESUME POINT. Written when you Quit from the pause menu, and by the
  two power-off paths (the 2.5 s button hold and the 3.30 V cutoff) before the latch drops, beside
  the book position and the map view that already save there. Loaded at launch; not finding one
  is the ordinary first launch and says nothing. Failing that, the manual bookmark is loaded once
  — the state a phone updated to this build already has.
- `<rom>-<cartid>.state` — the MANUAL slot, Save state / Load state, exactly as before: a
  checkpoint a quit never overwrites. With one file a manual save would have been pointless,
  since quitting overwrote it anyway.
- **Clear state** is the new pause-menu row: it asks (`OK again: wipes all saves`), deletes both
  files, hard-resets the core and resumes — the title screen is the confirmation. It wipes the
  cartridge's own battery save too (that lives only inside the states; COVEY's Start Fresh does
  the same); the help says so.

**The cart is in the file name** (`gnuboy_cart_id()`: the header checksum with the size codes),
because the name alone was never an identity: the sanitizer folds `Pokemon Red` and `Pokemon_Red`
together, the uploader overwrites a same-named file without a word, and a resume is now automatic
— a state written from one cart must never be loaded into another (review). Files from before
this build (`<rom>.state`, no id) are adopted once, if they are the right size for the cart:
phone 2's `TetrisDXWorldSGBEnhancedGBCo.state` became `…-00c82001.state` and resumed. Deleting a
ROM from the picker now deletes its states with it.

**Written atomically, and sized.** A state goes to `<file>.tmp`, its size is checked against what
the core says a state of this cart is (`gnuboy_state_size()`), then the old file is removed and
the new one renamed over it. Two cases closed: a power-off mid-write, which used to leave a
partial file that `gnuboy_load_state` copies block by block INTO the emulator's RAM before it
finds out the file ends early; and a full card, where FatFs returns a short write that gnuboy's
`fwrite(...) < 1` check reads as success. A load that fails on a file that exists is followed by a
hard reset for the same reason; a wrong-sized file is refused before it can touch anything.

**Where the work runs.** All of it on the blit task (`runAction`) — the one task that owns the
card and the panel and the one with an 8 KB stack (the fopen → FATFS → SD path overflowed the
old 2 KB stacks and rebooted the phone, 0.9.2). The main thread's side (`autoSaveNow`, from the
destructor and from `gbcSaveForPowerOff()`) parks the emulator and waits for the job. The park
is a handshake, not a guess: the emu thread drops `emuIdle` BEFORE it checks `paused` and raises
it only inside the parked branch, so a requester that sets `paused` and then waits for `emuIdle`
is waiting for the emulator's NEXT pass, never reading a stale flag while a frame is in flight;
the wait is unbounded (a streamed 4 MB cart can hold several 16 KB bank reads in one frame). A
job on the card owns the menu until it is done (keys are dropped, not queued), so nothing
replaces a job mid-write — and 🛑 **the destructor never deletes the blit task under a job**: a
write holds the SPI mutex the panel shares and FATFS's volume lock, and a task deleted inside one
takes both with it, freezing the next screen draw forever (review). A slow card is waited out,
with a log line a second.

**Screen: Fill by default**, and the toggle is remembered (NVS `gbc/fill`), written at Quit and
at power-off. The pause menu is six rows now and sized from the font (height 20, decoded from the
font's own bytes); the old fixed 176 px box had already put its fifth row under the status line,
and every un-pause now clears the screen, since the box reaches past the Fill frame.

**Measured (uCity, a 128 KB-SRAM cart — the largest state this core writes, 180 KB):** phone 1
auto-save on Quit 185-209 ms, resume 274-297 ms, manual save 204 ms, load 308 ms; phone 2 (a slower
card) auto-save 295-309 ms, resume 363 ms; Tetris DX (60 KB) 165 / 248 ms. Clear removed exactly the
one game's files and left the others. The proofs are `log_e` lines the cable sees — `GBC: resume ok
from …`, `GBC: auto-save ok to … (0 e0, 185 ms)`, `GBC: cleared … hard reset` — plus serial `gbc`
(what is up, which ROM, its two files and their sizes) and `gbc autosave`, which calls the SAME
function the power-off paths call and leaves the game parked under the menu.

🛑 **Found on the way: the phone could power itself off during a game.** Phone 1, on the bench,
on USB, with nobody near it, shut down three seconds into a launch — and the log showed the
power-off path running (it saved the game first, which is how it was seen). `SX1509::digitalRead`
answers LOW for a pin it could not read (a dead transfer returns 0 for the direction AND data
registers), the power button is active-LOW, and `gpioExtenderServiceInterrupt()` took that LOW as
a press: `powerButtonPressed` latched, and 2.5 s later — no release edge could clear it — the hold
handler pulled the latch. Under the emulator the two tasks starve the main loop and the extender
read timed out. Now `readPinChecked()` says whether the chip was heard, a failed read keeps the
old state, and the 2.5 s hold is confirmed by a fresh read before anything irreversible (a read
that fails there is asked again in 500 ms; a button that is up clears the stale flag).

Also: serial `open gbc` opens the ROM picker for the bench.

## 0.9.66 (2026-09-19) - the arrows stop on places and nodes too

{user}, an hour after 0.9.65 was cut: *"Can you make it snap to places in the mesh and nodes
with locations too?"* Yes. The snap now covers exactly the set the overlay draws — your pins,
places heard from the mesh, other nodes with a position — through one enumerator, so the
arrows and the screen cannot disagree. Landing on one names it on the strip: `Pin 3 (3 of 3)`,
`Camp (place from the mesh)`, `{user} H New Device (node)` — with the age when the position is
stale. The menu row is **Snap to markers**. Seen on phone 2: four taps from Pin 2's spot landed
on a place, a pin, a place and a node in turn, each exactly on its marker.

0.9.65 was staged but never pushed; its number is retired rather than reused — one version
string, one binary (the 0.9.57 lesson).

## 0.9.65 (2026-09-19) - the six things {user} noticed: menus, long messages, the buzz, the Game Boy's speaker, and the map's arrows

**The compose field stops at 200 characters, shows `N/200` as you type, and a send the radio
refuses stays on the screen with the reason.** {user}, 2026-09-19: *"The WiPhone lets me send
Meshtastic messages that are too long, and they just won't get through to anybody else."*

Three faults lined up behind that sentence, and the message fell through all of them:

1. **The radio driver gave up on long frames while they were still going out.**
   `MeshPhy::send()` polled TxDone with a flat 2000 ms timeout. A maximum-length frame at the
   registers this phone writes (SF11, BW 250 kHz, CR 4/5, 16-symbol preamble) is **2157 ms**
   on the air -- Semtech AN1200.13, now worked by hand in `tests/test_airtime.cpp` and computed
   by `mesh_airtime.cpp` -- so the frame was declared timed out, its IRQ flags cleared and the
   chip forced back to RX with the PA still keyed. Truncated on air, `false` returned, the
   whole stall paid for nothing. docs/HANDOFF.md had it filed as a latent P2 since 08-27. The
   wait now follows the frame: airtime + 25 % + 300 ms, never under a second -- 1000 ms for a
   37-byte position beacon (what it effectively always got), ~3 s at the very worst for a full
   frame. The timeout line names the frame length and the airtime it expected.

2. **The compose cap was the receive buffer, not the frame.** The field was built with
   `MESH_TEXT_LEN - 1` = 233, which is one byte MORE than a 255-byte LoRa frame carries once
   the 16-byte header and the Data envelope (portnum, payload tag + two-byte length, bitfield
   = 7) are around it. The wire budget is 232 on a channel and 220 for a PKI direct message --
   both measured in the host suite by building the Data with the shipping `meshBuildData()`
   and counting -- and the Meshtastic Android and iOS apps stop the user at **200 UTF-8
   bytes**. The field now takes `meshComposeCap()` = the smaller of the two, per thread type
   (200 for both today), and a strip above the footer shows `N/200`, turning orange with the
   word "full" at the cap. The header and footer are untouched: the footer is the T9 display
   and stays that.

3. **A refused send vanished.** `MESH_COMPOSE` ignored `sendChannelMessage()`'s return value
   and dropped into the thread either way, so a message the service refused -- too long, no
   channel, radio busy, no radio -- was neither on the air nor in the conversation nor on the
   screen. It now stays in compose with the text intact and shows `Not sent: <reason>` in the
   same strip; Send is a retry. `MeshtasticService::lastSendError()` carries the reason from
   every refusal site, including a new size pre-check on direct messages so an oversize DM is
   refused at the key rather than a tick later in the deferred PKI queue, where it used to
   fail with nobody looking. The serial `send`/`dm` commands print the same reason.

Nothing on the receive path, the channel hashing or the crypto changed; `MESH_TEXT_LEN` is
still the receive buffer. `./tests/run_tests.sh` gains `test_airtime` (46 assertions).

**Proven on air, 2026-09-19, both phones on the merged build:** a 200-byte and a 232-byte
channel text sent from phone 1 were heard back (`MESH RECEIPT`) and read on phone 2 in full;
233 bytes came back `REFUSED - reason: too long for the mesh`; no `TX timeout` line. The 232-byte
frame is the one the old 2000 ms timeout could never carry (2176 ms on the cable's clock). The
compose strip read `0/200`, then `5/200` after a committed T9 word. Two small things the proof
turned up were fixed with it: a refused serial `send` no longer says to watch for a receipt, and
a message with a newline in it (COVEY's `R1: lat, lon` position texts) no longer draws its second
line over the sender beneath it in the thread and Chats lists.

### The highlighted menu row scrolls when its text does not fit

{user}, 2026-09-19: *"Any menu or line that is too long just cuts off, it should either scroll or
wrap text when highlighting over it."* Every list in the firmware draws through `MenuWidget`, and
every row ended in `..` for ever when its title or subtitle was wider than the row -- book
names, node lines, message previews. Now the **selected** row scrolls: hold the start (800 ms),
one glyph every 200 ms, hold the end (1 s), wrap; unselected rows keep the ellipsis and a row
that fits is untouched. One implementation (`drawRowText()` and a single static marquee state in
GUI.cpp) covers all four row kinds and every app -- Books, Music, Files, Photos, Maps, Phonebook,
Messages, the mesh lists, Settings -- and no app was touched.

- **Its own clock.** `GUI::marqueeTick()` runs from the loop next to the app timer; apps own
  `msAppTimerEventPeriod` for their own purposes and keep it. Nothing to scroll costs one
  pointer test per pass; a scrolling row costs one ordinary `REDRAW_SCREEN` every 200 ms.
  Silent while the screen sleeps, the phone is locked, a power-off is pending or the mesh
  popup is up.
- **Whole glyphs, not pixels.** The vendored TFT_eSPI has no clip rectangle and shifts an
  over-wide `drawString` left instead of cutting it, so a pixel scroll would lose glyphs at
  both ends. Stepping by glyph and drawing only what fits needs no clipping, no extra sprite
  and no allocation. UTF-8 sequences are never cut in the middle.
- **Two texts, one offset.** A row that carries a name and a long second line scrolls both
  together; the shorter one parks at its own end and waits while the longer finishes.
- The clock and the glyph arithmetic live in `WiPhone/menu_marquee.h` so the host suite runs
  the code the phone runs: `test_marquee`, 50 assertions.

Seen on phone 2: Books > `Leviathan_Wakes_The_Expanse_1.epub` scrolls to `...1.epub`, holds,
and wraps; the main menu over the wallpaper and the Nodes list (short rows) are unchanged; the
loop-stall log gained nothing. Still hard-truncating, deliberately left alone: the header
title, `LabelWidget`, `ChoiceWidget`, the sliders and the text inputs -- none of them is a
list you highlight.

### The notification buzz and chirp are timed from the clock, and the chirp starts in 20 ms

{user}, 2026-09-19: *"Vibrate length of time is very inconsistent. Sometimes it's correct,
sometimes the motor just barely starts. Also when sound is enabled for notifications, sometimes
sound will play, but sometimes it won't."* One cause behind both. A mesh arrival is handled
inside the loop's `mesh` block, AFTER `meshService.loop()` -- the block whose DB save stalls the
pass 0.5-1.5 s, 556 times in phone 2's log -- and after a full repaint, and it stamped the motor
and the pop with the `now` taken at the TOP of that pass. The next pass, a few milliseconds
later, compared a fresh clock against a stamp already a second old, judged the 200 ms buzz and
the 280 ms pop long finished, and switched both off: a motor that "barely starts", a chirp cut
before it sounded. The SIP path stamped `millis()` itself and behaved, which is why it looked
random.

- **Stamps come from the clock.** `notifyMessageArrived()` lost its `now` parameter: the motor
  is stamped `millis()` right after the write that starts it, the pop right after `playPop()`
  returns, and the teardown compares against a fresh `millis()` too. A second message still
  re-arms the pulse. The mesh popup banner had the same stale stamp and is fixed with it.
- **The chirp plays from flash.** `playPop()` opened `/pop.pcm` on SPIFFS every time -- 505 ms
  for the open alone on the bench today, 1.2-1.6 s measured earlier -- with the motor already
  running, so a chirp both started late and stretched the buzz. The same bytes are compiled in
  as `pop_pcm[]` (they are what installs the SPIFFS copy at boot), so `Audio::playPcm()` now
  plays them straight from flash, once, then pads silence; the SPIFFS file is the fallback. No
  allocation. The stop timer is derived from the array's length in `notify_timing.h` (320 ms
  of samples; 360 ms stop) and pinned to the shipping bytes by `test_notify` (18 checks).
- **The motor write is checked.** The board's extender is an SX1509, whose `digitalWrite` was a
  three-transaction read-modify-write that returned nothing, and `allDigitalWrite()` said
  `true` regardless. The driver now returns the bus result and refuses to write back a word it
  could not read (a failed read used to write zeros over the LCD reset and the 3.3 V daughter
  enable); the motor write retries once and logs a failure. No such line has appeared yet.
- **It proves itself in the log.** Every arrival now prints `NOTIFY: buzz off after N ms
  (target M)`, `NOTIFY: pop start took N ms (flash, stop at S ms)` and `NOTIFY: pop stopped
  after N ms`; a chirp that could not start says why. Serial `notify` (and `notify sip`) fires
  the real arrival path from the cable.

Measured on phone 1 (Buzz 650 ms): three `notify` runs gave buzz off after **651 / 650 / 653
ms**, pop start **21 / 20 / 19 ms** from flash, pop stopped at 360-364 ms. A real message from
phone 2 that landed right behind a 1,062 ms `mesh` stall buzzed 869 ms and chirped in 20 ms --
the DB save that FOLLOWS an arrival can still hold the off-switch a little late (long, never
short; the pop is silence by then). Under the old code that arrival was the "barely starts"
case.

### The Game Boy plays through the loudspeaker

{user}, 2026-09-19: *"I believe the sound plays through the earpiece instead of the loudspeaker.
Can we make it play through the loudspeaker unless headphones are connected?"* `startGame()`
set the sample rate and the channel format and chose no output, so `Audio::start()` powered
the codec up with whatever `loudspeaker` was last -- the earpiece, the one-ear call speaker,
unless music had been playing. It now snapshots the routing, chooses the loudspeaker unless
headphones are in (the music player's rule, `music_player.cpp`), and puts the routing back in
the destructor on its own flag -- not `soundOn`, which the emulator clears on a starved I2S --
so the next call is on the earpiece again. Plugging or unplugging headphones mid-game routes by
itself through the jack interrupt's live `codecReconfig()`. The serial `audio` dump now shows
`route=headphones|loudspeaker|earpiece`.

Seen on phone 2: `route=earpiece` before, `route=loudspeaker gbc=1` with MICROCITY running,
`route=earpiece gbc=0 powered=no` after quitting through the game's own menu.

### Maps: a held arrow keeps scrolling; a tap stops on a pin, and walks the pins

{user}, 2026-09-19, two more: *"holding the scroll button doesn't allow it to keep scrolling and
speeding up, it just does a small jump and stops"* and *"it's hard to pan over an existing pin.
It jumps too far, so maybe the map center can either snap to a pin when very near, and if there
are many pins there it can kinda jump between them as the arrow pads are pressed. Also this
behavior should be togglable on the maps menu."* — with the rule *"for everything but maps, the
d-pad works well, so please only edit its behavior for the maps app."*

- **Why a hold did nothing.** The keypad path suppresses held-key re-reports for every app on
  purpose — that is what killed the menus' auto-repeat and the double-clicks in 0.9.2x — so the
  map's accelerator, which counts presses arriving within 400 ms, only ever saw one. Nothing on
  that path changes. The keypad reader now merely *answers a question*, `uiKeyStillHeld(mask)`
  (the key's down bit AND a heartbeat within 350 ms, so a lost release reads as up), and the
  map polls it on its own 50 ms timer: after 400 ms it pans again every 100 ms, climbing the
  same 24/48/72/96 px curve a run of taps climbs. Any other key ends the hold. Every other
  app is untouched.
- **Snap to markers** (Maps menu, persisted, on by default) — pins, **places from the mesh
  and nodes with a position** too (*"make it snap to places in the mesh and nodes with
  locations too"*), exactly the set the overlay draws, through one enumerator so the arrows
  and the screen cannot disagree. A *discrete* press — the first of a run, never a hold
  repeat — that would land on or fly past a marker (within the 14 px pick radius of the line
  of travel, no farther than the step plus that radius) stops **on** it; a press while sitting
  on one jumps to the nearest marker that way inside a 45° cone on screen. The crosshair ends
  exactly on it and the strip says what it is: `Pin 3 (3 of 4)`, `hunt camp (place from the
  mesh)`, `{user} H (node, 12m ago)`. OK on a pin opens it; OK on a place or node drops a pin
  there, as anywhere.
- Bench: `maps hold up|down|left|right <ms>` presses the arrow and answers "held" for that long.

Seen on phone 2: two pins three nudges apart — LEFT from Pin 4 lands on Pin 3, RIGHT lands
back on Pin 4, DOWN nudges off it, UP snaps back on; `maps hold right 2500` moved the view
0.082° east (~960 px at z14, four screens) with the tiles filling in behind.

## 0.9.64 (2026-09-18) - a map that downloads its own tiles, and the phone's first HTTPS

Built on the 09-14 map (below) and finished on hardware the night of 09-18, against {user}'s
list: download maps to the card from the phone, the COVEY features that make sense on a
keypad, the side buttons for zoom and centre, OK for pins, the soft key for the menu, a chosen
channel for sharing, and the view kept across power-off.

**Download maps on the phone.** `Menu → Download maps...`: USGS Topo, USGS Aerial or
OpenTopoMap; 2/5/10/20 km around the crosshair; z13–z16; the honest estimate before Start. It
needs WiFi and USB (or a battery above 3.8 V), keeps going when you leave the screen, pauses
for a call or a dropped WiFi, only fetches what is missing, and writes each tile under a
temporary name so a power-off mid-tile leaves nothing wrong on the card. Measured: a 5 km
area at z15 (271 tiles, 34 MB) in 260 s from USGS, zero failures; OpenTopoMap about four
seconds a tile, server-bound.

**HTTPS, for the first time on this phone.** An HTTPS pull of one USGS tile through the
existing `/fetch` path FAILED on 0.9.63 (`SSL - Memory allocation failed`, internal heap down
to 2.7 KB): the framework builds mbedTLS to allocate from INTERNAL RAM, its two 16 KB record
buffers exceed the largest free block, and the handshake runs on the calling task's 8 KB
stack. The downloader is the escape — its own task with a 10 KB internal stack, mbedTLS's
allocator redirected into PSRAM once (`mbedtls_platform_set_calloc_free`), one kept-alive
connection per area (handshake ~1.5 s once, then ~250 ms a tile) — measured with the new
`tlstest` bench: 70/70 GETs, PSRAM returned to the byte. The price is spelled out in
`tile_fetch.h`: 8 KB of internal RAM for the task's stack for the life of the firmware from the
first download (measured use 5.2 KB; the report prints the floor every run), and a ~12 KB
internal dip — lwIP's TCP buffers — at every handshake, the first and each time the server
closes keep-alive. A 10 km soak with the map open (863 tiles, 0 failed) took the internal heap
to 952 bytes before that was understood; now every handshake waits for room (free ≥ 16 KB,
largest ≥ 12 KB, "Paused" on screen) and the start refuses outright when the phone is already
low, saying to reboot.

**The phone decodes tiles now — on arrival, not on the draw path.** USGS tiles are baseline
colour JPEG and go through the ROM TJpgDec with private callbacks (`tile_decode.cpp`);
OpenTopoMap's palette PNGs go through the ROM `tinfl` inflater the e-reader already trusts,
wrapped in the 300-line PNG decoder `tile_png.cpp` (palette 1/2/4/8-bit, grey, RGB, RGBA;
41 host checks, every filter type, every refusal named). The card still holds raw `.565`;
the viewer is unchanged.

**The keys {user} asked for.** Side buttons 1/2 zoom, 3 centres on you — and the map owns the
side buttons while it is open (`gMapsActive`, the Game Boy's rule) so a loaded music track
cannot swallow them. OK drops a pin or opens the one under the crosshair; the top-left soft
key is the menu (tested BEFORE `LOGIC_BUTTON_OK`, which folds SELECT into OK on this
keyboard). `Menu → Go to coordinates` is a two-field entry: digits, `*` for the point, `#`
for minus.

**Share on a chosen channel.** *Share it on the mesh* opens the channel picker (the beacon's
rules: public rows say so and take two presses; the default is your position beacon's
channel). The channel's NAME is stored with the pin — `pins.txt` gains a field, older files
still load — so a rename re-send and *Take it off the mesh* go out where the pin already is.
The rows say where: "Update it on hunt-group", "Share on another channel...".

**Saved on power-off.** Both power-off paths pull the latch before any destructor runs; the
view is now written by `mapsSaveOpenView()` first, the way Books does it.

**Centre on me will not trust a bad fix.** A fresh fix under four satellites or over HDOP 10
— the bar the distance lines already use — ranks below your declared pin, is drawn grey, and
says "me? 3 sats".

**Fixed along the way.**
- 🔴 **Every raw-565 row this firmware ever pushed reached the glass byte-swapped.** A sprite
  stores pixels swapped and `pushImage` does not swap unless told, so Photos' BMPs and
  greyscale JPEGs, Books' greyscale pictures — and the map's tiles — showed red as blue and
  greys as near-black. Measured on the panel with a red/green/grey BMP, then fixed with one
  bracket (`lcdNativePixels`, GUI.h) at every site.
- The mesh's `NEW_MESSAGE_EVENT` forward discarded the app's redraw request for two releases;
  a marker that moved stayed put until a keypress.
- The mesh database save now waits (at most five minutes) for a running download, as a
  precaution against its ~55 small writes queuing behind 32 KB tile pieces. ⚠ The 0.5–0.9 s
  `'mesh'` stalls seen during downloads turned out to be the PRE-EXISTING ones the 2026-09-04
  handoff flagged (median 644 ms with nothing running); this did not remove them.

**For the bench.** `up on maps` + `tools/wiphone_send.py --app maps --tree <dir>` push a
converted tile tree over WiFi (the one uploader that accepts folder paths, validated by
`chunkSafeTreeName`); `open <app>` jumps into an app; `hold on|off` keeps the screen awake
(injected keys do not reset the 30 s sleep, which was eating scripted presses); `maps dl` and
`tlstest` drive and measure the downloader.

**The review's fixes (morning of 09-19, all on hardware).** The downloader is one persistent
worker task on a semaphore (a create-and-delete task raced its own TCB reuse, and this
FreeRTOS cannot tell a cleaned-up static task from a ready one — the second run was refused
forever, measured); `HTTPClient` is destructed by a normal return before the task idles (its
Strings leaked from internal RAM per run); an error body is drained or the socket dropped so a
404's tail cannot be read as the next tile; a body with no Content-Length fails the tile at
once instead of stalling 8 s; a body over 96 KB drops the socket; every reconnect waits for
internal-heap room (free ≥ 16 KB, largest ≥ 12 KB, 30 s) or fails the tile — never handshakes
low; a pause (call, no WiFi) longer than ten minutes ends the run; card write failures count
toward giving up and the free space is checked against the area before starting; a pin whose
channel has left the phone is not "retracted" on some other channel (it says so, keeps the
id); `tRNS` on grey and RGB PNGs is honoured (43 host checks); no decoder or TLS allocation
ever falls back to the internal heap; the task stack is 8 KB (measured use 5.2 KB).

**"What the buttons do..." is the first row of the map menu** — a drawn key: the four side
buttons down the left edge as they are on the case (zoom in, zoom out, centre on me), the
D-pad with OK as the pin, the soft keys under the screen, and the keypad shortcuts; More pages
into the menu's features and the colour key. {user} asked for a key that makes sense at a glance
(2026-09-19); a table of key names asked the reader to know which key was which.

**A download that loses WiFi lets go.** Measured on phone 1: the network dropped during a
run's first handshake and the job sat paused holding its TLS session and buffers while the
phone's own WiFi rescue — which needs internal RAM — ran against a largest block of 9 KB. Now a
pause drops the connection at once, a blip of under 20 s resumes (one reconnect), and a longer
loss ends the run with "WiFi dropped - stopped; start again to continue" (a restart skips
every tile already written). A call still pauses for up to ten minutes.

**Maps is on the main screen** with its own icon (a folded map, a route, a pin —
`tools/make_icon_maps.py`, drawn as maths like the Music icon), between Music and Tools.

**Phone 1 (the SIP phone) has run it**, and it changed the design twice. Its first download
worked — 21/21, SIP stayed registered — with a heap floor of 2,572 bytes during the handshake,
the thinnest margin in the firmware; the handshake bar was lowered to what phone 1 has
(largest ≥ 10 KB, free ≥ 14 KB) so it can download at all. Then a per-run task that freed its
8 KB stack afterwards was tried, to give phone 1 its RAM back between downloads — and it left
the heap FRAGMENTED (26.3 KB largest before, 11.3 KB twenty minutes after, free total
recovered): whatever WiFi and SIP allocated mid-run landed in the vacated region. The
downloader is one persistent worker again: a fixed, visible 8 KB from the first download
(phone 1, settled five minutes after its run: free 17.3 KB, largest 12.3 KB, from 25.8 /
20.5 KB before) rather than a number that depends on what else happened. **On phone 1, download at home on USB, and prefer the map's own screen closed
(`maps dl` over the cable, or Start and then Back).**

**Two more of COVEY's, the same morning.** The Pins / Places / Nodes lists are **nearest-first
with distance and bearing** on every row — "6.2km N Clifford, 0m ago" — measured from your
fix, else your declared pin, else the crosshair (the note under the list says which). And
**Menu → Follow me** is a latched mode: each fresh usable fix re-centres the map, any scroll
switches it off, and a lost or poor fix leaves the view where it is. **Menu → Measure from
here** drops an anchor at the crosshair; scrolling is the measurement — a dashed line joins the
two and the strip reads "Ruler: 1.1km SE of the anchor".

⚠ Only phone 2 has run this. Phone 1 (the SIP phone) has ~2–3 KB less internal headroom;
watch `heap` min-ever after its first download.

## 0.9.64 as first built (2026-09-14) - a map, because a list cannot tell you which side of the creek he is on

**Menu → Tools → Maps.** Tiles off the SD card, pins you drop yourself, and everything the mesh
already knows about where people are, on one screen.

Every fact this draws was already in the phone. Camp is a waypoint, the truck is a waypoint,
everyone's position arrives on port 3 every few minutes, and the Nodes list has turned that into
*"3.2km E of camp · 4 min ago"* since 0.9.7. That sentence is correct, and it is the wrong shape
for the question anyone actually asks in the woods, which is **is he on the other side of the
creek or this side**. So this adds no radio traffic, no second database and no new source of
truth — it is a view of state that already exists, plus one thing of its own: pins.

**The keys.** Arrows scroll (hold one and it accelerates; let go and the next press is a nudge
again, so a correction after a long sweep is still a correction). `*`/`1` zoom out, `#`/`3` zoom
in. `5` drops a pin on the crosshair and opens the name field. **OK picks up the pin under the
crosshair** — rename, move it here, share it, delete it — or opens the map menu when there is no
pin there. `7`/`9` walk the pins, `0` centres on you, Back leaves and remembers where you were.
`Menu → Keys and colours` is the same table on the phone.

**It opens where you left it.** Then, in order, only when the one before has nothing: this
phone's GPS fix, the mesh's reference place, the middle of whatever tiles the card holds, and
finally 0,0 with the screen saying so. The first of those is the one that matters, because it is
the only one that works with no sky and no signal.

**A pin is private until you say otherwise.** 64 of them live in a text file on the card and
nothing about them reaches the air. *Share it on the mesh* turns one into a Meshtastic waypoint —
out on the radio, onto COVEY's map and onto any device sharing the channel — and that is a
deliberate act every time. It prefers a **private** channel (a location on LongFast is readable by
every Meshtastic radio in range), it is **locked to this node** so nobody else can move your camp,
and **the phone says whether it actually left the radio**: "Saved, but NOT sent" means nobody else
has it. Renaming or moving a shared pin re-sends it, so the mesh never holds a stale copy, and
*Take it off the mesh* sends the deletion marker rather than leaving a camp where camp no longer is.

### The tiles are RAW, and that is the design rather than a shortcut

`/maps/<area>/<z>/<x>/<y>.565` — 256×256, RGB565, little-endian, **131072 bytes exactly**,
standard slippy `z/x/y` numbering. `tools/convert_tiles.py` makes them on the computer from a
tile tree or an `.mbtiles` file, and needs nothing installed: Pillow when you have it, macOS's
own `sips` when you do not.

🛑 **The phone decodes nothing.** There is no PNG decoder in this firmware — adding one costs a
vendored library plus contiguous internal RAM on a phone whose crashes are internal-heap
fragmentation — and the ESP32's ROM JPEG decoder **refuses greyscale outright**, which is the
entire reason `jpeg_grey.cpp` exists (33 of 45 pictures in one test book were 1-component JPEGs).
A map made of compressed tiles would work right up until the day it met a tile of the wrong
flavour, in the woods, with nothing on the screen to say what was wrong.

Raw costs 8× the card space — about **85 MB for a 20×20 km area at z12–z15**, nothing on a 32 GB
card — and buys zero decode time, zero decoder memory, and exactly **one** failure mode: the file
is not 131072 bytes. Which is also how a PNG copied across without being converted gets refused
instead of drawn as a screenful of confetti.

### A 128 KB read is a stall, so the card is read in pieces

⚠ **One tile is 128 KB and everything in this firmware shares one task.** A 128 KB SD read is
100–250 ms, and 250 ms is exactly the threshold 0.9.58's stall detector was built to complain
about — the freezes that drop WiFi and eat keypresses. Four tiles for one screen, read the obvious
way, is a second of frozen phone **every time you pan**.

So a tile arrives in 32 KB pieces, one per 25 ms tick, and the map draws the hole in the meantime
as a plainly-marked grey square. The file is opened, seeked, read and **closed** on every piece
rather than held open across ticks: that costs a directory lookup and removes the whole class of
bug where the app is torn down, or the card pulled, with a `File` still live. Once the screen is
painted the timer is disarmed entirely — a finished map must not keep the CPU awake.

### What it costs

**+72 bytes of internal RAM and 37 KB of flash** — measured, by building the tree with and
without it (87532 → 87604 bytes RAM, 2544759 → 2582199 flash). The working memory is **768 KB of
PSRAM** (six 128 KB tile slots) held only while the app is open, out of the ~3.6 MB free; the
~19 KB internal heap that SIP and WiFi fight over is untouched.

### Proven before the phone, because a wrong map looks exactly like a right one

A map that is one tile out is not visibly wrong on a 240×320 screen — it just quietly shows you
the wrong woods. That class of error cannot be caught by looking, so it is caught by arithmetic:
**`tests/test_maptiles.cpp`, 110 checks on the host**, over `map_tiles.cpp` (Web Mercator against
independently computed reference values; the blit rectangles checked *structurally* — every
viewport pixel covered exactly once, by a piece that came from the right pixel of the right tile,
including across the antimeridian seam and with a world shorter than the screen) and
`map_pins.cpp` (the pins file's round trip, and every malformed line a hand-edited file can hold).
`tests/test_pos.cpp` gained the Waypoint builder's bytes, checked against the field numbers and
then round-tripped back through the parser that reads COVEY's.

And because a screenshot cannot tell right ground from wrong ground either, the whole thing drives
over the cable: **`maps`** prints what the card holds, what the saved view is and how the pins
file parses; **`maps goto <lat> <lon> [z] [area]`** writes the same saved view the app writes, so a
known coordinate can go in over USB and the screen be compared against a map on the computer.

### Also

- `meshWaypointBuild()` in `mesh_pos.cpp` — the bytes that put a pin on somebody else's map,
  including the positionless **deletion marker** that is the only way to take one off again
  (expire cannot do it; `expire == 0` legitimately means "never").
- `MeshtasticService::shareWaypoint()` / `unshareWaypoint()`, and the private-channel choice the
  pin announce has always used is now in one function, `announceChannel()`, called by both. Same
  loop, same first-private-wins, same fall-through to `channels[0]` — two copies of *which channel
  does a location go out on* is how one of them quietly starts preferring LongFast.

## 0.9.60 (2026-09-07) - a radio you switched off is not a dry spell

**WiFi switched back on now tries immediately, instead of waking up already ten minutes behind.**
{user}, 2026-09-07: *"I had to manually join."* Phone 1 had been sitting with its radio off for
about 22 hours. Switching it on produced two scans that found nothing, a join the firmware
declined to attempt, and an owner who gave up after seven seconds and picked the network by hand.

The dry-spell clock was counting the time the radio was OFF. `autoSwitchTick()` maintains
`_drySpellStartMs` before its own gates -- deliberately, so the clock stays honest while the
auto-switcher is disabled -- but `connected` is false with the radio powered DOWN too, and
nothing cleared the stamp on the way back up. It was cleared in exactly one place: on a
successful connection. So a phone whose radio had been off overnight came back already far past
`WIFI_DRY_SPELL_LONG_MS` into a "long dry spell", and all four readers of `inLongDrySpell()` drew
the wrong conclusion in the same pass: the deaf-radio fast-path cure was DISABLED (it requires
`!inLongDrySpell()`), `worthAttemptingJoin()` stopped failing open and consulted scan evidence
instead, the scan cadence started at five minutes rather than two, and the join retry started at
ten.

The radio meanwhile was in the mid-connect deaf state 0.9.29 documented. **Both scans after the
switch completed `n=0` while SIX access points were on the air** -- a manual `wifi scan` on the
same radio minutes earlier listed all six, the home AP strongest at -59 dBm. An empty scan stamps
`_scanDoneMs` and clears `_savedSeenLastScan`, so a SENSOR FAILURE was written down as fresh,
confident evidence that the air was empty, and the join gate believed it: `[wifi] join skipped:
last scan saw no saved network (1 in a row, 1 total)`. With the curing bounce ten minutes out at
the eased cadence, the phone had nothing left to do but wait.

`Networks::forgetDrySpell()` clears the clock, the deaf-radio accounting and the stale air
evidence on every radio-ON path -- the Settings toggle through `setRadioOff(false)`, the network
edit screen, and `NetworksApp`'s destructor after the peek-and-back-out case. The dry spell
measures how long we have been LOOKING without success; time with the radio switched off is not
looking, and must not be counted as it.

**The easing is untouched.** The reset fires only on a deliberate radio-on, never on a drop, so a
phone that genuinely is somewhere without WiFi is back on the eased cadence five minutes later
exactly as before -- the 114-scans-in-280-s burn the tiers exist to prevent cannot return through
this door, and 0.9.50/0.9.51's scan-gated join (106 mV/h -> ~38 mV/h, measured on a real commute)
re-engages on the same schedule. What the phone no longer does is arrive pre-eased from a spell it
never served.

**One stale verdict went with it.** `_dryBounced` means "this spell's deaf hypothesis has already
been tested", and nothing cleared it across an off/on round trip -- so a phone that had bounced
before the radio was switched off came back hours later with that verdict still standing over a
radio that no longer existed, ready to certify the first empty scan of the new radio's life as
proof that the air was empty.

## 0.9.58 (2026-09-04) - the stall detector names the block that stalled; two silent dead ends closed

**The `LOOP STALL` line now says WHICH part of the pass took the time.** {user}, 2026-08-24:
*"sometimes when scrolling menus, the phone will freeze for a second or two, wifi will drop, then
it will unfreeze and WiFi comes back up."* 0.9.5x added the detector that catches it -- 62 stalls
recorded, and today's boot alone logged 890 ms, 578 ms and 314 ms -- but it could only ever say
how LONG a pass took, never what inside it blocked. Everything shares one task, so a stall is the
keypad, the screen and the WiFi stack frozen together, and "890 ms somewhere in 2,000 lines" is
not a lead.

Each of the superloop's 25 major blocks now names itself on the way in, and the detector reports
the slowest one of the pass that just ended: `LOOP STALL: 890 ms in one pass, worst block 'sip'
412 ms`. The same names go into `/health.log`, so a freeze that happens in the woods is
diagnosable afterwards rather than only while tethered. The instrument is a pointer store and one
`millis()` per block -- no allocation, no formatting, nothing that can itself stall, because an
instrument that can stall is worse than none. Costs 455 bytes of flash.

**A truncated ROM no longer runs uninitialised memory.** gnuboy's only size check was
`size < 0x200`, and it took the cartridge's bank count from the file's own header rather than
from how many bytes actually arrived. Its bank-mapping loop runs while `size - pos >= 16 KB`, so
a buffer smaller than ONE bank left the entire bank table NULL -- `gb_hw_updatemap()` then
allocated an UNINITIALISED PSRAM bank and the emulated CPU executed whatever was in that memory.
No panic, no message, just a game that does not work. Reachable by any upload that stopped early,
and by the 4,096-byte macOS `._` sidecars until 0.9.57 hid them. The arithmetic moved to
`gnuboy/gb_romsize.h` so the host suite can test the code that actually ships -- `gnuboy.c`
pulls in five ESP-IDF and emulator headers and cannot be compiled on the Mac, and a test that
re-implemented the rule would only prove the copy agrees with itself. New suite `test_gbrom`,
24 checks.

**The uploader refuses the sidecars by name.** `chunkSafeName()` rejected only `.` and `..`, so a
Mac client sending a folder could put `._Pokemon.gbc` on the card over WiFi -- which is both the
duplicate 0.9.57 has to hide and the truncated ROM the guard above has to refuse. Nothing this
protocol legitimately carries begins with a dot, so the whole leading-dot class is refused at the
door. This reverses one existing test vector on purpose: `.hidden` used to be asserted as "a
legitimate name".

## 0.9.57 (2026-09-03) - the Game Boy picker hides macOS sidecar files; serial `ls` and `rm`

Phone 2 showed every game twice in the Game Boy picker, the twin named `._Game.gbc`, and the
Files app could not find the twins to delete them. Read off the card with the new serial `ls`:
**12 ROMs in `/roms` and 12 `._` files beside them, 4,096 bytes each** — the AppleDouble
resource-fork sidecars Finder writes next to every file it copies onto a FAT card (the card also
carries `.Spotlight-V100/` and `.fseventsd/` at its root, for the same reason). Not corrupt
uploads, not duplicates of anything: metadata the Mac left behind.

**The picker was the one lister without the rule.** `FilesApp::scanDir()`, `BooksApp::scanBooks()`,
`PhotosApp`, `musicIsPlayable()` and the serial `wallpaper list` all skip a basename that starts
with `.` — the music library even carries a comment naming the macOS resource forks — and
`GbcApp::scanRoms()` listed anything with a `.gb`/`.gbc` extension. So the sidecar had the
extension, appeared in the picker, and was invisible everywhere a person could delete it. Same
class as the shared-state bugs of August: one consumer inherited a rule it never set. Fixed with
the same `base[0] != '.'` test the others use, with a comment that says why.

**Serial `ls [<dir>]` and `rm </path>`** are the instruments this needed. The picker draws straight
to the LCD, so `shot` cannot show it, and every on-phone lister hides dotfiles, so nothing could
show the card as it is. `ls` prints every entry with its size, marks hidden ones with `*` and
folders with a trailing `/`; `rm` deletes ONE file by full path and refuses folders and relative
names. The sidecars themselves are left in place — 48 KB of harmless metadata that no lister
shows any more; `rm` removes them if wanted, and Finder will write them again on the next copy.

**Serial `power` — the USB-power-meter bench.** Phone 1's full-to-empty run implies ~80-100 mA
of idle draw with WiFi off, and a datasheet sum of everything the firmware leaves powered reaches
only ~35-58 mA; the cell is soldered, so battery current cannot be metered, and a drain run
resolves 10 mA only after hours. The USB meter resolves 1 mA in seconds if the cell is FULL
(charger tapered) — so each `power` subcommand flips exactly one thing and one reading answers
one question: `power lcd sleep|wake` (ST7789 DISPOFF+SLPIN, the controller behind a dark
backlight), `power i2s stop|start` (the I2S peripheral and DMA that run from boot whether or not
anything ever played), `power lora sleep|rx` (the SX1276 parked in SLEEP — the mesh is DEAF until
`rx`; the radio health check accepts the parked state instead of re-initialising it), and
`power sleep <secs>` (light-sleep the ESP32 with a timer and keypad wake; WiFi must be off).
`power` alone prints every switch's state and the method. Bench only: none of it is a power
policy — the numbers it produces decide what becomes one. Smoke-tested on phone 1: all four
toggles round-trip, and a 5 s light sleep returned after 5,001 ms by timer with `millis()`
compensated.

⚠ Pulling `esp_light_sleep_start()` into the link overflowed IRAM by 16 bytes. `gb_hw_updatemap()`
(gnuboy/hw.c) lost its `IRAM_ATTR`: it runs per bank switch, not per bus access, and may call an
SD read anyway. The bus and scanline hot paths keep theirs. Watch for any Game Boy stutter on
bank-switch-heavy games; none is expected.

## 0.9.56 (2026-09-03) - the battery percentage comes from the voltage, off phone 1's measured curve

Phone 1 ran itself flat overnight on purpose — 9h16m from a genuinely full cell, WiFi off,
screen off, 80 MHz, the cleanest load this project has logged — and the run showed the one real
gauge error we have ever measured: **the CW2015 reported `soc=0%` at 3.54 V while the cell ran on
to 3.30 V, 1.3 hours of runtime reported as empty.** Its battery profile (BATINFO) is never
programmed, so it runs a generic model whose floor is ~240 mV too high for this cell, and which
reads ~7 points optimistic through the middle. Mean error 5.7 points against time-linear truth,
worst 16.4.

Off the charger, the display no longer uses it. `battery_curve.{h,cpp}` maps cell voltage to SOC
through an 18-breakpoint table derived from that run (10 mV bins of time-linear SOC, 50 mV means),
and the battery tick sets `battSoc` from it whenever the phone is not charging. Against the run it
was built from: mean error 0.54 points, worst 2. The chip's own number is still read and still
logged as `soc=` in the health line, with the estimate beside it as `est=`, so the next discharge
can score both exactly as this one was scored.

**On the charger the chip's number is shown instead**, and the first flash of this build is why:
45 minutes into a charge from empty at 3.89 V, the curve said **61 %** and the chip said **20 %**.
Charge current lifts the terminal voltage ~100 mV, and a discharge curve read against it
over-reports by 30-40 points — optimistic, the wrong way for a battery display to be wrong. The
chip is the only charge-aware estimate we have. ⚠ On phone 2 `battCharged` reads 0 even on USB
(the backplate holds the rail), so phone 2 shows the curve on the cable — where its internal cell
is genuinely full and the curve's 100 % is right anyway. Phone 1's `health.log` is now recording a
complete charge (chip vs curve vs voltage, once a minute); a charge-side correction can come from
it later.

⚠ **This is phone 1's curve, and {user}'s decision is that it is the firmware's standard** —
phone 2 and other people's phones use it as the best guess available. It is load-dependent
(voltage sags under load, so a lit screen or a hunting radio makes it UNDER-report — the
conservative direction), and it is fitted to one run and tested against that run, which proves it
reproduces the curve rather than that the curve holds on another day.

⚠ **The old `if (soc > 0.0)` guard on the chip's value made the display FREEZE at its last
non-zero reading once the chip hit 0 %** — so the phone showed "1 %" for its last 1.3 hours. Gone.

🛑 **Correction to the 0.9.54/55 notes: phone 2's 4.6 h at `soc=100%` was its woods BACKPLATE
feeding the internal cell, not a gauge dead zone.** Phone 2 wears the backplate unless {user} says
otherwise; its cell holds the internal one at ~4.2 V for hours, so `soc=100%` after unplugging is
real, `chg=0` on the USB cable is normal, and "12.5 h" is two cells in sequence. **Battery
experiments happen on phone 1.** Recorded in the handoff and in memory so it stops being
re-learned.

`tests/test_battery.cpp` (20 checks) loads the actual recorded run from
`tests/fixtures/p1_discharge_2026-09-03.tsv`, scores the table AND the chip against it, and pins
the specific fault: 3.54 V must still read ≥ 12 %. Mutation-tested — a breakpoint zeroed to the
chip's cliff, two breakpoints transposed, interpolation removed, the top clamp moved a band — all
caught.

## 0.9.55 (2026-09-02) - Position, Waypoint and the channel hash are pinned too; every varint reader hardened

The rest of the mesh surface gets the treatment 0.9.54 gave the Data and User protobufs.
{user} was hands-off for this one, so the work went in as limit-safe checkpoints (`8b93e72`,
then the pinning commit, then this release).

**Position and Waypoint are now pinned against upstream's own encoder.** `mesh_pos.cpp`
hand-rolls both, and `test_pos.cpp` had only ever checked them against bytes written by hand
in the same file. `tools/gen_pos_vectors.py` → `tests/vectors_pos.h` now carries what the real
Meshtastic runtime emits: Position encode byte for byte; Position decode including a
**stock-shaped broadcast with every field a RAK actually sends** (two-byte tags for
`sats_in_view`/`precision_bits` included) — which is the packet COVEY puts on the air every five
minutes; Waypoint decode for the full form, the id-only deletion marker, a truncated long name,
UTF-8, unknown fields 7/8, and no-id refused. This is the interop surface the hunting trip
depends on, and it was the one still resting on the author's own reading of the spec.

**The channel hash is pinned.** It lived in `mesh_crypto.cpp` beside AES, which needs mbedtls
and will not build on a Mac, so it could never join the suite — 0.9.54 listed it as a known
gap. `meshXorHash`/`meshChannelHash`/the default key move verbatim into **`mesh_hash.{h,cpp}`**
(`mesh_crypto.h` re-includes it, no caller changes), and `test_wire.cpp` asserts seven
(name, key) pairs against `meshtastic.util.generate_channel_hash`, plus
`meshDefaultChannelHash()` == the LongFast vector == `0x08`, the byte the boot line has always
printed. Of the two gaps documented in 0.9.54, only the 16-byte on-air header remains, and that
one is unpinnable from python by nature.

**UBSan found a real defect the moment the new vectors ran.** The stock-shaped Position carries
`timestamp_millis_adjust = -250`, and protobuf encodes a negative `int32` sign-extended to 64
bits — a **ten-byte varint**. `rdVarint` in `mesh_pos.cpp` shifted a `uint32_t` by 35 and then
63: undefined behaviour, which Xtensa masks to a 5-bit count. Nothing was ever misread (the walk
follows the continuation bit, and every field that file reads is `fixed32`), but `altitude` is
`int32` too and a fix below sea level is a real thing. `mesh_wire.cpp` had the same pattern in
three readers. All four now discard bits past 32; a below-sea-level Position vector and a
hand-crafted ten-byte-varint Data case pin it, and removing the guard makes UBSan complain again.

**The CallApp in-call volume handler can no longer wipe `configs.ini`.** Recorded as a known
hazard by the 0.9.52 review: it called `load()` (never `restore()`, unlike every other writer of
that file) and then `store()` unconditionally, so a failed load became a three-key file with
every other setting gone. It now tries `restore()` like its siblings and, if the file still will
not load, applies the volume to the driver and persists nothing, loudly. ⚠ Needs a completed SIP
call to reach, so it has never fired and **cannot be verified on hardware**; it is strictly
safer than what it replaces.

Every new assertion was mutation-tested — swapped tags, wrong fields, the old single-byte tag
read, XOR→OR in the hash — all caught. Host suite green under ASan+UBSan with zero runtime
errors. `test_wire` 128 → 139 assertions.

## 0.9.54 (2026-09-02) - the Meshtastic wire format is pinned against upstream's own protobufs

{user} asked whether this phone can keep up if Meshtastic changes their core files. It can — this
is a reimplementation rather than a fork, with the Data and User protobufs hand-rolled as literal
tag bytes, and every parser here skips fields it does not recognise, so upstream adding fields
(the overwhelming majority of what they do) steps straight past us. What was missing was any way
to notice the changes that *would* hurt: an existing field renumbered or re-meant.

`meshBuildData`, `meshBuildUser`, `meshFillHeader`, `meshParseData`, `meshParseUserName` and the
protobuf primitives move out of `meshtastic_service.cpp` into **`mesh_wire.cpp`**, byte for byte.
They were file-statics in a translation unit that drags in the ESP32 SDK, so nothing could ever
test them. `mesh_wire.cpp` is Arduino-free, and **`tests/test_wire.cpp` (84 assertions)** now
asserts its bytes against vectors generated by the real Meshtastic python runtime
(`tools/gen_wire_vectors.py`, meshtastic 2.7.11): Data encode and decode, User/NODEINFO encode
and decode, the `"!%08x"` id spelling, all six PortNums against `portnums.proto`, and — on
purpose — messages carrying fields this firmware does not know, so forward compatibility is a
test rather than a comment. `s_hopLimit` stops being shared mutable state read from inside
`meshFillHeader` and becomes a parameter.

The new test found a real divergence on the day it was written: **`meshBuildData()` always
emitted Data field 2, even for an empty payload** — `12 00` where upstream emits nothing, because
proto3 omits a field holding its default. It never misread a packet (an absent `bytes` field
decodes as empty) and it is unreachable in shipping code, since `meshTxText()` rejects empty
text. Fixed anyway: two bytes of LoRa airtime, and a permanent diff against every other node's
encoding.

The new suite was then pointed at itself by an adversarial review, which found four real
defects in code this change had just introduced or moved, all now fixed and all now covered:

* **`portnum` was written as one raw byte, not a varint.** Every portnum this firmware speaks
  is under 128, so nothing on air was ever wrong — but Meshtastic's enum runs to 511, and
  portnum 256 (`PRIVATE_APP`) encoded as `08 00`, a Data addressed to portnum **zero**. Silent,
  and it would have been inherited by whoever added the next portnum.
* **`meshBuildUser` emitted empty `long_name`/`short_name` as `12 00` / `1a 00`** where proto3
  omits them — the same defect as the empty payload, one field over.
* **`meshParseUserName` selected a present-but-empty `long_name` over a perfectly good
  `short_name`**, so a peer sending that form (which this firmware itself did until now)
  rendered as a bare node number with its name sitting unread two bytes away.
* **The generator emitted greedy C hex escapes.** `esc()` wrote bare `\xNN`, and a C lexer
  swallows a following hex digit — `"Caf\xc3\xa9a"` lexes `\xa9a` as one escape. The committed
  cases happened to be safe, which is precisely how it would have shipped broken later.

Coverage gaps the review found were closed too: decode vectors now include a multi-byte length
varint, an absent field 2, and unknown fields *before* a field the test asserts (the original
unknown-field vector put them all after, so a broken skip could not have changed the result).
Hand-written cases cover the two shapes upstream's runtime cannot emit — a `fixed64` unknown
field and a present-but-empty `long_name`. **Every fix was mutation-tested**: reverting each one
fails the suite, and the first version of the short-public-key test was found vacuous that way
and rewritten. 84 assertions → **128**.

⚠ Two surfaces are deliberately **not** pinned, and the handoff says so at length. The 16-byte
on-air PacketHeader is firmware-internal to Meshtastic and absent from the protobufs, so it gets
a structural test (size, field offsets, flag packing) that catches our drift but not theirs. The
channel hash *is* exposed upstream and matches ours exactly, but lives in `mesh_crypto.cpp`,
which needs mbedtls and will not build on a host; splitting the hash out would close it.

Rehearsed across a real version gap: the generator was run on COVEY (meshtastic 2.7.10) and its
output diffed against the committed vectors from 2.7.11 — byte-identical everywhere.

## 0.9.53 (2026-09-02) - the Buzz value is clamped on the way in

Follow-up to 0.9.52 from its adversarial review, which found **no defect** in the slider
itself and one hardening worth doing. `notify_vibro_ms` was stored at boot and on screen-open
straight from the ini with no range check, so a hand-edited `-1` became **65,535 ms — a
65-second motor run per notification** — and 0 became a sub-millisecond twitch that reads as a
dead motor. Worse, `audioDeviceBusy()`'s vibro term held the audio device awake for the whole
65 s. Both load sites now `constrain(v, 50, 650)`, the slider's own range. Consistent with how
the neighbouring `notify_*` fields load, so not a regression, but a bad file no longer becomes
a bad phone. Two stale comments still describing the pulse as a fixed 180 ms are reworded.

⚠ The review also surfaced a **pre-existing, unrelated** hazard, recorded in the handoff and
NOT fixed here: the CallApp in-call volume handler can rewrite `configs.ini` with only its
three volume keys if its own `load()` fails, dropping every other setting. It needs a completed
SIP call to reach.

## 0.9.52 (2026-09-02) - a Buzz slider, because a pocket is not a desk

{user}: *"can you add a 'vibration duration' slider to the notifications settings menu? I'd like
to customize what feels best when the phone is in a pocket."*

### ✅ Settings > Notifications > Buzz

50-650 ms in twelve 50 ms steps, default **200 ms** (the nearest on-grid value to the 180 that
had been hard-coded as `MESH_VIBRO_MS`). Stored as `ControlState::notifyVibroMs`, persisted as
`[audio] notify_vibro_ms`, loaded at boot in `GUI::loadSettings()` alongside the other
notification settings. The two places that time the pulse - the buzz in `notifyMessageArrived()`
and the release in `loop()` - read it through the same name they always did:

    #define MESH_VIBRO_MS (gui.state.notifyVibroMs)

⚠ **This is the NOTIFICATION buzz only.** The call ring pattern (`vibroOnPeriodMs` /
`vibroOffPeriodMs`, loaded from the ringtone config) is a separate mechanism and is untouched.
A burst of arrivals still re-arms the timer rather than being skipped, so several texts in a
row become one longer buzz at the chosen length.

### 🔧 The screen was full, so both sliders went inline

Header 30 + three 64 px label/choice rows + one 50 px stacked label-and-slider = 277 of the
280 px above the footer. A fifth stacked row did not fit. Both sliders now take the inline
form Screen config uses - label left, slider right, 25 px each, `labelWidth` 110 so the two
settings screens line up - which lands the whole screen back at 277. `FocusableApp(4)` → `(5)`.

**Verified on phone 2:** screen renders with both inline rows clear of the footer; four
`right` presses stepped 200 → 400 ms; **Save, reboot, reopen → 400 ms.**

## 0.9.51 (2026-09-02) - the join gate now asks the last scan, not the last 20 minutes

**0.9.50's scan-gated join was tested on a real 50-minute commute and both halves worked.** The
same log showed the gate engaging 25 minutes into the drive instead of 5, so half the trip still
burned blind joins.

### ✅ What the commute proved

```
up745   wifi=1   joins=12/0    lost WiFi setting off
up760   wifi=1   joins=18/0    last blind attempt
up770   wifi=1   joins=18/1    FIRST SKIP - gate engaged
up778   wifi=1   joins=18/2
up788   chg=1    joins=18/3    plugged in at work
up789   wifi=3   joins=19/3    REJOINED
```

`tried` frozen at 18 for **28 minutes** with zero join attempts, then one attempt the moment a
scan saw a saved network, and it connected. Drain over the gated stretch, least-squares on the
voltage trace: **~38 mV/h against 106 mV/h measured hunting on 0.9.46/47.**
⚠ **That magnitude is soft** — a 27-minute window, 10 mV gauge quantisation (±20 mV/h over that
span) and R²=0.54. The direction is strong; the number is not pinned.

### 🐛 The 20-minute lookback was buying nothing and costing twenty minutes

`worthAttemptingJoin()` compared `_savedSeenMs` against `WIFI_SCAN_EVIDENCE_MS` (20 min), so a
network seen just before setting off counted as "recent evidence" for twenty minutes after it was
gone. Scans run every **5** minutes in a dry spell, so the lookback could only ever delay the gate.

Replaced with `_savedSeenLastScan` — **did the MOST RECENT completed scan see a saved network?**
A boolean about the latest evidence, which is the question actually being asked. Cleared *before*
`autoSwitchEvaluate()` and set inside it, so the flag always describes the scan that just
finished rather than leaving a previous answer standing through an early return.

Expected effect on the same commute: the gate engages ~5-6 min after losing WiFi rather than ~25,
roughly doubling the gated portion of a 50-minute drive.

⚠ **The staleness check stays** (`_scanDoneMs` vs `WIFI_SCAN_EVIDENCE_MS`): no scan in a long
time still fails open. Only the *positive* evidence test changed.
⏸ **The safety valve is still untested** — it fires on the 4th consecutive skip and the drive only
reached 3. A longer trip would exercise it.

## 0.9.49 (2026-09-01) - one WiFi switch that actually means it

{user}: *"in settings let's just make a global Wi-Fi off toggle... it can shut everything Wi-Fi
related off with one button."* Plus, on the retry cadence: *"5 mins of fast reconnect then after
that period, slower retries every 10 minutes?"* — which is what 0.9.48 shipped, so the threshold
moves from 10 minutes to his 5.

### 🛑 "WiFi: off" was not off, and did not stay off

The toggle really did stop the PHY. Three other paths turned it back on without clearing it, and
boot never applied a stored OFF at all:

- **The WiFi list screen's 5 s rescan** (GUI.cpp) — `scanNetworks()` opens with
  `enableSTA(true)` → `esp_wifi_start()`, so standing on that screen put the radio back on air.
- **The ROM/file uploader's stop path** (app_gbc_xfer.cpp) — unconditional
  `WiFi.mode(WIFI_STA)` + `begin()`.
- **The Game Boy's exit** (app_gbc.cpp) — unconditional `mode(WIFI_STA)` + `reconnect()`.
- **Boot** — `Networks::init()` does `WiFi.mode(WIFI_STA)` and `setup()` only called `disable()`
  when there was NO saved SSID. Any naive "just persist the toggle" would have inherited this.
- **And the label could lie**: `wifiOn` was a bare global that the toggle set and those three
  paths did not.

### ✅ One switch, persisted, and honest

`Networks::setRadioOff()` flips it, persists it (Preferences, `wpwifi/radiooff`) and applies it in
one call. `Networks::radioOff()` is the single predicate all four paths above now consult.
`wifiOn` is **deleted** as a stored global and `#define`d to `!wifiState.radioOff()`, so the label
cannot drift from the radio.

⚠ **The original "NOT PERSISTED" comment gave a real reason** — *"a radio that stays off across a
power cycle is a setting you can forget you set, and the failure mode is a phone that silently
never connects again."* **That objection is answered, not ignored**: the menu row now carries a
subtitle (*"saves power - survives restarts"* / *"scanning and joining as usual"*), and boot
prints `WIFI: radio is OFF (your setting, kept across restarts)` at log_e.

**PROVED ON HARDWARE, both directions:** toggled off → row relabels, status bar clears; reboot →
`wifi=255` (station not started); toggled on → reboot → `wifi=3` associated.

### ⏱ The dry-spell threshold is now 5 minutes

{user}'s number, and he argued it down from my 10 himself. He is right: the screen-wake escape hatch
forces an immediate scan **and** an immediate retry, so a long back-off costs far less
responsiveness than it looks like it should. Out of range: join retry 180 → 600 s, scan
300 → 900 s, radio-on time ~604 s/h → ~181 s/h (16.8 % → 5.0 % duty).

### 💡 Not done, deliberately — the next idea

The retry still calls `connectToPreferred()` blind, and a failed join holds the radio associating
for up to 30 s. **Scanning first (~350 ms) and only attempting a join when a known SSID is
actually present** would remove almost all of that. It is the biggest remaining win, and it is NOT
in this release because it cannot be tested from a desk — it needs a drive away and back, and
shipping it untested before a three-day gap risks a phone that will not rejoin. For next session.

## 0.9.48 (2026-09-01) - a phone with no WiFi to find stops hunting for it

{user}, after driving 50 minutes home with no WiFi and watching the battery: *"can we make it so
if it doesn't see a Wi-Fi signal in 10 minutes, the retry interval is a lot less?"*

Yes, and the measurement says he was pointed at the right thing. Field data from three runs on
phone 1 puts **WiFi searching at ~106 mV/h against ~60 mV/h associated** — about 75 % more —
and that held both across runs and within a single one.

### 🔑 The scans were never the expensive part

A scan lights the radio for a few hundred milliseconds. The cost is the **join retry**:
`WIFI_RETRY_PERIOD_MS` is 20 s, easing only to 180 s after five failures, and **every failed
attempt leaves the radio associating for up to 30 s** before the quiesce in `loop()` disconnects
it. Out of range that is ~20 attempts an hour x 30 s — an **~17 % duty cycle at full radio
power**, with the scans contributing about 4 seconds of it.

### ✅ One predicate, both cadences

`Networks::inLongDrySpell()` — ten minutes with no association — now eases both:

| | before | after |
|---|---|---|
| join retry | 180 s | **600 s** |
| auto-switch scan | 300 s | **900 s** |
| radio-on time per hour | ~604 s (16.8 %) | **~181 s (5.0 %)** |

**A 3.3x reduction** in radio-on time for a phone that is simply somewhere without WiFi.

⚠ **Both cadences read the SAME predicate on purpose.** This file already learned that lesson —
see the note on `scheduleScanRetry()`, where the due-check and the retry scheduler disagreeing
produced 114 scans in 280 seconds.
⚠ **The deaf-radio fast path is now guarded by it too.** That branch pulls the cadence back to
30 s to confirm-and-cure a genuinely deaf radio, which is right for deafness — but a phone that
has been away from any network for ten minutes is not deaf, and letting it fire would have undone
the easing completely.
✅ **Both escape hatches survive**: connecting clears the spell instantly, and a screen wake still
forces an immediate scan *and* an immediate retry, so picking the phone up is prompt however long
the spell has run.

### 📉 Why this, and not the audio leak

The run that prompted it had **`aud=0/0` in all 225 samples** — no audio leak — and **`ps=1`
throughout, including 83 samples spent searching and 8 with the radio down**. So neither the codec
nor lost modem sleep was involved. It was the disconnected radio, and only that.

⚠ The saving is **estimated from duty cycle, not yet measured on the battery.** `ps=` and the
health log will show it on the next run away from a network.

## 0.9.47 (2026-09-01) - two shared devices that nothing ever switched off

✅ **PROVEN ON HARDWARE THE SAME MORNING.** {user} played a track for six seconds and paused it:
`aud=1/1` (playing) → `aud=1/0` (paused: powered, nothing moving — **the leak**) → held while the
screen was lit → screen slept → **`AUDIO: device was left powered with nothing using it -
released after 30 s`** → `aud=0/0`. Both phones flashed and confirmed on 0.9.47.

{user}: *"yesterday I unplugged wiphone 1 at like 6am-ish and left it mostly idle. at 10am it was
at 12 percent."* That is ~22 %/h against a measured baseline of ~10. Four battery ideas were put
up for assessment; **three of them turned out not to matter, and the two things that did were
found by reading the shared devices rather than the feature list.**

### 🛑 The audio codec, the external amplifier and I2S are left powered, indefinitely

`Audio::shutdown()` is the ONLY function that calls `codec.shutDown()` (Audio.cpp:274),
`i2s_stop()` (:311) and `amplifierEnable(0)` (:308). Every one of its call sites is a SIP call
teardown or a debug easter egg. **There is no idle path**, and on a phone whose proxy never
answers the call teardowns never run at all.

So stopping a track leaves the WM8750 powered — DAC, output driver, VREF/VMID, the left ADC and
the **microphone bias** (WM8750.h `powerUp()` sets POWER1 = VMIDSEL|VREF|AINL|ADCL|MICB
regardless of the mask) — plus I2S running and, conditionally, the separate loudspeaker
amplifier IC. `music_player.cpp` only ever calls `Audio::stopMusic()` = `playbackFile.close()` +
`ceasePlayback()`, and `ceasePlayback()` (Audio.cpp:673-682) touches none of it.

⚠ **PAUSING IS ENOUGH, AND IT IS WHAT {user} DOES.** `MusicApp::~MusicApp()` deliberately does not
stop playback, so "start an album, pause it, pocket the phone" is the ordinary shape.
⚠ **The amplifier is worse than the codec because it is CONDITIONAL.**
`restoreCallVolume()` calls `chooseSpeaker(s_savedLoudspeaker)` — whatever the routing was when
music STARTED (music_player.cpp:43) — and `chooseSpeaker()` no-ops when the value does not change
(Audio.cpp:223). The call screen sets loudspeaker unconditionally (GUI.cpp:5983). **So whether
pausing music switches your amplifier off depends on whether you had opened the dialer since
boot.**

🔑 **AND IT IS INVISIBLE.** After `stopMusic()`, `musicPlayerIsPlaying()` reads false, so the CPU
governor drops to 80 MHz and every instrument the project owns reports a perfectly idle phone.

### 🛑 WiFi modem sleep — one setter, six bypass paths (⚠ but see the correction below)

⚠ **DISPROVEN SAME DAY FOR THE PATH THAT MATTERED.** `wifi bounce` on 0.9.47 held `ps=1` right
through a full radio cycle and never triggered the repair, so `esp_wifi_start()` appears to
PRESERVE power-save on this SDK — contrary to the comment at Networks.cpp:171-172. The code facts
below stand; the consequence drawn from them does not. The invariant and the `ps=` field stay
(they cost nothing and will catch it if it ever does happen), but **the audio leak is the only
PROVEN drain of the two.** The Settings toggle OFF→ON, the Game Boy exit and the uploader stop do
a fuller stop/start and remain untested.


`esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` appears **exactly once in the tree** — Networks.cpp:174,
inside `connectToWiFi()`, which has exactly one caller. The comment there says why it sits after
`begin()`: *"the driver resets the power-save mode when the station starts, so setting it earlier
is silently undone."* Every other path that starts the station never goes near it, and the
comment above it prices that at *"tens of milliamps, forever"*:

- `Networks::bounceRadio()` (Networks.cpp:504) — ⚠ **the AUTOMATIC deaf-radio cure**, so an
  ordinary WiFi blip in a pocket silently doubles idle radio draw for the rest of the boot;
- the Settings WiFi toggle turning back **ON** (GUI.cpp:1839) and the edit screen (GUI.cpp:6632)
  — ⚠ the one control a user reaches for to SAVE power;
- the Game Boy on exit (app_gbc.cpp:328), the ROM uploader on stop (app_gbc_xfer.cpp:1766),
  and the blocking scan path (Networks.cpp:821).

### ✅ The fix, for the class rather than the callers

Both are the same bug: **a shared device whose mode one component sets and another silently
resets.** Fixing each caller fixes that caller; the next one breaks it again, as four previous
faults in this codebase already did. So:

- **An audio idle watchdog** in `loop()`: when nothing is entitled to the device for 30 s
  continuous, `shutdown()` — which clears codec, I2S and amplifier together. Safe because
  `shutdown()` uses `i2s_stop()` and never uninstalls the driver (see `Audio::configureI2S`), so
  release-and-reacquire costs no DMA reallocation.
- **A WiFi power-save invariant**, re-asserted every 5 s regardless of which path restarted the
  station.
- 🎁 The audio watchdog also closes the **hot-mic leak**: `shutdown()` is what clears
  `microphoneOn` / `microphoneStreamOut` / `rtpRemotePort`.
- ⚠ **Both log at `log_e` every time they fire.** A watchdog that cleans up silently is a
  bug-hider; these are instruments that name the leak each time, so the underlying caller can
  still be found.

### 🔎 And the instruments, because none of this was visible

- `aud=%d/%d` in the health line — powered / moving-samples. **`aud=1/0` is the leak signature**,
  and it is the state that reads as a perfectly idle phone in every other field on that line.
- `ps=%d` — 1 = modem sleep, 0 = receiver at full power, -1 = station not up.
- A serial `audio` command: who is entitled to the device, and for how long it has been idle.
  ⚠ Both report the firmware's SHADOW of the chip: the WM8750 is write-only (no `readReg`), so
  the registers cannot be read back. That is why the watchdog runs on a timer rather than
  trusting bookkeeping.

### 🐛 Two bugs the adversarial review found in the watchdog itself, before it ever ran

- **It could never have fired.** The first version used `sipCallActive()`, which counts
  `HangUp`/`HangingUp` — states measured stuck in **1,066 of 1,071** health samples on phone 2.
  Now `sipNeedsFullSpeed()`; a teardown with RTP genuinely flowing is held by `movingSamples()`.
- **It would have crashed the voice recorder.** `recordFromMic()` sets `microphoneRecord` and
  never touches `playback`, so `movingSamples()` read false during a live recording.
  `movingSamples()` now counts recording, and the predicate gained `screenBrightness > 0` as a
  catch-all for UI consumers with no non-sticky flag (the mic meters go through `turnMicOn()`,
  which sets only `microphoneOn` — cleared nowhere but `shutdown()`).

### 🐛 Pre-existing: `ceaseRecording()` freed the buffer and kept the write index

`freeNull(&recordRaw)` left `recordRawW` set, and `saveWavRecord()` does
`if (recordRawW > 0) write(recordRaw, recordRawW * 2)` — a read from address 0 on the next Save
after ANY `shutdown()` during a recording. Reachable from every teardown path; the watchdog would
merely have been the first thing to trigger it reliably. The buffer and its indices now die
together.

### 📋 Measured, and NOT worth doing — the three ideas that were assessed and rejected

- **A "do not relay other nodes" toggle saves nothing.** Measured `MARK mesh-relay`: 25 relays in
  20 h on phone 1, 9 in 18 h on phone 2 = 0.5-1.25/h. At ~0.5 s on air and ~90 mA at PA_BOOST
  +17 dBm that is ~0.016 mAh/h — about 0.01 % of the budget. Worth adding as mesh etiquette,
  never as a battery feature.
- **No Bluetooth is running.** Nothing in the tree calls BluetoothSerial/BLEDevice/NimBLE, and
  arduino-esp32 does not start the controller unless asked. (There IS RAM to reclaim:
  `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` is called only when the Game Boy starts.)
- **Turning the SIP account off saves nothing.** `sipNeedsFullSpeed()` excludes every non-call
  state and phone 1 idles at 80 MHz with `sip=1`; with no WiFi there is nothing to poll and
  nothing to re-REGISTER. The woods saving is the WiFi radio, not SIP.

## 0.9.46 (2026-08-29) - book sync stops being a stopwatch

{user}: *"If I sync my place from a device, the wiphone doesn't automatically have the new place.
I instead have to open the book on the receiving wiphone, then on the other device I have to
push sync at that point, then on the receiving wiphone I have to close the book, then reopen
it... I prefer how it works on covey where the 'jump to (device name)'s place?'"*

That sequence was not superstition. It was the **only** sequence that worked, and each of its
three steps was working around a different fault.

### 🛑 Opening the Books app threw away every parked position

`BooksApp`'s constructor called `bookSyncInboxInit()`. The app is `new`ed on entry to Books and
`delete`d on exit (GUI.cpp, `GUI_APP_BOOKS`), so **the parking spot was emptied at the exact
moment the reader was about to look in it.**

The parking spot exists precisely because a LoRa round trip does not fit inside the moment you
press Sync — a position is supposed to be able to arrive while the reader is shut and be waiting
when you open the book. Wiping it on entry inverted that: a position only survived if it landed
with Books *already open*, which is why the book had to be opened **first** and the send had to
come **after**. The inbox is a file-static array that the C runtime zeroes before `setup()`
runs; it never needed initialising, and it must outlive the app that reads it. The call is gone
and there is a 🛑 comment where it was, because it reads like tidy housekeeping.

### 🐛 A position arriving while you were reading was invisible until you closed the book

`checkForPending()` ran on book open and after a passcode edit, and nowhere else. Sitting on a
page while a position landed, nothing happened — hence the close-and-reopen. The reading screen
now watches the inbox on a one-second tick and raises the offer where you are.

The tick costs a **`uint32_t` comparison**: `bookSyncInboxSeq()` moves only when the inbox
actually changes, and the HMAC verify behind `bookSyncInboxFindFor()` runs only then. A mesh
**rebroadcast of a frame already parked deliberately does not move it** — flood routing means we
hear the same frame more than once, and re-raising the card for it would be a nag rather than a
sync. Pressing Sync again on the sender does move it, because that packet carries its own
`turnedAt` and nonce.

Removing an entry moves it too, so acting on one offer makes the reader look again: two devices
can each park a position for the same book, and the second must not stay invisible until the
next open.

### 🐛 Even when it WAS found, nothing said so

The only surfacing of a found position was a row inside `Menu` — "Go to X's place (47%)". A
position could arrive, park, verify, match the open book, and still be invisible to anyone who
did not think to press Menu. Opening a book with a position waiting now goes **straight to the
sync card**, which is COVEY's behaviour and what {user} asked for. Declining lands on the page;
the menu row stays for the passcode-edit path.

### ⚙ The tick, and the shared-state trap it walks past

`msAppTimerEventPeriod` is device-wide and apps routinely leave their own value in it. Rather
than add a thirteenth scattered assignment, `BooksApp::enterState()` now **defaults it to 0 at
the top** and lets the two screens that need a tick override it — so a state added later gets
"no timer" instead of whatever the previous screen wanted. Same bug class as the menu ID, the
I2S channel format and the notification pop's device config.

### What this means in use

Timing no longer matters in any direction:

| when the position arrives | before | now |
|---|---|---|
| Books app shut | discarded on entry | offered when you open the book |
| in the library, book shut | menu row only | offered when you open the book |
| book open, you are reading | invisible until close+reopen | offered within a second |

⚠ Still RAM-only: a reboot discards anything parked. And **you still do not need to tap Sync on
both devices** — *Sync my place* broadcasts, the receiver parks it with no tap.

### 🧪 Tests

`test_inbox` gains 17 assertions covering the change counter in both directions — the quiet
failure here is silent either way (the card never appears, or it re-appears on every
rebroadcast). Suite: **1,161 assertions, 0 failures** across eight suites.

## 0.9.45 (2026-08-29) - the instrument for the residual drift, and six audit fixes

### 📐 The measurement nobody has taken

0.9.43/0.9.44 removed the app-switching ratchet (47 app opens, net `largest` +0) but phone 1
still lost **2,292 bytes over 174 minutes in steps that fall BETWEEN app opens**. The app-open
probe cannot see that by construction and the DROP instrument says a step happened without
saying what caused it. So `heapEvent()` / `heapDelta()` now mark `largest` at the remaining
suspects, into `/health.log`:

- `MARK scan-pre` / `MARK scan-post` — brackets every auto-switch WiFi scan
- `MARK assoc` / `MARK disassoc` — every association change
- `MARK mesh-relay` — every flood rebroadcast, **logged only when `largest` moved ≥256 bytes**
  so a quiet TX costs nothing

⚠ **Loop task only.** `healthLogLine()` opens a file on the SD card, so association is detected
by polling `WiFi.status()` from the loop at 2 s rather than by hooking `processWiFiEvent()` —
that handler runs on the WiFi event task with a 4 KB stack and would race the loop's own SD
access. This is the same discipline that made the scan guard a *call-site* decision.

If the residual is stepwise and lands on these marks, we have it. If `largest` slides smoothly
between them, it is none of these and the next pass must instrument allocation *sites*.

### 🐛 Six from the audit — the ones that were cheap and safe

- 🛑 **Diagnostics ▸ Networks rebooted deterministically with WiFi down.** `nextToPing++` is
  unconditional but `pingedAll` was only set inside the `wifiRssi != 0` branch, so the sequence
  never terminated and `bbPings[i]->setText()` ran past the end of a two-element array. At
  `i==2` that calls setText() on a NULL `this`. One press of DOWN, ~3 s, every time. Index
  bounded; `buff` initialised (it was handed uninitialised to `extStrdup()` on that path).
- 🛑 **`ping_start()` could hang the phone until the battery came out.** The loop was bounded on
  `ping_seq_num`, which is only incremented *after* `mem_malloc()` succeeds — so one failed
  340-byte allocation made it an infinite `delay(1000)` loop with the watchdog set
  print-not-panic. **Contiguous-heap exhaustion arriving as a HANG rather than an abort: no
  reset_reason, no backtrace, nothing in the log.** Now bounded on attempts, and the trailing
  delay after the last ping is gone.
- 🐛 **ParcelApp read and copied the serial buffer after freeing it.** `free()` sat above the
  reads; `setText()` then did `strlen()` on the freed block and allocated *that* length from
  the internal heap. A use-after-free presenting as a random-sized allocation.
- 🐛 **`CriticalFile::backup()` leaked both buffers on every settings-screen exit** — it runs
  from the destructor of every settings app. PSRAM, so not a panic mechanism, but monotone and
  reclaimed only by reboot. `restore()` and `store()` always freed theirs.
- 🐛 **Three sprites leaked their pixel buffers on every open.** `TFT_eSprite` has no destructor
  anywhere in the vendored library. Digital Rain (98 B) and Ackman (340 B) are the stability
  problem, not Recorder's 32 KB: `esp32Calloc()` tries plain `calloc()` first, so the small ones
  come from the **internal** heap and are stranded mid-heap, splitting exactly what `largest`
  measures.
- 🐛 **UART Passthrough: a one-byte heap overwrite, an unchecked malloc, and a divide-by-zero.**
  `data[rxBytes] = 0` wrote `data[1024]` on a full read, one byte into the next block's header —
  silent, with the abort arriving later and elsewhere. The terminator was never needed. The
  buffer is checked and in PSRAM now. And an empty baud field gave `atoi("") == 0`, so
  `uart_set_baudrate()` computed `(UART_CLK_FREQ << 4) / 0` — an immediate Guru Meditation on
  the first OK press.

### Still open, deliberately

The Digital Rain two-writer SPI race (`~ThreadedApp`'s bare `vTaskDelete` plus `showMeshPopup`
drawing from the loop while the demo's task pushes sprites) needs an exit handshake and
hardware to verify. Not attempted blind.

## 0.9.44 (2026-08-29) - four more of the one bug, and 0.9.43 confirmed in real use

### 📏 First: 0.9.43 was measured on the handsets, and the ratchet is gone

{user} used both phones for 1-3 h. Read back from `/health.log`, which survives unplugging:

| | phone 1 (never restarted, 174 min) | phone 2 (2 reboots, a case swap) |
|---|---|---|
| app opens recorded | 26 | 21 |
| **net `largest` across all of them** | **+0 bytes** | **+0 bytes** |
| panics | **0** | **0** (both boots `reset_reason=1`) |
| `largest` start → end | 20,940 → 18,648 | 19,200 → **20,816, ended higher** |
| scan-guard false refusals | 0 (12 scans completed) | 0 |

On 0.9.42 the Dialer cost **−2,308 bytes of `largest` on first open, permanently** (−2,416 /
−2,524 / −2,416 / −1,876). Now it takes −1,712 and **gives every byte back on the next probe**,
and 45 of 47 opens are exactly `+0`.

⚠ **A RESIDUAL DRIFT REMAINS AND IS NOT APP SWITCHING.** Phone 1 still lost 2,292 bytes over
174 min in two steps that fall BETWEEN app opens, which netted zero. Unexplained. ⚠ `min-ever`
free internal reached **4,076 bytes** — still one bad transient from trouble.

### 🛑 Two write-only UDP sockets, opened on the WiFi event task, deleted

`processWiFiEvent()` called `udp.begin()` and `udpRtcp.begin()` on every `STA_GOT_IP`. Each
costs a `new char[1460]` inside `WiFiUDP::begin()`, behind the same **dead null check** as
`parsePacket()` and `_scanDone()` — and it runs on the **WiFi event task**, so unlike
`udpParsePacketSafe()` there is nowhere to put a try/catch. Two 1,460-byte contiguous internal
requests, on the association that follows a scan that has just taken ~3.6 KB.

**Nothing ever read either socket.** Verified across the whole tree: no
read/parsePacket/write/beginPacket/available/stop on either, ever. RTP audio uses its own `rtp`
socket (Audio.cpp:1500); the only `udpRtcp` mentions are in a commented-out block. So this was
~2,920 bytes of internal heap held for the life of the boot, plus an uncatchable throw, in
exchange for nothing. Sockets, externs and the port constant are gone.

The `delay(100)` went with them — its own comment said it existed solely to stop those
`begin()` calls erroring, so it was blocking the shared event task for 100 ms on every
association for the sake of code that no longer exists.

### 🐛 The base64 message decode leaked, every time, forever

`Section::getValueBase64()` had **no `free()`** — the sibling `putValueBase64()` twenty lines
up always had one. Any message whose text is not `isSafeString()` is stored base64, which is
**any message containing a newline, an emoji, a curly quote or an accent**, so opening a
conversation lost a few hundred bytes to ~2 KB of internal heap that never came back. It also
ran unattended: `ingestMirrored()` decodes each newly arrived mirrored SMS. Now freed, checked
(it was an unchecked write through NULL), zero-initialised (the decoder only NUL-terminates
when it decoded > 0 bytes), and taken from PSRAM.

### 🐛 Mesh DB save could write ~248 KB past its buffer

`selectPersisted()` returned 0 without touching the caller's `keep` mask when its own key
allocation failed. `saveDb()` then sized its snapshot from that 0 but still tested `keep[i]` —
uninitialised `ps_malloc` bytes — copying a 248-byte message for every garbage non-zero byte,
into the PSRAM that holds the node table, the book text and every widget. Silent and unbounded.
One `memset`. Latent rather than imminent (PSRAM has ~3.6 MB spare) but it would have been
blamed on anything but this.

### 🐛 Two unchecked mallocs on the SIP receive path

`normalizeLinearSpaces()` and `parseQuotedString()` both allocated and wrote through the result
on the next line, with no NULL test, on every inbound message with a multi-word display name or
an escaped quoted string. Both now bail: the name stays un-normalised / the string stays
escaped, which is cosmetic. ⚠ Latent on these phones today (no proxy) and live the moment SIP
works.

## 0.9.43 (2026-08-29) - the random restart, root-caused and cut off at both ends

{user} asked for a dive on remaining instability. **Every panic ever captured on either phone
(33 MB of serial, two distinct events) is one root cause: contiguous INTERNAL heap running
out, killing whatever allocates next.** Two death shapes, both observed — `abort()` via
`std::terminate`, and `Guru Meditation (LoadProhibited, EXCVADDR 0x20)` when
`_Unwind_RaiseException` itself faults on a null unwind context. In this build a throwing
allocation is a device-killer either way.

Two changes: one removes the **cause**, the other removes the **trigger**.

### 🔑 The cause: font glyph metrics were eating the internal heap, permanently

`SmoothFont::loadFont()` did **seven plain `malloc()`s per font face** (TFT_eSPI.cpp, both
the SPIFFS and the array variant). Plain `malloc` is INTERNAL heap on this build — there is
no PSRAM diversion at any size. Eleven faces × 7 = **77 permanently-held scattered internal
blocks, ~11.5-15.5 KB**, on a phone with ~20 KB of contiguous internal heap in total.

- ⚠ **Loaded LAZILY, at DRAW time.** `FontCollection::operator[]` builds each face on first
  use and caches it for the life of the boot, and the first use is a widget drawing — i.e.
  *after* `enterApp()` returns. **That is why the app-open probe reads `+0` while `largest`
  falls: the probe is blind to font loading by construction.** Worth knowing before trusting
  that instrument again.
- It explains the shape exactly: `largest` ratchets while you visit new screens and then
  PLATEAUS, because there are only eleven faces. Measured 2026-08-29 on one binary: run 1
  lost 7,380 bytes in 11 minutes of navigation then sat flat for 130; run 2, never touched,
  held 15,500 for 8.9 hours.
- Now `fontMetricAlloc()` → PSRAM, internal fallback. The arrays are read during text layout
  on the loop task — no ISR, no DMA, no `IRAM_ATTR` — so external RAM is safe for them.
- 🐛 **And the seven were used UNCHECKED**, so a font loaded on an exhausted heap wrote
  through NULL — a LoadProhibited blamed on whatever screen happened to open. Checked now.

📏 **MEASURED, phone 1, same `heap` command on a young boot:**

| | 0.9.42 | 0.9.43 | |
|---|---|---|---|
| free | 19,388 | 23,216 | **+3,828** |
| largest | 19,208 | 20,940 | **+1,732** |
| min-ever | 10,312 | 15,948 | **+5,636** ← the worst moment of the run |
| blocks | 404/15 | 374/18 | **−30** ← the metric arrays, gone from internal |

⚠ **NOT YET PROVEN: that this removes the RATCHET.** That needs the phone driven through
screens, and both phones lock after 40 s, so serial key injection is swallowed
(`lock: locked=yes`). Two attempts to test it reported "apps opened: 0" — flat numbers there
mean the test never ran, not that the fix worked. **It needs a thumb.**

### 🎯 The trigger: the periodic WiFi scan was the allocation that actually killed it

`WiFiScanClass::_scanDone()` does `new wifi_ap_record_t[n]` behind `if(!_scanResult)` — the
same DEAD null check as the `new char[1460]` in `WiFiUDP::parsePacket()` that
`udpParsePacketSafe()` already guards. ⚠ **And this one cannot be caught at the call site:
`_scanDone()` runs on the WiFi event task. The only lever is deciding not to scan.**

📏 **MEASURED, phone 1, three scans off the DROP instrument: ~192 bytes of contiguous
internal heap per access point** (n=19 → −3,648; n=19 → −3,648; n=18 → −3,536). Deliberately
not `sizeof(wifi_ap_record_t)` (~80) — the driver allocates alongside the array, and the
number that predicts the crash is the total effect on `largest`.

**The crash, exactly:** the phone died with `largest = 3,072` and `[autosw] scan started` as
the last line in the log. 19 APs needed 3,648. There were 3,072.

`wifiScanMemoryOk()` now gates all four scan sites, sizing its estimate from the last scan's
AP count so it adapts to the actual air. Checked against every heap state seen in the wild:
no false refusals at 19,208 / 15,500 / 11,016 / 9,512; refuses at 3,072. ⚠ It is a
PREDICTION, not a guarantee — the count comes from the previous scan and the allocation
happens later on another task. It turns certain death into unlikely, which is the same trade
`udpParsePacketSafe()` makes and the best available.

### Also measured, not yet fixed

- **The phone freezes for the whole LoRa airtime, every transmit.** `MeshPhy::send()` polls
  TxDone with `delay(1)`. 205 loop stalls in 9.9 h idle, median 649 ms, max 2,285 ms (= the
  2,000 ms TX timeout). Causally demonstrated: **6/6 deliberate transmissions produced a
  stall, 1/6 matched idle windows did** (p≈0.008). Airtime theory predicts 575 ms for a
  51-byte SF11/250k packet. ⚠ Most of these are **unlogged flood REBROADCASTS** — only ~31 of
  the 154 long stalls have a matching announce line: the phone freezes its own UI to relay
  other people's packets.
- ⚠ **A third instance of the same throwing-allocation bug is still open:**
  `processWiFiEvent()` calls `udp.begin()` twice on `STA_GOT_IP`, and `WiFiUDP::begin()` does
  `tx_buffer = new char[1460]` with the same dead null check — on the WiFi event task, on
  every reconnect, moments after a scan has just taken 3.6 KB.

## 0.9.42 (2026-08-28) - conversations know who they are talking to

Two things reported from use, both of them the same complaint: the Messages app knew the
phone number and expected you to know the person.

- **Chats, threads and thread lines are labelled with the CONTACT NAME** where the phonebook
  has one, falling back to the address exactly as before where it does not. {user}: "I don't
  have each contact member number memorized, I have to keep clicking the conversation to see
  who is who."
  - ⚠ **Matched on the grouping IDENTITY, never on the string.** The same person reaches the
    message store as `14255550142@seattle1.voip.ms`, `+14255550142` or a bare `4255550142`
    depending on which way the message came in, and the phonebook holds a fourth spelling
    again. `sipThreadIdentity()` — the normaliser the threads are already grouped by — is the
    one rule, so a name can never attach to a row the identity says is somebody else. Both
    the `s` and `l` fields are checked, since a LoRa-only contact has no SIP address at all.
  - ⚠ **The phonebook is loaded ONCE, in the app's constructor**, beside the message store.
    A lazy load at the first label would have put a SPIFFS read inside a menu redraw, and
    SPIFFS on this board takes its stalls in one indivisible piece. An unloaded phonebook
    just means no names, which is what the old code showed anyway.
  - ⚠ The open thread's label is **copied** into `MessagesApp::threadLabel`, not pointed at:
    `HeaderWidget::setTitle()` keeps the pointer rather than the string, and the name it
    would point at lives in the phonebook INI that the picker is free to rewrite. It is also
    one phonebook scan instead of one per rendered message.
- **New Message → Choose → someone you have already texted now OPENS that conversation**
  instead of starting a second, parallel history with the same person. {user}: "think how phone
  texting apps should work."
  - The composer reads the thread snapshot `buildChats()` already built — no rebuild, no
    message-store scan — and exits handing the peer back only when a thread actually exists.
    A genuinely new correspondent is unchanged: the address is filled in and you type.
  - ⚠ **Only with an EMPTY message body.** Choose is reachable after typing (move focus back
    up to "To:" and press OK), and somebody who has already written the message meant to
    send it, not to go read history.
  - Back out of the thread lands on that person's row in the chats list, not at the top.
- 🐛 **And picking a recipient no longer bins a half-written message.** Found while writing
  the guard above: `CreateMessageApp::setupUI()` rebuilds every widget on the screen — that
  is how the pick fills "To:" in — and the fresh body widget came back empty. So "type the
  message, then choose who it goes to", which is a perfectly ordinary order to do it in,
  silently threw the message away. The body is now carried across.

## 0.9.41 (2026-08-28) - predictive text in the note page

- **Note Page predicts.** Prose, like a message body — the other place you write sentences
  rather than an address or a key.
- 🔑 **And predictive text can no longer outlive the app that claimed it.** `t9Field` was set
  by the field that wanted T9 and otherwise cleared only by `setInputState()`, which nothing
  calls on the way out of an app — so it stayed set on menus with no text field at all,
  swallowing digit keys and stopping UP/DOWN/BACK from navigating. Now cleared at the two
  doors every app leaves by, beside the existing `clearTextFocus()`. This is the general fix
  for a bug already patched twice in one app; an app that wants T9 re-claims it when it
  builds its field.
- ✅ Verified on hardware: the note page reports `opted in: yes` and predicts "Hello";
  leaving it reports `opted in: no`.

## 0.9.40 (2026-08-28) - predictive text in the SIP message body

Reported from use: the SIP composer had no T9.

- The message body now predicts; the **"To:" address deliberately does not** — it is a SIP
  URI, and a dictionary has no business in it.
- ⚠ **This screen is the first with two text fields wanting opposite things**, and `t9Field`
  is one flag on ControlState, so it has to follow FOCUS rather than be set once per screen.
  Synced by the app after the focus move settles, NOT pushed from the widget:
  `FocusableApp::setFocus()` calls `setFocus()` on every widget in array order and each one
  resets the input state, so a widget positioned after the target would clear a flag the
  target had just set. Doing it in the app is immune to that ordering; doing it in the widget
  would have worked here by luck and broken on the next screen with three fields.
- ✅ Verified on hardware: `To:` reports `opted in: no`, DOWN to the body reports
  `opted in: yes`, and 4-3-5-5-6 predicts "Hello".

## 0.9.39 (2026-08-28) - the rest of the review

Ten more findings from the same adversarial pass, triaged and fixed. Nothing here was
reported by using the phone; all of it came from reading.

**Input path**
- **Both hold gestures fired while the phone was LOCKED or the screen asleep.** `uiKeyDown`
  is set by the keypad scan whether or not the press reached the app, and neither hold was
  gated. Holding `0` on a locked draft queued a backspace and a `0` straight into the text
  field — the last character you really typed was deleted and replaced, invisibly, behind the
  lock screen. Both now gated the way the F2 music hold already was.
- **One `#` press destroyed a pending automatic capital.** `t9Commit` cleared `t9Caps`
  unconditionally, and `#` called it on every press — so looking at the modes and coming back
  gave "hello" rather than "Hello". Only a commit that actually committed something consumes
  the capital now.
- **A mode change silently ate a half-typed multi-tap letter.** `#` cleared `inputCurKey`
  without emitting it, while the comment three lines above claimed the opposite. Both this
  and the `*` drain now share one helper.
- **The `#` hold was a toggle, not an arm** — and in Abc/ABC it landed on a flag those modes
  never read, making it a 500 ms no-op that also stepped the mode back. It arms now, and maps
  onto `inputShift` where that is the flag in play.
- **A held digit never redrew the footer**, so the strip contradicted the engine until the
  next keypress.
- **`t9LiteralDigit` was the one emit path that never called `t9NoteChar`** — hold a digit,
  then type, and the next word came out capitalised mid-line.

**Dictionary**
- **Slurs were in the table at ranks 4,600-6,500** and would have been offered as
  predictions. Blocked, along with subtitle plumbing (`subtitles`, `sync`, `corrected`,
  `ripped`). Ordinary profanity stays — people type it and the phone is not a chaperone.
- **Contractions inherited their base word's rank and each family was inserted in reverse**,
  so `4-3` offered "I'd" before "he" and `4-6` offered "I'm" before "in". They have their own
  rank band now, ordered by their own frequency.
- **`y'all` and `o'clock` were being dropped entirely** — no usable base word, so the old
  fallback appended them past position 46,000 and the cut discarded both.
- **The documented SD path was not the path the firmware reads.** The usage line said
  `/t9-extra.txt`; the code opens `/t9/extra.txt`. Following the instructions produced zero
  extra words.

✅ Verified on hardware: cycling all four modes then typing gives "Hello"; `#` mid-letter now
emits the letter. `43` gives "he" first and `46` gives "in" first. Host test 71 checks.

⚠ Known and NOT fixed: common first names outrank ordinary words on some keys (`sam` over
`ran`, `jack` over `lack`, `mike` over `mile`), and they are stored lowercase so they are
wrong even when they are what you wanted. Removing names is worse than the problem;
demoting them properly needs a name list and a rank penalty, which is a change worth making
deliberately rather than in a batch like this one.

## 0.9.38 (2026-08-28) - a test that knows English, and the words the corpus was too old to have

The 0.9.37 review found "cat" missing from the dictionary while all 68 host checks passed.
This closes that gap and the vocabulary hole it exposed.

- 🔑 **The host test now asserts what the dictionary must CONTAIN**, not merely that it is
  self-consistent. 166 hand-written everyday words — the commonest hundred, a deliberate
  block of two- and three-letter words, things people text about, the contractions, and the
  phone vocabulary — each checked as present AND as actually typable. Hand-written on
  purpose: a list derived from the corpus that builds the table could never disagree with it.
- **Plus a blunt instrument that needs no English:** how many of the 512 three-digit keys
  match at least one word. 111 when cat was missing, 373 now, against a ceiling of 433 for
  the whole 50,000-word corpus. The floor is 350.
- ✅ **The test was proven against the bug it was written for.** Rebuilding the old dictionary
  and running it gives 3 failures naming cat, sat, mat, tip, gym, pop, pub, map and usb, and
  reports coverage collapsed to 111 of 512. It would have stopped this shipping.
- **Words a 2018 subtitle corpus is too old to rank.** wifi, bluetooth, usb, username, login,
  logout, browser, screenshot, emoji, smartphone, hotspot and friends were all absent from
  the table — film dialogue rarely says them. ⚠ They are PROMOTED, not added: most were in
  the corpus already, just far below the 25,000 cut ("wifi" sat at rank 32,756), so the first
  attempt skipped them as "already present" and changed nothing at all.
- Deliberately NOT added: meshtastic, lora, repeater. Project vocabulary belongs in the SD
  extra dictionary, which is exactly what that design is for — baking one project's jargon
  into everybody's firmware is the thing it exists to avoid.

## 0.9.37 (2026-08-28) - four T9 bugs found by review, three of them mine from today

An adversarial read of the finished feature, run while the phone was being used. Every one
of these was confirmed on hardware before it was touched.

- 🔑 **"cat" was not in the dictionary.** The rank cutoff that correctly removes two-letter
  initialisms (tv, dr, pm, cd) was applied at THREE letters as well, which left 125 of 1,115
  three-letter words and made **401 of the 512 three-digit keys match nothing at all**: cat,
  bat, cop, bus, cup, art, cry, sat, mat, tip, gym, pop, pub were all untypable. The
  generator's own worked example — `228 -> cat, bat, act` — described a table containing
  neither cat nor bat. `--short-len` now defaults to 2. Costs 1.1% of unambiguous keys and
  one on the worst run; the initialisms that come back (bff, omg, fbi, dna) are mostly things
  people type anyway.
- **Punctuation landed after the space it preceded.** Every T9 path returns before the
  multi-tap machine, which is the only code that commits an armed '*' selection — so typing
  a word, '*', then '0' gave "Hello ." instead of "Hello. ", and if the screen changed before
  the 2 s idle timeout the full stop was lost outright. It also left inputCurKey set, which
  blanks the prediction strip. The commit is now hoisted to where T9 can reach it, and the
  space moves to r2 so the symbol still goes first.
- **A held digit jumped ahead of the word it interrupted.** t9LiteralDigit popped the held
  digit off the pending word but never committed what remained, so 4-3-5-5-6 then holding 7
  produced "7hello". Anyone typing mp3, covid19 or route66 would have hit it.
- **Predictive text outlived the screen that owned it.** t9Field is set by buildCompose and
  cleared only by setInputState, which leaving compose does not call — so T9 stayed live on
  the thread list, swallowing digits and stopping UP/DOWN/BACK from navigating.
  MeshtasticApp::enterState now clears it; Compose re-claims it.
- ✅ All four verified on phone 1: `228` gives "Act 1/4" and DOWN gives "Cat 2/4"; a word then
  '*' then '0' gives "Hello. "; leaving Compose reports `opted in: no`.

⚠ Worth recording how these were found. The engine's 68 host checks passed throughout — they
test t9.cpp, and three of these four live in the input path above it. The one bug the host
test could have caught (the missing three-letter words) it did not, because it only asserts
that words IN the table are reachable, never that words a user expects are in it.

## 0.9.36 (2026-08-28) - bring your own T9 words

An optional second dictionary, read from `/t9/extra.txt` on the SD card at boot. Its words
are always offered AFTER the built-in English ones, which is exactly what you want from
jargon: reachable when you need it, never in the way.

- 🔑 **It is a file on the card, not part of the firmware.** One person's vocabulary has no
  business in a stranger's phone, and keeping it off the image means the licence of whatever
  was harvested is never a question for anybody else. It also means adding words costs a file
  copy rather than a reflash, and it generalises — callsigns, place names, a team roster.
- **PSRAM, read exactly once, at boot.** ~105 KB for 8,700 words (a 70 KB blob plus a 35 KB
  pointer array, two allocations for the whole dictionary — the words are never copied
  individually). Measured on hardware: internal heap unchanged at 18,736 free. A filesystem
  read per keystroke is the freeze this firmware has fought before, which is why the built-in
  table is in flash and this one is read once.
- **Every failure is silent and named.** No card, no file, a malformed file, no PSRAM: the
  table stays empty and predictive text works exactly as it does without it. `t9` on the
  console says which of those happened, because a dictionary that quietly failed to load
  looks identical to one with none of your words in it.
- **`up on t9`** starts the WiFi uploader pointed at `/t9`, and **`t9 reload`** picks up the
  new file without a reboot — a word list is a thing you iterate on.
- New tools: `fetch_sarna_titles.py` (page TITLES via the MediaWiki API — never article text;
  the prose is GNU FDL 1.2 and a vocabulary list is not a copy of it) and `gen_t9_extra.py`,
  which takes any one-per-line list, drops what the built-in dictionary already has, and
  sorts the rest for the phone's binary search.
- ⚠ The useful knob is `--min-titles`. Words appearing in exactly ONE title across a 93,000
  article wiki are where both the junk and the cost live: at 2 you get 8,753 words for 68 KB
  and the worst candidate run stays at 9, where the full tail takes it to 11.
- ✅ Proven on phone 1 end to end: 93,649 titles harvested, 8,753 words uploaded over WiFi in
  0.6 s, `Kerensky` predicted and auto-capitalised — while `4663` still gives `good` as
  candidate #1, now 1 of 8 instead of 1 of 7.

## 0.9.35 (2026-08-28) - T9 predictive text

Five keypresses for "hello" instead of thirteen. Type d-o-n-t and get "don't". Off by
default nowhere: it is on, with a Settings row to turn it off, and '#' steps out of it
mid-sentence without a trip to a menu.

- 🔑 **25,000 words in flash, ZERO bytes of heap.** The table is `static const` in
  memory-mapped `.rodata`, so a lookup is a plain pointer dereference and performs no
  allocation at all — which is the entire safety argument on a phone whose internal heap
  has been measured at 2,276 bytes free at its worst, and where a failed `new` throws with
  nothing to catch it. 293 KB of flash (8.7% of the free DROM window), +64 bytes of static
  RAM, and a worst-case lookup of 25 binary-search probes.
- **Sorted by keypad digit key, then by frequency.** Neither the key nor the frequency is
  stored: the key is recomputed from each word, and the rank IS the position in the run. So
  "most likely first" and "press DOWN for the next" both fall out of an index, for free.
  91.9% of digit keys match exactly one word; the worst collision is 9.
- **UP/DOWN cycle candidates, but ONLY while a word is pending** — with nothing pending they
  still move focus and the cursor, exactly as before. Same rule the OK key already followed:
  consume a key only in the state where it means something new. '*' and '#' were left alone
  for the same reason; '#' is the only way to type a capital.
- **'#' cycles T9 -> Abc -> ABC -> 123.** A superset of the shift toggle it used to be, so
  nothing was taken away. 123 exists because "testing 1 2 3" was otherwise untypable.
- **Hold a number key to type the number**, the gesture every keypad phone had. The short
  press has already happened when the hold fires — keys are edge-triggered, and holding
  every digit back until release would put half a second of lag on ordinary typing — so it
  is REVERSED instead: the digit comes back out of the pending word. Holding '0' also
  retracts the space it already typed.
- **Capitals both ways:** automatically for the first word and after `.` `!` `?` plus a
  space, and by hand with a long press on '#' for a name in the middle of a sentence.
- **The pending word lives in the FOOTER, not inline in the text field**, and is inserted
  only on commit. That single decision is why this touched no widget code and why the digit
  buffer cannot drift out of step with what is on screen.
- **Per-field, default OFF, and message bodies are the only field that opts in.** `InputType`
  cannot decide it — the WiFi password field is `InputType::AlphaNum` too — so the other
  twenty-odd text fields keep today's behaviour without anyone having to remember them.
- The dictionary took three rounds, each written into `tools/gen_t9_dict.py` so it is not
  re-learned: `/usr/share/dict/web2` is the wrong KIND of list (headwords only — it rejects
  every plural and does not contain "has", while it does contain "ba" and "aa"); the corpus
  tokenises on the apostrophe so every contraction had to be repaired; and 279 two-letter
  initialisms were removed by a rank cutoff rather than a curated list, because a real short
  word is by definition frequent.
- ✅ Both handsets. Verified end to end over the serial bridge and by hand: prediction,
  candidate cycling, mode switching, held digits, held '#', auto-capitals, the Settings row
  and its persistence across a reboot.
- Source: hermitdave/FrequencyWords (OpenSubtitles), MIT. Only an ordered list of ordinary
  English words is compiled in — no counts, no n-grams, no source text. The 612 KB corpus is
  not committed; `tools/t9-corpus/FETCH.md` has the URL and the hash the generated header
  records.

## 0.9.34 (2026-08-28) - two latent panics on the typing path

Found while scoping predictive text; none of it is T9, all of it is live today.

- `MAX_INPUT_SEQUENCE` was 18, making `inputSeq[]` 19 bytes, but the `*` symbols row is
  `.,!?@$/+-=%^ _:;'*#` - nineteen characters, so `strcpy` (GUI.cpp:1068) wrote twenty.
  Every press of `*` on a text screen overflowed by one, landing harmlessly in the padding
  before the next field. Harmless only by luck, and anything added beside `inputSeq` would
  have been silently zeroed on every press.
- `TextInputWidget::insertCharacter` and `PasswordInputWidget::insertCharacter` both called
  `allocateMore()`, ignored its bool, and then wrote `inputStringDyn[0]` - a NULL store, and
  a StoreProhibited panic, one failed realloc away on a heap measured at 2,276 bytes free at
  its worst. Both now refuse the character instead, which callers already handle.
- `TextInputBase::allocateMore` grew the text buffer with plain `realloc` on the internal
  heap, doubling each time, ceiling 64,000 bytes for some callers. Now `extRealloc`, matching
  `MultilineTextWidget::allocateMore`. `PasswordInputWidget` deliberately stays internal:
  those buffers are tiny, and a secret is better off out of external PSRAM.

## 0.9.33 (2026-08-28) — the Nodes screen fits in memory again

Opening **Nodes** rebooted phone 1. The node database was never the problem — it has
lived in PSRAM since it existed. **The menu rows did not.**

- 🔑 **`MenuOption` was the one hole in the PSRAM rule.** `AbstractWidget::operator new`
  routes every widget to PSRAM, but `MenuOption` does not derive from it, so each row was
  a plain `new` plus a `strdup`'d title and subtitle — **three internal-heap allocations
  per row**, on the ~20 KB heap. `newMenu()` in app_meshtastic.cpp had already written
  this down in passing; nothing had acted on it. `MenuOption` now has the same
  `operator new`/`operator delete` pair, and both strings use `extStrdup`. One definition
  covers `MenuOptionIconned`, `MenuOptionIconnedTimed` and `MenuOptionPhonebook`, so
  **every list screen** benefits, not just Nodes.
- **Why it started now:** the cost is data-driven. `sizeof(MenuOptionIconned)` is 36 B and
  a row measures **~124 B** across its three blocks. Phone 1's database has grown to
  **163 nodes** — 20.2 KB — requested from an internal heap the phone's own `heap` command
  reported at 14.5–17.4 KB free with a largest block of 7.0–13.8 KB. It could not fit, and
  `operator new` throws on failure with nothing catching it: `std::terminate` → `abort` →
  `reset_reason=4`. The database crossed the threshold (~125–135 rows) and the screen
  stopped opening.
- 🛑 **Do not "fix" this by catching `std::bad_alloc`.** This phone has a decoded panic
  where the throw itself was fatal — `_Unwind_RaiseException` faulted with `LoadProhibited`
  before any handler could run (the SIP/UDP abort, helpers.h). The allocation has to not
  fail.
- **`MenuWidget::deleteAll()` leaked every row it owned.** It called `options.clear()`,
  which frees only the `LinearArray` pointer block — the `MenuOption` objects, their
  strings and their two `IconRle3`s were never deleted. `~MenuWidget` had always done it
  correctly; `deleteAll()` simply never adopted it. Its one caller is the SIP accounts
  VIEWING screen, so this leaked three iconned rows per account opened, for the life of
  the boot.
- **Apps go to PSRAM at the root now.** `BooksApp` and `SipAccountsApp` had each grown
  their own `operator new` after pa{user}ing; the other ~40 apps allocated from the internal
  heap only because nobody had pa{user}ed on them yet. The pair moved to `WiPhoneApp` — the
  same "fix it at the root, not per-app" argument `AbstractWidget` makes. The two per-app
  copies are left in place, redundant but harmless, because each carries the measurement
  that justified it.
- **Widget-lifetime text follows its widget.** `ButtonWidget`, `LabelWidget`,
  `MultilineTextWidget` and `MenuWidget`'s own strings were still `strdup`'d internally
  while the objects holding them sat in PSRAM. Also finishes a **partial conversion**:
  `MultilineTextWidget` already used `extStrndup`/`extRealloc` for its wrapped `rowsDyn`
  rows, but the insert-character `realloc` and the split/merge `malloc`/`strdup` paths
  were missed and quietly pulled edited rows back onto the internal heap. `registeredWidgets`
  moved from `LA_INTERNAL_RAM` to `LA_EXTERNAL_RAM` for the same reason — every widget it
  indexes is already external.
- ✅ **Proven on phone 1 over the serial bridge, no hands.** Opening Nodes with 163 nodes:
  internal heap **11,384 → 12,512 bytes free** (it goes UP — the previous menu is freed),
  PSRAM **3,600,696 → 3,580,524** (−20,172 B, ~124 B/row). No panic, no reboot, list
  renders with starred nodes on top. Three further open/close cycles: internal recovers to
  17,856, PSRAM flat — no leak. Before the fix the same screen asked ~20 KB of a heap that
  had ~15 KB.
- ⚠ **Left deliberately undone.** `buildNodes`, `buildThread` (up to `MESH_MSG_CAP` = 1000
  rows), `PhonebookApp::createLoadMenu` and `SipAccountsApp::createLoadMenu` are still the
  only four uncapped list builds in the firmware — every other list caps and says so
  (Files 200, Photos 200, Books 48, Music 96, Chats 24, Places 8). Capping Nodes would hide
  nodes from the user, which is a product decision, not a memory one; with the rows in
  PSRAM it is no longer a crash. `IconRle3` is also still a plain `new` on the internal
  heap, two per iconned row, and it lives in vendored `src/TFT_eSPI/`.

## 0.9.30 (2026-08-27) — peeking at the WiFi list no longer kills auto-rejoin

The 0.9.29 session's P2 wart, closed the same day, with two relatives found in the walk:

- 🔑 **The peek wart:** `NetworksApp`'s constructor clears `reconnect` (so a background
  rejoin cannot race the user's choice while the list is open) — but leaving WITHOUT
  joining left it cleared, so merely LOOKING at the WiFi list silently killed auto-rejoin
  (and the radio, and the driver's remembered AP) until the next join or reboot. The
  destructor now restores it (`Networks::resumeReconnect()`), gated on `userDisabled()`.
- **`disable()` says so in the LIVE flag:** every `disable()` caller is an explicit user
  action (menu WIFI-OFF, Remove, Disconnect, edit-screen WIFI-OFF), but only
  `loadPreferred()` ever set `_userDisabled` — from the INI, typically at boot — so a
  mid-session Disconnect left the live flag stale-false. `disable()` sets it now;
  a deliberate `connectTo()` clears it (the join path already writes `disabled=false`
  to the INI in the same breath).
- **The WIFI-ON toggles hand control back:** both ON paths only called
  `esp_wifi_start()` and left `reconnect` false — an innocent OFF→ON cycle killed
  auto-rejoin until a join or reboot. Both now call `resumeReconnect()`.
- ✅ All three scenarios proven live on phone 1 over the serial bridge, no hands:
  peek & back out → `rec` 0→1, scan, `switching to '{user}-wifi'`, `conn=1`, SIP
  re-registered; explicit OFF + peek + back out → stays off (wifi=255, 50 s silent);
  OFF→ON toggle → `ud=0 rec=1`, hands-free rejoin + SIP REGISTERED.

## 0.9.29 (2026-08-27) — the deaf-scan state is diagnosed, and the phone cures it itself

Phone 1 idle-dropped WiFi and would not rejoin until a manual rescan — and the whole
mechanism was caught live at the work desk with phone 2 as the on-air control:

- 🔑 **The deaf-scan state.** After hours of disconnected retry churn the ESP32 WiFi driver
  reaches a state where **every scan completes with zero results** — async and blocking,
  80 and 240 MHz, screen on or off — while the twin phone hears the same AP at −50 dBm.
  The auto-switcher believed each empty scan ("out of range") and backed off forever.
  Nothing recovers it except a **radio off/on** — which is why "open the WiFi screen and
  rescan" always fixed it: `NetworksApp`'s constructor calls `disconnect(true, true)`, an
  accidental radio bounce (a reboot cures it the same way: a fresh radio heard 6 networks
  and SIP registered inside a minute).
- **The fix:** the auto-switcher now counts consecutive empty disconnected scans and, at
  two, **restarts the radio itself before the next scan** (`_dryScans`, Networks.cpp) —
  logs `[autosw] N consecutive empty scans: restarting the radio`. Deliberately without
  `eraseap`: the driver's remembered AP is not the disease.
- **New bench command `wifi bounce`** (`Networks::bounceRadio()`): the same cure on demand,
  no reboot. ⚠ Its first cut called `WiFi.disconnect(true)` directly from an associated
  phone — radio-off swallowed the STA_DISCONNECTED event, `connected` stayed true, and
  both rescue loops sat gated: wedged half-down until a reflash. The shipped version
  delivers a plain disconnect first (and sets the flag explicitly), then cycles the radio,
  then makes the next auto-switch scan due immediately. Proven end to end: bounce →
  scan n=1 at −36 dBm → rejoined in 9 s → SIP re-registered, hands-free.
- ⚠ Honest limit: the `_dryScans` trigger itself cannot be fabricated at a desk with the
  AP present, so its first live firing will be the next natural idle-drop — watch the
  field log for the line above.

## 0.9.28 (2026-08-27) — the upload page stops eating the phone, and WiFi rejoins by itself

### 🔑 The uploader: the page moves to the raw server, and the browser stops losing

The chunked transfer engine (0.9.2x era: one TCP connection, CRC-checked pieces, stop-and-wait)
was already solid — **what was broken was everything around it.** The page, the favicon, and the
probe still rode the framework WebServer, whose request machinery costs a **~10 KB
internal-heap transient per connection** on a phone that idles at 12–15 KB largest with SIP up.
Measured: TWO loads of the 829-byte page drove the largest free block 12,688 → 3,180 and
tripped the low-heap breaker, which frees the server with escalating backoff (90 → 600 s) —
so nearly every browser VISIT read "site cannot be loaded". No pacing fixes a per-request cost
that starts below the danger line.

So the raw server is now the front door: it listens on **:80** (and :8081 for the existing
tools) and serves the page itself for ~zero heap, keep-alive. The WebServer moved to **:8080**
as the legacy path — multipart `/upload` for `curl -F` and no-JS browsers, `/fetch`'s HTML
replies, `/log` (`http://<ip>/log` 302-redirects there; use `curl -L`). Its breaker was
reworked to match: instant trip below 3 KB stays (that is PHY-abort altitude), the 6 KB line
now requires the pressure to HOLD for 1.5 s (a single request's transient decays in under a
second and is not a flood), raw traffic inhibits trips entirely, and resume is
**recovery-only at ≥ 10 KB** — the timer-resume kept re-planting a ~6 KB server into a heap
that could not hold it, which is the measured allocate/free churn leak. mDNS now begins once
per boot and never ends: this core's `MDNS.end()` leaks a WiFi-event-callback entry per cycle,
and the breaker used to cycle it on every pause.

**Measured after:** page loads went from 2-of-10 answered to 19–20 of 20 under deliberate
hammering; three consecutive 4-book batches (~52 MB) landed 12-for-12 byte-verified at
48–69 KB/s with zero breaker events, zero panics, zero wedges. New: `tools/wiphone_send.py` —
one command from a computer, resumes partial files, retries transient SD 507s, byte-verifies
every file. ⚠ Honest note: the test card threw three 507 write errors across twelve files
(all recovered by retry) — watch that card.

### 🔑 The first REAL-browser run caught a bug every bench tool was blind to

The acceptance bar always ended with "then a real browser", and this is why: the raw-served
page set its piece size but left `BASE=''`, so the sender's `if(BASE)` routed it into the
MULTIPART branch — which the raw server refuses (413: multipart overhead pushes a 16 KB piece
past the cap). Every bench pusher speaks the raw protocol directly and structurally could not
see it. Found in the first minute of driving the page in a real Chrome over an Android
hotspot; fixed (`RAW = BASE || window.RAWPAGE`); re-proven: 2.8 MB in three files through the
page's own JS, byte-verified, an empty file skipped and said so.

### 🛑 WiFi auto-rejoin was a ONE-WAY FLAG, and the WiFi settings screen was the finger on it

`Networks::reconnect` — the flag that permits any automatic rejoining — was set true in the
constructor, false in `disconnect()`/`disable()`, and restored **nowhere**. The settings
screen's SAVE path calls `disconnect()` before rejoining, and that call is
`WiFi.disconnect(true, true)`: radio OFF and the driver's stored network ERASED. So **editing
or saving any WiFi network left that phone unable to auto-rejoin until reboot** — which is why
phone 1 (the phone whose networks actually get edited) spent months "losing WiFi" while an
identical phone 2 never did, and why it once sat 78 minutes next to a hotspot it could join in
two seconds by hand. Fix: a deliberate join (`connectTo()` — the settings screen, the
preferred-network retry, and the auto-switch hop all flow through it) restores the flag.
Proven the same hour: fresh boot associated hands-free, and `wifi drop` rejoined itself in
~2 s — the exact test that had just failed. ⚠ Two related warts seen and deliberately left:
removing a network and the Disconnect button both call `disable()` (radio full-off), heavier
than either action means.

### 🛑 RETRACTED, 30 minutes after being written: "phone 1's radio is hardware-dead"

The deaf readings were real — `wifi scan` 0/0/-2 on fresh boots while a neighbouring phone
heard the hotspot at -60 dBm — but the verdict was wrong, and the flaw was the CONTROL: the
"healthy" phone differed in WiFi STATE (associated), not just in radio, and scans issued while
the connect-retry machinery is mid-attempt (or the radio is off — see the flag above) read
empty or fail outright. {user} joined the hotspot from the phone's own UI in seconds; the same
serial scan then listed six networks. The wrong verdict stands struck-through in the handoff
as a specimen. Kept from the hunt, both useful: **`wifi calreset`** (erase the RF calibration
in NVS, reboot, recalibrate) and **`wifi restore`** (`esp_wifi_restore()` — factory-reset the
WiFi driver's stored state; ⚠ forgets the last-used network).

## 0.9.27 (2026-08-26) — `wifi scan`, and a retraction

- 🛑 **RETRACTED: "the idle downclock is what kills the WiFi association" (commit `3535a85`).**
  It rested on a 2-vs-2 A/B of an intermittent fault. With the full data set the claim does not
  survive: on a **standard router the same phone is 9 for 9 clean at 80 MHz** (eight 180-second
  boot-and-watch trials plus one 30-minute run), the downclock firing every time and
  `empty_scans=0` throughout. **Both failures happened on an iPhone personal hotspot**, which
  stops advertising when it is not actively serving a client — so `scan done: n=0` meant the
  SSID was not on the air, and the phone was right. **No CPU frequency change is being made.**
- 🛠 **NEW: `wifi scan` on the console** — a blocking scan listing what the radio can actually
  hear, with the RSSI and channel of each network. `n=0` has two completely different causes —
  a deaf radio or an absent AP — and nothing on the phone could tell them apart. That ambiguity
  is what sent a whole afternoon after the wrong suspect, so the fix is an instrument, not a
  guess. **Verified on the handset:** `wifi scan` -> `20 network(s)`, with `SmithWifi` at
  -60 dBm at the top of the list.
- ⚠ **Recorded in the handoff: three times today the INSTRUMENT was the fault, not the phone.**
  `grep` silently suppressing matches in a binary-ish capture; every serial port open RESETTING
  the phone and restarting the very association race being measured; and the Mac entering Idle
  Sleep mid-capture (confirmed in `pmset -g log`), which read exactly like a 16-minute firmware
  wedge. Long captures now run under `caffeinate -i -s`.

## 0.9.26 (2026-08-26) — two more that had never run, from the same sweep

### 🛑 The SIP re-init after a network-loss teardown has never once run

`WiPhone.ino:3202` was `gui.state.sipState == CallState::NotInited;` — **a comparison whose
result is discarded, where an assignment was meant.** The `if` block's only effect was clearing
the flag on the next line, so the re-init test below it fired only when `sipState` happened to
be `NotInited` already, or the account had changed. Present since the initial commit;
`platformio.ini` strips `-Werror`, so GCC's unused-value warning could never fail the build.

Written through `setSipState()` rather than as a raw `=`: every other transition in this
firmware goes through it, and it owns the ring-armed and missed-call bookkeeping, which a raw
assignment would silently bypass.

⚠ **THIS TURNS ON A PATH WITH NO HISTORY, AND IT IS THE ONE CHANGE HERE THAT IS NOT PROVEN.**
Bench it: with a real account, drop WiFi mid-call, restore it, and watch for `SIP is going to
init` followed by registration returning rather than flapping. If it misbehaves, that one line
is the whole change.

### Music never paused for a second call

The pause was edge-triggered — `if (callBusy && !wasCallBusy)` over a static. But
`sipCallActive()` counts `HangUp` and `HangingUp` as a call, and this repo has **measured 19+
consecutive minutes parked at `sip=6`** with the proxy unreachable. While parked there the latch
stays true, so an incoming INVITE moves `HangUp -> BeingInvited` — also true — and **there is no
rising edge.** The codec/I2S single-occupancy that the pause exists for was never honoured.

⚠ Fixed at the caller, NOT in `sipCallActive()`: that predicate is shared, and narrowing a shared
predicate to suit one caller is the bug class that produced four of the seven faults in the
2026-08-15 audit. It is idempotent now — `if (sipCallActive() && musicPlayerIsPlaying())` — so it
cannot latch and cannot repeat, and nothing resumes afterwards, as before.

## 0.9.25 (2026-08-26) — a sweep for unswept fixes, and it found five

A multi-agent hunt for the two shapes that cost a working day earlier today: **a fix applied
where it was noticed and never swept**, and **a comment that outlived its change**. Every
finding below was re-verified by hand against the source before anything was touched.

### 🛑 1. Every timestamp this phone put on the air was eight hours wrong

`clock.h:77` — `getExactUnixTime()` is `getExactUtcTime() + timeOffsetSeconds`, and
`WiPhone.ino:1479` loads `[time] zone` at boot and applies it (shipped config: `zone=-8`).
So that call returns the **LOCAL-shifted** epoch.

That was swept in exactly one place. `meshtastic_service.cpp` already carried the note proving
somebody measured it — *"getExactUtcTime, emphatically not getExactUnixTime … stamping it here
put every replayed record 7 hours off"* — on the replay path. **Seven sibling sites still used
the shifted clock, every one of them stamping or comparing a value that meets COVEY's real UTC:**

| site | what it broke |
|---|---|
| the **GPS position beacon** | COVEY reads the Position's `time` field verbatim and renders `time.time() - ts`. **Every WiPhone position on COVEY's map read eight hours stale.** In the woods that is "he moved twenty minutes ago" versus "that fix is from this morning." |
| booksync `turnedAt` (×3) | `bookSyncSuspectClock()` is `turnedAt > now + 300`. Against the shifted clock, **anything COVEY turned in the last ~7.9 h was painted "(their clock looks wrong)"** — the warning was about our own arithmetic. And this phone's page looked 8 h old to COVEY. |
| waypoint expiry (×2) | `expire` is COVEY's UTC. A pin set to die in a day died a day **and eight hours** later. |
| SMS-mirror freshness | the comment says "Ten minutes of slack"; the arithmetic made it **eight hours ten minutes**, so a full resync buzzed for everything under 8 h. |

⚠ **CORRECTED 2026-08-26, and the number matters.** The tables above said "eight hours" from
the shipped `data/configs.ini` (`zone=-8`). **MEASURED on the live phone** — `sun 47.6062,-122.3321`
→ `2026-08-26 at 'given' (local, UTC-7:00)` — the offset in force is **UTC−7 (PDT)**, so the real
shift on {user}'s two phones is **SEVEN hours**. A freshly-flashed phone taking the shipped config
would see eight. Read "eight hours" above as "seven on these phones, eight on a stock one".

🔑 **AND THAT SETTLES THE "7.7-HOUR-OLD POSITION" of 2026-08-25.** At a 7.0 h shift, a position
that had *just* been taken would arrive reading 7.0 h old. It read **7.7 h** — so the fix really
was about 42 minutes stale, which is exactly what a phone indoors with `sats in view: 0` should
look like. **The earlier session's conclusion was right about the cause and wrong about the
magnitude: roughly seven of those 7.7 hours were this bug, and the remaining ~42 minutes were
the sky.** Both halves needed fixing and only one had been.
Left alone deliberately, all local-only: the SIP message store's load stamp, `myPinAtUnix`
(never transmitted), and the two places that compute the offset *on purpose*.

### 🛑 2. Use-after-free on two live SIP teardown paths

`wifiTerminateCall()` and `rtpSilent()` each `delete` every dialog, `dialogs.clear()`, and then
write `currentCall->terminated = 1;` — **through a pointer into what they just freed.**
`currentCall` is only ever assigned from a `findCreateDialog()` result, and that function puts
every dialog it returns into `dialogs`. It was **never set back to NULL anywhere in the file**,
and `sip` is a file-scope global — so `isBusy()` went on reading `->terminated`, `->confirmed`
and `->early` out of freed memory **on every superloop pass, for the rest of the session.**
Both callers are ordinary: a WiFi drop mid-call, and an RTP silence timeout.
The sibling `terminateCall()` opens with `if (!currentCall) … return TINY_SIP_ERR;` — it got it
right and was not copied. Now: marked terminated *before* the delete, pointer dropped after,
and the destructor drops it too.

### 🛑 3. A received message could walk off a 256-byte stack buffer

The channel-URL parser clamped with `(i + l <= (size_t)len)`. That is 32-bit arithmetic and it
**wraps**: a length varint of `0xFFFFFFFE` with `i == 8` makes `i + l == 6`, which passes, and
the walker then runs to `0xFFFFFFFE`. Every other protobuf walker in the repo clamps by
subtraction (`if (l > len - i)`), which cannot wrap; this one receive-side walker missed it —
and "Apply link" hands a received message's own text straight to it.
Fixed by clamping the safe way in both the outer and inner loops, and advancing the cursors by
the **clamped** length. **Verified on the handset**: the actual overflow payload
(`chan Cv7___8P`) now returns "0 channel(s) added", channels intact, no crash.

### 4. The GPS quality gate guarded the radio and not your own screen

`meshPosFixUsable()` had exactly one caller: the **transmit** path, under a measured note that a
`sats=3 hdop=6.4` fix was **20 km wrong**. `resolveReference()` never called it. So the phone
refused to put a bad fix on the air and then computed every distance and bearing **on your
screen** from it — against its own written argument that "a confident wrong answer, in the woods,
is worse than no answer". The gate is now on both reference branches; a bad fix falls through to
the manual pin.

### 5. Screen settings was a third reader of the lock/dim/sleep keys, with the old defaults

0.9.16/0.9.17 fixed the boot reader and the shipped config and called it "all three now agree".
**`GUI.cpp`'s Screen-settings app is a fourth party and was never swept** — seven sites falling
back to `0` (and `dim_level` to 100 against boot's 15). Save writes the widget values back, so
**opening Screen settings on a config missing a key and pressing Save persisted `dimming=0`,
`sleeping=0`, `lock_keyboard=0`** — and the lock rides on sleep, so that silently took the lock
with it. Also `data/configs.ini` still shipped `lock_keyboard=0`, so any `pio run -t uploadfs`
handed out a phone that never locks; `INTERNAL_FLASH.txt` documents it as `1`.

### 6. The guard written to stop the key-0 bug recurring missed three of its four spellings

The `git grep` added this morning would **not have caught the bug it was written for**. Checked
against the real historical lines: it misses a **named constant** (`ROW_INERT = 0`, which is
exactly what Photos had), a **wrapped call** (the `Channel 'booksync': MISSING` row had its
`, 0, 1);` on a continuation line), and the **two-argument form** (`style` defaults to 1).
Replaced with `tests/check_menu_keys.py`, which joins continuations, accepts both terminators,
and resolves an identifier key against enum/`#define`/`const` zeros in the same file. Proven
against all three previously-missed spellings, with no false positives, tree clean.

### 7. Two more copies of the SIP-flap myth, in the file 0.9.21 was written to purge

0.9.21 corrected `tinySIP.h:624` and MEMORY.md **by name and never grepped `docs/HANDOFF.md`** —
so that release is itself an instance of the bug it was fixing. Two copies survived, and the
worse one is headed **"READ BEFORE RE-TRYING"** and concludes *"THE LESSON: THE FLAP WAS
LOAD-BEARING"* — an instruction to a future session not to re-apply a fix that is shipped,
measured and working. Both marked superseded.

### Found and deliberately NOT shipped

- **`WiPhone.ino:3202` is `gui.state.sipState == CallState::NotInited;`** — a comparison whose
  result is discarded where an assignment was meant, present since the initial commit, so the
  SIP re-init after a network-loss teardown **has never once run**. One character. ⚠ Changing it
  turns on a path with no history at all, and that wants a real account and a real call to
  bench, with {user} present.
- **Music never pauses for a second call.** The pause is edge-triggered on `sipCallActive()`,
  which counts `HangUp` — measured at 19+ minutes in this repo — so while parked there an
  incoming INVITE produces no rising edge and the codec is never released.
## 0.9.22 (2026-08-26) — the rename field had no way to delete a character

The last screen in the Photos app that nobody had ever driven, driven at last — and it shipped
with a hole, exactly as 0.9.18's note said to assume.

- 🛑 **`PHOTOS_RENAME` tested `LOGIC_BUTTON_BACK(event)` BEFORE the event could reach the text
  widget**, and on the 21-button keyboard that macro is `BACK || END` (`Hardware.h:213`). So
  **Back never got to the field, and Back is backspace.** The screen opens prefilled with the
  current filename, so the only edit possible was to APPEND: you could turn `BT06.JPG` into
  `BT06.JPGxyz` and nothing else. Renaming a photo was, in practice, impossible.
- **MEASURED on the handset before the fix:** four Back presses walked four screens *out of the
  app* instead of deleting four characters, while `key 7` and `key 2 2` typed `p` and `b`
  perfectly well. The field worked; only the erase was missing.
- ⚠ **Every other text screen in this firmware already had it right and said so** —
  `app_books.cpp:1944`, `app_meshtastic.cpp:1372/:1396/:1419`, with the convention written out
  at `app_meshtastic.cpp:198`: *cancel is END, because Back is backspace in a text field.*
  **Photos was the single exception in the whole firmware.** Fixed to match them rather than to
  invent a fourth spelling — the rule at `Hardware.h:216` is that every consumer declares what
  it needs.

### The whole rename path, exercised end to end on the handset

Every step photographed, and the card finished byte-for-byte as it started:

| step | result |
|---|---|
| open Rename | field prefilled `BT06.JPG`, cursor at the end, Save/Back softkeys |
| type | `key 7` → `p`, `key 2 2` → `b`; multi-tap candidate strip renders (`tuv8`) |
| `#` | toggles case — strip switches to `ABC2` |
| `*` | opens the punctuation picker (`.!?@$/+-=%^_.,;'*#`) |
| 8× Back | erases all eight characters, placeholder "New name" returns, **screen does not exit** |
| Save with no extension | refused, and **says so**: "Keep a .jpg/.jpeg/.bmp ending" |
| rename to `BT06.JPEG` | **"Renamed to BT06.JPEG"**, list updates, 13 photos |
| rename back to `BT06.JPG` | **"Renamed to BT06.JPG"**, list restored |

🔑 **Both of those result lines are messages that could not render at all before 0.9.18** — they
go through the row that `addOption(..., 0, ...)` was silently dropping. This is the first time
anyone has seen a Photos rename report its own outcome, success or refusal.

## 0.9.21 (2026-08-26) — the SIP flap really was fixed; two things near it were not

{user}: *"I think the sip flap is fixed, I remember a session a bit back about it… but let's check."*
**He is right, and it is now measured rather than remembered.**

### The flap: measured, not inferred

330 s of console on phone 1 (the phone that actually holds the account), WiFi up throughout:

```
SIP REGISTER -> sending (registered=0 everRegistered=0)     <- the initial one
SIP REGISTRATION -> REGISTERED                              <- fired ONCE, at boot
SIP REGISTER -> sending (registered=1 everRegistered=1)     x7 refreshes, ~41 s apart
```

**8 REGISTERs, one state change, zero "lost".** Before 3d9f329 the sequence was
REGISTERED → lost → REGISTERED once per refresh, each one firing REGISTRATION_UPDATE_EVENT, a
redraw and a 240 MHz boost. Both halves of that fix are present and doing their job:
`requestRegister()` no longer clears the flag, and `REGISTER_PERIOD_MS` 45 s sits under the 60 s
expiry.

🛑 **What made this worth re-checking was not the code — it was two stale English sentences.**
`tinySIP.h:624` still said *"THE REAL FIX, deliberately NOT shipped tonight"*, sitting directly
above a paragraph describing that same fix as in place; and MEMORY.md said the real fix was
"left for a session with {user} present". Both were written on 08-22 and were obsolete by 08-24.
**They cost a session's work to disprove.** Both corrected. A comment that outlives its own
change is worse than no comment.

### 🛑 `rtpSilent()` was asserting SIP registration from a media timeout

`TinySIP::rtpSilent()` — reached when the OTHER PARTY'S RTP stream goes quiet
(`Audio.cpp` raises `RTP_SILENT_ON`, `WiPhone.ino:3186` calls it) — ended with
`TinySIP::registered = true;`. It reads like a copy of `wifiTerminateCall()` just above it with
the boolean flipped; that one sets `false` and has a reason to.

**What it cost:** `registered` drives `controlState.sipRegistered`, and CallApp's END/BACK
handler branches on it (`GUI.cpp:5575`, `:5592`) — not registered exits the app, registered
takes the hang-up path. So after an RTP silence timeout on an **un**registered phone, the flag
was forced true and END stopped being the one press that leaves the call screen. Bounded only
because `registrationInvalid()` also checks the expiry clock, so the lie corrected itself within
`REGISTER_EXPIRATION_S`. The assignment is gone; registration state is not this function's
business.

### 🔋 The WiFi scan backoff was defeated by its own retry line

Found while watching the console for the flap, and it is a battery bug.

`autoSwitchTick()` has a careful backoff — scan every 2 min while disconnected, easing to 5 min
"once it is clearly not a brief blip", written specifically so *"a phone carried out of range
all afternoon"* does not pay for scan after scan. An empty scan then set
`_msLastScan = now - AUTO_SCAN_PERIOD_MS + AUTO_SCAN_RETRY_MS`.

🔑 **That expression is only correct while CONNECTED.** It means "570 s ago", and the due-check
compares against `AUTO_SCAN_PERIOD_MS` *only when connected*; disconnected it compares against
120 s (or 300 s). A stamp 570 s in the past is already older than either, so the scan was due
**immediately** and the next tick started another one.

**MEASURED on phone 1 with the access point gone: 114 scans in 280 seconds — one every ~2.5 s,
against a design of one every two minutes.** Each scan lights the radio for a few hundred ms.
⚠ **And an empty scan IS the out-of-range case**, so the one situation the backoff was written
for was the one situation it never applied to.

Fixed by splitting the two outcomes, which the old code ran together:
- `n == 0` — a completed scan that found nothing. That is out-of-range; stamp it as an ordinary
  scan and let the 2/5-minute easing run.
- `n < 0` — the scan was **aborted** (a reconnect cycled WiFi under it). It learned nothing, so
  retry soon — via `scheduleScanRetry()`, which computes the stamp against whichever period the
  due-check will actually use. `currentDiscPeriod()` is now the single definition of that
  period, because the due-check and the retry scheduler disagreeing is the whole bug.

✅ **VERIFIED ON HARDWARE 2026-08-26**, same phone, same cable, hotspot switched off by hand:

| | scans while disconnected | cadence |
|---|---|---|
| before (0.9.20) | **114** in ~280 s | one every ~2.5 s |
| after (0.9.26) | **3** in 420 s | one every ~2 min — the designed rate |

`sinceScan` now reads `0s` after a scan completes where it read `570s` before, which is the
stamp the whole bug was about. **About a 38x cut in radio-on time when the phone is out of
range.** And it still comes back at once: with the hotspot restored the phone reported
`wifi: up` and `registered: yes` on the next boot, so the slower cadence costs nothing on
return — the reconnect loop is a separate path from the scan.

## 0.9.20 (2026-08-26) — the rest of the menus were dropping their rows too

0.9.18 fixed `addOption(text, 0, ...)` in Photos. **It was never swept.** Eight more call sites
were still passing 0 in Books, Files and Music, so those rows have never once appeared:

| where | what nobody has ever seen |
|---|---|
| `app_books.cpp:525,528` | "(no books yet - add some)", and *why* a book would not open |
| `app_books.cpp:1512` | what the last booksync send did, good or bad |
| `app_books.cpp:1657,1659` | "Channel 'booksync': **MISSING**", "Parked positions: N" |
| `app_files.cpp:156,182` | the result of the last file operation; "(more files not listed)" |
| `app_music.cpp:89,93` | "(no music yet)", and *why* a track would not decode |

⚠ `app_files.cpp:156` carried the comment **`key 0 = inert`** — the exact false belief 0.9.18
was written to kill, still sitting one file over. That is what an unswept fix looks like.

- **`MenuWidget::addNote()`** now exists so there is one correct way to add a display-only row
  and the wrong way has something to point at. The refusal message names it:
  `menu option key is 0 - row DROPPED, not added. For a display-only row use addNote()`.
- 🛑 **A NOTE ROW IS SELECTABLE, AND A RANGE TEST WILL EAT IT.** `MENU_ROW_NOTE` is
  `0xFFFFFF01`, and screens that test `sel >= ROW_FIRST` compute `(int)(sel - ROW_FIRST)` —
  which **underflows to a NEGATIVE index and sails straight through `idx < entryCount`**, then
  reads off the front of the array. Files, Books and Music each reject `MENU_ROW_NOTE`
  explicitly *before* their range test. This is why the note key is not simply "some number
  above the action base".
- **A source guard in `tests/run_tests.sh`** fails the suite on any new
  `addOption(..., 0, ...)`. A grep and not a unit test on purpose: the wrong spelling compiles,
  links, runs and silently shows nothing, so there is no return value to assert on.
- **Verified on the handset**: marking a file for copy in Files now shows
  **"Copy marked - open a folder an.."** as the first row — a line that had never rendered.
  OK pressed on it three times: nothing happens, no crash, screen unchanged.

### Chat history: the round trip that 0.9.19 still owed

With {user}'s go-ahead, one text on the private hunt-group channel, end to end:

- phone 1 `send 2 persist-check 0826` -> `MESH RECEIPT: 'persist-check 0826' -> in mesh`;
  msgs 26 -> 27 and `/meshdb.bin` on the card 9272 -> 9680 bytes.
- **phone 1 rebooted: the message is still in the thread.**
- **phone 2 received it over the air and it survived phone 2's reboot too** — `loaded from SD
  at boot`, 1 msg, its card file 1704 -> 2032 bytes. That is the incoming direction, which is
  the one that matters: phone 2 is the phone that had been losing everything.

## 0.9.19 (2026-08-26) — the phone read the chat history from one filesystem and wrote it to the other

{user}: *"after rebooting I lose the chat history in meshtastic chats on the wiphone's."* He was
right, and the cause is not in the message code at all.

- 🛑 **`meshFs()` ANSWERED "NO CARD" FOR THE WHOLE OF `setup()`.** It reads a file-static
  (`s_meshCardIn`) that was assigned in exactly one place — `MeshtasticService::loop()` — and
  `loadDb()` / `loadFavourites()` run in `setup()`, before any loop pass. So at load time the
  answer was always the initial `false` and both read **SPIFFS**, while every save from the
  first loop pass onward wrote the **SD card**. The history went into a file nothing ever
  opened, and each boot then saved the stale RAM state back over the card.
- ⚠ **The fallback looked like a fallback and was the same file twice.** `loadDb()` opened
  `meshFs()` and then "fell back" to `SPIFFS` — two spellings of SPIFFS. The card's database
  was unreachable at boot no matter what was in it.
- **MEASURED ON BOTH HANDSETS 2026-08-26, over the cable, before anything was changed:**
  `/meshdb.bin` on phone 1's card was **9272 bytes** and on phone 2's **6352 bytes** — about
  26 and about 14 messages — while phone 2's chat list came up with **no messages in any of
  its five conversations** and phone 1 showed three. The two numbers side by side are the
  whole diagnosis, and neither was obtainable before `meshdb` existed.
- 🛑 **AND EACH REBOOT DESTROYED A LITTLE MORE.** The save is unconditional: it writes what is
  in RAM over the card. On phone 2, which loaded nothing, three diagnostic boots took the
  card's database from 6352 bytes to 1704 — **20 nodes and zero messages.** Phone 2's stored
  history is gone; it was overwritten before the cause was found. Phone 1 survived only
  because its SPIFFS copy happened to be nearly as fresh as its card copy.
- **Fixed** by making `setCardPresent()` the single writer of that flag and calling it from
  `WiPhone.ino` **before** `meshService.setup()`, and by naming SD and SPIFFS explicitly in
  both load paths so a future reordering cannot silently re-create the fault.
- **Also fixed the same way: the starred-node list.** `loadFavourites()` had the identical
  two-spellings-of-SPIFFS fallback, so a star set by hand was written to the card and read
  back from flash.

### How much history now survives, and why those numbers

{user} asked for "a certain amount of the most recent messages of each chat… your call how many".

- New `MESH_PERSIST_*` caps decide what is **written**, separately from what RAM holds:
  **40 per conversation and 200 overall on SD**, **12 and 60 on SPIFFS**. The split is not
  arbitrary — `bench` measures SD at 48–57 ms for an 8 KB image against SPIFFS's 2599–2845 ms,
  about fifty times slower, and a save blocks the keypad, the screen and the WiFi stack
  together. 200 messages is ~48 KB, a fraction of a second on a card.
- Scrollback **within a session** is unchanged (`MESH_MAX_PER_CHAT` is still 150). These caps
  are the floor on what a reboot gives back.
- The rule is "newest first, per conversation", in `mesh_retain.cpp` — pure arithmetic with no
  Arduino headers, so `tests/test_retain.cpp` exercises it on the host (15 checks, including
  that a quiet channel is not starved by a busy one, and that a DM with node 3 is not the same
  thread as channel 3).
- ⚠ **The conversation key here is NOT `chatKeyOf()`**, which folds every broadcast into 0.
  Using it would have put LongFast, hunt-group, booksync and smsmirror in one bucket sharing a
  single allowance.

### The instrument this needed, which did not exist

- **`meshdb` on the console** prints which filesystem is in use, which one `loadDb()` actually
  read at boot, what is in RAM, what the next save will keep, and **the size of `/meshdb.bin`
  on both filesystems**. The fault was invisible from every angle without it: the normal load
  path logged at `log_i`, which this build's log level drops, so the one line that would have
  shown the phone reading the wrong file never reached the cable people were watching. That
  line is now `log_e` and names the filesystem.
- **Verified on the handset**, not just in a test: phone 1 boots with `loaded from SD at boot`
  and 26 messages where the old build loaded 3 into LongFast; a node starred over the cable
  survived a reboot; and `meshdb` reports the same 26 across successive reboots.

## 0.9.18 (2026-08-25) — every message the Photos app ever tried to show was silently dropped

Found while deleting a file at {user}'s request, which is the first time anyone had watched the
app react to an action.

- 🛑 **`ROW_INERT = 0`, AND `MenuWidget::addOption()` REFUSES A KEY OF 0.** It logs
  `menu option key is 0` and adds no row at all. So the Photos list's result line — the one
  thing that tells you what just happened — **has never once appeared on the screen**:
  "Deleted", "Renamed to X", "Wallpaper set", "Not set - <reason>", "X is locked - unlock it
  first", "Too big: N KB", and the "list truncated" warning. All of it written, none of it
  shown. The constant read perfectly (`key 0 = not selectable`) and did the opposite of what
  it said.
- **Proven three ways before it was touched:** a successful delete redrew the list with no
  message at all (screenshotted); the code path in `GUI.cpp:13253` rejects the key outright;
  and the handset logged exactly one `[E] addOption(): menu option key is 0` at the moment the
  list was rebuilt with a note set.
- ⚠ **THIS QUIETLY UNDID THIS MORNING'S 0.9.13 WALLPAPER WORK.** `setAsWallpaper()` was
  rewritten then to stop claiming success and instead report what the loader actually said —
  and the line it reports through is this one. It was only ever visible over the serial cable,
  **which is exactly why the gap survived being "verified"**. A fix checked through one channel
  is not checked.
- **Fixed** by giving the two display-only rows real keys (`ROW_NOTE`, `ROW_TRUNCATED`, both
  `ROW_ACTION + n`). Nothing acts on them: the list handler matches `ROW_RESTORE_WALL` or
  `k <= entryCount` (max 200), so OK on either does nothing — which is the behaviour "inert"
  was reaching for. The widget is untouched; other menus may rely on it rejecting 0.
  **Verified on the handset: "Wallpaper set" now renders as the first row.**
- **Also done, at {user}'s request:** the 0-byte `/photos/20260823_093939.jpg` was deleted
  through the app's own UI — which incidentally exercised the delete confirmation for the first
  time (`Cancel` is first and selected by default, so a mis-timed OK cannot destroy anything).
  A before/after listing confirms exactly one file went and the other thirteen are byte-
  identical.

## 0.9.17 (2026-08-25) — a beacon slot is owed, not spent; and a fix has to be worth believing

{user}: *"I want to make sure the GPS locations are actually reporting and being sent over the
mesh."* They were — COVEY held a real, full-precision position from WiPhone 2 — but the last one
was **7.7 hours old**, because the phone was indoors with zero satellites in view. Two things
came out of looking at why.

- 🔑 **THE SLOT IS NOW OWED, NOT SPENT.** The beacon interval is 300 s and
  `MESH_POS_TX_FRESH_MS` is 30 s, and the old code sent-or-lost the slot in the same instant.
  With a solid fix that is fine — the fix refreshes every second, so every tick sends. But
  **under canopy, where the fix comes and goes, the odds it happens to be under 30 s old at the
  exact instant a 5-minute tick lands are poor** — so a phone that had a perfectly good fix
  forty seconds ago said nothing for another five minutes. That is the hunt scenario exactly.
  Now the slot stays owed and fires the moment a fresh fix arrives.
  ⚠ **The freshness rule is NOT relaxed** — nothing older than 30 s is ever transmitted, which
  was the entire point of it. "At most this often" replaces "only at these instants".
  ⚠ **One slot maximum.** `posDue` is a flag, not a counter, and the deadline keeps advancing
  while it is set, so an hour with no sky owes exactly one beacon rather than twelve.
  **Verified on the handset:** with no fix, `pos` reports nothing owed at t+0 and
  `A SLOT IS OWED - it will send the moment a fresh fix arrives` after one full interval.
- 🛑 **NEW GATE: A FIX HAS TO BE WORTH BELIEVING, and this one is measured.** On 2026-08-25 an
  indoor WiPhone reported `sats=3 hdop=6.4` at a position (scrubbed from this public repo)
  while the phone was in fact at a known spot **about 20 km away** — and nothing on the beacon
  path would have refused it. Had that landed on a tick, COVEY's map would have shown {user} 20 km from where he
  was, with no hint anything was wrong. **Three satellites is a 2D fix**: it solves lat/lon by
  *assuming* an altitude, and a wrong assumption pushes the error sideways, into the one number
  a hunting party reads. Four is the arithmetic minimum for a real 3D fix, so `MESH_POS_MIN_SATS
  = 4` is the standard bar and not a strict one — under canopy a working receiver tracks five to
  ten. `MESH_POS_MAX_HDOP_X10 = 100` is a backstop.
  ⚠ **Refuses only what it positively knows is bad**: both fields are −1 until a GGA supplies
  them, and unknown is *not* failure — refusing on silence would break any receiver that does
  not emit GGA, a different bug than the one being fixed.
  🔑 It lives in `mesh_pos.cpp` as the pure `meshPosFixUsable()`, beside the movement rule, so
  `tests/test_pos.cpp` proves it — including the measured 20 km fix as a named check.
- ⚠ **`lock_keyboard` now defaults to 1 when the key is absent**, matching the missing-*section*
  branch that always did. Two configs that both said nothing about locking used to give opposite
  answers. Same disagreement the 2026-08-22 battery audit found in `[screen]`, where it shipped
  a phone that never dimmed and never slept. Not the cause of the 0.9.16 lock bug — the landmine
  beside it.
- **An empty photo now says it is empty.** `/photos/20260823_093939.jpg` on phone 1 is 0 bytes —
  a died-half-way upload — and Photos said "Could not read this file", which sends the reader
  looking for a corrupt card. It is still listed and still deletable on purpose: COVEY's gallery
  learned the hard way that the pictures you cannot open are exactly the ones you cannot get rid
  of.

⚠ **Only WiPhone 2 has this build** — WiPhone 1 was unplugged and is still on 0.9.16. The GPS
half is irrelevant to it, but the lock default and the empty-photo message are not (the 0-byte
photo is on phone 1).

## 0.9.16 (2026-08-25) — the screen lock was being thrown away by the first button press

{user}: *"whiphone two doesn't like its screen to lock ever (even when set to do so in screen
config). phone 1 works like normal… I just don't want to be accidentally pushing buttons while
it is in a bag."*

- 🔑 **THE PHONE WAS LOCKING. THE FIRST KEY PRESS WAS THROWING THE LOCK AWAY.** `GUI::inCall()`
  answered "yes, we are in a call" for **any** SIP state that was not `NotInited`, `Idle` or
  `Error` — which includes `HangUp`, `HangingUp`, `HungUp` and `Decline`, none of which is a
  call. Its only caller is the unlock path, which reads it as *"a call is coming in, let any key
  answer it"* and clears `locked` on the spot.
- 🛑 **ONE PRESS OF END UNLOCKS A LOCKED PHONE — reproduced on BOTH phones.** END sets `HangUp`
  from anywhere with no call needed, and it does so **before the same keypress reaches the
  unlock check** — so the press that makes `inCall()` true is the press that gets the free
  unlock. END is the red key. A bag presses it.
- 🛑 **And on a phone with a SIP account, EVERY key unlocks for as long as SIP sits in
  teardown.** Those states *stick* when the proxy is unreachable, and this repo had already
  measured it twice: **19 minutes** (2026-08-15, car log) and **SIX HOURS** (2026-08-20) at
  `sip=6`. Reproduced here by dropping WiFi and pressing END. **That is the whole phone-2
  -versus-phone-1 difference: phone 2 has a SIP account loaded and registered, phone 1 has
  none** — so phone 1 only ever had the single-END-press window, and phone 2 spent real time
  with no working lock at all.
- **Fixed:** `inCall()` now means a genuinely live or ringing call — `InvitingCallee`,
  `InvitedCallee`, `RemoteRinging`, `Call`, `BeingInvited`, `Accept`. ⚠ **Answering a ringing
  phone on any key still works**, because `BeingInvited` is in the set; that is the case the
  shortcut existed for. What is gone is the free unlock for hanging up, having hung up, and
  declining.
- ⚠ **THIS IS THE THIRD GUARD IN THIS CODEBASE TO NEED THE SAME CORRECTION**, and the set used
  is deliberately identical to the two that were already fixed — `sipNeedsFullSpeed()` ("includes
  HangUp/HangingUp: teardown, and they stick") and the WiFi auto-switch gate. A fourth spelling
  of "in a call" is how they drift apart.
- **Proven before and after, on both phones, with the same script:** idle 80 s → `locked=yes`;
  press END → **was `locked=no`, is now `locked=yes`**; `OK` then `*` → `locked=no`. The
  legitimate two-key unlock is untouched.
- 🔬 **NEW: `lock` on the console** — the setting, the sleep gate it depends on, the SIP state,
  and what is actually on the card. It prints the sleep gate because **the lock is not its own
  timer**: it fires inside `SCREEN_SLEEP_EVENT`, so a phone that dims but never sleeps never
  locks either, and the dimming makes the timeout look like it is working. Two different causes,
  one symptom.
- ⚠ **Noticed while measuring, not fixed:** `lock_keyboard` defaults to **0** when a `[lock]`
  section exists without the key, while a missing *section* defaults to 1. That is the same
  disagreement that shipped a phone which never dimmed and never slept (2026-08-22 battery
  audit). Both phones here have `lock_keyboard=1`, so it was not the cause — but it is still
  loaded.

## 0.9.15 (2026-08-25) — the clock face gets the same treatment, and the phone can name its own build

- **The clock face now scrims its text**, at {user}'s choice when asked. `00:00`, the date /
  network line, a missed-call line and the softkey label each get a plate sized to the words —
  **not a panel across the screen**, because covering the middle of the idle screen would
  defeat the point of choosing a picture at all.
- 🛑 **ONE PLATE FOR THE CLOCK AND THE LINE UNDER IT, NOT ONE EACH.** The date sits directly
  beneath the clock and their bands overlap by a pixel or two — and two translucent plates over
  the same pixel **blend twice**, painting a visibly darker stripe exactly where the eye lands.
  Both strings are measured first, one union rect is laid down, then the text is drawn.
  Verified by reading the screenshot back row by row: the plate is smooth from y=68 to y=182
  with no dark seam.
- **Measured on the photograph, not asserted:** median plate luminance behind the network line
  is **105 → 5.47:1** against white text, and behind the softkey label **79 → 8.21:1**. Both
  clear the 4.5:1 floor on the brightest wallpaper on the bench.
- Incidentally simplified: the known-time and unknown-time branches used to duplicate the
  drawing. They now build two strings and share one drawing path.
- 🔬 **NEW: `ver` on the console** — firmware version and build timestamp of the binary actually
  running. ⚠ It exists because **two different builds wore 0.9.14** during this session before
  anyone noticed; `d9ed477` warned about exactly that ("a different binary must not wear
  0.9.8's number") and the phone had no way to be asked. Now it does.

## 0.9.14 (2026-08-25) — menu text you can read on any wallpaper, and a cable that can press keys

{user}, straight after the wallpaper fix landed: *"the menus have no contrast and make it
difficult to see the text when a picture is behind them. can the menu items have a translucent
grey background so I can see the words no matter what background I pick?"*

- 🔑 **THIS GOT WORSE THE MOMENT THE WALLPAPER STARTED WORKING, and that is not a coincidence.**
  Until 0.9.13 every phone was showing the same near-black SPIFFS texture, which white text
  happens to sit on perfectly. The main menu is the one menu built non-opaque — it deliberately
  lets the wallpaper through — so the instant a real photograph got behind it, four of the five
  rows became unreadable.
- **Fixed with a scrim**, which is the only answer that holds for a picture the phone is not
  allowed to know anything about: a translucent grey plate between the photo and the words.
  A darker default wallpaper or a brighter font would both have been guesses about the image.
- ⚠ **Tuned by screenshot and then CHECKED AS A NUMBER**, because "looks fine to me" is not the
  property that was asked for. Grey 56 at alpha 190 lands the plate between **41.7** (over a
  black photo) and **106.7** (over a pure white one), so white text on it measures **14.4:1 at
  best and 5.35:1 at worst** — above the 4.5:1 accessibility floor for *every possible
  wallpaper*, which is exactly "no matter what background I pick". 140 was tried and is still
  washed out over a bright photo; 225 is legible but has all but deleted the picture.
- **Only the main menu changes.** Every other menu already paints a solid background, so none
  of them look any different.
- 🔬 **NEW: `key` on the serial console — the cable can now press buttons.** This is the other
  half of the gap `shot` opened in 0.9.13: a cable could already *see* the screen, but not
  change what was on it, so anything you had to navigate to was still unverifiable. The menu
  complaint above lives three key presses from the clock face.

  ⚠ **It injects into `keypadBuff`, the real keypad buffer**, so the wake, the drain loop, the
  easter-egg tracker and each app's own `processEvent` all run exactly as they do for a thumb.
  It is not a simulation of a press; it *is* one, from a different source. A second dispatch
  path would be a second thing to keep in step — and the class of bug that hides is precisely
  the one that put an untested Photos app in a user's hand.

  ```bash
  tools/shot.py /dev/cu.usbserial-025A3F65 menu.png --wait 16 --cmd "key menu" --cmd "key down down"
  ```
- **NEW: `scrim [<alpha> [hex]]`** — retune the plate against a real photograph and take a
  screenshot. ⚠ A bench knob, RAM only, deliberately not persisted: the shipped answer is
  `THEME_SCRIM_*` in `GUI.h`, and a value that exists only in RAM cannot quietly become the
  thing everyone is testing against. `scrim 0` is the pre-0.9.14 look, which is how the
  before/after above was taken from a single build.
- **Measured, not assumed:** nine key presses navigating the menu with the scrim on produced no
  `LOOP STALL` and no panic. The blend is ~55 k pixels per menu redraw and `pushTransparent()`
  caches its blend across runs of identical background pixels.
- ⚠ **The CLOCK face has the same complaint and was deliberately left alone here** — a grey
  box across the middle of the idle screen is an aesthetic call, not a bug fix, so it was put
  to {user} rather than assumed. He chose the scrim; **see 0.9.15 above.**

## 0.9.13 (2026-08-25) — "Set as wallpaper" wrote a good file that nothing ever read

{user}, from the handset: *"I tried to apply a background from a photo but nothing changed, so
there is something broken."* He was right twice over, and the second fault was hiding behind
the first.

- 🛑 **THE WALLPAPER WAS LOADED BEFORE THE SD CARD WAS MOUNTED.** `setup()` mounts SPIFFS,
  calls `gui.init()`, and only ~50 lines LATER calls `SD.begin()` — the card shares SPI with
  the screen and that order is deliberate. The loader lived inside `gui.init()`, so its
  `SD.exists("/background.jpg")` asked an unmounted filesystem and got `false`, every boot,
  forever. The Photos app writes the chosen wallpaper to the **SD card**. Nothing ever read it.

  The fingerprint had been in every boot log for as long as the feature existed:

  ```
  [E][vfs_api.cpp:72] exists(): File system is not mounted
  ```

  **Proven on the hardware, not argued:** phone 2's card was still carrying
  `/background.jpg` at **31,440 bytes — byte-for-byte the size of `BT09.JPG`**, the photo {user}
  had picked. His copy had worked perfectly. The firmware had simply never looked.
- 🔑 **IT WAS SILENT BECAUSE THE FALLBACK IS SHARED.** A rejected override lands in exactly
  the same path as "no wallpaper has ever been chosen", so the screen and the log were
  identical either way. Worse, this phone ships a `/background.jpg` **in SPIFFS** — the plain
  dark texture everyone reads as "the default". That file loaded fine, every time, which is
  what made "nothing changed" so convincing: something *was* being loaded, just never the
  user's choice.
- **Fixed:** `GUI::loadWallpaper()` is now a function with three call sites — `init()` (SPIFFS
  only, but the sprite must hold a picture before the next line of `setup()` redraws),
  **again after `SD.begin()`, which is the call that actually finds the file**, and from
  Photos when the user picks one, so it appears **at once** instead of "restart to see it".
- **`setAsWallpaper()` no longer trusts its own copy.** It asks the loader and repeats the
  answer verbatim, rolling the override back if the loader refuses. A greyscale JPEG still
  *views* in Photos (jpeg_grey.cpp decodes it; TJpgDec refuses greyscale outright) and still
  cannot be a wallpaper — and now it says so instead of claiming success.
- ⚠ **A second silent dropper, found on the way: two different size ceilings on one picture.**
  The viewer opened up to 2 MB; the wallpaper loader capped at 1 MB and fell back with no
  message. A photo between the two viewed perfectly, reported "Wallpaper set", and vanished at
  the next boot. Both are 2 MB now, and Photos refuses an over-limit photo *before* copying
  megabytes to the card.
- 🖼 **Wallpapers now FILL the screen.** TJpgDec only halves — 1, 1/2, 1/4, 1/8 and nothing in
  between — so every photo on the bench card (all 480x270) landed as a **240x135 band across
  the top** with the old background showing underneath, and a 4032x3024 phone photo cannot
  reduce past 504x378 and had its top-left corner cropped out instead. The loader now decodes
  into a temporary PSRAM sprite at the smallest halving that still covers the screen, then
  resamples to fill, cropping to centre. Screenshotted before and after.
- 🔬 **NEW: `shot` on the serial console, and `tools/shot.py`** — the live frame as base64,
  turned into a PNG on the host. **This is the instrument this repo did not have.** Every
  screen in Photos needs a key press, a cable cannot press keys, and that is exactly how a
  broken wallpaper reached a user untried. Everything claimed above about how the screen
  *looks* was checked with it.
- **NEW: `wallpaper` on the console** — what the loader found, from which filesystem, at what
  pixel size, and why it refused. Plus `wallpaper reload | list | clear` and
  `wallpaper set <name>`, which runs **the same `photosSetWallpaper()` the menu runs**, so the
  whole path can be proven over a cable. A test hook that re-implements the feature proves
  nothing; this one cannot drift.
- ⚠ **Noticed, not fixed:** `/photos/20260823_093939.jpg` on phone 2 is **0 bytes** — a failed
  upload that Photos still lists and cannot open.

## Unreleased (2026-08-24, late) — the scrolling freeze was never the database: it is `WiFi.disconnect(true)`

- 🔑 **`WiFi.disconnect(true)` BLOCKS FOR 5007 ms. `WiFi.begin()` next to it takes 30.** The
  argument is `wifioff`, so that call runs `esp_wifi_stop()` and tears the entire radio down
  purely in order to reassociate:

  ```
  SLOW WIFI: connectToWiFi [disconnect(true)=5007 begin=30 other=0]
  ```

  One task means those five seconds froze the keypad, the screen and the WiFi stack together.
- **This is the freeze {user} kept feeling while scrolling, and the database work never touched
  it.** It explains every part of the report that did not fit: why it was rare (it needs the
  hotspot to blip), why it always came with "and WiFi dropped" (**the drop was not a symptom of
  the freeze — it was the trigger for it**), and why it survived moving the save off the input
  path and then onto the SD card.
- **How it was found, because the guessing had to stop.** The stall detector was made to persist
  into `/health.log`, which showed two distinct populations: `scr=0 cpu=80` (idle — the database
  saves, landing exactly where they were meant to) and `scr=65 cpu=240 wifi=6` (screen on, phone
  active, WiFi already down). That second group pointed at the reconnect path rather than at
  storage. A `TIME_STEP()` macro then named the call, and three successive splits walked it down:

  | | |
  |---|---|
  | `connectToPreferred` | 5296 ms |
  | → `loadPreferred` 0 ms, `connectTo` | **5296 ms** |
  | → `loadNetworkSettings` 7 ms, `connectToWiFi` | **5037 ms** |
  | → `begin` 30 ms, `disconnect(true)` | **5007 ms** |

- **The fix is one argument: `WiFi.disconnect(false)`.** A plain disassociate is all a reconnect
  needs — `WiFi.begin()` sets the new config regardless, so the "delete old config" comment was
  describing work that `begin()` redoes anyway. ⚠ The genuine full cycle
  (`WiFi.disconnect(true, true)`) is untouched where it belongs, in the hard reset.
- **Verified both directions after the change:** a forced drop produces **no `SLOW WIFI`, no
  `SLOW STEP` and no `LOOP STALL`** — and the phone still reconnects, with SIP re-registering on
  its own (`registered: yes | wifi: up`). A fix that removed the freeze by breaking reconnection
  would have been worse than the bug.
- 🛠 **New serial command `wifi drop`** — disconnects WITHOUT marking the radio user-disabled, so
  the field retry path runs on demand. A bug that needs somebody else's access point to
  misbehave cannot be measured otherwise, and this one had already survived two rounds of
  fixing the wrong thing.
- ⚠ **A note on the two earlier rounds, kept honestly:** the database save really was blocking
  for ~1.5 s and really is better on the SD card. It simply was not what {user} was feeling. The
  lesson is the one the stall detector exists to enforce — **the symptom named a component, and
  the component was innocent.**

## Unreleased (2026-08-24, night) — the mesh database moves to the SD card, and the benchmark is why

- 🔑 **BENCHMARKED BEFORE COMMITTING, because the previous recommendation in this same session
  was wrong.** {user} asked me to look hard at whether SD was actually the right choice rather
  than assume it. A new serial command, `bench`, writes 8 KB the same shape a real save has —
  open, write, remove, rename — to both filesystems on this exact phone:

  | | total | open | write 8 KB | remove | rename |
  |---|---|---|---|---|---|
  | **SPIFFS** | 2599–2845 ms | **1645 ms** | 163–809 ms | 62–324 ms | 326–639 ms |
  | **SD** | **48–57 ms** | 10 ms | 11 ms | 15–24 ms | 12 ms |

  **About fifty times faster, and consistent** — no wear-levelling spikes across three passes,
  which was the specific risk that made SD worth measuring instead of assuming. Cheap cards are
  notorious for unpredictable multi-hundred-ms pauses and that would simply have moved the
  problem.
- ⚠ **AND IT SHOWS THE EARLIER DIAGNOSIS WAS AIMED AT THE WRONG OPERATION.** `open` alone costs
  **1.6 seconds** on SPIFFS. The write was never the bottleneck — which is exactly why batching
  the writes did nothing and why chunking them made it worse. **Creating a file is the
  pathological operation on this part**, and the temp-then-rename save hits it every save.
- **Measured end to end, same three-save test, same phone:**

  | | stalls over 250 ms |
  |---|---|
  | chunked write, SPIFFS | 31 |
  | single write, SPIFFS | 12 |
  | **single write, SD** | **5** |

  And the survivors are no longer the save: the largest is 5049 ms at `wifi=6`, i.e. WiFi
  association, which the stall detector will now keep catching on its own merits.
- ⚠ **SPIFFS REMAINS THE FALLBACK AND MUST KEEP WORKING.** `meshFs()` decides per call rather
  than caching a verdict at boot — a card can be removed, and `cardPresent` was itself lying for
  the life of the project until earlier today. `loadDb()`/`loadFavourites()` prefer the card but
  fall back to SPIFFS, so **a database written before this change is still found**, and the next
  save migrates it to the card. Verified on hardware: 34 nodes, 26 messages and 3 starred nodes
  all came back across the move.
- **Channels stay on SPIFFS** — a few hundred bytes, written when a channel is added, which is
  rare enough that the card being absent matters more than the speed.
- The service takes card presence from the main loop (`setCardPresent`) the same way it takes
  `uiIdle`, so it keeps no dependency on the UI.

## Unreleased (2026-08-24, evening) — the scrolling freeze, found and timed: it is the database save

- 🔑 **{user}: "sometimes when scrolling menus, the phone will freeze for a second or two, wifi
  will drop, then it will unfreeze and WiFi comes back up."** Reproduced and timed on the first
  attempt:

  ```
  MESH saveDb: 1712 ms BLOCKING for 5944 bytes (34 nodes, 13 msgs)
  LOOP STALL: 2196 ms in one pass - WiFi/keypad/screen were all frozen for this long
  ```

  **Everything in this firmware is one task**, so a blocking write stops the keypad, the screen
  and the WiFi stack together — which is why the symptom is a freeze *and* a WiFi drop, always
  together, always the same length. The loop's own sms-mirror comment already warned about this
  shape ("not slow, it is the 5-second freeze bug rebuilt on purpose").
- **Why it is only occasional:** `dbDirty` fires on real changes — a new node, a name, a learned
  key — not on every packet, so on a quiet mesh saves are rare. It is only when one lands
  mid-scroll that you feel all 1.5 s of it.
- ⚠ **TWO PLAUSIBLE CAUSES WERE MEASURED AND BOTH WERE WRONG.** Recorded so nobody re-runs them:
  - *"It is the ~55 tiny `f.write()` calls."* Batching them into one PSRAM-buffered writer moved
    1275 ms to about 1000 ms. Real but marginal — the call count was not the bottleneck.
  - *"SPIFFS is nearly full, so garbage collection is thrashing."* It is **2.6% full**
    (87 KB of 3.3 MB). Not fragmentation.

  What is left is the flash itself: **~6.9 KB takes ~1050 ms, about 6 KB/s.** The phase
  breakdown is `[rm_tmp=123 write=1053 rm_old=24 rename=125]`, so the temp-then-rename added for
  crash safety costs ~250 ms of it — real, but not the story.
- **The fix shipped is a deferral, and it is honest about being one: the save now waits until
  nobody is touching the phone** (three seconds after the last key). It does not make the write
  faster — **it makes it land where it cannot be felt.** The data still persists within seconds
  of the phone being put down, and the save is unchanged in every other respect.
- 🛠 **A permanent superloop stall detector ships with it.** Any pass over 250 ms now logs at the
  compiled-in level with the screen, CPU and WiFi state at the time. A freeze this rare cannot be
  caught by watching for it; it has to be caught by the phone. Costs one `millis()` compare per
  pass, and is silent until something real happens.
- ⚠ **I RECOMMENDED CHUNKING THE WRITE ACROSS LOOP PASSES, BUILT IT, MEASURED IT, AND IT WAS
  WORSE.** Recorded in full because the reasoning was plausible and someone will have it again.
  The per-byte model (~6 KB/s, so 128 B ≈ 20 ms a pass) is an **average, and the average is a
  lie here**: SPIFFS writes are quick until one crosses a block boundary and forces an ERASE,
  and an erase is atomic and blocking however small the write that triggered it. More, smaller
  writes means more boundary crossings. Same three-save test, same phone:

  | | stalls over 250 ms |
  |---|---|
  | chunked, 128 B per pass | **31** |
  | single write | **12** |

  **A stall the filesystem takes in one indivisible piece cannot be chunked around.** Reverted
  to a single write, which is at least *one* pause rather than many.
- **What did survive from that work, and is worth keeping:** the image is now snapshotted into
  PSRAM in one pass (memcpy, no I/O) and written from there. That makes the save atomic in a
  second sense — nodes and messages can no longer change halfway through and tear the file —
  and it removed the redundant `SPIFFS.remove()` that logged `/meshdb.tmp does not exists` on
  every single save.
- 🛠 **Stall records now also go to `/health.log`**, rate-limited to one a minute. The detector
  was serial-only, which meant it could only ever catch a freeze while somebody was watching a
  cable — and the freeze it exists to catch happens while the phone is being *used*. Same
  argument as the health line itself.
- [ ] 🔑 **THE REAL FIX IS TO STOP USING SPIFFS FOR THIS, and the evidence now points hard at
  it.** `/health.log` lives on the SD card, appends every minute, and has never once tripped the
  stall detector. Moving the database there is a deliberate piece of work — it needs a SPIFFS
  fallback for a missing card, which is exactly the case the `cardPresent` bug hid for years —
  **but it is the next thing to do, not another attempt to outsmart this filesystem.**
- [ ] **The root cause is untouched and worth returning to.** ~6 KB/s means any future
  full-database write blocks for over a second, and at the message cap that would be minutes.
  Two real options: chunk the write across loop passes (the "one bounded step per pass" idiom
  this codebase already uses for the SMS mirror), or move the database to the SD card, where
  `/health.log` already lives and writes without trouble. **Do not raise `MESH_MSG_CAP` before
  one of those is done.**

## Unreleased (2026-08-24, later) — `chg=` settled: the flag was inverted as well as mis-read

- ✅ **THE POLARITY QUESTION IS ANSWERED, AND IT TOOK FIVE MINUTES OFF THE CHARGER.** The charger
  IC's STAT output is open-drain — it pulls the line DOWN on the charger and releases it (pull-up
  → high) off it. The read was `== HIGH`, so the flag has been **inverted for the life of the
  project**, on top of reading the wrong GPIO entirely until earlier today, which is what hid it.
- **Measured, one continuous boot, one build stamp, no confound:**

  ```
  up=20  v=4.20  chg=0     on the charger
  up=21  v=4.15  chg=1     <- unplugged; voltage starts falling
  up=25  v=4.11  chg=1     still on battery
  up=26  v=4.18  chg=0     <- replugged; voltage jumps back
  ```

  `chg=1` tracked **not charging** throughout, and the voltage curve says so independently of the
  flag. This is exactly the run that could not be done overnight, and the build stamp added hours
  earlier is what makes it attributable rather than arguable.
- ⚠ **The overnight caution was right to hold, and so was refusing to act on it.** A 135-sample
  run last night looked like proof of this same inversion, and it could not be separated from the
  pre-fix confound where the flag was reading a UART TX line that idles high. It turns out to have
  been real — **but it was not knowable then**, and the correct call was to record both readings
  and go get a measurement that admits only one.
- ⚠ **WHAT IS STILL NOT ESTABLISHED:** whether the line reports *charging* or merely *charger
  present*. Post-fix, plugged in at soc=100% and 4.19 V, it still reads `chg=1` — which is either
  a charger still topping off, or a pin that tracks USB presence rather than charge current.
  **`chg=1` may safely be read as "on the charger". Do not yet read it as "current is flowing".**
  Settling that needs a run left plugged until charging genuinely terminates.
- **Also:** the Nodes screen title now reads `Nodes  *=star`. Starring worked from the day it
  shipped and was still undiscoverable — the footer's two slots are the OK action and Back, and
  `*` is neither, so nothing anywhere told you the key existed.

## Unreleased (2026-08-24, afternoon) — the SIP flap, the starvation under it, and two loose ends closed

- 🔑 **SIP registration no longer flaps, and the reason the last attempt failed was never the
  flap fix.** Three changes; the important one is not the obvious one.
  1. **Ping starvation.** The caller is an if/else chain — `if (ping due) ping(); else if
     (register due) registration();` — and `ping()` wrote `msLastPing` only INSIDE its
     `connected()` branch. With the connection down the stamp never advanced, the ping branch
     was taken on *every pass*, and **`registration()` was never reached again**. A phone that
     lost its connection could not re-register itself; only a reboot recovered it. ⚠ **That is
     the "went `lost` and never came back" of 2026-08-22, and it is reachable on its own — it
     has been there since the initial commit.** `ping()` stamps the ATTEMPT now.
  2. **The flap itself:** `requestRegister()` cleared `registered` on every REGISTER, so the
     phone declared itself unregistered for one round trip per refresh, by construction. A
     failed refresh is still covered — `registrationInvalid()` measures from `msLastRegistered`,
     which only a 200 OK advances.
  3. **`REGISTER_PERIOD_MS` 60000 → 45000.** ⚠ Neither 2 nor 3 works alone: with period equal
     to expiry the expiry check tripped at the same instant the refresh fell due, which is why
     the 45 s attempt on its own only moved the flap. The on-air `Expires` header is unchanged,
     so a dead phone's binding still clears at the registrar within 60 s.
- **Measured over 4 minutes on the hotspot: 6 REGISTERs, 1 REGISTERED transition, ZERO `lost`
  events** — against 53 flaps in 31 minutes before. And the starvation fix got an unplanned live
  test: WiFi genuinely dropped after a reset and **registration recovered on its own**, which is
  precisely what used to need a reboot.
  ⚠ **Still owed: confirmation that an INBOUND call lands.** That cannot be proven from the cable.
- ⭐ **The node order is now frozen between rebuilds, and that is a correctness fix, not a
  preference.** `getNode(index)` re-sorted on demand while `index` is a row number the UI got
  from a list it drew earlier — and the Nodes screen uses it for hit-testing as well as drawing.
  A node heard between the draw and the key press silently renumbered the rows, so `*` would
  star (or OK would DM) **the node next to the highlighted one**. COVEY had the identical bug in
  `_activate_at` and it was fixed the same day. Only a size change forces a rebuild now; the UI
  re-sorts explicitly in `buildNodes()`, which is the one moment the numbering may change.
- 🛠 **`/health.log` boot lines carry a build stamp** (`build=Aug 24 2026 07:51:57`). The log
  recorded only `up=` minutes — no wall clock, nothing saying which firmware wrote a sample — and
  that gap is the sole reason the `chg=` polarity question could not be settled from four hours
  of data: a long run of `chg=1` tracked discharging perfectly, and there was no way to tell
  whether those samples predated the fix, when the flag was reading a UART TX line that idles
  high. Every build is distinguishable now, so a run of samples can be attributed.

## Unreleased (2026-08-24, midday) — the node list grows 6x and learns to be starred

- **`MESH_MAX_NODES` 32 → 200, and the table moved to PSRAM.** It was a plain member array on a
  global, so it lived in BSS — **the internal RAM that actually panics this phone** — at 84 bytes
  a node. `ps_malloc`'d now, the same treatment `messages` already had. Measured on hardware:
  free internal heap at boot went **133,816 → 136,232**, so the bigger table costs *less* real
  RAM than the small one did.
- **Why 200 and not more:** PSRAM is not the constraint (3.6 MB free ≈ 44,000 nodes) and neither
  is the database — the message store can already take that file to 242 KB against the node
  table's 16 KB. The binding constraint is a person scrolling the list, which is what starring
  answers. Saves are debounced and fire only on a real change, so a bigger table is not
  proportionally more flash writes.
- ⭐ **Starred nodes.** Press `*` on the Nodes list. They sort to the top, and **they are the last
  thing evicted** — eviction now has three tiers (plain stranger → keyed peer → starred), each
  only touched when every lesser tier is empty. On a public LongFast mesh full of strangers,
  that second half is the one that matters: it is the same failure that silently killed DMs to
  COVEY, made impossible for the nodes {user} actually cares about.
- 🔑 **The starred IDs live in their OWN small file (`/meshfav.bin`), not just as a bit in the
  database.** {user} asked whether they could live somewhere a power cut cannot reach. The reason
  to do it is better than durability alone: **the node table is machine data — every entry is
  rediscoverable by listening to the mesh. The stars are the one thing in there that is not.**
  So they get their own file, written only when you press `*`, temp-then-renamed, instead of
  riding inside a 250 KB blob rewritten whenever any node is heard.
  It also makes a star **survive eviction**: the list is the truth and the flag on `MeshNode` is
  a cache re-applied whenever a node is created, so a starred node pushed out of a full table
  comes back starred — and immediately eviction-proof again — the moment it is heard.
  ⚠ **SPIFFS, deliberately not the SD card**: 4 bytes per star makes size irrelevant, and SPIFFS
  is always mounted whereas a card can be absent.
- **New serial command `star`** — bare it lists, with a node it toggles. Same reason `send`
  exists: a feature reachable only by a thumb on a screen cannot be proven from the cable.
- 🔴 **THIS SHIPPED A BOOT LOOP FIRST, AND {user} FOUND IT BEFORE I DID.** The constructor still had
  `memset(nodes, 0, sizeof(nodes))` from when `nodes` was a fixed array — where `sizeof` gave the
  whole 2,688 bytes. Against a pointer `sizeof(nodes)` is **4**, and at construction time it is
  still NULL, so this was `memset(NULL, 0, 4)`: **StoreProhibited inside a global constructor**,
  pa{user}ing before `setup()` ever ran. It compiles without a warning. Decoded from the backtrace
  with `addr2line`, which named the constructor in one pass after guessing had named nothing.
- ⚠ **The process failure is the more useful lesson: I flashed and moved on without reading the
  boot log.** The verification I *did* run filtered serial for expected strings, got nothing, and
  I read "no matches" instead of "no readable output at all" — which is itself the boot-loop
  signature, because the ROM bootloader talks at **115200** while that capture listened at
  500000. **A flash is not finished until something confirms `Booted` and the absence of `Guru`.**
  The check now does exactly that, and reports the readable fraction so garbage cannot look like
  silence.

## Unreleased (2026-08-24, overnight) — `saveDb()` truncated the database in place, and a night of resets proved it

- 🔴 **`saveDb()` OPENED THE REAL FILE WITH `"w"` — TRUNCATE, THEN REWRITE IN PLACE.** Anything
  that stopped the phone mid-write left a **short file**, and `loadDb()` `break`s out of its read
  loops on a short read and silently keeps whatever it got. Nodes are written first so they
  survive intact; messages come next and get cut off; waypoints are written **last** and go
  first. Nothing anywhere reports a problem — the file still has a valid magic and version.
- ⚠ **NOT THEORETICAL, AND I CAUSED IT.** A night of flashing and serial work took the message
  store from **55 messages to 4**, and the database still loaded cleanly with all 32 nodes.
  **Every serial port open resets this board**, and a save runs off a dirty flag, so the window
  is open far more often than "during a manual save". Traced by the `MESH DB: N nodes, M
  messages` boot line across nine captures: 43 → 43 → 49 → 54 → 54 → 54 → 55 → **4** → 5.
- **Fixed with the pattern this codebase already uses elsewhere:** write to `/meshdb.tmp`, then
  rename over `/meshdb.bin`. Rename on SPIFFS is atomic, so a reset now leaves either the old
  complete database or the new complete one, never half of either. `healthTrim()` has done this
  all along for the same reason; `saveDb()` simply never adopted it. If the rename fails the
  existing database is left untouched and it says so.
- **Verified by abusing it deliberately:** three sends across three reset cycles counted up
  6 → 7 → 8 → 9, then four bare reset cycles held at **9, 9, 9, 9**. Before the fix the same
  treatment is what collapsed it.
- ⚠ **The lost messages are lost** — roughly 50 of {user}'s stored mesh messages. The node table
  (32, with keys) survived because of the write order. **Waypoints currently read as none**, and
  I cannot say whether any were set beforehand: I never captured a `pos` baseline, and COVEY has
  no `~/.covey/waypoints.json` either, so there is no second copy to compare against.
- ⚠ **A correction to my own entry above**, which said this class of loss would take *"Camp"*.
  There is no evidence Camp was ever set on this phone — that name comes from a **code comment**
  explaining why waypoints are persisted, and I wrote it up as though it were observed data. It
  is now phrased as "any stored waypoints", which is what is actually known.
- [ ] **Worth considering:** `loadDb()` treats a short read as "that is all there was" and
  carries on. That is what made this silent. A record count that does not match the header is a
  fact worth logging even when the file is otherwise usable.

## Unreleased (2026-08-24, later) — DMs to COVEY were silently failing, because a stranger evicted its encryption key

- 🔑 **THE NODE TABLE THREW AWAY COVEY'S PUBLIC KEY TO MAKE ROOM FOR A STRANGER, AND EVERY DM
  TO IT HAD BEEN FAILING SILENTLY EVER SINCE.** `upsertNode()` evicted strictly oldest-heard
  at the 32-node cap, and the slot wipe (rightly) takes `pubKey`/`pkiFlags` with it. On
  LongFast — a public channel full of nodes we will never DM — COVEY goes quiet, loses its
  slot to the 32nd stranger, and comes back keyless the next time it speaks. Keyless means
  DMs fall back to the pre-2.5 legacy form, which COVEY refuses to decrypt.
- **Measured, and the diagnosis is airtight:** the phone held COVEY as a bare `'!62b8d2fd'`
  with NO KEY, while the docs recorded it as `'{user} H New Device'` key `HepOEI6R…`. After
  recovery it reads `'{user} H New Device' key HepOEI6RPnBxEmLqSb8gezCVaVd0qO+3JYo1Ky2+fCM=` —
  and `HepOEI6R` base64-decodes to `1d ea 4e 10 …`, exactly the `learned key for !62b8d2fd
  (1dea4e10...)` the phone logged on relearning it. **The key never rotated. Eviction simply
  binned it.**
- **A node whose public key we know is now evicted LAST.** The scan prefers the oldest
  *keyless* node and only falls back to oldest-overall when every peer is keyed — which
  cannot deadlock and matches the old behaviour exactly on a table full of keyed peers. A
  peer we can do end-to-end crypto with is worth more than the 32nd stranger heard, always.
  Evicting a keyed peer now logs, because it should be rare enough to notice.
- ⚠ **This was found by the delivery receipt shipped hours earlier**, which is the entire
  argument for that feature. The failure was completely silent before: the message appeared
  in the thread and looked sent. The receipt said `failed: they could not decrypt it - key
  mismatch (err=6)` and pointed straight at it.
- **Recovery, and it is worth writing down: restarting `covey-ui` makes COVEY re-announce.**
  The phone's own `announce` (NodeInfo with want_response) drew replies from two strangers
  across three minutes but never from COVEY — `nbr` shows **0 direct neighbours**, so COVEY is
  multi-hop and its replies were not surviving the trip, even though routed NAKs were. A
  `sudo systemctl restart covey-ui` on the Pi triggered its NodeInfo immediately and the phone
  logged `MESH PKI: learned key for !62b8d2fd - DMs unlocked`. No config was written and no
  key was injected by hand; trust-on-first-use is intact.
- ✅ **BOTH RECEIPT BRANCHES ARE NOW PROVEN ON AIR.**

  ```
  dm: sent to !62b8d2fd (PKI)
  MESH DM ACK from !62b8d2fd for id=0x056414bc err=0
  MESH RECEIPT: 'hello from the wiphone' -> delivered
  ```

  together with the earlier failure case (`err=6` → `failed: they could not decrypt it`).
- ✅ **AND `in mesh` IS PROVEN TOO — all three states are now measured, none inferred.** A new
  serial command closed the gap: `send <chan-index> <text>` sends a channel text from the
  cable, which `dm` could not do, and which is why "in mesh" shipped unproven a few hours
  earlier.

  ```
  send: sent on [1] 'Howe group'
  MESH RECEIPT: 'goodnight from the wiphone' -> in mesh
  ```

  Because the acknowledgement is implicit, that line is also proof that something out there
  relayed the packet — the receipt and the reachability check are the same observation.
- **Also fixed, same family as the extender-pin bug:** `setup()` called plain `pinMode()` on
  `BATTERY_CHARGING_STATUS_PIN` (`EXTENDER_PIN(0)` == 64) in both board branches — configuring
  a pin the ESP32 does not have, so those lines did nothing. Benign, because the real
  configuration happens correctly through `allPinMode()` in the extender block earlier in
  setup — but **a line that looks like it sets up the charge-status input and does not is
  exactly how the sibling bug in the battery tick survived so long.** An audit of every raw
  `digitalRead`/`digitalWrite`/`pinMode` call against the extender-pin list found no others:
  the remaining ones are all genuine ESP32 GPIOs.

## Unreleased (2026-08-24) — delivery receipts on mesh messages, and the WiFi row moves into Settings

- **The WiFi toggle lives in Settings now**, leading the WiFi group, above "Edit current
  network" / "WiFi auto-switch" / "Scan WiFi networks" — the three rows that all assume a
  radio which is on. It spent a few hours on the main menu on 08-23 and {user} moved it. The
  change is one table line: both the runtime label builder and the dispatch key off `action`
  and never look at the parent, so the row can be re-homed freely. Verified after flashing —
  `GUI::init()`'s boot check reported zero duplicate menu IDs.
- 🔑 **Outgoing mesh messages now carry a delivery receipt**, shown on the line that already
  said "me" in a thread: **`me - sent`**, **`me - delivered`**, **`me - in mesh`**,
  **`me - failed`**.
- ⚠ **THEY ARE DELIVERY RECEIPTS, NOT READ RECEIPTS, AND THE WORDING IS DELIBERATE.**
  Meshtastic has no concept of a human having read anything — the strongest signal in the
  protocol is that a *radio* acknowledged the packet. COVEY's UI says the same thing for the
  same reason. Anything labelled "read" would be a claim the mesh cannot support.
- ⚠ **`in mesh` and `delivered` are not the same claim.** A DM is acknowledged by the
  destination node itself, so *delivered* means delivered. A broadcast is acknowledged
  **implicitly** — hearing your own packet rebroadcast is the acknowledgement — which proves
  the message entered the mesh and **nothing whatever about who received it**. Two words,
  because one would have been a lie half the time.
- **Almost none of this was new machinery.** The stack already set `want_ack` on PKI DMs,
  already parsed `request_id` off inbound ROUTING packets, and already logged
  `MESH DM ACK ... err=0` per message. `meshtastic_service.cpp:1570` even called it *"the
  only proof of delivery this phone gets"* — and then dropped it on the floor. This wires
  that existing signal to the message store and the UI.
- 🔑 **THE RECEIPT LIVES IN SPARE BITS OF `MeshMessage.flags`, AND THAT IS THE WHOLE DESIGN.**
  The obvious implementation adds a `uint32_t packetId` to `MeshMessage` — and `saveDb()`
  writes `sizeof(MeshMessage)` into the header while `loadDb()` **rejects the entire file** on
  a mismatch: nodes, messages and waypoints together. That would have silently binned the
  message history, the node list, and any stored waypoints on the first boot after the update. Measured on the real phone after
  flashing: **"MESH DB: 32 nodes, 49 messages stored"** — all still there. The packet-id
  association instead lives in an 8-entry RAM table for the seconds between sending and the
  ack; the resolved state persists in `flags`, exactly like `MESH_MSG_READ`.
- ⚠ **That table is keyed on `timeMs`, NOT on the message's index.** Indices are not stable —
  the store compacts on `removeMessageAt()`, so the per-chat and global caps can shift every
  message down while an ack is still in flight, and the receipt would land on a stranger's
  message. Pending entries do not survive a reboot, which is honest: an ack for a message
  sent before a restart can no longer be attributed to it.
- ⚠ **A DM MUST NOT TAKE THE IMPLICIT ACK, and this was caught before it shipped.** DMs flood
  too, so we hear our own DMs rebroadcast by neighbours exactly like broadcasts. Treating that
  as delivery would mark a DM "delivered" the moment *any* node relayed it — saying nothing
  about whether the person it was addressed to ever got it. The implicit path is gated on
  `hdr.dest == MESH_ADDR_BROADCAST_ONAIR`; a DM waits for the destination's own routing ACK.
- **Broadcasts deliberately do NOT ask for an ack, and this costs nothing on the air.** Flood
  routing rebroadcasts any packet with hops left whether or not `want_ack` is set, so the bit
  would add no information and risks stock answering with a routing packet nobody needed. The
  first implementation did set it; measuring what the implicit ack actually is removed the
  cost entirely. `want_ack` remains on DMs only, where it produces a real end-to-end reply.
- ⚠ **`meshTxData()` takes `wantAck` defaulted OFF and it must stay that way.** That function
  also carries NodeInfo, position, neighbour info and **our own outgoing routing ACKs** —
  asking for an ack on an ack is a loop. Only the text path opts in.
- **A queued DM that never reaches the air now says so.** DMs whose AES key is not cached are
  echoed into the thread at queue time and transmitted a tick later; both failure paths
  (`queued PKI send FAILED`, `key derive failed`) previously logged and left the message
  looking exactly like one that went out fine. They mark it failed now — which is why the
  pending-DM record carries its message's `timeMs` across the tick.
- **Words, not ticks, for a second reason:** the Akrobat/OpenSans faces are generated bitmap
  glyphs and **nothing anywhere in this firmware renders a non-ASCII character**. A checkmark
  would come out as a box, which is worse than no marker at all — COVEY hit exactly this and
  had to draw its ticks by hand.
- ✅ **PROVEN ON AIR — the failure path, twice, end to end.** A DM to COVEY produced the whole
  chain in the serial log:

  ```
  MESH DM to !62b8d2fd sent LEGACY (no key known) - a 2.5+ node will drop it
  MESH DM NAK from !62b8d2fd for id=0x9adaa67b err=6
  MESH RECEIPT: 'receipt test' -> failed: they could not decrypt it - key mismatch
                                          (legacy DM to a 2.5+ node?) (err=6)
  ```

  Send → packet id tracked → inbound ROUTING matched → receipt stamped on the right message.
  (The phone's `0 direct neighbours` reading beforehand was an empty table after a reboot, not
  silence — COVEY was there the whole time and answered.)
- ⚠ **The `delivered` and `in mesh` branches are NOT yet proven.** They are the same code path
  reached with `err == 0`, and the transport underneath them is exactly what the NAK above
  travelled on, so the risk is small — but "highly likely" is not "measured" and it should be
  watched rather than assumed. `announce` did not draw a NodeInfo out of COVEY inside 40 s, so
  its public key is still unknown and every DM keeps falling back to the legacy form.
- 🔑 **AND THE FEATURE IMMEDIATELY EARNED ITSELF: DMs from this phone are NOT REACHING COVEY
  AT ALL right now.** `pki` shows no key for `!62b8d2fd`, so they go out in the pre-2.5 legacy
  form, which COVEY refuses to decrypt (`err=6`). That has presumably been true for a while and
  was invisible — the message appeared in the thread and looked sent. **This is exactly the
  class of silent failure the receipt exists to surface**, found within minutes of it existing.
  The fix is hearing COVEY's NodeInfo; `pki` names who has keys.
- **A plain-English reason accompanies every failure in the log**, from the same Routing.Error
  table COVEY carries, with the raw code kept for searching. "err=6" tells nobody in a tree
  stand whether to move, wait, or re-send. The small screen keeps the short word (`me - failed`)
  and the log carries the sentence.
- **Built, flashed, boots clean, menu table verified (zero duplicate IDs), 1167 host
  assertions green.**

## Unreleased (2026-08-23, night) — two status flags were reading the wrong silicon, and one of them is the dead `chg=`

- 🔑 **`cardPresent` and `battCharged` were read with plain `digitalRead()` on GPIO-EXTENDER
  pins, which does not reach the extender at all.** Both live on the SX1509:
  `TF_CARD_DETECT_PIN` is `EXTENDER_PIN(1)` == **65** and `BATTERY_CHARGING_STATUS_PIN` is
  `EXTENDER_PIN(0)` == **64** (`EXTENDER_FLAG` is `0x40`). The ESP32 has GPIO 0–39, so both
  numbers are out of range — **and `digitalRead()` does not reject them.** It calls
  `gpio_get_level()`, which for `pin >= 32` evaluates `(in1.data >> (pin - 32)) & 1`; the
  Xtensa shift masks its amount to 5 bits, so `>> 32` becomes `>> 0` and `>> 33` becomes
  `>> 1`. The two flags were therefore reading:

  ```
  battCharged  <-  GPIO 32   (USER_SERIAL_TX / MotorEN)
  cardPresent  <-  GPIO 33   (I2S_WS_PIN, the I2S word-select clock)
  ```

- ✅ **THIS IS WHY `chg=` IS DEAD IN EVERY HEALTH SAMPLE.** It was filed as "its own small
  mystery" and then promoted to a measured fact — 0 across all 807 samples of the 3.0 h run,
  straight through two unmistakable charging periods. **The flag was never reading the
  charger. It was reading an idle-low pin on the wrong die.** `allDigitalRead()` — the
  accessor that honours `EXTENDER_FLAG` — has been sitting in `Hardware.cpp` the whole time,
  and every other extender read in the file already used it.
- ⚠ **`cardPresent` was reading the I2S word-select line, which is idle-low — so it reported
  TRUE whether or not a card was seated, and would FLICKER while audio plays**, since WS
  toggles at the sample rate and the tick samples it at an arbitrary phase. A card-dependent
  path could fail intermittently during music for reasons nothing else could explain. Not
  observed in the wild; recorded because it is now closed rather than waiting.
- **`cardPresent` is also seeded once in `setup()` beside `SD.begin()`.** It is declared
  `false` in `GUI.h` and was assigned in exactly one place — the battery tick — so for up to
  15 s after every boot the phone believed it had no card, and `healthDump()` bails on
  `!cardPresent`. That is what made `health` answer *"nothing to read - no card, or no
  /health.log yet"* **with a perfectly good card seated**, in exactly the window in which
  someone plugs a phone into the cable and asks it what happened.
- **Verified on hardware after flashing** (app partition only, 230400 baud, hash verified):
  `health` issued **11.3 s after boot** returned 25,745 bytes and 191 samples, where the same
  call before the fix returned the no-card message. That also proves `allDigitalRead()` on the
  extender works — a garbage read would have failed the dump.
- ⚠ **`chg=` STILL READS 0, AND THAT IS NOT YET RESOLVED.** With the phone on USB at soc=96%
  and terminal voltage *rising* (4.14 → 4.15 → 4.17 across three ticks), the flag stayed 0.
  Two readings fit and this session cannot separate them: the pack is nearly full and the
  charger has legitimately tapered off, **or the polarity is inverted** — charge-status
  outputs on this class of charger IC are typically open-drain and **active LOW while
  charging**, which would make `== HIGH ? true : false` exactly backwards. **Settle it on a
  run that starts from a low pack**, where "charging" is unambiguous, before trusting or
  re-inverting the flag. The read now reaches the right chip; the meaning of what it reads is
  the open half.
- Also fixed: a `log_d` that called `gpioExtender.digitalRead(POWER_CHECK)` without masking
  `EXTENDER_FLAG` (the working call at the top of `loop()` does mask it), so it queried
  extender pin 66 and logged nonsense. Cosmetic — `log_d` is not compiled into this build.

## Unreleased (2026-08-23, evening) — the idle floor is real, and the first 20 minutes off a charger are a lie

- **Idle drain, WiFi OFF, screen off, 80 MHz: ~8.6 %/h — about 1.8x better than the
  15.7 %/h mixed-use figure.** One hour on phone 1, 62 once-a-minute samples recovered
  from `/health.log` over the cable (`health all`). Off-charger window is unambiguous in
  the log: terminal voltage steps 4.19 → 4.14 at the unplug (up=32) and 4.02 → 4.10 at
  the replug (up=95), with `wifi=0 cpu=80 scr=0` on every sample in between.
- **⚠ THE ENDPOINT READ SAYS 15.74 %/h AND IT IS WRONG.** Taken start-to-finish the hour
  reads 100% → 84% = 15.74 %/h, which is *identical* to the mixed-use run and would have
  supported the flatly false headline "turning WiFi off saves nothing". Splitting the
  window into 20-minute blocks kills that reading:

  | block | SoC | rate | voltage slope |
  |---|---|---|---|
  | up 33–52 | 100 → 92% | **25.3 %/h** | −188 mV/h |
  | up 53–72 | 92 → 88% | 12.6 %/h | −41 mV/h |
  | up 73–94 | 87 → 84% | **8.6 %/h** | −40 mV/h |

  The first block is **post-charge surface-charge relaxation**, not current draw — a
  freshly-unplugged cell sheds its surface charge fast whatever the load. The last two
  blocks agree with each other (−41 and −40 mV/h) and that agreement is what makes them
  the real number. **Do not start an idle-drain measurement at the instant of unplugging;
  discard the first ~20 minutes.**
- The same artefact inflates the 3.0 h mixed-use run that produced ~6.4 h, though a
  20-minute contamination is diluted over three hours rather than dominating one. **~6.4 h
  is therefore mildly pessimistic**, and the two runs are less far apart than the raw
  numbers suggest. Neither is worth restating until a run starts an hour after unplugging.
- 🔑 **AND THE RETIRED ~10 h FIGURE WAS MEASURED CORRECTLY ALL ALONG — it is the 15.7 %/h that
  is the outlier.** `docs/HANDOFF.md:2142` records how it was taken: *"steady state with the
  first 30 minutes discarded (surface charge): 94% → 82% over 1.20 h ⇒ 10.0 %/h"*. **That is the
  same correction rediscovered from scratch tonight**, applied by whoever took that reading and
  then forgotten. Line the three up:

  | run | method | rate |
  |---|---|---|
  | original "~10 h" | steady state, first 30 min discarded | **10.0 %/h** |
  | tonight, WiFi OFF | steady state, first 20 min discarded | **8.6 %/h** |
  | 08-23 mixed use | endpoint-to-endpoint, **nothing discarded**, WiFi hunting 86% of it | 15.7 %/h |

  **The two corrected runs agree with each other to within 1.4 %/h across different builds and
  months; the uncorrected one is nearly double both.** So "10 h is unproven from here on, do not
  requote it" — written in this changelog on 08-23 and propagated into the memories — was an
  **overcorrection against the better-measured number**. The 15.7 %/h run is contaminated twice
  over: no relaxation discard, and the radio hunting for 86% of it.
- ⚠ **What that does NOT license:** the runs are not equivalent — different firmware, different
  months, different radio states, and one is 1.2 h against another's 21 min. **Neither is a
  full-discharge measurement, and nothing here has watched a pack go from full to empty.**
  Quote a rate with its conditions attached, and stop converting either into an hours figure
  until a run starts an hour after unplugging and continues into the flat part of the curve.
- **Confidence, stated honestly:** the gauge quantises to 1%, so three counts across a
  21-minute block carries roughly ±3 %/h; the voltage slope is the firmer half of the
  result. The 11.7 h that 8.6 %/h projects to is a *top-of-curve extrapolation* — the
  4.02–4.05 V band is nearly the flattest part of the Li-ion curve and the back half of a
  discharge will not be this kind. Quote the rate, not the projection.
- **⚠ Found while fetching this: `health` lies for the first minute after every boot.**
  `healthDump()` bails on `!gui.state.cardPresent` (WiPhone.ino:1786), but `cardPresent`
  is initialised `false` (GUI.h:318) and only ever assigned by the once-a-minute battery
  poll (WiPhone.ino:2727). Run `health` inside that first minute — exactly what you do
  after plugging in a phone — and it reports `nothing to read - no card, or no
  /health.log yet` **with a perfectly good card seated**. It reported precisely that here,
  and the log it was denying turned out to hold all 926 samples. Waiting 80 s and asking
  again returned the whole file. NOT FIXED: the card-detect read wants doing once in
  `setup()` beside `SD.begin()`, not only on the minute tick.

## Unreleased (2026-08-23) — the battery number gets measured instead of claimed, and the radio gets a switch

- **~6.4 h on battery, not the ~10 h the handoff had been claiming since the power
  work.** Measured over 3.00 h off USB: 99% → 52%, 4.13 → 3.80 V = 15.7 %/h. The first
  sample ever taken off the cable, and it is a third short of the figure it replaces.
  **10 h is unproven from here on.** Ruled out by the same samples: not the screen (off
  for 177 of 181) and not the CPU governor (177 of 181 at 80 MHz). The draw is radios
  plus baseline, in a pocket, doing nothing.
- ⚠ **Leading suspect, explicitly NOT proven:** 155 of 181 samples were `wifi=1`
  (WL_NO_SSID_AVAIL) — away from any known network, hunting, and the auto-switch scans
  every 2 min while disconnected against 10 min while connected. The cost could not be
  separated honestly: voltage slope is not linear in SoC, so comparing a hunting block at
  3.9 V against a connected block at 4.1 V measures chemistry, not radio. **Do not change
  the scan interval before measuring it.**
- ✅ **`chg=` is definitively dead** — 0 across all 807 samples, including two unmistakable
  charging periods (89→100%, and 53→86% on a drive home) with voltage rising through both.
  Previously filed as "its own small mystery"; now a measured fact. **Nothing may infer
  charge state from that flag.**
- **New: `health` / `health all`** stream `/health.log` to UART from a 256-byte stack
  buffer — no WiFi, no sockets, no heap. ⚠ The obvious route, `http://wiphone.local/log`,
  is the dangerous one: it needs the uploader up, and heap at the moment of reading was
  free=11448 largest=10560 min-ever=196 — below the ~16 KB at which an 11 KB allocation
  has already aborted the WiFi PHY and rebooted this phone. **The log exists to explain
  restarts; fetching it must not cause one.** ⚠ It was also one boot from being
  half-eaten — 98,453 bytes against a 96 KB cap, so the next boot line would have trimmed
  it to the newest 32 KB and silently taken the start of the run with it. Cap raised
  BEFORE flashing (`HEALTH_LOG_MAX` 96 K → 256 K, `KEEP` 32 K → 128 K, ~16 h) which is the
  only reason the run survived.
- **WiFi on/off is one press from the main menu, and the row says which it is.** The
  switch already existed and was real, but lived inside a *network's edit form* — menu,
  WiFi, pick a network, edit, find the widget — and told you nothing about the current
  state until you got there. Now the main menu reads `WiFi: on` / `WiFi: off` and one
  press flips it. This is a battery feature, not a convenience: the experiment that would
  settle the hunting question needs the radio switched off quickly and repeatably.
  ⚠ **Deliberately NOT persisted** — WiFi returns ON after a reboot, because a radio that
  stays off across a power cycle is a setting you can forget you set, and the failure mode
  is a phone that silently never connects again. ⚠ Menu ID 47 (0–46 taken; 8 and 25 are
  gaps and the rule is COUNT UP, not fill). A duplicate id is SILENT — `findMenu` matches
  on id alone, first hit wins — and has shipped twice; `GUI::init()`'s boot check is the
  only thing that catches it.
- **The wiring sheet is audited geometrically rather than by eye**, before being shared.
  🔑 The one {user} spotted: 143 px of W20 (VBAT) and W11 (GND) drawn on the same x=740
  lane, y 1072–1215 — GND is stroke-3 over VBAT's 2.6, so VBAT vanished underneath it and
  appeared to *stop* at the junction when it actually carries on to the OR node at y=1240.
  Four more collisions found the same way. The audit is three kept scripts that decompose
  every line into segments and report collinear overlap, with text boxes measured from a
  real renderer's `getBBox()` because estimating glyph widths was not accurate enough to
  trust. A v1→v2 change sheet ships with it so the plate does not have to be re-checked
  wire by wire.

## Unreleased (2026-08-22, late) — the missed keypresses, root-caused: every release the phone ever saw went to the wrong key

- **THE BUG WAS PRESENT SINCE THE FIRST COMMIT.** {user}: menu scrolling "misses some
  inputs but not much", and — new, and true all along — "tentap typing, sometimes it will
  miss a tap and I have to tap again while scrolling through letter choices."
- **Found by measurement, not by reading.** A raw event trace (`keys raw`, added for the
  purpose) said what the chip actually sends:

  ```
  +0ms    0x41 P    DOWN pressed
  +141ms  0x00 r    ...and the release comes back as key code ZERO
  +1047ms 0x54 P    OK pressed
  +115ms  0x00 r    ...zero again
  ```

  **Key code 0 is CALL in this keymap, so every release this phone has ever seen was
  applied to CALL.** The key actually pressed never had its bit cleared, stayed "held",
  and was therefore deaf to its own next press until the 350 ms stale sweep let go. A
  person re-taps in 150–250 ms — inside that window. It produced double presses too: with
  `keyLastUpMs` stamped for CALL instead of the real key, a real contact bounce (a
  re-press 6 ms after a release, visible in that same trace) met a bounce filter watching
  the wrong key. The vendor code half-knew — `newState < keypadState` is commented "Some
  buttons were released silently" — but that re-armed `keypadState` only.
- **A release carrying code 0 is now attributed to the key that is actually down.**
  Interrupt-driven reads only: an empty FIFO reads back as 0x00 as well, and an interrupt
  is the only thing proving an event happened at all. The UI press edge is `uiKeyDown`
  ALONE — it had been nested inside the `keypadState` edge, making `keypadState` a second
  undocumented veto, so a lost release left the key deaf even after a sweep that cleared
  only `uiKeyDown`. `SN7326::readReg()` also stops discarding a byte it successfully read
  (it was returning the status of a trailing zero-length write, so any NAK there reported
  failure for a key already off the wire).
- ⚠ **THE FIX SHIPPED A REGRESSION, AND A SOURCE REVIEW CAUGHT IT — NO TRACE COULD HAVE.**
  Attribution used `mask = held`, which may carry several bits, and the release branch
  handles a multi-bit mask — that was the bug. Release one of two held keys and BOTH were
  marked up, so the key still under a finger left `uiKeyDown` and its next ~109 ms
  heartbeat arrived looking like a **brand-new press nobody made**. Reachable three ways:
  the Select+Back sleep chord (lift one finger early and the other key fires — a spurious
  Back navigates if the chord had not yet reached SLEEP_CHORD_MS), rolling two-key typing
  (duplicate letter), and the Game Boy (releasing A while holding RIGHT drops RIGHT out of
  `keypadState` for ~109 ms, and `app_gbc.cpp` reads that mask directly). **Attribution now
  requires EXACTLY ONE key down** (`(held & (held - 1)) == 0`); otherwise the byte stays
  decoded as CALL exactly as it always was and the sweep tidies up — no worse than what it
  replaced, and far better than inventing a keypress. Counted as `relamb` so the ambiguous
  case is visible rather than assumed rare. **It needs two keys at once, and every trace so
  far was single-key.**
- **The held-key heartbeat is ~109 ms, not the 40 ms `SN7326.h` claims** — and a bug got
  built on the 40. {user}: "push okay then push the star key to unlock, it immediately thinks
  I'm trying to type the star key in the dialer." The stale-hold rescue used a 100 ms
  threshold taken from that 40 ms comment, *below* the real interval, so every heartbeat of
  a slightly-long press was promoted into a second keypress: the unlock swallowed the
  genuine `*` and the heartbeat went to the dialer. The rescue now counts and does nothing —
  with code-0 releases attributed correctly, releases stop going missing (`swept` sits at 0),
  so it had nothing left to save and one clear way to do harm. ⚠ **Anything keyed on a
  held-key gap must sit well above 109 ms with jitter room and below the 350 ms sweep.
  Re-measure before trusting any number here; the datasheet comment in the driver is wrong.**
- ⚠ **A wrong measurement of my own, corrected in the tree rather than quietly dropped:**
  earlier the same day I recorded that there are *no* held-key heartbeats at all, because an
  early trace showed nothing between a press and its release. Short taps (110–140 ms) simply
  end before the first re-report. There are heartbeats; they are just slower than the driver
  claims.
- **`KEY_BOUNCE_MS` 40 → 25.** This window had never actually been tested: while releases
  were going to CALL it was armed on the wrong key, so attributing them correctly armed it
  for the first time — an untested 40 ms sitting directly in the path of fast same-key
  tapping, which *is* the tentap case. Measured bounces are 6 ms and 13 ms and no human
  re-taps inside 80 ms, so 25 ms clears the real bounce with margin either side.
- **`alphanumericInputEvent()` is no longer called with `event == 0`.** `IS_KEYBOARD()` is
  `event <= 0x7f`, so 0 enters the keyboard block, and the unlock, the screen-off wake and
  the swallowed music transport keys all zero it deliberately. Zero is not the active key,
  so the "different button" branch ran and **committed the pending multi-tap letter** —
  nudge the volume mid-cycle and the letter lands early.
- **Tried and retired by measurement, recorded so they are not re-attempted:** a 40 ms UI
  FIFO poll (added believing the 10 ms INT pulse stranded events — `drained` stayed 0 through
  whole sessions while `empty` ran into the hundreds), and a same-batch bounce exemption (its
  counter never moved, and it is exactly what would let that 6 ms bounce through).
- Instrument: `keys` and `keys raw` on the console, plus a KEYS line into `/health.log` on
  the minute tick only when a counter moves. On hardware after the fix: `relfix` climbing,
  `swept=0`, `killed` catching real bounces. {user}: menu scrolling "feels great", typing "way
  way better", unlock "works it seems". 1167 host assertions green.

## Unreleased (2026-08-22, night) — the 80 MHz menu experiment is backed out, and the SIP flap is named but deliberately left alive

- ⚠ **THE 80 MHz MENU EXPERIMENT IS OFF, AND THE OBVIOUS READING OF WHY IS WRONG.**
  {user}, on hardware: "the menu is a bit laggy and doesn't pick up every button push."
  Missed input is a break, not a tuning problem, so it went OFF rather than being dialled
  down to 160 as offered — and 160 would not have helped. **The evidence contradicts the
  clock explanation: the new "screen idle" state NEVER FIRED**, zero occurrences across
  the whole session, only "busy" and "idle". The phone was never actually running its
  menus at 80 MHz. The lag came from the other half of the change: `cpuRaiseForUi()`
  calls `setCpuFrequencyMhz()` from **inside the key-drain loop**, and a PLL switch landing
  mid-keypad-read is exactly how a keypress goes missing. **The level was never the
  problem, the SWITCHING was — and 160 MHz switches just as hard.**
- `UI_IDLE_DOWNCLOCK 0` restores the previous behaviour exactly (the raise hook is compiled
  out too, so "off" is genuinely off), with six invariants asserted mechanically rather than
  eyeballed. The work and the reasoning stay in the tree for whoever revisits it, with the
  two conditions written down: **raise the clock somewhere that is NOT the input path, and
  MEASURE the saving first** — it was never quantified, and the backlight probably dominates
  screen-on draw anyway.
- ⚠ **SIP registration has flapped REGISTERED → lost → REGISTERED every 60 seconds since the
  initial commit, and it is STILL FLAPPING.** `registrationInvalid()` measures
  `REGISTER_EXPIRATION_S` (60 s) from `msLastRegistered` while `REGISTER_PERIOD_MS` refreshed
  at exactly 60000 — so the registration was declared dead at the precise moment the refresh
  fell due, every cycle. Nobody saw it because the only line reporting it was `log_d`, and
  `log_d` is not compiled into this build. It costs more than a flickering icon: each flap
  fires `REGISTRATION_UPDATE_EVENT`, which redraws, which took the clock to 240 MHz for the
  hold window — once a minute, indefinitely, for nothing, quietly undermining the battery
  work it shares a build with.
- ⚠ **The 45 s refresh that was supposed to fix it was REVERTED, because it was wrong.** It
  did not stop the flap, it moved it to every 45 s. The cause is not the period:
  `registration()` clears `registered` on EVERY refresh (`tinySIP.cpp:1288`) and sets it back
  when the 200 OK lands, so the phone declares itself unregistered for one round trip per
  refresh **by construction**. The real fix is to not clear it while merely refreshing, since
  the expiry check already covers a failed one — **deliberately not shipped**: it is core
  registration state on the phone that owns {user}'s number, it needs him present to confirm
  inbound calls still land, and the same evening had already broken his input handling once
  by shipping something that could not be verified. An earlier, deeper attempt worked on its
  own terms (53 flaps in 31 min → 0) and was also reverted — 20 minutes later the phone went
  `lost` and never came back. **The flap is load-bearing: it hides whether refreshes actually
  succeed.**
- **The registration line is promoted `log_d` → `log_e`.** Only `log_e` is compiled into this
  build, so a phone that registers and one that never does looked IDENTICAL on serial — which
  cost real time chasing a registration that turned out to be merely slow on a fresh boot.
  It prints on both edges now. This promotion is the entire reason the once-a-minute flap was
  discovered at all.
- ⚠ **The retransmit ring members were NEVER INITIALISED.** The patch meant to do it aborted
  on a failed assertion *before* writing the file, and the run reported success from the half
  that did apply — so `seenMsgCallId`/`seenMsgCSeq`/`seenMsgNext` shipped with nothing setting
  them while `messageAlreadySeen()` indexes the array with `seenMsgNext`. It corrupted nothing
  **by luck**: `TinySIP sip` is a global, so it lands in BSS and the C runtime zeroes it. But
  `Test.cpp:731` constructs one ON THE STACK, where those members are garbage and the first
  inbound text would index out of bounds. Fixed with member initialisers in the class, which a
  later constructor cannot forget the way a line in the .cpp can. **The lesson worth keeping: a
  scripted multi-edit that asserts per-edit must write the file per-edit or not at all — a
  partial apply that reports success is worse than a clean failure.**

## Unreleased (2026-08-22, evening) — a five-domain battery audit: one real regression, and three bugs that had nothing to do with it

- **"Battery life is a bit worse than it was, recently."** The audit found one thing both
  recent and big enough to feel — and the phone's own health log had already recorded it
  happening.
- 🔑 **The upload server was pinning 240 MHz until someone remembered it.** The transfer
  server is a term in the DFS busy predicate, so while it is up the CPU is pinned at 240 MHz,
  the idle tick runs 5× faster, and the screen sleep timeout stretches to ten minutes —
  roughly **double idle current**. Nothing ever released it: no idle timeout, no client-gone
  timeout, no WiFi-loss timeout. Survivable while the only way to start it was a transfer
  *screen*, whose destructor always calls `xferStop()`; `up on` / `up on books` start it
  **headless**, with nothing to press Back on, and that path was two days old. **Measured, not
  inferred:** last file landed at up=14 min, then **328 consecutive health samples at
  cpu=240MHz with scr=0, from up=10 min to up=92 min** — 82 minutes at double idle draw with
  the screen off, ended only because `up off` was typed by hand. Every single
  240-MHz-with-screen-off sample in 4640 samples across 15 boots is inside that one window.
  It now stops itself after ten idle minutes, deferred by any client traffic on either
  transport, and the stamp is set at start so a server nobody visits still ages out.
- **NTP backs off when there is no clock to be had** — the vendor's own
  `// TODO: increase delay, if there is no Internet`, sitting above that loop since the
  initial commit, and {user} was standing in it on an Android hotspot that blocks UDP 123.
  Unbacked-off, a never-answering NTP woke the task **twice a second forever** and re-sent
  every 2.5 s: ~24 radio wake-and-transmits a minute, indefinitely, against SIP's ~2, and it
  never gives up because there is nothing to give up on. The fast 500 ms poll is still correct
  at FIRST — it is how the reply gets picked up promptly on a working network — so it keeps
  that for ~10 s, then doubles out to a one-minute ceiling. **A working network is completely
  unaffected; a dead one costs about one wake a minute instead of 120.** This is a longstanding
  cost, not the recent regression.
- ⚠ **Two independent save/restore holders over one pair of globals do not nest.** Reading a
  book and running the transfer server both stretched the screen timeouts by snapshotting
  `dimAfterMs`/`sleepAfterMs` into their own fields. Open a book, `up on books`, close the
  book, `up off` — and the last release writes back a value captured before the other holder
  moved it, leaving a **ten-minute screen timeout at full backlight for the rest of the
  session, silently, until a reboot**. Now `ControlState::holdScreenAwake()`, reference
  counted, one snapshot on the first hold, restored by the last release; an unbalanced release
  does nothing rather than restoring a snapshot nobody took.
- ⚠ **`data/configs.ini` shipped `dimming=0` and `sleeping=0` with `bright_level=100`**, and
  the missing-KEY fallbacks agreed with it — while the missing-SECTION branch twelve lines
  below defaults BOTH TRUE. **A phone that got that image ran full backlight until flat,
  silently**, and any `pio run -t uploadfs` would hand a device that state. All three now
  agree: ON.
- **The screen gate is relaxed from "lit" to "lit AND something was drawn in the last 2 s"**,
  so a static menu no longer pays full clock for being stared at; every redraw stamps that
  clock through one funnel in `GUI::redrawScreen` (twelve call sites), and anything with a
  deadline — emulator, transfer server, audio, live SIP — forces 240 regardless. ⚠ **The trap
  the obvious version falls into:** `busy` was ONE boolean feeding both the CPU frequency and
  the 5 ms idle tick; loosen it and a lit screen also gets the slow tick, dragging every pump
  in the loop. The predicates are separated and the tick keeps the ORIGINAL `busy` verbatim —
  **do not re-merge them.** ⚠ **Never target below 80 MHz:** `calculateApb()` returns a flat
  80 MHz for any CPU frequency ≥ 80, so an 80↔240 move reprograms no peripheral at all; below
  80 the source becomes the XTAL, APB follows, and every SPI divider goes wrong.
  *(The user-facing half of this was backed out the same night — see above.)*
- **Also ruled OUT with numbers rather than hand-waving, so nobody re-chases them:** the DFS
  gate itself (structurally leak-proof, one call site, level-triggered; 89% of samples idle
  correctly at 80 MHz), the mesh health-check retry on a plateless phone (~0.1%, and it now
  backs off from 10 s to 60 s after the first two minutes), the GPS UART left enabled with no
  GPS (0.8 mA hard ceiling), and the duplicate-SMS storm (~0.01 mA — three orders of magnitude
  below noticeable, correcting an earlier guess that it mattered).

## Unreleased (2026-08-22, midday) — every text arriving twice, and the SIP receive path had no de-duplication at all

- **{user}, out of town: every text arriving twice or more.** COVEY was POWERED OFF, which
  rules the SMS mirror out completely — both copies came in over SIP, and the SIP receive
  path called `saveMessage()` for whatever arrived, with no de-duplication of any kind.
- **A UDP MESSAGE whose 200 OK does not get back to the server is retransmitted by that
  server** (RFC 3261 timer E/F, up to seven times over ~32 s), and each copy became another
  message in the thread. The phone acks correctly per RFC 3428; the likely reason the ack goes
  unmatched is **a symmetric NAT on an unfamiliar network** mapping the reply to a different
  port than the request arrived on. That is a property of the network, so the phone has to
  tolerate it rather than fix it.
- **De-duplicated on Call-ID + CSeq** — the RFC identity of a retransmission, and exactly what
  a genuine second text does NOT share: send "ok" twice and you get two Call-IDs. **So this
  cannot merge two real messages**, which matters, because people do send the same word twice.
  Eight-entry ring, far more than one transaction's ~7 retries.
- ⚠ **Every copy is still acked, repeats included.** Going quiet on the second one is what
  would guarantee five more.
- ⚠ **It is also an instrument.** If duplicates survive it, the dropped-retransmit log line
  names the Call-ID — and duplicates carrying DIFFERENT Call-IDs would mean VoIP.ms is
  re-pushing the queued text as a fresh transaction after each REGISTER (we re-register every
  60 s) rather than retransmitting one. That is a different fault needing a
  content-plus-window guard instead.
- **The count is what proves it**: a measured triplicate, and phone 2 cleared on four
  independent grounds. COVEY seeing exactly one copy is the load-bearing clue — it puts the
  duplication in SIP delivery, not in the account.

## Unreleased (2026-08-22, morning) — the woods plate is finished, and a radio that lied about existing is made honest

- 🔑 **THE PLATE MADE A NEW FAILURE MODE POSSIBLE, AND IT WAS SILENT.** The radio lives on
  the external pack and the phone can outlive it by hours. Measured on the bench with the pack
  disconnected and the phone alive: **the phone's own driven pins back-fed the whole plate
  through ESD clamp diodes.** `3V3_PLATE` floated at 2.54 V (one diode below 3.3), the 5 V
  node at 2.18 V (backwards through the TLV's high-side body diode — the 0.36 V delta is the
  diode saying so). An RFM95W runs at 2.5 V: **it answered its version probe, the mesh screen
  said READY, RX worked, and TX was silently impossible.** The GPS LED blinked dimly on a plate
  with no battery. **Silent is worse than wedged.**
- **R3–R6 (4 × 1 kΩ, series) go in the four phone-driven lines** — W14 MOSI, W15 SCK, W16 NSS,
  W18 GPS-RX — capping each clamp path at ~2 mA so the rail collapses and **absence reads as
  absence**. W13 MISO and W17 GPS-TX stay plain wire: plate-driven, and the EN gate means the
  plate is never powered while the phone is off. Timing untouched (~15 µs/bit bit-bang against
  ~30 ns of added RC). R7 is the NSS pull-up.
- **`MeshPhy::healthCheck()` — REG_VERSION *plus* REG_OP_MODE**, deliberately not version
  alone: a radio whose pack comes BACK answers 0x12 from POR defaults (FSK standby, LoRa bit
  clear) — present, deaf and mute. Op-mode catches both that and the collapsed-rail case. The
  boot probe gets the same second opinion, a readback of the SLEEP+LoRa write, **so a floating
  MISO cannot fake a radio by producing one lucky 0x12**. Checked every 5 s while READY,
  dropping to `MESH_RADIO_ERROR` loudly; while ERROR it retries a full reinit every 10 s —
  `begin()` is now a thin shell over `reinit()`, so recovery re-runs the entire register config
  rather than trusting whatever survived. A swapped pack rejoins in ≤10 s with a boot-style
  announce, no reboot. Cost while healthy: two bit-banged register reads (~240 µs) every 5 s.
- **Proven on air, both halves witnessing the same hour:** {user} watched the mesh screen turn
  Error with both supplies out, and the serial log caught RECOVERED plus the announce when
  power returned. Plus the day's second discovery — **USB is a legitimate second supply for the
  whole plate**: a power bank runs radio and GPS with no woods pack.
- 🔑 **The woods GPS talks at 115200, not the 9600 written down — and COVEY knew all along.**
  The module was fitted, powered and talking, with bytes climbing and sentences stuck at zero,
  which is the documented wrong-baud signature. Measured by scanning: 9600 → 0 sentences,
  38400 → 0, 57600 → 1, 19200 → 0, 230400 → 0, **115200 → 991 sentences and a live fix**. The
  9600 was copied from COVEY's D-033, which recorded the *plan*; COVEY's own `covey_ui/gps.py`
  has run this module at 115200 since D-062 measured 8.6 KB/s of NMEA off it. **One grep of the
  sister repo would have replaced the whole scan**, and `Hardware.h` now says so where the next
  person will read it. Verified: 22,773 sentences, 9 sats, HDOP 1.3, zero overruns.
- **New: `gps baud <n>`** retunes the live port and persists, applied only while the reader owns
  the UART (the user-serial GUI path keeps `USER_SERIAL_BAUD`), with every owner-changing path
  now retuning — boot, `gps on|off`, and the My node toggle. **`gps raw`** dumps hex+ASCII of the
  last 64 bytes: readable ASCII means the baud is right and something else is wrong, `b5 62`
  means the module is speaking UBX binary, neither means the rate is still off. It ruled out UBX
  here in one command.
- **Wiring sheet v2: the rail is dual-sourced, so a dead pack no longer kills the radio.** D1
  and D2 OR the PowerBoost's 5.2 V and the phone's own VBAT into a single buck-boost — pack
  alive, 5.2 V wins and the phone's cell is untouched (v1 behaviour, kept); pack dead, VBAT
  takes over. **No logic, no firmware, two diodes.** The part has to change with it: v1's
  TLV62569 is a BUCK with a 3.4 V input floor and a 1S cell ends at 3.0 V, so it would drop out
  exactly at the bottom of the discharge — **which is why the original design rejected VBAT and
  recorded the rejection. That objection was about the part, not the idea.** Nothing removes a
  charging path.
- **TPS63020 ordered, and its PS pad gets a decision** rather than being left to chance: PS is
  the power-save select, LOW gives pulse-skipping at light load which is what a plate idling
  around 45 mA wants, **and the pin must not float**. ⚠ Two traps recorded while the part is
  fresh: the 3V3/4V2/5V jumper ships UNSET for our purpose, and this part's EN thresholds are
  tighter than the TLV62569's it replaces — V_IL 0.4 V against the Pololu alternative's 0.7 V.
  The measured 0.22 V EN divider still passes, **with less headroom than v1 had**, which
  promotes checklist item 4 from formality to load-bearing.
- **Two phones now**, so the port gets named rather than assumed.

## Unreleased (2026-08-21, evening) — first power on the plate, and a fuse that was sized for the wrong current

- **First power, steps 1–4 passed:** 5 V rail 5.15 V, plate 3.3 V rail 0 V across C2 and C4,
  identical on the bench supply and the LiPo. **The header 3.3 V pin measures 0.22 V with no
  phone attached, and that single reading is worth more than it looks** — it IS the EN node via
  W12, the divider predicts 0.231 V, and only a continuous W12 plus a correctly-valued R1 plus
  an intact internal pull-up can produce it. It is also far easier to reach with a probe than
  the TLV EN pad, so it is now the documented probe point.
- ⚠ **Mount it cold.** The mesh is live, so the ESP32 drives SPI into an unpowered RFM95W if
  the plate is mated hot, and ten contacts mate in an arbitrary order with 5.2 V sitting on the
  5 V pin. ⚠ **The BT2.0 unplug, not the phone's power button, is the real off switch** — the
  EN gate spares the plate rail, not the 5 V charge path.
- ⚠ **The battery-leg polyfuse was undersized: 2 A hold, not 1 A** — and then **3 A**, once the
  temperature derating was written down. Fitted as a GF300.
- **The last open RF question closes:** it is a v2.2, and R6 tunes a trace this build does not
  have. **R6 is 0 Ω, from WiPhone's own netlist — there is nothing in the antenna path.**

## Unreleased (2026-08-21, later still) — the phone tells the mesh who it hears

- **Neighbour info (portnum 71).** The phone keeps a RAM table of nodes heard with NO relay —
  `hop_start == hop_limit`; **a missing `hop_start` is UNKNOWN, not direct**, because claiming a
  relayed node as a neighbour would draw an edge that does not exist. Announced on the PRIVATE
  channels only: **never the public primary, where it would hand our topology to any stranger
  with a radio**, and never booksync/smsmirror, which carry machine traffic nobody reads.
- My node > Neighbor info cycles off / 1 h / 4 h (1 h is the useful one for a hunting day; 4 h
  matches what the stock module calls its floor). Serial `nbr` lists the table with signal and
  age; `nbr on|4h|off|now` drives it, because the bench cannot wait an hour to learn whether a
  packet is well formed.
- **The encoder is hand-rolled protobuf pinned byte-for-byte to the real runtime's output via
  generated vectors — which immediately earned it:** proto3 elides default-valued scalars, and
  emitting `snr=0` where the reference emits nothing made our packet 5 bytes longer and
  unreadable by comparison. ⚠ Multi-channel announces drip one per pass; three back-to-back
  LongFast sends would stall the superloop into the watchdog.
- Proven on air: COVEY logged `[nbr] !00449040 hears 1 node(s): !62b8d2fd`. 16 host suites green.

## Unreleased (2026-08-21, later) — the phone becomes COVEY's memory: mesh history replay

- **New: mesh history replay** (docs/replay-spec.md). The phone keeps a ring of the
  last 64 channel texts it hears or sends (PSRAM, ~13 KB — internal RAM untouched);
  when COVEY regains its radio after a blind window (lent to the phone app, or
  powered off in a bag) it asks `RPL?` on the booksync channel and the phone replays
  compacted records — original timestamps, oldest first, one packet per 3 s so a
  reply never storms the band, closing with an honest gap marker when history has
  already fallen off the ring. Proven live end-to-end: two messages sent through
  COVEY's own radio while covey-ui slept came back on wake and rendered as OUTGOING
  in its history, timestamps within seconds of truth.
- The wire format is pinned to COVEY's reference implementation by generated vectors
  (tools/gen_replay_vectors.py → tests/test_replay.cpp), down to whitespace grammar
  and uint32 bounds — 15 host suites green. Serial `replay` shows the ring, pending
  replies, and the last served request.
- **Adversarial review caught a privacy leak before the woods did**: legacy
  channel-encrypted DMs (a form this phone deliberately accepts, and which
  decrypts with the CHANNEL key) were entering the replay ring, so a private
  message could have been rebroadcast to every booksync member. Capture is now
  broadcast-only — that dest check is what actually enforces the spec's DM
  exclusion, since "DMs are PKC and unreadable" was never true of the legacy
  form. Also fixed: a reboot used to hide its own losses (coverage was proxied
  by ring fullness, and the ring is empty after exactly the event the gap flag
  exists for) — a coverage-start stamp replaces it, verified on air.
- **Clock lessons, encoded**: record timestamps are `getExactUtcTime()` —
  `getExactUnixTime()` is the LOCAL-shifted epoch and put the first live records
  7 hours off; and the asker pads its window generously (t2 + 600 s) because a
  no-RTC Pi and a drifting phone disagree — a tight window silently served 0
  records with both sitting in the ring. Dedup makes generosity free.

## Unreleased (2026-08-21) — uploads redesigned: chunked, checksummed, resumable, and finally fast-network-proof

- **The upload architecture is new.** Files travel as CRC32-checked pieces, one in
  flight at a time, each acknowledged before the next departs — so a fast network can
  no longer flood internal heap with radio RX buffers (the measured 14 KB → 868 B
  collapse), and a dropped connection, breaker pause, or reboot costs pieces, never
  files: the client asks `GET /chunk` how many bytes the card holds and continues from
  there. Two transports serve the same protocol: a **raw single-connection port
  (8081)** — one TCP connection per batch, bodies read incrementally at the phone's
  own pace, zero per-request heap cost — and the original web server's `/chunk` as
  the multipart fallback the page probes for automatically. Piece size is measured,
  not guessed (16 KB raw / 4 KB fallback; in-flight bytes stay bounded by the 5.7 KB
  TCP window either way). **Acceptance: three consecutive 4-book batches (~52 MB) over
  the fast phone-to-phone hotspot, 12/12 byte-verified, zero breaker events, zero
  panics, 66–97 KB/s (round-trip-bound on that link).**
- **The heap has an instrument now**: serial `heap` prints internal/DMA/PSRAM free,
  largest block, and the boot-long floor; the upload path logs per-file heap floors.
  First fruit: the "some boots have less memory" mystery was uptime fragmentation,
  not boot luck.
- **Truths this work surfaced, now encoded**: `File::size()` on an open FatFS file
  reads the stale directory entry (durable bytes are counted in RAM instead); the
  legacy whole-file POST parser can wedge the main loop if its client vanishes
  (chunked pieces are smaller than the TCP window precisely so that cannot happen);
  breaker teardown cycles inside tight heap leak (pauses no longer fire mid-batch,
  and they persist-without-closing the batch file); the page declares its charset
  (mojibake), keeps the native form fallback for fetch-less browsers, skips
  empty files honestly, and one failed file no longer sinks the rest of the batch.
- **Bench tools**: `tools/chunk_push.py` speaks the whole protocol (both transports,
  `--corrupt`, `--restart-at`, `--resume`, `--gap`, piece-size experiments);
  `tests/test_chunk.cpp` pins the protocol core (verdicts, name safety, CRC vectors).
  ⚠ Flashing now requires panicwatch stopped first — it eats esptool's sync replies.

## Unreleased (2026-08-20, day) — the phone grows a GPS ear, asleep until the plate exists

- **The upload heap saga, evening chapter — fast networks were the killer all along.**
  Push-mode uploads on a fast LAN (3 ms RTT) let TCP open wide and the radio flood
  internal heap with RX buffers faster than the SD drains: heap measured falling from
  14 KB to 868 BYTES inside seconds — while the sluggish phone-hotspot link had been
  masking the whole class by keeping bursts tiny. Defenses now in: mid-upload
  BACKPRESSURE (heap tight ⇒ stop consuming the socket ⇒ TCP window closes ⇒ sender
  stalls ⇒ radio buffers drain), and the breaker defers mid-file to it (trips only on
  catastrophe below 3 KB; between requests the 6 KB line stands). Push uploads on fast
  links now SURVIVE but crawl — the honest recommendation for bulk delivery is the
  page's "paste a download link" PULL path, which lets the phone read at its own pace
  and moved 5 MB books at full speed with heap intact, straight into /books.
- **`up on books`** — the Books uploader is serial-startable now (bench work needs to
  feed the reader with no hands on the phone); `?` lists it.

- **The upload server can no longer strangle the network stack** — the true anatomy of
  "the page locked up", measured live: an upload burst (worst with aborted connections)
  pins 12-15 KB of internal RAM in dead TCP control blocks for minutes, the largest
  free block collapses toward 3 KB, lwIP starts refusing connections, the listener
  dies, and finally the stack goes mute — not even ping answers — while WiFi still
  shows connected. A LOW-HEAP CIRCUIT BREAKER now pauses the server (listener closed,
  nothing lost, honest log line) when the largest internal block drops under 12 KB and
  resumes on its own at 20 KB. Proven on hardware: a stress burst tripped it cleanly —
  instant refusals instead of hangs, phone alive throughout. Per-upload heap telemetry
  (one log line per file) turns future leak-hunting into arithmetic: today's measured
  slope is ~350-400 B lost per upload plus the transient churn. The breaker's first cut
  carried two bugs its own first field trip exposed: a 20 KB resume threshold that
  steady-state fragmentation never reaches (one trip = paused forever), and pause state
  in a function-local static that outlived server restarts. Retuned to the measured
  band — trip 6 KB (uploads run happily at 7-8), resume 10 KB (idle recovers to 13-16)
  — and the state resets on every server start. The SECOND field trip ({user}'s kitchen,
  the same evening) exposed the deeper truth: ANY resume threshold can be pinned
  unreachable by the paused server's own allocations, and the softAP fallback race
  (2 s for a flapping association to look connected) tears down the home-WiFi link
  each time it loses. Breaker v3 therefore FREES the server outright on pause
  (delete + MDNS.end — the memory actually comes back) and resumes on heap recovery
  OR a timer with re-trip backoff — a stranded pause is structurally impossible.
- **An upload no longer strands the phone off WiFi** — the reconnect machinery (and the
  auto-switcher) were disabled whenever the transfer server ran, guarding a real softAP
  crash. But the guard also fired in plain STA mode, where it protects nothing: a
  one-second hotspot blink mid-upload left the phone at "SSID not available" with the
  page dead and NO path back until the server was stopped — found live during a 4-book
  upload that ended in a panic-reboot. The gates now block only genuine softAP mode.
  (The panic itself did not reproduce once the stranding was fixed: four 3-5 MB EPUBs
  uploaded back-to-back over a phone hotspot, heap steady. Its habitat — a wedged,
  heap-starved server session — no longer exists; if it ever recurs on the bench cable,
  the watcher will catch the backtrace.)
- **The README tells the truth again** — a six-agent audit checked every public claim
  against the code: the phantom touchscreen, the "may require a couple tries" sleep
  gesture (it works every time; hold Select+Back 2 s), the 4-of-17 serial-command
  table, phonebook advice that 0.9.5's number completion had already made obsolete,
  and a `pio device monitor` tip that printed garbage (monitor_speed was pinned to
  115200 — now 500000, so the plain command works). The 0.9.6/0.9.7 features the
  README never mentioned — PKC DMs, Places, distances, Sun — are in it now, plus a
  What's-new section that surfaces this changelog. Firmware nits the audit caught:
  `sip` was missing from the serial `?` help, and the on-phone flashing help promised
  "fonts, sounds" from uploadfs (it carries config files, the ringtone, the wallpaper).
- **The WiFi auto-switch deadlock is fixed** — the reason the phone sat next to a saved
  hotspot ALL DAY without joining it. The switcher's call-guard blocked on HangUp — a
  TEARDOWN state the END key enters from anywhere, which STICKS when the proxy is
  unreachable (its BYE can never send). So: leave home WiFi, press END once, and the
  thing that restores the network is blocked by a state that needs the network to end.
  Measured live: sip=6 for six hours, 7,549 host-unreachable errors, zero scans, the
  hotspot in range and saved. The guard now blocks only the six genuinely audio-delicate
  states — and only WHILE CONNECTED, because with WiFi down there is no audio a scan
  could glitch: the deadlock is structurally impossible for any stuck state, ever.

- **Sun & legal light is a SCREEN now** (Meshtastic > Sun & legal light): countdown
  first — "LEGAL LIGHT: 13h 34m left" — then the four times, at the reference place;
  the same almanac-verified core the serial `sun` uses. When no place is known it
  says every way to get one, including the GPS toggle one screen away.
- **The GPS enable is a MENU TOGGLE** (Meshtastic > My node > GPS receiver:
  off / on (no fix yet) / on (fix)) — same flag and NVS pref as serial `gps on|off`;
  "fix" is only claimed while the fix is FRESH. {user}'s standing rule, adopted today:
  features ship with a UI surface; serial commands are the diagnostics, not the feature.

- **The woods backplate's GPS half is in the firmware**: `nmea.{h,cpp}` reads RMC/GGA
  (checksum-strict, junk-tolerant — wake garbage before '$' is the expected steady
  state on this UART) into the mesh's own fixed-point coordinates, over the stock
  USER_SERIAL UART2 on GPIO 38/32 @ 9600 — the exact pins and baud the plate wires.
  A fix fresh within 2 min becomes the reference between a chosen waypoint and the
  manual pin, so `sun`, node distances and Places run from the phone's own position.
  **Dormant until serial `gps on`** (persists); `gps` prints the counters that
  diagnose a bench (bytes up + sentences 0 = wrong baud). No automatic position
  broadcasts — going on the air stays the manual I'm-here act. 39 new host checks
  against Python-computed vectors; +944 B static, zero heap. Not yet flashed.
- **`pos` names the true missing piece** when waypoints are heard but none chosen
  (it used to claim "no waypoints heard" directly above a listed waypoint).
- **panicwatch is repo-tracked** (`tools/panicwatch.py`) and types a wake newline
  before every command — the first bytes after DFS idle arrive mangled, which cost
  a garbled `pos` this morning; the old leading-`\n` ritual never survived strip().

## 0.9.7 (continued, overnight 08-19→20) — lists that wrap, text that fits, places that expire, and the sun

- **Places live and die like they do on the mesh**: deleting a pin on COVEY now removes
  it here too (the delete packet was being rejected as malformed — it never worked), and
  a place with an expiry ages out on its own clock, not just when a packet mentions it.
- **Long text wraps or says ".."** — the node list is two lines (name / distance+age),
  chats preview their newest message, and every menu, header, label, popup and caption
  in the firmware ellipsizes instead of running under the clock or off the screen
  (a 36-agent sweep found 16 offenders; all fixed, plus the review's follow-ups).
- **`sun`** — dawn / sunrise / sunset / dusk and a legal-light countdown for the
  reference place, offline (NOAA math, almanac-verified). Groundwork for a clock-face
  version.
- Position broadcasts on COVEY can now ride a **chosen channel** (Node & Location) —
  the public default is labeled with the truth: "LongFast — every radio can read this."
- Fixed: two SIP addresses sharing a long prefix no longer merge into one thread;
  `unread clear` survives reboots (a stale partition cache was resurrecting the flags).

## 0.9.7 — the phone knows where everyone is (no GPS required)

### 📍 Places and positions

COVEY broadcasts its GPS position every few minutes and shares waypoints (camp, the
truck, a stand) from its map. The phone now listens: **Meshtastic → Places** lists every
shared waypoint, and the **Nodes** list shows each node as *"{user} H New Device — 1.4km
NE"* measured from a reference you pick (any waypoint, or your own pin). Positions and
places survive reboots.

**"I'm here"** — open a place and declare yourself at it. The phone remembers (and
shows distances from you), and it announces the position to the mesh once, so COVEY's
map shows the phone. It's a statement, not a fix — the phone has no GPS; a future GPS
module would drop into the same slot (see docs/maps-plan.md for the map-app research).

### 📬 The white "new message" icon tells the truth again

Re-mirrored history (like the chip-erase recovery's) arrived unread and buried itself
deeper than the read-marking scan ever looked — the icon stayed lit with nothing visibly
new. Opening a conversation now clears its unread messages at ANY depth, and the new
serial `unread` command counts the truth from the records themselves, repairs the
counters when they disagree, and names a thread that still holds unread.

### 🔧 For the toolbox

- `pos` — every waypoint, every node fix with age and distance, your pin, the
  reference. The whole picture in one paste.
- `unread` — see above; `unread clear` marks EVERYTHING read, for flags orphaned by a
  deleted conversation (nothing left to open = nothing else could ever clear them).
- Fixed: serial `?` help was silently truncated at 192 bytes — every command added
  since `chan` was invisible in the one place a stranded user looks.

### 🥾 First field test, same day

The whole chain worked on air — waypoint received ("vashon"), reference set, COVEY
placed at "3.2km E of vashon" — but the UI hid the payoff: "Measure from here" now
jumps straight to the Nodes list where the distances live (the first field report was
"nothing happened anywhere?"), and the place-menu labels no longer run off the screen.

### 🛡 From the adversarial review (before this ever shipped)

Positions with modern fields (precision_bits) parse correctly; locked waypoints can't
be moved or deleted by anyone but their owner; a recycled node slot no longer inherits
the evicted node's position or crypto key; restored fixes say "old" instead of a
71,000-minute age; a full node table can't corrupt the waypoint store when RAM is
tight; the "I'm here" announce prefers a PRIVATE channel and says so loudly when only
the public default exists — and if the send fails, Status says "set (send FAILED)"
instead of letting silence read as safety.

## 0.9.6 — direct messages work again: the phone speaks Meshtastic's DM crypto

### 🔐 PKC — the fix for "the RAK refuses every DM to this phone"

Modern Meshtastic (2.5+) requires public-key cryptography for direct messages, in **both
directions**: a stock node refuses to *send* a DM to a phone with no known public key
(`PKI_SEND_FAIL_PUBLIC_KEY`), and silently *drops* the old channel-encrypted DM form on
receive ("Rejecting legacy DM"). Channels never noticed; DMs were dead both ways.

This release implements the whole scheme, verified line-by-line against the 2.7 firmware
source: an X25519 identity keypair (generated once, kept in NVS), the public key riding
in every NodeInfo broadcast, peers' keys learned from theirs (trust-on-first-use, like
stock — a changed key is flagged, never silently swapped), and DMs encrypted with
AES-256-CCM under the SHA-256 of the shared secret. Incoming DMs are ACKed — including
re-ACKing retransmissions — so the sender's app shows ✓ delivered instead of timing out.
DMs to a node whose key hasn't been heard yet still go out the legacy way (a pre-2.5
device will take them) with a loud serial note that a modern node won't.

The crypto is self-contained and host-tested against Python's `cryptography` playing the
stock-firmware side — byte-for-byte frames both directions (`tests/test_pki.cpp`), the
same philosophy as the book-sync hashes. RAM cost: +1.7 KB static, **zero heap**; the
X25519 math runs only at superloop depth where its 3 KB of stack is safe.

### 🔧 For the toolbox

- `pki` — our key (base64, comparable against COVEY's node list), every node's key
  state, mismatch flags, and the loop task's stack headroom.
- `announce` — broadcast NodeInfo now, asking hearers to answer with theirs (the fast
  way to swap keys with the RAK after this update).
- `dm <!node> <text>` — send a DM from the cable; the peer's ACK in the serial log is
  end-to-end proof its firmware decrypted us.
- Fixed while in there: a maximum-length message could build a 256-byte frame whose
  length wrapped to 0 in the radio call (sent nothing, claimed success). Now refused
  with a log line.

## 0.9.5 — install from a browser, browse your files, type just the number

### 🔌 Install and update from a browser

**[nikguy321.github.io/wiphone-meshtastic](https://nikguy321.github.io/wiphone-meshtastic/)** —
plug the phone into a computer, open the link in Chrome or Edge, click **Install**, pick
the port. About a minute; nothing to install on the computer. **Updating never touches
your data** — settings, contacts, messages and the SD card all stay; a full factory-reset
install exists only as a clearly-marked advanced option that asks first. The phone's own
**Settings → Firmware update** screen now leads with this route.

### 🗂 A file browser (Menu → Tools → Files)

Browse the whole SD card. Press OK on a file for **Open / Copy / Move / Delete** —
copy and move use a paste-where-you-land clipboard that survives leaving the app, never
overwrites, and can never damage the original (a move only completes after the copy is
verified). Text files open in a pager that wraps like the e-reader. **Upload into this
folder** turns the WiFi upload page toward wherever you're standing — the whole card is
now reachable over WiFi, not just ROMs and books.

### ☎️ Type just the number

New contact or new text: type `4255550142` and the phone completes it to a full SIP
address using **your own account's server** — so it adapts to any provider with nothing
to configure. Addresses with `@` or letters pass through untouched. Also understands a
leading `+` and entries older versions saved as bare numbers.

### 📖 Reader fix

Some lines lost their first character at the margin (a font measure/render disagreement).
Fixed at the root; wrapping is now exact.

### 🔧 For the toolbox

Serial console additions: `chan <url>` applies a Meshtastic channel invite over the cable
(the mesh can't deliver invites to this phone — modern Meshtastic requires public-key
crypto for DMs that this firmware doesn't implement), `chans` lists channels, `sip`
answers "why is texting dead" in one line, `bookpage` dumps the open reader page.

## 0.9.4 — the audit release: hardening, battery, and small courtesies

### 🛡 Hardening (from a 38-agent audit of the whole codebase)

- **Two remotely-reachable memory-safety holes in the phone stack are closed**: a SIP
  message could overflow a stack buffer via long names/URIs, and a peer sending more than
  100 headers wrote past an array. Both now bounded.
- **A latent double-free in message storage is defused** (partition cache switching
  shallow-copied one cache over another).
- The unread-messages counter no longer under-counts after reading a message when unread
  texts span storage partitions.
- In-call volume keys no longer start from uninitialized memory on a fresh flash, and
  what gets saved is what the hardware actually accepted.
- The mirror poll's stall timeout actually fires now (its re-arm condition was always
  true), and typing-screen redraws survive an out-of-memory strdup.

### 🔋 Battery, for days in the field

- **Out of Wi-Fi range, the radio finally rests.** The phone used to hard-cycle the Wi-Fi
  radio every 20 seconds all day when no network was in range — with the driver
  re-probing in between. After five quick retries it now eases to one attempt every
  3 minutes with the radio quiesced in between; picking the phone up or coming home
  reconnects immediately.
- The LoRa radio's status register is read every 10 ms instead of ~1000 times a second
  (it was bit-banged SPI each pass); nothing can be missed — packets take >100 ms on air.
- When truly idle (screen off, no call, no music, no transfer), the main loop ticks at
  5 ms instead of 1 ms — a fifth of the wakeups, imperceptible latency.

### 📱 Small courtesies

- **Reboot asks first** (it was a one-press menu item next to Settings).
- **Redial**: OK on an empty dialer brings back the last number you called (from the
  dialer or the phonebook); OK again calls it.
- **Battery percentage** next to the icon on the clock screen.
- **# on the clock opens Messages** when the envelope icon is showing.
- **Replying to a text returns you to that conversation**, not the conversations list.
- Serial console gains `bookpage` — dumps the open reader page's layout for diagnosing
  rendering reports.

## 0.9.3 — the day-of-real-use fixes: order, buzz, and the reboots

### 🧾 A conversation reads in the order it happened

- **Texts stored before the phone knew the time no longer pin themselves to the bottom
  forever.** A message arriving between power-on and the first clock sync used to be
  stamped "unknown", which sorted as *newest for all time* — so everything after it
  landed above it, i.e. in the middle of the thread. Now, the moment the clock syncs,
  those messages are stamped with the sync time and fall into place. Their shown time can
  be a little late (the phone has no clock chip to do better); their order is right.
- **Two openings of the same conversation now always show the same order.** Messages
  sharing a timestamp were previously ordered by coin toss. Ties now break on the
  provider's own message number when there is one, and deterministically otherwise —
  which also keeps a burst of catch-up texts from COVEY in their true order.

### 📳 Every arriving text announces itself

- **Texts arriving over WiFi from COVEY buzzed nothing and drew nothing** — they simply
  appeared in the store, and worse, they *claimed* the text so the radio copy stayed
  silent too. Both transports now announce through one shared path: buzz for a text
  somebody sent you, silence for mirrored copies of your own, and an open conversation
  refreshes on screen instead of waiting for you to back out and re-enter.
- The buzz now fires **before** the chirp, so if the chirp's wiring ever interferes with
  the vibration motor again, it loses that race instead of winning it.

### 🧯 The reboots while scrolling or opening conversations

All of the found causes are fixed, and the biggest is structural:

- **Parsed message files now live in the big memory bank (PSRAM), not the small one.**
  Opening a conversation parses message files into hundreds of little objects; they all
  came out of the ~20 KB pool the WiFi/phone stack needs to breathe, and a decoded crash
  backtrace caught exactly that running out. Those objects now go to the 3.6 MB bank.
- **A cached page no longer drags a second message file into memory for nothing**, and a
  long-standing accounting slip (the second file reporting the first file's number) is
  fixed with it.
- **Scrolling a conversation no longer allocates at all** — the redraw made up to 13
  little text copies per keypress, unchecked, and a failed one crashed the phone with the
  trigger "while scrolling". It now draws in place.
- **Running out of room while laying out text truncates the text** instead of writing
  past the end of a block — an out-of-memory moment used to become silent corruption and
  a reboot with no readable crash record.
- **After a reboot, the phone no longer re-downloads and re-checks the entire mirrored
  history from COVEY.** It remembers where it left off on the SD card. (Delete
  `/roms/smsmirror.since` to force a full resync on purpose.)

### 🔇 Housekeeping

- Deleted leftover debug spam that wrote to the field log on every settings keypress.
- Fixed a dormant accessor that returned a message's time when asked for its delivery
  time.

## Unreleased — texting looks like texting, and two devices share one number

### 💬 Messages are conversations now, not an inbox and an outbox

- **One row per person, newest first.** Open it and you see the whole exchange in order,
  oldest at the top, with `You:` on the ones you sent. Replying is one press — it
  already knows who you are replying to.
- The old **Inbox / Sent** split is gone. It was never how anyone thinks about texting;
  it was just how the messages happened to be filed on the card.
- **Unread counts sit on the row**, and opening a conversation clears it.
- The same person is one conversation however their number is written — with the country
  code, without it, punctuated, or as a full address.
- ⚠ The list shows your **recent** conversations, not every message you have ever had.
  Someone you have not texted in a very long time may not appear.

### 📨 Texts sent from another device show up here too

If you run a second device on the same phone number — this was built for COVEY, a
Raspberry Pi handheld — the texts it sends now appear on the phone as well, so the two
do not drift apart.

- **Two ways in, and they cover different gaps.** Over **WiFi** you get the full history,
  including texts that arrived while the phone was off. Over the **radio** (LoRa) you get
  new texts anywhere in range, no WiFi needed. Running both is fine — the phone
  recognises a text it already has and will not show it twice.
- **Setup:** put a two-line `smsmirror.txt` on the SD card — the other device's address
  on the first line, its shared token on the second. Upload it with the same WiFi upload
  page you use for ROMs and books. Nothing happens until that file exists, and the phone
  says so on the serial log rather than sitting there silently.
- ⚠ **Give it its own mesh channel** rather than reusing one. A channel invite contains
  the key, so sharing it hands over everything else on that channel too.
- ⚠ **The WiFi side is unencrypted**, because this phone cannot do HTTPS at all (see
  below). The token stops a stray browser, not somebody watching the same network. On a
  network you do not trust, turn the WiFi side off and let the radio carry it — the radio
  side *is* encrypted.

### 🔌 A serial console, for the things you can otherwise only reach by tapping the screen

Plug in USB, open a terminal at 500000 baud and type `?`. You can start and stop the WiFi
upload page, ask the phone to fetch texts right now, and check what it thinks its state
is — useful when the phone is on a bench and you are not holding it. No password:
whoever has the cable has the phone.

### ℹ️ Why the phone cannot fetch its own texts from the provider

The obvious design would be for the phone to talk to the provider directly. It cannot,
and it is worth writing down so nobody spends a day rediscovering it: a secure connection
wants about 33 KB of a particular kind of memory, and the whole phone has about 31 KB of
it. No setting changes that, and the usual workaround for small devices does not exist in
the version of the encryption library this build is stuck on. It is the same wall that
stops over-the-air firmware updates working.

## Unreleased — it makes phone calls now

### ☎️ Calls and texts work, on a real phone number

- **The phone can call and text ordinary phone numbers, both directions.** A VoIP.ms
  number, over WiFi. Calling out, texting out, ringing on the way in, and texts
  arriving — all confirmed on real hardware against a real mobile.
- ⚠ If inbound calls give a **busy signal** and inbound texts never arrive while
  outbound works fine, the number is routed to the wrong account at the provider,
  not broken on the phone. Point the DID at the sub-account the phone registers as.
- **Put the full address in the phonebook** — `15551234567@yourserver` — not just the
  number. Calling fills in the server for you; texting does not, so a bare number
  calls fine and silently fails to text.

### 🔕 A ringer you can silence

- **Settings > Audio → Ringer:** Ring + vibrate, **Vibrate only**, or Silent. The phone
  always vibrated on an incoming call; now you can keep the buzz and drop the noise.
  Remembered across reboots, and applied from the moment the phone starts rather than
  the first time you visit the settings screen.

### 📬 The status bar says what kind of message is waiting

- **A white envelope means a text; a green one means a Meshtastic message.** Both
  waiting shows them overlapping. Read one and the icon collapses to whichever kind is
  still unread, so it tells you what is left rather than just "something arrived".

### 🧊 The phone stopped freezing for seconds at a time

- **Those five-second freezes while clicking around were not crashes** — the phone was
  stuck waiting on a network name lookup, and everything on screen shares one thread.
  It was asking the local network about internet addresses it could never answer for,
  half a second each time, and retrying a failing lookup twice a second. It now skips
  the pointless question and remembers failures instead of repeating them.

## Unreleased — typing, and honest buttons

### ⌨️ Typing the same letter twice no longer means waiting

- **The middle of the D-pad now means "yes, that letter".** Typing `AZAZ` was
  always quick, because moving to a different key finishes the letter you were
  on. Typing `AAA` was not: pressing `2` again cycles `a → b → c`, so the only
  way to get a second `a` was to stop and wait two seconds. Now you press the
  key, press OK, and it is committed — as fast as your thumbs go, the way old
  phones worked.
- **The two-second wait still works exactly as before.** OK is an extra way to
  finish a letter, not a replacement, so nothing you already do stops working.
- **Sending is unchanged.** The top-left **Send** key and the green call key
  still finish the letter you are on *and* send, in one press. Only the middle
  of the D-pad changed, and only while a letter is waiting.

### 🏷️ The button under "Cancel" never cancelled

- **Four screens labelled a button "Cancel" when it actually deletes a
  character** — writing a message, editing your node name or short name, and the
  book sync settings. The key under that label is backspace; cancelling is the
  key *below* it. They now say **Clear**, which is what the rest of the phone
  already called it.

### 🔌 Firmware updates: honest instructions instead of a broken button

- **Settings > Firmware update** used to offer a Check button that could only
  ever fail. It now shows **step-by-step instructions for updating over USB**,
  on the phone's own screen, so you can read them while holding the phone —
  scroll with up/down, jump a page with left/right.
- **Why the old one could not work:** the phone cannot open a secure connection
  at all. It needs about 33 KB of fast memory to do so and has about 19 KB in
  total. That is a hardware limit, not an expired certificate or a stale link,
  and no amount of retrying was ever going to fix it.
- **The phone also stopped trying at every startup.** It had been attempting that
  same impossible connection on every single boot — quietly, with nothing on
  screen to say so — and the attempt costs both time and the scarce memory the
  phone then goes on to need. Measured on the device afterwards: the largest
  block of free fast memory at startup went from about 14.4 KB to about 15.9 KB,
  which is the number that decides whether the phone stays up.
- The new screen costs the phone **less** memory than the settings form it
  replaced, and does not allocate anything to display its text.

## 0.9.2 — book sync works, stability, and Game Boy speed

⚠ **Known limitation: over-the-air updates do not work on this phone**, and the
reason turns out to be memory rather than anything to do with the link. A secure
connection needs about 33 KB of the phone's scarce fast memory and there is only
about 19 KB of it, so the connection can never be opened. Install over USB. An
earlier note claiming the updater was working again was only half right — the
expired certificate it fixed was a real problem, but not the one stopping it.

### 📖 Reading position sync works between devices

- **Your place in a book now travels over the mesh, for real.** Read on the phone,
  tap *Sync my place*, and the other device offers to jump to where you got to.
  This had been built and tested against recorded data for months but had never
  been tried between two actual devices until now.
- **It works both ways** — phone to COVEY and COVEY to phone.
- **Only the device you are reading on needs the tap.** Tapping *Sync my place*
  sends your position out; the other device picks it up on its own, whatever it
  happens to be doing. You do not have to tap on both.
- ⚠ **Worth knowing, because it looks broken the first time:** the receiving
  device shows nothing at all when the position arrives. Radio messages are slow,
  so it is saved quietly and offered **the next time you open that book**. If sync
  seems to do nothing, open the book on the other device before assuming it failed.
- **Books > menu > Sync settings** tells you what is going on: whether the shared
  channel exists, and how many positions are waiting to be picked up.

## Stability, and Game Boy speed

A day spent on the phone restarting by itself. The cause is now identified and
fixed, a settings screen that had been quietly broken works again, the Game Boy
runs at full speed again, and the tools for reading what the phone was doing no
longer make the problem worse.

### 🔊 Notification sounds stop hijacking the speaker

- **A Meshtastic notification used to permanently change your audio settings.** The
  little "pop" reconfigures the speaker to play itself — mono, loudspeaker, full
  volume, low quality — and then never put any of it back. So one message left the
  phone that way for good: music afterwards played in mono out of the loudspeaker
  even with headphones plugged in, and the Game Boy dropped to half speed, because
  the emulator takes its timing from the speaker.
  Notification sounds now put everything back exactly as they found it. This was
  most likely the real cause of the half-speed games — a message arrives far more
  often than anyone opens the music player.
- ⚠ Still true: a notification stops whatever track is playing. That is a separate
  problem and is not fixed yet.

### 🎮 Game Boy back to full speed

- **Games were running at half speed.** The emulator deliberately lets the audio
  hardware set its pace — it hands over a frame of sound and waits for the
  speaker to be ready for more, which keeps the game locked to real time with no
  drift. But it only ever told the audio hardware *what rate* to play at, never
  whether the sound was stereo, and it quietly relied on whatever the last thing
  to use the speaker had left behind.
  Once the music player started switching between stereo and mono to follow the
  headphone jack, the Game Boy could inherit mono — and then the hardware drained
  its sound at exactly half the expected rate, so the game waited twice as long
  for every frame and ran at half speed. It now sets both, and never inherits.
- ⚠ Note for the future: the on-screen speed readout going orange, and the
  emulator dropping display frames to compensate, both look like "the processor
  is too slow". Neither was. Skipping frames cannot fix a game that is being
  paced by sound.

### 🔋 Battery

- **The phone could get stuck running flat out, on battery, with the screen off.**
  If a call ended while there was no signal, it waited forever for a confirmation
  that could never arrive — and while it waited, it treated itself as being on a
  call and never slowed the processor down. Caught in a car log: nineteen straight
  minutes at full speed, screen off, out of range. That is roughly double the
  idle drain, in exactly the situation where battery matters most.

### 🔁 The random restarts

- **Opening Books was quietly eating the phone's working memory.** Every time you
  opened it, about 3 KB of the phone's small pool of contiguous memory went and
  did not come back — and after enough opens, the next thing that needed a
  contiguous block could not get one and the phone restarted. It now lives in the
  large memory the phone has plenty of. Measured before and after: the cost of
  opening Books fell from about 3,000 bytes to 140.
- ⚠ Note for anyone reading older notes: Books' big *lists* were moved out of the
  scarce memory a while ago, but the app itself was left behind. "Books was fixed"
  was only half true, and the remaining half is what kept causing restarts.

- **The likely cause is fixed.** Every time the phone tried to join WiFi it
  registered another copy of its network event handler, and never removed any of
  them. Out of range it retries every 20 seconds, so a drive with no signal
  stacked up hundreds of copies — one measured journey was 143 minutes without
  service, which is around 430. The moment signal came back, **all** of them ran
  at once, each tearing down and rebuilding the same network buffers. That churn
  breaks the phone's small pool of working memory into fragments until something
  eventually cannot find the unbroken space it needs, and the phone reboots.
  It is registered once now, when the phone starts, instead of on every attempt.

- **The phone can now say why it restarted, and it has been answered.** Four
  restarts were captured and every one of them was a crash — not a flat battery,
  not a stall, not the power being interrupted. The battery never dropped below
  3.99 V across the whole record, which rules that out entirely.

- **First real evidence it worked.** An 85-minute stretch out of signal — the same
  condition that caused a restart in the car — with the phone's free-memory figure
  not moving by a single byte, at nearly three times the headroom the failing run
  had. Still not a complete proof: the phone never rejoined a network in that
  window, which is the other half of the problem.

- **The health log no longer throws away its own evidence.** It kept a fixed
  amount of history and, when full, deleted all of it and started again. That is
  exactly what happened during a restart nobody was watching: the record of *why*
  it restarted was erased half an hour later, before it could be read. It now
  keeps the most recent few hours instead.

### 🧭 Settings

- **Settings > WiFi auto-switch works again.** It was showing the Music icon, and
  choosing it opened the Music player instead of the toggle — so the screen was
  effectively unreachable. The two entries had been given the same internal ID and
  the menu picks the first match, so the WiFi row was standing in for Music.
  Renumbered.

- **The phone now checks its own menu for repeated IDs when it starts.** The same
  mistake had already shipped once before, as "Music opens Books", and was fixed
  in a way that moved the clash rather than removing it. Nothing was watching for
  it, so now something is.

### 🩺 Diagnostics

- **The health log can be read without disturbing what it measures.** Pulling the
  whole file kept truncating, and a truncated read loses the *end* — which is
  exactly where the reason for the last restart is written. Retrying was worse
  than the problem: around thirty fetches, each its own connection, on a phone
  whose scarcest resource is the memory all of that consumes. It can now hand back
  just the most recent part, so one request gets what matters.

## 2026-08-14 — Music player, e-reader, real battery life, and working firmware updates

The biggest release so far. Two whole new apps, a serious pass on power, and the
over-the-air updater brought back from the dead and pointed at this repo.

### 🎵 Music player (new)
- **Plays MP3 and WAV straight from the SD card** — no converting anything.
  Menu > Music. Stereo through the headphone jack, mono to the loudspeaker
  otherwise, and it follows the jack live: unplug mid-song and it carries on out
  of the speaker from the same place.
- **The four side buttons are the transport**, from the top: play/pause, next
  (**hold** for previous), volume up, volume down. Holding "previous" behaves
  like any music player — within 3 seconds it goes back a track, after that it
  restarts the current one.
- **Playback keeps going when you leave the screen.** Read a book or check the
  mesh with music playing; tracks advance on their own.
- Shuffle and repeat (off / all / one), a library scanned from `/music`,
  `/books`, `/roms` and the card root, and its **own WiFi uploader**.
- **Volume starts low** (−18 dB) and is remembered until the phone restarts. It
  is deliberately separate from the call volume, so a quiet album can never
  leave you unable to hear the next phone call.
- Pausing and playing **resumes where you were** rather than restarting.

### 📖 E-reader (new)
- **Reads EPUB and plain text from the card**, with pictures inline. Menu >
  Books.
- **Your place is saved** and survives the phone losing power at any instant.
  Position is stored as (chapter, character offset), not a page number, so it
  still means something after a font-size change — and on a different device.
- **Greyscale JPEGs decode**, which the ESP32's built-in decoder cannot do
  (it is 3-component only, and most book art is 1-component).
- **Book sync over LoRa** — share your reading position with another device on a
  private channel. Jumps are confirmed, never taken silently.

### 🔋 Battery life
- **The phone now idles.** The main loop used to spin flat out at 240 MHz
  forever, screen off, doing nothing. It now sleeps between passes.
- **The CPU drops to 80 MHz when nothing is happening** and returns to 240 the
  moment the screen comes on, music plays, a call starts, the emulator runs or
  an upload begins.
- **WiFi modem sleep is on.** The receiver used to stay powered continuously;
  it now parks between beacons. Nothing is missed — buffered traffic still
  arrives, a fraction of a second later.
- **Scanning backs off when there is nothing to join.** Out of range, it used to
  scan every 2 minutes forever; now it eases to 5 after a sustained absence, and
  snaps back the moment anything connects.

### 🛠 Diagnostics
- **The phone records why it restarted.** The reset reason (crash, watchdog,
  brownout, or a normal power-on) is logged at boot, which turns "it rebooted
  again" into a specific answer.
- **A health line every minute** — free memory, the lowest it has ever been,
  largest free block, battery, CPU speed — written to `/health.log` on the card
  so it survives both a reboot and being unplugged.
- **Read it over WiFi** at `http://wiphone.local/log` with an upload screen open.

### 📤 File transfers
- **The upload page works on a phone now.** It was built around drag-and-drop,
  which does not exist on a touch screen; there is a proper "Choose files"
  button, and the file picker no longer greys everything out.
- **It stops after one page load — fixed.** The server kept the single
  connection slot occupied, so nothing loaded again until it was restarted.
- **Opening an uploader no longer knocks the phone off WiFi.** If the radio was
  still associating it would tear the connection down and host its own network
  instead; and on the way out it never reconnected.
- **Uploads are faster and retry themselves** — a weak link no longer means
  starting over by hand.
- **The screen stays awake while a server is running**, so a long transfer does
  not go dark halfway and look like a crash.

### ⬆️ Firmware updates over the air
- **Settings > Firmware settings works again**, and now points at this repo.
- It was not a dead link: the certificate the phone pinned **expired in April
  2021**, so every check failed the TLS handshake regardless of the URL. It now
  trusts a proper root certificate (valid to 2035) instead of a pinned leaf.
- Releases are published from `ota/` in this repo; `tools/publish_ota.sh` builds
  the binary, writes the manifest and bumps the version together.

### Known issues
- **Slight crackle in music playback.** Much improved — the decoder used to be
  rate-limited so it could never catch up after any interruption — but not
  entirely gone. The now-playing screen shows a `gaps:` counter to help pin it
  down.
- **No seeking within a track**, and no track/artist tags: the library lists
  filenames.

## 2026-07-09 — Reliable menu input, forgiving screen-off, working WiFi auto-switch

### Input
- **Menus register one press per press.** Buttons no longer double-fire, and
  holding the d-pad no longer auto-scrolls the menu — UI key events are now
  strictly edge-triggered (one event per physical press, re-armed only by a
  real release). The Game Boy emulator's input path is untouched.
- **Triple-tap-to-sleep is easy now** — three taps of the top-right button
  with up to ½ second between them (was a near-impossible fixed 0.7 s window
  for all three).

### WiFi
- **Auto-switch actually works.** It reliably scans and connects to the
  strongest saved network now (previously it could sit disconnected
  indefinitely). It also scans more often while disconnected (every 2 minutes)
  so it finds a network without waiting.

## 2026-07-07 — Game Boy polish, big-ROM support, WiFi auto-switch, input reliability

### Game Boy emulator
- **Sound!** The emulated Game Boy APU plays through the phone's speaker
  (32 kHz stereo over I2S). The **top two side keys** adjust volume in-game.
  Audio can never slow the game down: if the audio path misbehaves, the game
  drops sound and keeps running at full speed.
- **Full speed on heavy games** — even demanding Game Boy Color titles
  (e.g. Resident Evil Gaiden: 44% → ~100%). How: removed a PSRAM silicon
  workaround the phone's rev-3 chip doesn't need, hot emulator paths moved to
  zero-wait IRAM, `-O2/-O3` on the emulator core, cache-friendly screen
  blitting, and adaptive frameskip (heavy scenes drop *display* frames, never
  game speed).
- **Big ROMs (4 MB carts) now playable** — ROMs too large to hold in RAM are
  streamed from the SD card in 16 KB banks on demand.
- **Save states fixed** — saving used to reboot the phone (a task stack was
  4× smaller than intended).
- **Transfer ROMs & Help moved into the Game Boy app** — top two rows of the
  game list. The help screen documents controls and everything else.
- **Multi-file ROM upload fixed** — dropping several ROMs at once used to
  reboot the phone (RAM starvation + watchdog); uploads now go one-per-request
  with per-file progress, and the transfer screen frees the emulator's memory
  and the unused Bluetooth reserve before serving.
- **Reliable buttons** — held directions no longer stutter or stick: fixed a
  keypad-handler bug that wiped held keys, added I2C error retry, periodic
  FIFO draining, and a 40 ms hardware "still pressed" heartbeat so a lost
  release un-sticks in a third of a second.
- Long ROM names marquee-scroll in the picker; honest error screens (a ROM
  that can't load no longer silently launches the built-in game).

### Phone
- **WiFi auto-switch** — the phone periodically scans (battery-friendly, async)
  and hops to the strongest *saved* network; if the screen wakes with no
  connection it scans immediately. Only switches when meaningfully stronger
  (10 dB hysteresis). Toggle under **Settings → WiFi auto-switch**.
- **Status bar shows the connected network name** (small text, auto-shortened).
- **"Edit current network" no longer freezes** — a toggle handler ran on every
  keystroke and blocked 5 s reconnecting each time.
- Pressing End during a game no longer triggers the phone's call/hang-up UI.

## 2026-07-05 — Meshtastic: channels, relaying, unread counts

- Multiple channels with AES-128/256; import channels from a Meshtastic share
  link fully offline (apply from a received DM).
- Mesh client role: relays other nodes' packets (flood routing) to extend range.
- Configurable hop limit; per-chat unread counts; status-bar unread icon.
- Game Boy Color emulator first milestones: playable → full-speed dual-core,
  ROM picker, WiFi ROM transfer, save states, pause menu, 1:1/Fill scaling.

## 2026-07-04 — Initial public release

- PlatformIO build of the stock WiPhone firmware (Arduino-IDE only upstream).
- Meshtastic on the WiPhone's SX1276 LoRa radio: two-way encrypted text with
  standard Meshtastic nodes on LongFast (US), chats/threads UI, direct
  messages, node names, persistent history, notifications (icon, popup, sound,
  vibration), editable node name, low-power green/black theme.

# WiPhone — session handoff

## ▶▶ STATE NOW (header refreshed 2026-10-01 07:30)

Read this first; everything below it is narrative.

📱 **2026-10-11 ~08:40: NH DEVICES COMPANION, the Android app (`android/`), 1.0 beta. Replaces gpsdRelay for the phone hotspot.**
- **Why:** gpsdRelay stops sending within 1-3 min of a Samsung screen lock (no POST_NOTIFICATIONS). {user} asked for a
  small app, as automatic as possible, which can grow into the companion app for all NH-firmware devices (file
  transfer later). Display name **NH Companion**; package `io.github.nikguy321.nhcompanion` (permanent).
- **Design (android/README.md):**
  - Location foreground service with a real notification, port 10110.
  - Admits only connections that arrive on a tethering interface. Refuses the networks Android says the phone uses,
    and loopback (debug builds allow loopback).
  - GPS on only while a device is connected, plus 5 min. Partial wake lock while the GPS is on.
  - Relays the chip's NMEA; builds GGA/RMC from the fix only when the chip gives no text.
  - Sends a "no fix" pair after 4 s of silence.
  - Setup button: location, notifications, battery exemption, "all the time", then the hotspot page.
- **Toolchain:** JDK 17 + SDK + emulator in `~/android-dev` (env `JAVA_HOME=~/android-dev/jdk/Contents/Home`,
  `ANDROID_HOME=~/android-dev/sdk`, AVD `hgps35` in `~/android-dev/avd`). Build with
  `~/android-dev/gradle-8.10.2/bin/gradle` (or `./gradlew`).
  - 🔑 The release key is `~/android-dev/private/nhcompanion-release.jks`, passwords in `~/.gradle/gradle.properties`
    (both mode 600, never in a repo). **Losing it means no updates over installed copies:** {user} should back it up.
- **Proven on the Android 15 emulator:**
  - 28 unit tests;
  - setup flow;
  - `tools/phone_stream_check.cpp` (the firmware's gate on the Mac) kept 77-81 fixes per 90 s with the screen off and
    in forced deep Doze, over a non-loopback connection;
  - reboot auto-start and crash restart;
  - mobile-data side refused; release build refuses loopback.
  - The emulator never passes chip NMEA text to apps, so the relay path is proven only by unit tests.
- **Independent review, then fixed (2026-10-11 ~08:45):**
  - HIGH: an app on the phone could read the position by connecting to the phone's own hotspot address
    (local delivery, neither end loopback). Now refused when the remote is one of the phone's own or upstream
    addresses; Wi-Fi Direct/Aware peers are refused; the device must be in the local interface's subnet.
    Proven on the emulator: `nc` from adb shell to 10.0.2.15 is refused "from an app on this phone".
  - The reminder extra is acted on once and only when sharing is still switched on. The PendingIntents have
    distinct request codes.
  - A sticky restart without "all the time" location posts the "paused, tap" reminder instead of failing
    silently. Proven: on Android 15 the restart's startForeground throws, the reminder shows, one tap resumes.
  - Hub add/stop fenced; onDestroy stops the hub directly; a `destroyed` flag guards every runnable.
  - Keep-alive is GGA only (a "V" RMC could veto a same-second fix); mock locations are never sent;
    setupStep survives rotation; the setup flow no longer stalls on switched-off notifications.
  - 32 unit tests. dist APK sha256 `9e0392e3...`, cert SHA-256 `0af3ab08...`.
- **REAL NOTE 20 TEST, same day (Note 20 Ultra SM-N986U, Android 13; over adb + an on-phone logcat recorder):**
  - 1.0 kept its process and socket alive when locked (keep-alives flowed; gpsdRelay could not), but GPS data
    stopped a few minutes after the phone was put down. On unlock it came straight back.
  - CAUSE: Samsung nsflp `OldStopController_FLP` "stop GPS by sensor". It switches GPS off for ALL apps when the
    phone is stationary, the screen is off and it is unplugged, unless `isMeasurementsMode` is true. That flag is
    Developer options → "Force full GNSS measurements" (Global `enable_gnss_raw_meas_full_tracking`).
  - With it ON: a live fix for 16 min, locked, unplugged and still.
  - Did NOT help: the fused-provider helper; the app's own raw-measurements registration (even with full
    tracking). Both were removed.
  - Plugged-in tests never trigger it (`isPlugged`), and `dumpsys battery unplug` doesn't fool it.
- **1.2 (versionCode 3), pushed as `android/dist/nh-companion.apk`** (stable name).
  - A Samsung setup row and a "Set up everything" step explain the switch and open Developer options. They warn
    that turning Developer options off resets it.
  - A Diagnostics timeline; dark-mode switch and checkbox tints.
  - Installed on {user}'s phone by adb, and running.
- X4 fork `5e8e2b77` (local; push after this repo): Locate Me waits up to 30 s while the phone answers "no fix yet".
  `~/x4pro/builds/1011-warmgps.bin` FLASHED 11:27, bench PASS with `nmea_phone_stub.py --warm 15` (pushed).

🛰 **2026-10-11 morning: the woods GPS stale-fix fix + the phone-hotspot retry + the FIRST REAL gpsdRelay TEST. BOTH PHONES flashed (`Oct 11 2026 06:22:47`).**
- **GPS plate (phone 2):** `selfGpsApply` (self_pos) owns the UART fix. Only a sentence that measured a position stamps it
  (RMC A + measuring mode, or GGA quality 1-5 with its own coordinates). A no-fix GGA can only make a fix look worse. An
  RMC is rated by its paired GGA (3 s), and a bad older rating carries. The Almanac rates "last GPS" by the fix's own
  rating. test_selfpos 289 (22 failed on the old rule).
- **Hotspot retry:** a joined network whose BSSID is locally administered (a phone hotspot) is asked again every 30 s,
  never the 5-min dead-round wait. Proven live: after gpsdRelay restarted, phone 1 found it in about 20 s.
- **REAL TEST, {user}'s Note 20 + gpsdRelay, phone 1 on its hotspot:**
  - "this network: fix, 12 sats, HDOP 0.7"; tz "confirmed by phone GPS".
  - ⚠ gpsdRelay adds servers switched OFF (port refused); now in docs.
  - ⚠ With the screen locked, gpsdRelay stopped sending within about a minute (connections accepted, no data).
  - Separately, phone 1's auto-switch then hopped to the stronger home WiFi, because the stream was no longer live.
  - Second test running: generation ticked + "Keep open" in Recents.
  - Second test (generation ticked + Keep open + every Samsung battery setting): the fix was live ~3 min after
    the lock, then gone. The port alternated refused / accepted-no-data, which looks like Samsung stopping and
    restarting the service.
  - Root cause found in gpsdRelay 2.2's manifest: it targets SDK 35 with no POST_NOTIFICATIONS, so its FGS
    notification can never show (App info has no Notifications row).
  - Works whenever the Note 20 is awake; that is enough for tz Auto (2 looks) and the map's "last phone".
  - Share GPS is not in the Play Store for the Note 20.
  - Later ({user}: "we can work on this later"): a tiny purpose-built Android app (a proper location FGS +
    notification), or a gpsdRelay patch/issue upstream.
  - Phone 1's WiFi auto-switch was turned back ON after the test (log `[autosw] ... en=1`).

📶 **2026-10-11 ~00:05: WI-FI LOCATION (round 2), BOTH PHONES flashed by cable (0.9.82-NH dev; phone 1 BOOT `Oct 11 2026 00:01:25`, phone 2 `Oct 10 2026 23:40:59`, the same source), mute ON.**
- **Setting:** My node > Wi-Fi location = off (default) / map only / map + mesh. The serial console may set only off|map.
- **Rule** (the owner's): >= 4 DEVICES at -85 dBm or stronger, split into two halves, both beaconDB answers HTTP 200 with
  no fallback, each accurate to 1-100 m, and agreeing within the larger accuracy. Otherwise NOTHING.
  - Also checked against the last measured fix (300 m/s jump check).
  - Never IP: considerIp false, ipf false.
  - The phone hotspot's BSSID is never sent.
- **Mesh** (only "map + mesh", chosen on the screen):
  - precision_bits 17, location_source unset, the cell centre;
  - sent only after a confirming scan within 60 s, and two agreeing scans >= 60 s apart before the first send;
  - "precise fix wins"; tz_auto still refuses Wi-Fi fixes.
- **Code:** geoloc.{h,cpp} (pure: filters, sameDevice, split, parser, judge, schedule) + geoloc_net.{h,cpp} (NVS, scan copy,
  HTTPS job NET_JOB_GEO), plus roots ISRG X1+X2+YE in isrg_roots.h (YE checked by its key, like YR).
- Workflow wf_159e9994-ae3: core, integrate, 3 reviewers, 12 findings, all fixed. test_geoloc 278, suite exit 0.
- **Bench:**
  - Home: phone 1 "24 heard ... 3 devices: too few", phone 2 "1 device". Nothing sent, nothing shown.
  - The TRANSPORT was proven with a temporary build (min devices 2, -95 dBm; NOT committed, phone 1 re-flashed with the
    real build after): half A HTTP 404 notFound, dns 34 ms, connect+handshake 5,837 ms (the P-384 chain verifies), worker
    stack floor 2,960 of 8,192, internal largest floor 42,864. Half B was skipped as designed. Verdict no-fix, nothing
    reported.
  - `netloc answer` (bench, never sent): the map draws "Wi-Fi +/-70 m" with a dotted ring.
- **Both phones left on "map only"** ({user} asked for network location when accurate). "Map + mesh" is his choice on the
  screen.
- **Owed:** a real located fix in a busy place (hotel/office), and a map + mesh beacon arriving at COVEY with precision 17.

🕑 **2026-10-10 night: AUTOMATIC TIME ZONE, BOTH PHONES flashed by cable (0.9.82-NH dev, BOOT `build=Oct 10 2026 20:28:13`, mute ON).**
- **Why:** for travel across time zones. The clock was a typed offset with no DST, so every time and legal light would
  have been 3 h off. Settings > Time zone (was "Time offset") now has three modes:
  - **Auto** (default): a zone from a MEASURED fix only (own GPS, or the phone over the hotspot). Never a bench, IP or
    Wi-Fi fix. Same zone on two looks ≥ 2 min apart; never near a zone line or at sea. iPhone fixes need 10 km clear.
  - **Named zone** (10 of them): DST changes by itself.
  - **Custom:** the typed offset, as before.
- **Code:**
  - `tz_grid.{h,cpp}` + `tz_grid_data.h`: 0.05° RLE grid, about 82 KB flash, generated by `tools/gen_tz_grid.py`
    from timezone-boundary-builder 2026d (ODbL, see THIRD-PARTY-NOTICES). Shared byte-for-byte with COVEY's
    `tzgrid.bin`.
  - `tz_auto` = the pure decision logic; `tz_service` = the glue.
  - NVS `wptime` holds the zone. configs.ini keeps only the TYPED offset; SPIFFS writes would stall the loop.
- **Tests:** test_tzgrid 15,993 checks, test_tzauto 5,048, suite exit 0. Workflow wf_55b07e72-752: data, integrate, 2
  reviewers per device, 7 findings, all fixed.
- **Bench, phone 1:**
  - `tz test` correct for Columbus OH, Olympia, Phoenix (no DST), Chicago, Denver, Honolulu, Anchorage; a Tennessee
    point near the line is refused for a phone fix; open sea is refused.
  - `tz eastern` -> UTC-4 with "DST ends Sun Nov 1" -> `tz auto` keeps it until a fix finds another clock.
  - Both phones left on **Pacific, Auto** (DST automatic Nov 1).
  - The Settings screen renders on 240x320.
- **Not provable at home:** Auto switching needs a real fix (`phonepos at` fixes are bench, refused on purpose). Expect
  a switch about 2-2.5 min after the first good fix in a new zone, plus a one-time banner.

📍 **2026-10-10: PHONE LOCATION, BOTH PHONES flashed by cable (0.9.82-NH dev, BOOT `build=Oct 10 2026 13:07:40`, mute ON).**
Read `docs/phone-location.md` first. It has the user steps for gpsdRelay, Share GPS and GPS 2 IP, plus every status line.
- **Rule** (owner): own GPS > the phone's GPS over its hotspot > nothing. NEVER IP geolocation: on a phone hotspot it
  lands at the carrier's internet exit, hundreds of km off.
- **Code.** `self_pos.{h,cpp}` is the pure part, test_selfpos 168/0:
  - slots, pick, freshness;
  - the per-GGA phone gate, with no sticky values;
  - 2 s settle plus a time check;
  - two fixes in a row must agree (gpsdRelay's generated coordinates can be about 11 km off);
  - probe/back-off;
  - GPS 2 IP's stand-in 8 sats / HDOP 0.9, read as "not said", so iPhone fixes are shown and never sent.
- `phone_pos.{h,cpp}` is the non-blocking socket glue. It never sets the clock and never joins busy/hardBusy.
- `meshPosBuildEx` encodes location_source + precision_bits: phone = EXTERNAL/32, plate = INTERNAL/32.
  Inbound fields 5/23 are parsed.
- Consent:
  - bench fixes (`pos inject`, `phonepos at`) are never beaconed and are sent as MANUAL;
  - the cable can never arm a public channel;
  - the upgrade turns Phone location OFF where a beacon was armed with the GPS off or to a public channel.
- Built by workflow wf_817d618e-4cd: core, integrate, 3 reviewers, 9 findings, all fixed. Plus the GPS 2 IP rule (mine).
- **Bench** with the Mac stub: `tools/nmea_phone_stub.py --port 10111 --bind <mac>`, then `phonepos at <mac>:10111`.
  - Normal stream: "this network: fix, 9 sats", map "me (phone)", `pos` source = phone [BENCH].
  - The stub's GPS 2 IP mode on 11123: "iPhone app gives no accuracy: shown, not sent".
  - Home router: "no app found on this network", backs off to 5 min.
  - 45-min soak on phone 1 (stub at 1 Hz): 5,946 sentences, 0 bad checksums, 0 WiFi disconnects, 0 LOOP STALLs,
    internal heap flat (free 63,204, largest 61,404), CPU 80 MHz, 0 PLL re-locks with the radio on.
- **Owed:**
  - a real Note 20 + gpsdRelay test ({user} installed it 10-10), then `pos now` on a private channel: COVEY should show
    "External";
  - COVEY reading precision_bits;
  - round 2 = Wi-Fi location (BeaconDB two-half agreement), map-only until receivers draw rings. The e-reader fork
    already has it (e852a9fc).

📁 **2026-10-09 evening: `mv </from> </to>` on the console, BOTH PHONES flashed by cable (0.9.82-NH DEV, BOOT
`build=Oct  9 2026 18:58:21`, hash verified, mute ON).** {user} wanted a book's file name to match on every reader. KOSync's
second document id is md5(basename), so a book whose names differ across readers only matches on the partial MD5. The phones
had no rename, and `rm` plus a re-upload would delete a file, so `mv` does a FAT rename in place. It quotes paths with spaces,
never overwrites, and refuses folders and relative names. It also refuses while Books/Music/Files/Photos/the Game Boy are on
screen, a track is loaded (the player outlives its screen and reopens by path) or the uploader is up (a chunked upload holds
its file between passes; FF_FS_LOCK is 0 here). It prints the verdict first, because say() cuts lines at 192 bytes. 🔑 Reading
places survive a rename, since positions.cbs keys on epub ids. KOSync's per-book NVS memo (movedAt, offeredSig, unsent) and
the park ledger key on md5(basename), so `kosyncBookRenamed` -> `kosyncRekeyBook` (host-tested, test_kosync 2129/0) carries
them to the new name. Run on both phones: one book renamed to the COVEY/X4 spelling, "KOSync memory ... moved" on each.
A two-lens review found 0 blockers and 5 LOW items; all fixed.

🛰 **2026-10-05 evening: 0.9.82-NH DEV - map tiles from the Mac over WiFi, on BOTH PHONES by cable (NOT released; the
web flasher still serves 0.9.81-NH).** {user} chose "over WiFi from the Mac" over pulling the SD cards. `maps dlurl <template>
[area]`: the custom source (3) writes into a chosen map folder (`otm` = the real OpenTopoMap map); only the FOLDER changes
(Job.area / Job.url copies; the OTM z17 throttle stays keyed on the server key "custom"; custom jobs still never persist, so a
reboot or stop ends one - re-send both lines, the re-run skips what is on the card). Refused areas: `elev` any case and any
name ending in `.` (FatFs drops trailing dots: `elev.` IS /maps/elev). `tools/tile_relay.py` serves ~/tiles-master read-only
on en0 (only /<src>/z/x/y.png; 404 = the phone's "no tile"). MEASURED on phone 1: a z17 tile 124 ms from the Mac vs 1,220 ms
from OTM (tlstest x20); a whole relay job 1.2-1.3 s/tile (decode + 128 KB card write dominate) vs OTM 2.07 s at z17 / ~4 s
below; USGS gains little (252 vs 382 ms GET, 1.2-2.1 s all in). Review: 5 dimensions + skeptics, 8 real findings fixed
(trailing-dot alias, st.source/label widths, report showing the live relay, form preselect by folder, 1.2 s estimate, a
404-ing docs example, privacy wording), a second pass confirmed all fixed. Also: WX User-Agent built from FIRMWARE_VERSION
(the owed item). Suite exit 0; test_tileplan 177/177.

🚀 **2026-10-04 08:2x: 0.9.81-NH RELEASED** ({user}: "Yes do both" - the release and the X4 Crossword). Clean build (BOOT
`build=Oct  4 2026 08:20:41`, RAM 27.8 %, flash 45.0 %), suite exit 0, stage regenerated (boot part byte-identical to
0.9.80's; app sha `95ff9d55…`, = boot_app0 + firmware.bin). BOTH PHONES flashed by cable with this build (app @0x10000,
hash verified): 0.9.81-NH, mute ON, mesh names and the Gemini key intact. Four read-only reviewers: bytes PASS (fresh
merge+split == stage, nvs in neither part, header 16MB/dio/80m), page/publish PASS (only manifest + app change on
gh-pages), docs PASS after fixes, privacy: the BINARY and diff are clean, but the full-tree scan found OLD public
personal data - the home position in 4 comments + this file (scrubbed in cdbc119; still in HISTORY) and the voip.ms
sub-account + the SMS-mirror phone number in code comments, tests, CHANGELOG and this file (SCRUBBED in 981acab on
{user}'s "Sure swap it": 425-555-0142 / 100000_sample, vectors regenerated from COVEY's smsmirror.py; HISTORY still holds
both - no rewrite unless {user} asks; COVEY's private repo keeps the real ones). Fixed before publishing: `wiphone_send.py --app keys`
resumed by size (a same-length new key was skipped and "verified"; now always sent whole), README key/WiFi/Files paths,
the 3-hourly wording, the 2-hour topic needs a trusted clock, the 20-question/16 KB cap. OWED next round: WX_USER_AGENT
in weather.h hard-codes "0.9.81" (build it from FIRMWARE_VERSION); make_webflasher's gate checks timestamps only.
✅ **LIVE 2026-10-04 ~08:55:** `main` = `2f1d2c8` pushed (cdbc119 privacy + the release commit); gh-pages republished;
ALL SIX live files sha-match the stage on plain and cache-busted URLs (app `95ff9d55…`, manifest `db9377a9…`, index/boot/
bundle/THIRD_PARTY unchanged); the page reads "Latest version: 0.9.81-NH", no console errors. Not yet installed through
a real browser (both phones got the same build by cable).

🧪 **2026-10-03: BOTH PHONES RUN AN UNCOMMITTED 0.9.81-NH DEV BUILD** (app only @0x10000, hash verified). It adds
the **AI app** (Gemini; `app_ai`, `ai_net`, `gemini.{h,cpp}`, tests/test_gemini.cpp), the `/API Keys` folder made at
boot (WiPhone.ino, after SD.begin) and `up on keys` (`tools/wiphone_send.py --app keys gemini.txt`). The key is NEVER
in this repo: it lives on the cards (`/API Keys/gemini.txt`) and outside every repo on the Mac. `.gitignore` ignores
gemini.txt / chutes.txt / `/API Keys/`. The roots are pinned (GTS R1 + R4) with NO override file on purpose (the
keys uploader has no password and can run on an open hotspot). Free Flash = 20 answers a DAY per key: bench with
Flash-Lite. Web flasher still serves 0.9.80-NH.
🌦 **2026-10-03 evening: WEATHER on the Almanac (0.9.81-NH dev), both phones flashed + proven.** Open-Meteo + NWS alerts
through ONE shared HTTPS worker (https_worker.cpp: AI and WX job kinds, netRequestActive in busy/hardBusy/10-tick); pinned
ISRG Root X1 + Root YR; `-Wl,--wrap=mbedtls_ecp_grp_id_list` puts P-256 first (an unknown curve id would fail EVERY
handshake - keep the list exact). Bench: `wx fetch` -> ok in 5.6-6.0 s (Open-Meteo connect+handshake 2.7-3.1 s, NWS
2.0-2.1 s), worker stack floor ~3.2 KB. AI (Google), OTM tiles and GitHub still connect; OTM accepts ONLY P-256/X25519,
so tiles negotiate the same curve before and after the wrap (phone timings are noisy: 7-15 s first GET incl. the
phone-side ECDSA work). Cache /wx/weather.txt. `wx`, `wx fetch`, `wx show`, `wx clear`, `open weather`.
✅ **Bench 2026-10-03 13:00-14:00:** both phones answer real questions (pinned-root TLS verifies; worker stack
floor ~3.1 KB of 8 KB). 🛑 The handshake is ~4.5 s of software ECC and Google resets one that takes > ~10 s
(`start_ssl_client: -80`): with the screen on and a 1-tick loop it failed 3/3; the loop now sleeps 10 ms a pass while
a question is in flight (3/3 answered, 3.9-7.6 s) and a playing track PAUSES for the question ({user}'s call; with music
the handshake still failed). README "AI (Gemini)" is a get-a-key / add-it / use-it guide. NOT committed.

🚀 **2026-10-01 07:2x: 0.9.80-NH RELEASED** ({user}: "Push commit and update web flasher"). Clean build (BOOT
`build=Oct  1 2026 06:46:39`, RAM 27.6 %, flash 43.7 %), suite exit 0, stage regenerated (make_webflasher.sh; the boot
part is byte-identical to 0.9.79's), four independent read-only reviewers passed (stage bytes == a fresh merge+split,
nvs in neither part, bootloader 16MB/dio/80m; privacy: nothing new public; docs claims vs code; page/manifest shape).
One doc fix from the review: civil twilight at 47 N is 31-41 min (31 equinoxes, 36 December, 41 June), longer than 30
all year - README/CHANGELOG/almanac.md said "from about October"/"up to 36" (COVEY D-164 text corrected too, cf88984).
`main` = `eb87221` pushed (10a81a6 + the release commit); gh-pages republished; ALL SIX live files sha-match the stage
on plain and cache-busted URLs (app `5c3be7a1…`, manifest `67df7805…`); the page reads "Latest version: 0.9.80-NH", no
console errors. NOT yet proven by a real browser install. Phone 1 is on the Mac's USB (025A3F65) still on 0.9.79-NH -
{user}'s to install from the page; phone 2 runs the 09-27 17:51 build of the same source (not the published bytes).
Phone 1 still needs the elevation tiles (card day step 4 or `wiphone_send.py --app maps --tree ~/elev-master/elev`).
COVEY D-164 pushed (cc03066, cf88984). The panicwatch bridges are NOT running.

🌙 **2026-09-27: 0.9.80-NH BUILT, UNCOMMITTED (branch `kosync`) - THE ALMANAC, ALTITUDE ON THE MAP, METRIC/US UNITS,
ELEVATION DOWNLOADS. Phone 2 runs a snapshot of it; phone 1 is with {user} on 0.9.79-NH. Commit/push/publish only when
{user} says.** Spec `docs/almanac.md`; CHANGELOG 0.9.80-NH has the user-facing list. COVEY got the same app (its D-164,
DEPLOYED, device 79/79) and `tools/almanac_crosscheck.py` there proves its Python == this C++ (0 differences - re-run it
whenever astro/wmm/geo_grid change). Ground truth: `tests/vectors_almanac.{h,json}` from `tools/gen_almanac_vectors.py`
(`pip install --user ephem utm mgrs`; NOAA's WMM file in `tools/data/`) - regenerate and copy the JSON to COVEY's
`tests/fixtures/` together. **Proven on phone 2:** `almanac 47.4957,-121.7868` == COVEY == PyEphem (every event within
1 s); the day's tables = ~154 ms CPU at 160 MHz, now sliced (longest measured slice 27.8 ms; minute-tick rebuild 10 ms;
it WAS a 334 ms LOOP STALL); map row "Elev 116m"; `elev` reads ~10 ms; a real `maps dl 2 46.853 -121.760 2 12 1` fetched
4 terrain tiles from S3 over HTTPS (heap floor 37.7 KB, stack floor 3,324 of 8,192) and Rainier's summit reads 4,391.2 m
== the Mac-built tile (true 4,392 m); GPS SET THE CLOCK at boot (`clock: source gps`). Elevation master
`~/elev-master/elev` (2,532 tiles, 317 MB) is on phone 2's card and on COVEY (/root/covey-elev); phone 1 gets it on
card day (`cardday.sh` step 4) or `wiphone_send.py --app maps --tree ~/elev-master/elev`. 🛑 **The phone's clock is a
fixed offset**: days past a US clock change are shown in the offset they WILL have (a review found Nov 7 seen from Oct 27
read legal light an hour late); new setting `wpmesh/almdst`. Legal light default = WA 30-min rule, rounded INWARD.
Bench: unlock over the cable = `key ok *`; `maps dl` takes a source INDEX (2 = OTM); `shot` itself is a ~4 s stall.
FINAL build (17:51, 0.9.80-NH) on phone 2: six Left/Right day steps, NO LOOP STALL; longest slice 28 ms, a day 147 ms CPU in
6 slices (on screen 0.42 s after the key), builds ~10 ms, paints 27-32 ms + the band push (~48 ms, estimated). ⚠ OPEN (0.9.79's
GPS time, not this work): at this boot GPS set the clock 4 s AHEAD and NTP moved it back 30 s later ("GPS disagrees with a
fresh NTP by 3998 ms"); at 11:18 the gap was +0.8 s (NTP's dropped fraction). Suspect the first 'A' RMC pair after a warm
start - look at it before trusting a GPS-only clock to the second (legal light rounds inward to the minute, so not urgent).

✅ **2026-09-26 13:40: 0.9.79-NH IS LIVE AND PROVEN THROUGH A REAL BROWSER.** {user}'s second install on phone 1 from the
republished page: "Works!" — phone 1 boots the exact published bytes (BOOT `build=Sep 26 2026 12:58:56`), mesh key
`mpHJ…`, KOSync id `52de577b…` and mute all intact. `main` = `a6aca39` pushed; live shas of all six files verified
against the stage. Both phones on 0.9.79-NH (phone 2 by cable, same bytes). The X4 Pro arrives Sunday 2026-09-27.

🩹 **2026-09-26 13:25: THE FIRST REAL BROWSER INSTALL FOUND A PAGE BUG — FIXED AND REPUBLISHED.** {user}'s install
ended "Failed: loader.after is not a function": the write had finished, the page's reset call named a method esptool-js
0.4.5 does not have (`after()`; it has `hardReset()`), the catch said "Failed" and phone 1 sat dark in the bootloader with
the new firmware on it (esptool `--after hard_reset chip_id` brought it up: 0.9.79-NH, mesh key + KOSync id intact — the
two parts kept NVS through a REAL browser). Fixed: `hardReset()`; a failed reset now reports "installed, unplug and replug";
esptool-js is VENDORED (`webflasher/esptool-js-0.4.5.bundle.js`, sha `ac4fab0d…`, THIRD_PARTY.md) and publish copies it;
`check_webflasher_split.py` §6 proves every `loader.*(`/`transport.*(` the page calls is defined in the served bundle
(comments stripped — the guard first tripped on its own explanatory comment). ⚠ LESSON: a mock that mirrors the page's
assumptions proves nothing about the library — my mock had `after()`. Test against the served bytes, or the real thing.

🚀 **2026-09-26 13:10: 0.9.79-NH RELEASED** ({user}: "push and commit and publish"; splash "looks good", ringtone "sounds
good"). `kosync` fast-forwarded into `main`, pushed; the installer stage regenerated from the release build
(make_webflasher.sh; `Ver. 0.9.79-NH` read out of the app part) and published to gh-pages as the two parts; live shas
verified against the stage (see the block below this one for the flasher's two-part story). Both phones run the
12:48:05 build of the same source. The X4 Pro arrives Sunday 2026-09-27 (the [[x4-reader-sync-idea]] memory and
docs/booksync-simple-plan.md Bench 5: `http://covey.local:8088` as the hub URL first). Not released: the OTA path
(`ota/` is 0.9.2 since Aug; the transport cannot work on this heap), unchanged as in 0.9.78.

🏷 **2026-09-26 12:50: THE VERSION IS `0.9.79-NH`** ({user}'s ask: mark his firmware apart from stock). Suffix, not
prefix — ota.cpp's `compVersions()` sscanf's `%u.%u.%u` and ignores the rest; a prefix parses as 0.0.0 and every server
version looks newer. Never `rc`/`db` in a tag (they mean things there). The splash draws "Custom firmware by {user} H."
above the version line (GUI.cpp SplashApp). make_webflasher.sh reads the tag from config.h into the manifest.

🎵 **2026-09-26 11:55: {user}'S LISTENING PASS PASSED ("Music works good! Passed all the tests you listed") — and his
photo showed TWO Now-playing TITLE bugs, both fixed and proven on phone 1 (build 11:49:47), phone 2 flashed after.**
(1) Titles overlapped after next ("Pulse" over "Space Explorer"): the two title lines were redrawn at 1 Hz with NO
clear — every other line of that screen clears its strip. Now drawn only when `cur != titleShown`, over both strips
cleared; `titleShown` is -2 after a panel clear and -1 for "Nothing playing" (which clears the whole panel).
`tests/check_music_title.py` pins the shape (3 mutations trip). (2) EVERY title lost its last character to line two
("My_Name_I" / "s" — the stray letter under {user}'s title was the "e" of "Pulse"): the pixel probe loop ran `k < len`
and never measured the whole title. The measuring is `WiPhone/music_title.h` (`musicTitleFit`/`musicTitleBreak`,
`tests/test_music_title.cpp` 13 checks). **12:25 the live A/B on the SPEAKER (headphones out; on headphones he could not
tell): tone → "clean seems the best" (default kept); `music swap` → "couldn't really tell a difference" (swap=on kept);
the chirp sounded (phone 2's notifications had been buzz-only, mode=1 — {user} set a sound mode) — and "it stopped
playing the song after the chirp": the pop's cut was kept as a pause at its place (0.9.79's fix for "every mesh message
restarted the song") and waited for F1. FIXED 12:35: the pop's teardown carries the track on (`musicPlayerResumeAfterPop`,
after `restore()`, `meshPopPlaying` down first — with it up, `startTrack()`'s notifyPopFinishFor tore the pop down a
second time on the first bench); a user's pause stays paused (`s_pausedByStop`); three contracts + mutations in
check_call_audio.py (18). Phone 2 proven: "Works on this one!". Ringtone by ear: Settings > Diagnostics, key 2 ({user}).
⚠ BENCH TRAPS learnt today: `key select` on Now playing opens the options MENU (the top side button `key f1` is
play/pause; `key 6`/`4` next/prev); a dimmed screen eats the first key; and NEVER drive menus blind on phone 2 — a
`key back, key select, 7×down, key ok` path walked Phonebook → contact → the CALL row ({user} took control).**

✅ **2026-09-26 11:35: §5.1-5.3 ARE ON BOTH PHONES AND PROVEN ON HARDWARE; COVEY D-163 IS DEPLOYED (in the right
order).** Both phones run `kosync` 76e4886 (build 11:04:27, two-part install, keys/mute kept; the 10:50 repair is squashed
into that one commit because the marker commit alone did not compile `test_kosync.cpp`). Proven on phone 2 (Leviathan
Wakes, window on the station IP 192.168.1.59): an x-auth-only GET (KOReader's shape) → `{}`, the same GET with
`X-BookSync: 1` or with `Authorization: Basic` → the record; the status line counted `no-percentage=2` and the reader
menu showed the `!` warning. The reader menu's **Sync my place** logged, in order, `the window ... waits for the home job
(at most 25 s)` → `home answered the push - opening the window ... now` → `KOSYNC window OPEN 300s` (⚠ serial
`kosync sync` is `kosyncSyncHome` ONLY — it never opens a window; the menu row is the path). COVEY was then deployed
(`8b614f3` + follow-ups `efa70cc`/`a801f9e`/`1b2c851`, 11:33; `test_kosync` 382 checks PASSED ON THE DEVICE, its first
run there) and the marker proven against it: phone 2's own pull read its record back (`nothing newer from another
device`, not "not on the server yet"), an unmarked curl of the same record got `{}`, a marked one the record. 🛑 Still
not on hardware: the hotspot window (needs a client — the X4 on Sunday), a real KOReader (Bench 3/4).

🔧 **2026-09-26 10:50: THE REVIEW'S FIVE FINDINGS ON §5.1-5.3 ARE REPAIRED — LOCAL on `kosync`, NOT pushed (now
FLASHED, see above).** 🛑 **DEPLOY ORDER (the one system-level finding): FLASH BOTH PHONES WITH THIS BUILD BEFORE COVEY D-163 GOES
LIVE.** The phone's own home client sent no marker (`kosyncBuildGet` was Accept/x-auth-user/x-auth-key/Connection, and
`tests/test_kosync.cpp` pinned exactly those bytes); COVEY D-163 answers `{}` to an UNMARKED GET when the stored
record has no XPointer — every record a phone wrote — so against D-163 a marker-less phone reads `{}` for its own
place, decides "this book is not on the server yet" (`evaluateOffer`) and PUTs its older place over the newer one.
Now `KOSYNC_MARKER_LINE` (`X-BookSync: 1`) goes on the phone's GET AND its PUT (one client, the same on both verbs,
as the fork's `applyAuthHeaders` does); `test_kosync` pins the new bytes and that the phone's own requests pass
`kosyncClientTakesPercentage` beside a valid x-auth pair; the string reads out of `firmware.bin`. The other four:
**(1)** `check_positions_write.py` ran its self-test FIRST on absolute presence, so a real `savePosition(true)`
planted in `windowAfterHome` came out as three `SELF-TEST FAILED` lines and never the file:line — now the real
sources are checked first (`CONTRACT BROKEN: kosync_sync.cpp:710 references savePosition(` on that scratch, exit 1)
and each plant is judged as a DELTA (line numbers ignored, a Counter of what it ADDS); **(2)** glue contract 12 now
requires a `windowAfterHome(` in the same block as every `s_pushWant = s_pullWant = false;` of `clientStep()`'s idle
branch (off WiFi, no memory) — deleting either release used to leave it green; both deletions are MUTATIONS (57 →
59); **(3)** `meshtastic_service.cpp` (the LoRa receive that parks) and `app_gbc_xfer.cpp` (the :80 pump that carries
every KOSync request) are on CARRIERS (both clean today; 10 → 12 plants); **(4)** "stock CrossPoint and Readest send
Basic" was wrong — CrossPoint sends Basic BESIDE x-auth-*; Readest sends Basic INSTEAD and is 401'd by `kosyncAuthOk`
before the marker is looked at, so nobody serves it on 0.9.79 (the plan's §5.5 "any account" is where that changes):
`kosync.h`, CHANGELOG, `test_kosync` reworded, with a test that the Basic-only shape is not authorised.

📗 **2026-09-26 10:20: THE PLAN'S §5.1-5.3 ARE BUILT ON THE PHONE — three commits, LOCAL on `kosync`, NOT pushed,
NOT flashed (a host-side session: the serial bridge owns both phones).** `8867db1` **§5.1 reply per client**: the
window's GET hands the record only to a reader carrying `X-BookSync` (the fork, once it sends it; and, since the
10:50 repair, the phone's own home client) or `Authorization` beside `x-auth-*` (stock CrossPoint; Readest sends Basic
INSTEAD and is 401'd first — served by nobody until §5.5); everyone else — KOReader sends `x-auth-*` only — gets `{}`
for the book ("No progress found", never page 1); PUTs unchanged (a KOReader push still parks and raises the card); the screen says `! A reader
that can't take a percentage asked (x1) - told 'No progress found' (KOReader?)`, one serial line a window, and the
close line counts `no-percentage=`. `9685d7c` **§5.2 window after home**: `kosyncSyncMyPlace` queues the home job
FIRST and the window opens from the loop on `finishJob`'s verdict (or when the idle branch drops the ask) or after
`KOSYNC_WINDOW_AFTER_HOME_MAX_MS` = 25 s (covers "did not answer": 6 s DNS give-up + 3 × 3 s connects + 1 + 4 s
= 20 s; the theoretical ~56 s of four requests each stalling to 12 s is cut off); the reader menu says `Asking home
first - the window opens after`, serial `kosync` says `window waits for home (N s of at most 25 s)`; off WiFi, no
`home=`, or a home job that refused: the window opens at once as before. `ded2930` **§5.3 the position-write
contract**: `tests/check_positions_write.py` (in run_tests.sh) — no carrier file (`booksync*`, `booksync_inbox*`,
`kosync*`, `kosync_sync*`, and since 10:50 `meshtastic_service.cpp` + `app_gbc_xfer.cpp`; a NEW carrier goes on its
CARRIERS list) references savePosition / booksSaveOpenPosition /
applyPending / epubLocate / store->put / BOOKS_POS_FILE / positions.cbs; app_books.cpp has ONE `store->put(` (inside
savePosition) and ONE `applyPending()` call (the card's OK after the arming guard; "Go to X's place" only enters the
card); `epubLocate(` only inside applyPending. **PROVEN (host, re-run after the 10:50 repair):** `./tests/run_tests.sh`
exit 0 — 37 C++ suites + 11 source guards; test_kosync 2070 → 2106 → 2110 (the marker on the phone's own GET and
PUT, the Basic-only shape refused); the glue guard's 59 mutations (the old window-first order and the idle branch's
two releases among them) and the position guard's 12 plants + 6 app_books mutations all trip, and it names a planted
`savePosition(true)` by file:line; `pio run` SUCCESS (RAM 27.5 %, flash 42.4 %, 2,779,263 B) and the new strings —
`X-BookSync: 1` twice, GET and PUT — read out of `firmware.bin`. 🛑 **NOT PROVEN:** nothing has run on hardware — the deferred window,
the `{}` reply to a real KOReader (plan Bench 4), and **the hotspot has NEVER run on hardware**; **station windows
STAY in 0.9.79** (hotspot-always is 0.9.80's). ⚠ **INTEROP UNTIL THE FORK CHANGE IS ON THE X4:** fork `52e1ad9d`
sends `X-BookSync: 1` on every request (built, not yet on the device); an X4 on an older fork build omits Basic on
the peer and hub networks, so against this build its GET gets `{}` (it then shows no place from the phone; its PUT
still parks) — on a network where it sends Basic it is unchanged. COVEY's §5.1 half (`{}` to an unmarked client only
when its stored progress is empty) is COVEY `8b614f3` D-163 — 🛑 see the 10:50 DEPLOY ORDER above. ⚠ The deferred
open obeys every deferred window's rules — refused during a SIP call — where the old inline open was not.

📘 **2026-09-26 ~10:40: THE "BOOK SYNC FOR EVERYONE" PLAN IS `docs/booksync-simple-plan.md`** ({user}'s ask: router-free
sync for public owners, on the go, with or without e-readers/LoRa; a 10-agent design pass + critic). Shape: phones+COVEY
broadcast the signed CBS1 record on the WiFi (UDP 8083, verify-before-park, no discovery); e-readers ALWAYS meet the phone
on its own WPA2 hotspot `WiPhone-Books-<mac4>` at 192.168.4.1 (windows on a station IP removed — closes the foreign-WiFi
gap); LoRa/WiFi/E-readers are separate switches; help pages on the phone. 🛑 **Found on the way: KOReader jumps to PAGE 1
when it pulls a WiPhone/COVEY place** (we send `progress ""`; `GotoXPointer("")` → crengine null → page 1) — fix per client
(§5.1) BEFORE KOReader appears in any instruction; and the window must WAIT for the home push under a hotspot-always rule
(`kosyncSyncMyPlace` opens the window first today). Pending {user}: go on §5.1-3 now vs after 0.9.79; the privacy choice §7.

🛠 **2026-09-26 09:00: THE LIVE WEB FLASHER IS 0.9.78 IN TWO PARTS — the NVS wipe is stopped for today's updaters.**
{user}: "republish first". The parts were cut from the very image that had been live (byte-identical slices, sha
`71ba15e8…` → boot `6c0d3558…` @0x1000 + app `e87b9fba…` @0xe000; live == stage verified; the merged image now 404s).
The cut is `tools/webflasher_split.py` (nvs/otadata read from the partition table INSIDE the image, refuses anything
else; `tests/check_webflasher_split.py`, in run_tests.sh, suite exit 0), used by make_webflasher.sh. `webflasher/index.html`
now flashes EVERY manifest part (the old page wrote `parts[0]` only — with a two-part manifest it would have written just
the bootloader and said "Installed") and refuses a part covering nvs, the range read from the table in the parts (0x9000-
0xe000 fallback): tested in the in-app browser against a mock esptool-js (`scratchpad/pagetest`): two files, right offsets,
byte checksums equal, progress 0→100 %, eraseAll false; an old-style manifest → "Stopped: … nothing was written".
HARDWARE: phone 1 flashed from the LIVE parts booted `0.9.78`, mesh key + 50 node keys + mute intact; both phones then
back on the wf/ build of HEAD (the image's BOOT string is `build=Sep 26 2026 00:45:29`), keys/mute intact. ⚠ `ver`'s "built" stamp is
serial_cmd.cpp's OWN compile time (an incremental build keeps the old object: it says 00:24:01 on the 00:45 image) — read
the BOOT line for the image's stamp. ⚠ A page cached ≤10 min (GitHub Pages max-age 600) reads the new manifest with the
old JS and flashes only the 32 KB boot part: harmless (same bytes), but it says "Installed" — reload and run again.
Anyone who updated through the OLD page has already lost the booksync passcode (Sync settings "Passcode: (none)") and
their mesh key (others need `pki forget`). Committed webflasher/ stage = the live 0.9.78 parts.

🌒 **2026-09-26 00:50: BOTH PHONES RUN `kosync` ec25ce5 (two-part install, keys/mute kept).** **Overnight idle soak
00:52-06:42 (5.8 h) PASSED on both:** 0 reboots/panics/RADIO LOST/WiFi drops, wifi=3 in all 1,402 HEALTH lines each, ~150
frames each 0 timeouts, largest flat (p1 62-65 KB, p2 59,980). Phone 2's healthCheck absorbed 2 single-bit glitches
(ver=0x1A; op=0x87) that 0.9.78 would have called RADIO LOST. Stalls: one 'mesh-ui' 257 ms, one 'battery' 722 ms (the
health.log card append — SD write latency, pre-existing class).
The 2-hour soak on 160b2a0 (19:57-21:57) PASSED: 0 reboots/panics/RADIO LOST/WiFi drops/STALLs, wifi=3 in all 481 HEALTH
lines per phone, 60 + 56 frames 0 timeouts, largest flat. Integration review 3 (56b56ad..160b2a0) confirmed 10, all fixed +
verified + repaired (2e84e1b, 9c1887a, 772b855, 340e2f9, ec25ce5): 🛑 **A1 — the notification CHIRP was erased before it could
sound on a fresh ring since 0.9.66** (the IDF 3.3 TX free queue starts EMPTY: a fresh ring reaches the DAC one full trip,
512 ms at 8 kHz, after install; the pop tore down at 360 ms) — the stop now waits the ring's lead (872 ms on a fresh ring);
NEEDS {user}'S EAR (mute off; phone 1's notify mode is buzz-only, `mode=1`). R3-1: in-call UP/DOWN no longer rewrites
configs.ini per press (persisted once, 3 s after the call). A2: no I2S writer runs without a driver. A3-A6 audio ordering
(stash rule for call levels under music, the jack re-read, rings stopped when installed with the device off — the BOOT
ring is now stopped until first use). KOSync N1 hold: one join per wait; a job past idle waits for the rejoin. R3-4: the
guards' surviving hand mutations now trip. Benched on ec25ce5: music after boot drops 0 / minLead 409 ms; a Game Boy game
with sound, reseat `26704 -> 59980`, WiFi JOIN in 6 s.

🌙 **2026-09-25 NIGHT ({user} away, full permission): BOTH PHONES NOW RUN `kosync` 160b2a0 (built 19:52), same two-part
install, keys/mute/GPS kept.** Added since the block below: **SA-1/SA-2** (e87eeb0, 6900e63 — in-call UP/DOWN moves only the
route in use and stores under its own key; the ring and a call apply the stored levels; a call picks its own route, the
earpiece first; connect reads no file); **the post-game heap split fixed** (05a5f4e, fd2c54f — `Audio::reseatI2S()` at game
quit, after the emulator's RAM is freed; PROVEN: phone 1 largest 65,580 after a game (was 32,816), phone 2 music-then-game
`largest 27592 -> 60432` (was 28,732 for the boot)); **the music budget studied and left alone** (160b2a0 — ~90 rules in the
host model; any rule that decodes less after a long pass drops out when a stall follows; the ~280 ms "pan stalls" were the
catch-up AFTER a frame already on screen; 324 long-pass-then-stall guard cases added). Idea for {user}, not done: keep the
phones' music as 22.05 kHz MPEG-2 files (helix decodes them in ~half the time; the phone already plays 44.1k at 22.05k).
A 2-hour idle soak on 160b2a0 started 19:57.

🧪 **2026-09-25 EVENING: BOTH PHONES RUN `kosync` a42110f (0.9.79 dev, built 17:24), INSTALLED THE WAY THE
FIXED WEB FLASHER INSTALLS (two parts: `wiphone-boot.bin` @0x1000 + `wiphone-app.bin` @0xe000 — mesh key,
KOSync id, mute and `positions.cbs` proven unchanged across it).** Local only, not pushed, not published.
Proven on hardware today, on this build or the one before it (87393a6):
- **Phone 1's WiFi drops are gone** (the CPU gate re-locked the BBPLL under the radio): uploader x4 = 0/4
  broken, twice (0.9.78: 4/4); 0 PLL re-locks with the radio on; Game Boy quit -> JOIN in 7-11 s on both phones.
- **Mesh transmits never stall the phone**: 30-min soak, 12 + 9 frames on air, 0 LOOP STALLs (0.9.78 logged a
  0.6-1.5 s 'mesh' stall per frame), receipts ("in mesh", DM "delivered") through the queue, deaf gap 2-4 ms.
- **KOSync**: phone2 push -> phone1 offer -> Stay leaves the page / Go there lands on the same page; window PUTs
  park and offer; `.local` home= (mDNS 48-271 ms, NVS fallback across a reboot, nosuchhost / DNS names all
  honest, lookup pass <= 8 ms); KS-1 (a page turn mid-pull no longer hides the X4's place), KS-2 (Sync my place
  refuses to send over an unanswered card), KS-3 (the X4 fork's second id is not "a DIFFERENT book") — all with
  `tools/kosync_home_stub.py` (holds the phone's GETs 3 s so the races are drivable over the bridge).
- **Music (details in the 🎵 block below)**: Pulse 192k on phone 2, 60 s on Now Playing = drops 0, minLead 442 ms;
  map pans during music = drops 0, minLead 265 ms. ⚠ With music playing a pan hitches ~280 ms about every 2 s
  (the 80% catch-up decodes up to 8 frames, ~90 ms, on the pass that also loads tiles) — a tiered per-pass
  budget is the follow-up if {user} minds; untouched tonight on purpose (80% is what closed the drop case).
- 🛑 **The web flasher ERASED every user's NVS through 0.9.78** (merge_bin padded nvs with 0xFF, written from 0):
  fixed in tools/make_webflasher.sh + publish_webflasher.sh (87393a6). The COMMITTED `webflasher/` stage is still
  the old 0.9.78 merged image — regenerate it at release (the release rule below still applies).
- `pki forget <!node>` (a42110f) re-trusts ONE node's key; used on phone 2 for phone 1's key (regenerated by the
  NVS wipe this morning) — DMs phone1 -> phone2 delivered again. `radio` counts health-read glitches (a single
  garbled SPI read no longer declares RADIO LOST).
- DHCP reservation on the R7000 (192.168.1.1, the LAN's only DHCP server): covey B8:27:EB:53:B8:A4 -> 192.168.1.55.
**Open:** {user}'s listening pass (checklist: tone A/B incl. a ringtone, mono pair swap, pause/resume, music while
using the phone); GPS time outdoors (phone 2 `clock`); a real SIP call (neither phone has an account SELECTED:
`sip` says NOT LOADED); X4 setup Sunday (x4-reader-sync memory).
**Soak on a42110f (17:52-18:22, both phones idle, no commands): PASSED** — 0 reboots/panics/RADIO LOST/WiFi drops,
22 frames on air 0 timeouts, 0 health-read glitches. Phone 1 logged two ~370 ms `mesh-ui` stalls = a mesh arrival's
full repaint + popup at 80 MHz with the screen OFF (pre-existing cost, newly named by the phase split; skipping the
repaint while scr=0 is a small follow-up).
⚠ **After a Game Boy game the internal heap stays SPLIT until something reinstalls I2S** (largest 64 -> 29-33 KB on
both phones; free returns to ~65 KB): the game's I2S install puts its DMA buffers above the emulator's blocks, which
then free below them. Proven by a music play on phone 2 (music installs its own ring): largest 28,732 -> 64,316.
FIXED later that night (05a5f4e/fd2c54f, see the 🌙 block).
Bench junk on phone 1's card (bench_*.bin ~52 MB, piece_*.bin, test.txt, uprep_probe_*.txt in /books) is {user}'s
to delete. Both phones are MUTED (`mute on`, stored) — keep them so unless a test needs sound.

🎵 **2026-09-25: MUSIC REBUILT (0.9.79 dev, NOT FLASHED when written — see the bench below). {user}: music
*"has always been bad"*; he accepts lower quality and a load pause between tracks.** Four faults, all
measured (phones + a Mac replay with the real decoder), all fixed — `WiPhone/music_feed.h` has the story:
(1) the **loudspeaker path played garbage** since the first MP3 commit (stereo PCM sent as 1152 mono
samples: half of every frame an octave down, half never played); (2) **real dropouts** under any loop
pass > ~70 ms (the old 4 x 1023 stereo ring; LOOP STALL only logs > 250 ms); (3) 🛑 **`gaps` WAS
ARITHMETIC, NOT DROPOUTS** — 512 B reads vs 627 B frames = 8.6 "gaps"/s on every 192 kbps file with no
dropout at all, and 0 through real ones: **every `gaps` reading in this document is void**; (4) a resume
re-fed its first frame (MAINDATA_UNDERFLOW read as "need bytes") = bursts, and ~1% wrong speed.
**Now:** mono, 44.1/48 kHz at HALF RATE (half-band filter), music's OWN I2S ring (24 x 512, TX only,
24 KB — 8 KB less than the boot install — 534 ms deep), whole-frame feed, codec tone = adaptive bass /
no treble cut / no de-emphasis (the old linear +9 dB clipped in the DAC at any volume). The counter is
**`drops`** (ring CERTAINLY ran dry, never invented) + **`minLead`** (> 0 = certainly no gap): Now
Playing `drops:N buf:X.Xs`, serial `music` / `music reset` / `music swap on|off`, `audio tone
old|clean|flat`. A pop/ring/call that cuts a track now leaves it PAUSED at its place. ⚠ A fresh ring
(first track after a pop, call or game) plays ~0.5 s of its own silence before the music: that is the
IDF 3.3 driver (empty free queue at install), not a bug. Host: `tests/test_musicfeed.cpp` (0.9.78 feed
reproduced: 8.6 gaps/s with 0 dropouts at 10 ms passes, 4.8 dropouts/s at 50 ms; new feed 0 through
5-80 ms passes and 100/300/450 ms stalls).
**Review round, same day (still NOT flashed):** catch-up mark 40% -> **80%** (map-pan passes + a 300 ms
stall dropped out at 40%); a mid-track **card read error** now stops the track at its place as "Card read
failed" (was: end of file, and a pulled card walked the queue); a failed **music I2S install** is refused
(was a NULL panic); `setSampleRate()` marks the queue **stale** so every pop/ring/call gets a clean ring (from
the 2nd pop after music they played late and scrambled); **pop first** before a track, a game and the mic
apps; a WAV's first unit no longer inverts the pair swap. 🛑 **BENCH: judge busy runs by `minLead > 0`, not
`drops=0`** (after a drop `drops` is a lower bound until the ring is next full). 🛑 **RELEASE GATE: the codec
tone (`audio tone clean|old|flat`) changes calls, the ring and the pop too — {user} must A/B a ringtone or a
call as well as music before release.** Bench also: a second `notify` 60 s after one that paused music
(must sound identical, `NOTIFY: pop stopped after` the same), a card pull mid-track (stops with "Card read
failed", no queue walk), `heap` largest after ~10 play/notify/resume cycles + a Game Boy start.

📚 **2026-09-24 NIGHT: READING-POSITION SYNC WITH THE XTEINK X4 PRO (arrives Sun 2026-09-28) IS BUILT ON
ALL THREE SIDES, OVER KOSync (KOReader's sync protocol).** {user}: *"push 'sync location' on the device I
was just reading and have it ready when I open it on another device"*; must work with NO COVEY and NO
internet (a WiPhone + the X4 in the woods); nothing {user}-only in the public WiPhone firmware; and 🛑 *"we
can't let it screw up the position people are at in their own wiphones after they update"*.
**Where everything is:**
| | state |
|---|---|
| WiPhone 0.9.79 dev | LOCAL branch `kosync` (`3a4def6` + review round `733803f`), NOT pushed, NOT released. BOTH PHONES FLASHED (rc2, sha `313d47df…`, built 21:04). OFF on both: no `/books/kosync.txt` |
| COVEY | D-161 `7bec3b9` + follow-up `6b8803f`, pushed + deployed (device 73/73). Server on :8088 is OFF until {user} sets a user + password in Books > Sync settings |
| X4 fork | github.com/Nikguy321/crosspoint-reader branch `booksync` (`be76d1a1`, `3a5c352f`, `17c12cc7`), pushed; 1.6.105; NOT flashed; NO release until it works on the X4 ({user}) |
**How it works.** The phone only ever HOSTS: "Sync my place" opens a 5-min KOSync WINDOW for the open book
on its own hotspot `WiPhone-Books` (http://192.168.4.1; open, or WPA2 with `hotspot_pass=`) or on its WiFi
address, and on WiFi reads `home=` (COVEY) first, then sends. A place PUT to the phone arrives as the
ordinary sync card ("X4 says: they are at 52% (43% here)"). The wire percentage is CrossPoint's
BYTE-weighted number over CrossPoint's spine (rules: docstring of `tools/gen_kosync_vectors.py`), computed
on the way out and converted back on the way in; positions.cbs / epubFraction / epubLocate / the LoRa
record are UNCHANGED (`tests/golden_positions.h`, generated from 0.9.78, reproduces byte for byte). Both
document ids everywhere (partial MD5 + md5(basename)). Offer rule (all three): another device, |Δ| > 0.001,
not already offered, not provably older than MY LAST MOVE (clocks trusted only when known; COVEY serves
timestamp 0 when its clock is unsynced). Automatic pushes only when the place MOVED.
**Bench-proven tonight (phone 2, home LAN, a throwaway COVEY server on :8098):** push under both ids
(`95060042…`/`3a6d9a60…` for Ghosts_of_Timkovichi = the Mac's own computation), unknown doc → "not on the
server yet", window GET both ids / `{}` / 401 / upload page 404, fake-X4 PUT → card at the right chapter
(generator agrees: 0.4502 → index 35, 57.7 %), window closes 10 s after the PUT, a second pull says "already
offered", Sync my place after a decline sends ours, three tries 1 s/4 s apart with no loop stall, the
"DIFFERENT book" and "Wrong user/password (x1)" warnings. Stay kept ch 25/90 27 %. NOT proven: the
`WiPhone-Books` hotspot itself (open/WPA2) — the Mac cannot join it without losing its own network.
**SUNDAY, IN THIS ORDER:** (1) COVEY: Books > Sync settings — KOSync user, password (12+ chars; letters and
digits — the phone trims end spaces), server ON; it must read `http://192.168.1.55:8088` (⚠ COVEY's DHCP
lease has no reservation — add one on the router, or `home=` and the X4 URL silently go stale). (2) Phones:
`/books/kosync.txt` with `user=`, `key=<kosync_key from COVEY's /root/.covey/prefs.json>` (the password never
travels), `home=192.168.1.55:8088`, a DIFFERENT `device=` per phone, comments on their own lines only; serial
`kosync` must say `config ON`, `kosync push` → "Sent N% to home". (3) The SAME book file byte for byte on all
three (Calibre's send-to-device rewrites metadata → different id; phone Book info shows `kosync <id>`, COVEY
`sudo python3 -m covey_ui.kosync --list`, Mac `gen_kosync_vectors.partial_md5()`). (4) X4: back up the stock
flash FIRST (esptool `read-flash 0 0x1000000` twice, compare MD5), then `~/.local/bin/pio-x4 run -e x4pro -t
upload`; serial `Starting CrossPoint version 1.6.105-x4pro`; KOReader Sync: same user/password, Sync Server
URL `http://192.168.1.55:8088` (⚠ EMPTY = the public sync.crosspointreader.com), Document Matching BINARY
(set it by hand if stock CrossPoint was configured first — it keeps Filename), never "Sign up". (5) At home:
Sync progress on the X4 ↔ COVEY. (6) Woods: phone WiFi off, open the same book, Sync my place, then Sync
progress on the X4 within 5 min (the fork saves `WiPhone-Books` itself; set its Peer Wi-Fi Password if the
phone has `hotspot_pass=`).
**Traps:** 🛑 the X4 builds ONLY via `~/.local/bin/pio-x4` (own core `~/.platformio-x4`) — sharing
`~/.platformio` made each project overwrite the other's framework/esptool/scons (esptool is now at
`~/.platformio/packages/tool-esptoolpy/esptool.py`). ⚠ The phone's window is ONE book; a PUT for another id
is answered 200 and dropped (now shown on screen). ⚠ A book already open on the phone does not see a later
X4 push until reopened. ⚠ Phone 1 lost WiFi twice tonight right after a screen wake (deaf-radio class,
`wifi bounce` cured it; phone 2 never did) — watch it. ✅ (2026-09-25, UNBENCHED) The `~GbcApp` trap — the
station restored on the off switch alone — is fixed with its whole class: EVERY site that gives the radio back
(game exit, uploader/window teardown, both Settings "WiFi on" controls, leaving Settings > WiFi, boot) goes
through `wifiRestoreStation()` (Networks.cpp; decides in `wifi_policy.h`, host suite `test_wifi_policy`), which
logs `WIFI restore (<who>): JOIN|UP_IDLE|OFF|LEAVE [ro ud rec game ap conn young dry]`; the loop's retry is
gated on `wifiStationWanted()` (no game, no hotspot, both switches); `tests/check_wifi_restore.py` fails the
suite on a bare `esp_wifi_start()`/`WiFi.reconnect()`/any `WiFi.begin(`/`WiFi.mode(WIFI_STA)` outside Networks.cpp,
and on 13 POSITIVE contracts (each guard where it must be; a revert of any of them now fails the suite).
⚠ Connect on the edit screen with WiFi off now flips the persisted switch ON — only once the join has
STARTED (review: a Connect on an unsaved/blank SSID used to flip it and then the loop joined the old network).
{user}'s call to keep or revert. 🛑 BENCH TRAP: after a reboot with the network Disconnected, `open wifiedit`
opens BLANK (wifiSsidDyn is set only by a join or a save) — clean up through `open wifi` + the network row +
`key ok` + `key select`, never Save on a blank screen (Save now refuses it; it used to make "" the preferred network).
The durable record is the commits, CHANGELOG 0.9.79, COVEY's D-161 (+ follow-up) and the fork's commit messages; the
design contract and the two review rounds lived in the 2026-09-24 session scratchpad, which does not survive.

📞 **2026-09-25: THE SIP AUDIT'S "UNREACHABLE" CALL-AUDIO FINDINGS ARE FIXED (`9213fc0` + a review-round
repair on top), LOCAL on `kosync`, UNBENCHED, NOT FLASHED.** Both phones register now (`sip=1`), so the
2026-08-15 table further down (search "NINE FINDINGS") was live; every row now carries its status there.
In short: F1/F2 do nothing in a call and `playMusic()` refuses an armed RTP session; END sets HangUp ONLY
in a call; the ring and the call get the call levels back from music (pop finished first, then music,
then the loudspeaker); the `restore*` 0/0/0 dB globals are gone; Settings > Audio reads/writes music's
stash while music plays; RTP silence is counted PER CALL (the second call of a boot used to die at
connect, no BYE); an RTP session with no live call is shut down within 3 s (held while the motor runs or a
pop plays); the **202## live-mic-to-a-LAN-IP egg needs `-DAUDIO_DEBUG_EGGS`.
🛑 **{user}'S CALL — END IN `CallState::Error` NOW RETRIES SIP.** Error (`sip=12`) is where `sip.init()` lands
when the proxy is unreachable at boot/join (COVEY's no-internet hotspot, a captive portal, a DNS miss), and
NOTHING leaves it by itself. The old END-everywhere was, by accident, the only way out (Error → HangUp →
`terminateCall` fails → HungUp → Idle → `checkCall()` reconnects); gating END on a call removed it, so the
repair adds an explicit one: END (not in a game, with an account) in Error → NotInited → ONE `init()` on
the next SIP pass with WiFi up (`SIP: END in Error - one re-init attempt` at log_e). One try per press, no
timer; `resolveDomain()` caches a DNS miss for 60 s. The alternative is a timed retry out of Error — ask
{user} which he wants before release. `tests/check_call_audio.py` (19 positive contracts + 4 banned spellings,
in `run_tests.sh`) now fails the suite if any of these guards is reverted.
**Bench, when a phone is free (serial):** (1) `audio` — the `rtp:` line: after ANY call `mic=0 stream=0
rtpPort=0 armed=0`; `vol=` is the codec now, `callvol=` where the call levels are (`(in music's stash)` while
music plays). (2) With music PAUSED or stopped: `audio orphan` → within ~3 s `AUDIO: RTP session armed with no
live call ... - shut down`, then `audio` shows `armed=0`. (F1 inside those 3 s now answers "In a call" and
KEEPS the paused place; before the repair it unloaded the track.) (3) Boot on a network with no route to
the proxy → `sip=12`; END on the home screen → the re-init line, then `failed to connect to SIP` (Error
again); on the home network END → `SIP REGISTRATION -> REGISTERED`. (4) Real calls: two in one boot (the
second must survive its connect); music playing → incoming ring at the CALL level on the loudspeaker
(`NOTIFY: pop finished early (the phone is ringing)` if a chirp overlapped); F1/F2 mid-call do nothing;
END on the home screen with music playing leaves the music alone. Lines to expect: `CALL: no RTP from
the far end for 120 s - call ended (no BYE sent)`, `NOTIFY: pop finished early (a call's audio is
starting)`, `AUDIO SETTINGS: configs.ini did not load - ... nothing is written unless you Save`.

🗺️ **2026-09-23 NIGHT: 0.9.78. ALL THREE DEVICES GET OPENTOPOMAP z17 DOWNLOADS (20 km,
THROTTLED, MULTI-DAY) AND "STRETCH THE MOST DETAILED TILE THERE IS". COVEY NOW TELLS STREAMED
TILES FROM DOWNLOADED ONES, AND MAY EXPIRE THE STREAMED ONES. THE MAC GETS `cardday.sh covey
pull`. BOTH PHONES ON 0.9.78 (LEFT MUTED); COVEY DEPLOYED AND CUT OVER; device 72/72.**
{user} asked for these, in order: (1) *"open topo has a native z17 tile set. For all three
devices, can we make that a downloadable option?"* (2) COVEY must show which tiles it only
STREAMED, *"so when I update the tiles on the wiphone via the pool method, I won't miss it"*.
(3) *"all streamed tiles to be retained permanently, unless we get near a certain large
percentage of my max storage... Then some of the tiles far from where I go can expire"*. (4)
*"if the more detailed zoom levels don't exist, don't just let the higher zooms go blank, just
stretch the most detailed available tile and say what tile is being stretched and by how much"*.
(5) *"allow up to [20]km but throttle the download a bit... overnight downloads or multi day
downloads even pre-trips"*.
The binding spec was DESIGN.md §9 in the session scratchpad; where the code deviated, the code
wins.
🛑 **CORRECTION TO THE BLOCK BELOW:** "online it streams the real z17 instead" was
**OpenTopoMap-only**. **USGS (topo and aerial) answers 404 at z17 AND z18.** On COVEY each 404
set a 20 s CLASS-WIDE streaming backoff, so over USGS at z17 even the z16 parents stopped
streaming and the chip said "offline" on a good link. Fixed: a 404/410 rests THAT tile for 10
min; only OSError/HTTPException set the shared backoff. **OpenTopoMap's z17 is real.** Its **z18
is a 200 with a placeholder reading "max zoom layer = 17"**: text on a transparent background
(5,198 of its 65,536 pixels at alpha >= 128). **So the "fully transparent PNG = no tile" guard
on all three devices does NOT catch it.** The real guard is that nothing asks past the source
ceiling: the phone's `otm` zMax is 17, COVEY's `TILE_ZMAX["otm"]` is 17, and cardday never
converts z>=18. **The block below names `MapsApp::overZoom()`/`tileZoom()` and
`mapOverzoomView()`: they are GONE.**
**PHONE, THE VIEW** (P1, `7be5cbb`): **The walk.** Each tile POSITION draws from the deepest
level on the card, walking from min(zoom, area zMax) down to max(area zMin, zoom−8)
(`MAP_ANCESTOR_MAX_D` 8: past it `256 >> d` is 0 and a divide by zero PANICS). The source is
cropped to the exact sub-square, with the phase of a piece that starts mid-ancestor-pixel kept
(`mapAncestorSrc()`), and one ancestor row is widened and pushed once per screen row it covers
(`mapAncestorRow()`). Overzoom is just d>=1 on the same path. A real tile is never stretched.
Grey = no tile at any level. **The labels.** `mapStretchLabel()` writes DEEPEST FIRST: `z15
tiles stretched 4x`, `z16-14 stretched 2-8x`, compact `z16-14 x2-8`. The corner star means ANY
position is stretched, and `z%d*` is never trimmed. **The status chip, highest priority first:**
no memory; no map; N tiles...; `card read error` (a BAD miss: wrong length or unreadable twice);
no tiles here; the stretch label. **The loader and the miss cache.** Per position the loader
wants the finest level not in a slot and not known missing; a coarser level is read only once
every finer one is known missing. `tileLoadStep` returns PIECE/DONE/MISS, and a MISS redraws
(which also fixed the area-edge stall that waited for a key). The miss cache is 64 entries,
least-recently-hit, ABSENT vs BAD. A download writing into the viewed area is polled every 10 s
(`MAPS_JOB_POLL_MS`), and the misses at the levels it wrote are forgotten (every level once
the job's second pass begins). A job writing a level DEEPER than the area's scanned zMax (the
first z17 run over a z16 area) raises that zMax on the open map, found with a POSIX `stat` of
the level's folder (`SD.exists` logs every miss). **The ruler's dash**
is int64 now. 🛑 **A `const uint16_t*` row selects TFT_eSprite's PROGMEM `pushImage` overload.**
It inverts the byte swap and does not clip, so every tile would show in swapped colours. The
host tests cannot see it; there is a 🛑 comment. Do not add `const` there. **Tests:**
`test_maptiles` 130 → 179, and 12 mutants of map_tiles.cpp all go red.
**PHONE, THE DOWNLOADS** (P2, `987958e`; the merge is `1144c7c`; `FIRMWARE_VERSION` "0.9.78"):
**The Detail row** offers z17 only where the source's zMax >= 17 (OpenTopoMap and the custom
relay) (`tilePlanDepthTop/Shown/Next`). There is no phantom step, and the depth shown is the
depth used. **The throttle** is a 2.0 s minimum interval between OTM z17 request starts, and
at z17 it IS the pace (2,068 ms a tile measured). 429/502/503/504 retry the same tile at 60 s
doubling to 15 min, in 200 ms slices, and the pace doubles (cap 10 s). **Limits and the
estimate.** `TILE_JOB_MAX_TILES` 100000; `tileFetchEstimate` bytes are uint64 (they used to
saturate at ~4 GB). `tilePlanSeconds` takes z11-16 at the source's rate (OTM 4.0 s) and OTM
z17 at the interval + 0.1 s = 2.1 s: 20 km z17 at 47.5° N is ~36-37 h (it said "2.4 days"
before the measurement). `tileDecode` returns OK/BLANK/ERROR. The order is centre-out 16×16
blocks, column-major inside each. **The saved job** is an NVS blob `maps`/`dljob`: source KEY,
SSID, centre, radius, depth, and the cursor (pass, z, ordinal, written), saved every 500 tiles,
every 5 min, and at each level end. Never for a custom source. **Auto-resume** = ONE pure
function, `tileResumeCheck()` in the new `tile_plan.{h,cpp}`, called from the loop next to
`tileFetchPause`. It needs: the SAME SSID; USB or >= 3.8 V (`MAPS_DL_BATT_FLOOR`; a running job
stops at 3.75 V off USB); no call for 60 s, no game (`gGbcActive`), no Files folder job, a card
present, and WiFi up for 60 s. **Cool-downs** are 10, 20, 40, then every 60 min. **It gives up
after 26 failure stops / refused starts in a row** (`TILE_PLAN_STALL_TRIES`, about a day of
cool-downs), counted in TRIES: days waiting at a gate (another network, off USB) are not
retrying. The first cut used "24 h since the last tile", which gave a job up on its first
failure after such a wait. CARDFAILS never resume by themselves. **A card too full for the
rest KEEPS the job, given up** (`TILE_GIVEUP_SPACE`, "free some, then Resume"); the space check
runs from the cursor, not from `written`. **Crash strikes** are committed to NVS BEFORE each
BOOT resume and cleared by a recorded stop reason or 50 new tiles. After 3 strikes it stops
resuming, and the form says why. **A power-off is not a crash:** the held power button, the
low-battery cut and the menu's Restart all call `tileFetchPowerOff()`, which saves the live
cursor and records `TILE_STOP_POWEROFF`. An esptool reset, a reflash or a panic still strikes.
**On another network:** `Paused: resumes on <SSID>, where it started`, plus **Resume on this
network** (**Resume it now** for any other waiting gate, e.g. a give-up or a cool-down). A
manual resume never strikes. **One automatic second pass.** Stop takes two presses within 3 s,
and `tileFetchStop()` clears the NVS job synchronously. Every entry point (`tileFetchStop`,
`tileFetchResumeNow`, a Start) loads the saved job before acting on it. The form shows a running
or waiting job read-only, and settles a run that just ended before drawing; the screen hold
lets go 2 min after the last key. Files refuses a delete or move over a running OR waiting
job's `/maps/<src>`. **Tests:** `test_tileplan` is new, with 140 checks, and `test_tilepng`
42 → 52.
**COVEY** (`131feb3`, deployed 2026-09-24 00:26; decisions D-159/D-160 in its repo):
**Per-source ceilings:** `tilestore.TILE_ZMAX` = otm 17 / usgs 16. The view goes one level past
(otm z18, usgs z17). Nothing ever fetches past `tile_zmax`. **Nearest-ancestor stretch** to
max(ZMIN, z−8). The chip reads `· z16-14 stretched 2-8x`. **Streamed tiles go to their own
tree:** `/root/covey-tiles-streamed/<src>`, a SIBLING of `/root/covey-tiles`, so **the
filesystem is the provenance record.** They show an amber wash, a 2 px edge, and a chip
reading `N streamed tiles in view`, toggled with `mark_streamed`. **The streamed-only list:**
Map Downloads → **Streamed-only (not downloaded on COVEY)** → the places list → **Download
this area properly** (a PRESET; nothing starts until DOWNLOAD is pressed). **Expiry of
streamed z>=12 only**, at >=90 % used or <2 GiB free, down to 85 %. Never within 10 km of GPS
visits (the new `visits.py`), pins, downloaded tiles, live map centres or the running job.
Anything streamed in the last 7 days goes last. Never anything downloaded, and a streamed tile
is deleted as a "duplicate" only when the downloaded copy starts AND ends like a tile
(IEND/FFD9). **Area downloads:** OTM to z17, 20 km, the same 2 s interval, a job file resumed
at start on the same WiFi, and a second pass. Its time estimate still rests on
`TILEDL_FETCH_S = 0.5`, a guess, but at z17 its model pins the rate to the 2.0 s interval
whatever the guess, which matches the phone's measurement; only its z11-16 part is a guess.
**New modules** `tilestore.py`, `visits.py`, `streamplaces.py` (so **47** files in the md5
rule), and 72 suites.
**MAC TOOLS** (`4719a8b`): **`tools/covey_pull.py` + `cardday.sh covey pull`** take
DOWNLOADED tiles only. The manifest comes from `sudo find -printf`. Tiles are staged in
`.incoming`, checked for COVEY's size AND the tile's own end (a PNG's IEND, a JPEG's FFD9),
then linked in with `os.link` (EEXIST = the master wins, so it NEVER overwrites). 🛑 **The end
check was the review's one critical finding:** when a tar stream stops mid-tile, bsdtar pads
that tile to its full header size with ZEROS, so a size-only check pooled corrupt tiles; the
tile bsdtar was writing when a stream stopped is also dropped by name. Zero-byte tiles are
never pulled (`covey_zero_bytes.txt`). The pass test is a set difference, and an empty master
file counts as missing. It prints COVEY's streamed-only places BEFORE transferring, with how
much of each the master already has. **It REFUSES (exit 2) when COVEY cannot give that
report**; `--without-report` overrides, only once the cut-over is known done. **The card-day
order:** `status` (the evening before) → `covey pull` → phone pulls → phone pushes → `covey
push`. `covey push` uses `-rt`, counts with `-size +0`, and refuses to leave COVEY under 4 GiB
free. **The phone push** goes in bands: z0-16 for every source, then z17, each after a `df`
check. Never z18. **Fixes:** pipefail; the backtick message; `convert_tiles` exits 1 on STOPPED
and refuses a fully transparent tile; `tiles_565_to_png` fills 0-byte PNGs; `card_clone`'s
glob. **Tests:** `check_covey_pull.py` 83, now in `run_tests.sh`; `check_convert_tiles.py`
22 → 35. ⚠ **Do NOT run the old `~/tiles-master/pull_covey.*`**: it untars straight into the
master, and bsdtar overwrites. The same commit makes the map's 25 ms load tick stand down when
a load ends (while locked, the redraw goes to the clock and would not re-arm it).
**THE REVIEW** (`0bb0146` for the phone, `4719a8b` for the tools; COVEY's are in `131feb3`):
six adversarial reviewers, plus a verifier on every finding. Phone view 3 (1 major), phone
downloads 6 (2 major), COVEY store 4, COVEY UI 9 (2 major), Mac tools 3 (1 critical), fidelity
to {user}'s asks 5. **All 30 fixed.** The majors: 🛑 **a tile that finished loading while the
phone was LOCKED was read again 40 times a second until unlock** (the redraw went to the
clock, so the stale "want" still named it; `startLoad` now refuses a tile already in a slot);
the form could show a waiting multi-day job as "no job" with Start under the cursor, in the
second after a run ended; the 24 h give-up counted days at a gate; on COVEY an undecodable
tile was fetched again on every frame, and pass 2 could retry one refused tile forever.
**BENCH, THE REVIEWED BUILD** (`firmware 0.9.78, built Sep 24 2026 00:04:42`, firmware.bin
2,663,296 bytes, a clean build, RAM 27.1 %; both phones flashed, esptool hash verified; muted
first, `hold on` before every shot). **Phone 2:** at z16 over a spot where the pool has z13 but
nothing deeper, `z16* otm` / `z13 tiles stretched 8x`; at that tile's corner at z15, `z13-12
stretched 4-8x`, and the roads line up across the seam. **A real 2 km OTM z17 download:** 567
tiles (555 new, 12 already there), 0 failed, 1,243 s (21 min); z17 at 2,068 ms a tile (the 2.0 s
interval sets the pace), z11-16 at ~3.5-4 s; internal largest-block floor 49,616 B, min-ever
(since boot) 38,868 B; worker `task stack floor 4572 of 8192`; the saved job cleared itself at
the end. Over it, z17 is REAL (`z17 otm`, no star, house numbers readable), and z18 reads `z18* otm` / `z17
tiles stretched 2x`, a 20 m scale bar and the one-shot "Past the tiles" note. **Crash resume:**
an esptool reset in the middle of a 5 km z17 job; at boot the job "waits to resume", then
`resumed the otm download at z16 #1 (pass 1, 217 written, strike 1)`, and the strike cleared
after 50 new tiles. A reflash in the middle of the same run later resumed `at z17 #399 (pass
1, 923 written, strike 1)`. ⚠ **That 5 km z17 job was LEFT RUNNING on phone 2** on purpose
(useful coverage, and a long-run test): `maps dl` shows it. **Phone 1:** `z17* usgs-topo` /
`z16 tiles stretched 2x`; the Download form for OpenTopoMap 20 km z17 reads `50451 tiles, 6.2
GB, up to about 36 h` and `Keep it on USB: about 36 h of downloading`; USGS Topo's Detail
stops at z16. ⚠ **BOTH PHONES WERE LEFT MUTED** (master mute on) after the bench; {user} unmutes
with a held `#`. **Still not exercised on hardware:** a resume after a WiFi drop over 20 s, a
double Stop, a battery stop off USB.
**THE COVEY CUT-OVER, AS RUN (2026-09-24, covey-ui stopped).** 749 tiles were on COVEY but not
in the 2026-09-22 pool. 🛑 **478 of them were NOT streamed: a deliberate OpenTopoMap download
made on COVEY on 2026-09-22 20:21-20:32** (z15-16, written one at a time ~1.2 s apart, column
by column, which is the Downloader's signature). They were KEPT as downloaded. The other 271
(otm 222: z9-16 plus 101 at z17; usgs-topo 49) were streamed browsing, and moved into
`/root/covey-tiles-streamed` with `tilestore --mark-streamed` (dry run first: 271 to move, none
refused, every mtime after the push; then `moved=271 failed=0`). After it: otm downloaded
30,774 / streamed 222; usgs-topo 62,579 / 49; usgs-img 39,951 / 0. `tilestore --places`: 4
places (3 OpenTopo + 1 USGS topo), every one radius 2 km, plus overview lines (otm 98,
usgs-topo 16); COVEY's card 15.4 % used, 101.7 GB free. 🔑 **LESSON: "newer than the push"
did NOT mean "streamed". Check the write CADENCE before any cut-over:** a download writes one
tile every ~1.2 s, column by column; browsing does not.
**THE MAC, AFTER THE CUT-OVER.** `cardday.sh status` shows COVEY's streamed-only places, each
with how much of it the master already has. `cardday.sh covey pull` ran for real: +250 tiles
(the rest of that COVEY download; 228 were already in the master from the overnight fetch),
"0 on COVEY missing from the master", and the streamed ones not pulled (otm 222 / usgs-topo
49). openrsync `--files-from` against COVEY's rsync WORKED. **The overnight fetch** (running
as this was written): OpenTopoMap z11-17, 10 km around two of {user}'s pins, into
`~/tiles-master` by `~/tiles-master/fetch_otm_areas.py` (outside the repo), 2.0 s between z17
requests: 25,546 tiles in the two squares, 19,714 not yet pooled, ~11 h. When it finishes,
`cardday.sh covey push` puts it into COVEY's DOWNLOADED tree. ✅ DONE 2026-09-24 08:41: 19,713 new tiles, 1 the server does not have, 0 failed, no network or server waits, 10.9 h. `cardday.sh covey push` (08:42-08:44, under 2 min): otm on COVEY 50,259 = the master exactly, 0 zero-byte; usgs-img 39,951 and usgs-topo 62,579 unchanged. The streamed-only place inside the home square left COVEY's list (now downloaded); 3 places remain. COVEY then downloaded OpenTopo 5 km z11-17 around a third pin ITSELF (2026-09-24, the new Downloader's first real run: 3,369/3,369 tiles, streamed ones promoted) — that area is on COVEY only until the next `cardday.sh covey pull`. Phone 2's 5 km z17 test job also finished by itself overnight (resumed after the reflash, pass 2 done, 2 failed, 16 no tile).. The phones get it on the next card day.
**STATE:** **WiPhone:** P1 `7be5cbb` + P2 `987958e`, merged `1144c7c`; review fixes `0bb0146`;
tools + `run_tests.sh` + the map's tick `4719a8b` (HEAD when the release image was built),
all on main. Host
suite: `./tests/run_tests.sh` exit 0, with `test_maptiles` 179, `test_tileplan` 140,
`test_tilepng` 52, `check_covey_pull` 83, `check_convert_tiles` 35, and every other suite
unchanged and passing. **Both phones:** `firmware 0.9.78, built Sep 24 2026 00:04:42`, muted,
phone 2 running a 5 km z17 job. **COVEY:** `131feb3`, deployed 2026-09-24 00:26: md5 47/47 ==
HEAD (the three new modules included), `covey-ui` restarted 00:27:03 PDT, NRestarts=0, no
tracebacks. Mac 72/72; device 72/72 as `covey` (~00:45), no test leftovers in `/home/covey`,
covey-ui still active. **Pushed and published:** ✅ PUSHED `9edbedd..0d84b15` and PUBLISHED: live manifest 0.9.78, live image sha256 `71ba15e8…` = the stage byte for byte = the binary both phones run (build stamp 00:04). COVEY pushed `493c49e..88d9359`..

🔭 **2026-09-22 NIGHT: 0.9.77 — ONE LEVEL PAST THE TILES, ON ALL THREE DEVICES; AND COVEY HAS
THE WHOLE TILE POOL.** (Partly superseded by the block above: the helpers it names are gone,
and "online it streams the real z17" was true for OpenTopoMap only.) {user}: *"for all 3 devices,
can we allow them to zoom in one level more
(maybe add a little bit of text saying its zooming past the tile level...) i totally get that
it will get fuzzy, but would be usefull regardless."* THE MAP: the view may sit one level
deeper than the area's deepest tiles (`MAP_OVERZOOM_MAX` 1), those tiles stretched 2x. 🔑 THE
VIEW STAYS AT THE CHOSEN ZOOM — scale bar (50 m where z16 read 100 m), pins, crosshair readout
and distances are all still exact; only the tile FETCH is pulled back (`MapsApp::overZoom()` /
`tileZoom()`: everything touching a tile uses the tile level, everything geometric uses
`zoom`). Three labels, because a fuzzy screen reads as a bad tile: corner chip `z17*`, status
chip `z16 tiles stretched 2x`, and a ONE-SHOT note on the step that crosses the line. Past it:
"z17 is as close as this map goes, tiles and all". The stretched level survives a power-off and
travels between areas (one level past THAT area only). `mapOverzoomView()` is the pure, tested
part — centre pulled back rounded, viewport divided ROUNDED UP (down leaves a 1-2 px unpainted
strip that reads as a missing tile); `test_maptiles` +7 including a z16→z17→tile-view round
trip. The push widens one source row into a 480-byte stack buffer and pushes it twice.
COVEY: `ZMAX` 16→17 + `TileCache.get_stretched()` (ancestor scaled AND cropped to the right
quadrant — the plausible-looking failure), chip `z17 · USGS topo · z16 stretched 2x`, online it
streams the real z17 instead; `tests/test_map_overzoom.py` 17 checks with a four-quadrant
parent. **STATE: WiPhone `c171158` (0.9.77) — both phones flashed and benched (aerial + topo
z16→z17*, refusal past it, panning, back out to the same coordinates, the zoom still there
after a reboot), stage = the flashed binary, host suite 323 assertions 0 failures. COVEY
`493c49e`, DEPLOYED (44/44 md5-matched first, 66/66 Mac and 66/66 device, restarted with no
game running). ✅ PUSHED + PUBLISHED at {user}'s word ("Works great. Feel free to push and
publish"): WiPhone `5da55dd..475a705` (0.9.76 + 0.9.77 + the tile tools), live manifest 0.9.77,
live image sha `0463474f…` = the stage byte for byte; COVEY `791cf4a..493c49e` — ⚠ that push
also carried FOUR earlier commits other sessions had committed and never pushed (D-155..D-158:
the fleet node on 2.7.26, slide-to-unlock + GPS on the RAK's always-on rail, the L+R+START
unlock chord).** COVEY's tiles:
`cardday.sh covey push` ran — **132,826, matching the master exactly** (otm 30,296 / usgs-img
39,951 / usgs-topo 62,579, 0 zero-byte, 95 GB free); the dud 0-byte `usgs-img/14/2621/5733`
was deleted there as well. ⚠ THE SCRATCHPAD WAS WIPED mid-session (flash.sh, p1.py, p2.py and
the shot helpers had to be rewritten — they are back under
`/private/tmp/claude-501/-Users-{username}-wiphone/<session>/scratchpad/`). ⚠ `hold on` before a
bench that takes screenshots: the phone locks in 30 s and the shots come back as the clock.

🔇 **2026-09-21 NIGHT: 0.9.76 — HOLD '#' TO MUTE.** {user}: *"make holding the pound key mute and
unmute... only when you're not entering text or when that key isn't being used for something
else"*, then *"stop that behavior"* for the clock's dialer-on-'#'. The '#' hold tracker in
WiPhone.ino is now capital (predictive field) OR mute (`uiHashMuteArmed()` = no focused text
field — `FocusableWidget::textFieldFocused()`, focus regardless of content — and not the
map, whose '#' is zoom). 🛑 THE PRESS IS SWALLOWED while the mute hold is armed (key loop:
`hashDeferred`), delivered on an early release (`hashTapPending` → the same processEvent, ~100
ms late) or forgiven by the hold — so a tap still opens the dialer/Messages from the clock and
menus and a hold no longer does. Feedback: a 70 ms buzz (`meshVibroLenMs` per pulse, `MUTE_BUZZ_MS`)
+ the header icon, and the CLOCK FACE now draws the mute icon too (`ClockApp::redrawScreen`).
🛑 THE FIRST IN-GAME HOLD WEDGED PHONE 2: the icon refresh's `REDRAW_HEADER` pushed the header
rows to the LCD under the emulator's blit task (SPI from the other core) → loopTask hung, task
watchdog, MOTOR LEFT RUNNING until an esptool reset ({user}: "The buzzer is constantly running").
Fixed twice over: no redraw request while `gGbcActive`, and `GUI::redrawScreen` clears
redrawHeader/redrawFooter while `gGbcActive` (the pause menu's REDRAW_SCREEN still reaches the
app). Bench: `key hold <key> [ms [blip]]` (uiKeyDownOrBlip answers from the bench;
`uiKeyMaskFor`). ⚠ Injected keys do NOT reach a running game (`key end`/`key up`/`key ok` all
dropped; known since 0.9.75) — `gbc autosave` parks it, esptool `--before default_reset read_mac`
resets the phone from the cable (also the way to stop a stuck motor). Proven on both phones:
clock (mute, no dialer; tap → dialer with '#'), dialer (types '#'), submenu, Phonebook, Compose
To: + T9 body (no mute; T9 hold stepped the mode back), map (zoom, no mute), running game
(unmute → mute, `buzz off after 70 ms`, loop alive). **STATE: `46db7bd` committed, both phones on
it, stage = the flashed binary (sha `bfcc0aa7…`), NOT pushed/published — {user}'s word. Both
phones left UNMUTED.** `ver` still prints the 19:55 stamp: serial_cmd.cpp was not recompiled by
the fix build (the stamp lives there) — the in-game bench is the identity, not the stamp.

🗺️ **2026-09-22 MORNING: BOTH PHONES CARRY THE WHOLE POOL — 132,826 TILES EACH, VERIFIED.
ONLY COVEY IS LEFT.** Card day ran on `tools/cardday.sh`: phone 2's old 16 GB card (60,464
tiles) and phone 1's old 32 GB card (26,025) pulled, then both new SanDisk 32 GB cards written
and named `WIPHONE2` / `WIPHONE1`. Per card, checked BEFORE ejecting: every master tile present
at exactly 131,072 bytes (132,826/132,826, 0 missing, 0 short), every one of the old card's own
files restored at an identical size (63 for phone 2, 77 for phone 1 — books, ROMs, GBC saves,
music, photos, wallpaper, t9, configs.ini, meshdb.bin, pins.txt, health.log), 0 `._` sidecars,
16 GB used / 13 GB free. ~13-14 min a card at **~23 MB/s** (otm 164-167 s, usgs-img 230-242 s,
usgs-topo 336-350 s) — the earlier "an hour a card" fear was wrong. 🔑 THE PULL DOES NOT COPY
TILES (`card_clone.sh --no-tiles`): on the real card they came off at ~4 MB/s (half an hour) to
be thrown away, since the master holds a PNG for every one and the new card is rebuilt from it —
3.5 min instead of 30, and the OLD CARD is the safety net. Unique contributions: phone 2 **8,635**
(topo 7,110 / otm 1,214 / img 311), phone 1 **101**. ⚠ ONE DUD: `usgs-img/14/2621/5733.png` is
0 bytes in COVEY's cache (dated Jul 20, a failed fetch there) — removed from the master, so the
union is **132,826**, and absent means a downloader can still fill it. Delete it on COVEY too.
⚠ `diskutil eject` is dissented by loginwindow on these cards — `sync; diskutil unmount force`
is the way, and the slot needs a fresh insert afterwards (phone 1's card took two re-seats and
enumerated nothing until then; the readers are a Genesys USB3.0 and a Samsung uSD). Master free
space on the Mac: 74 GB before, ~71 after. ▶ **NEXT: `tools/cardday.sh covey push` (8,736 tiles
it lacks) when COVEY is on the network — {user}: "talk later tonight for the covey transfer".**

🗺️ **2026-09-21 EVENING: POOLING THE TILES — THE MAC-SIDE HALF (superseded by the block above).** {user}: *"All three of my devices (both wiphones and covey) each
have random tiles from different downloads. Can you pull the files from each and combine them
all to get the max tile coverage and put them back on the devices?"* Then, on the transport:
*"I have a card reader. When the new cards come, I will first put the old ones on so you can
pull the info. Then I will put the new ones in, that way you could both clone what is on the
current cards, and put the entirety of the maps together. On covey and both phones."* THE
NUMBERS THAT CHOSE THE METHOD: COVEY holds 124,091 tiles (usgs-topo 55,368 / usgs-img 39,641 /
otm 29,082, z8-16; `/root/covey-tiles/<src>/z/x/y.png` — ⚠ the usgs-img files are JPEG BYTES
under .png names); phone 1 26,025 raw tiles (usgs-topo z11-16 13,165 + usgs-img z11-16 12,860),
phone 2 60,464 (usgs-topo z11-16 32,918 + otm z11-16 14,412 + usgs-img z11-16 13,134). UNION =
**132,827 tiles = 16.2 GB per phone** at 128 KB a tile; the WiFi uploader does 48-69 KB/s (3
days) and NOTHING serves a file off the phone (`/log` is the only GET, 60-78 KB/s) — so cards
in the reader. Almost everything flows COVEY → phones: phone 1 has 226 tiles COVEY lacks and
gains 106,802 (13.0 GB); phone 2 has 8,635 and gains 72,363 (8.8 GB); COVEY gains 8,736. STATE:
`~/tiles-master/<src>/z/x/y.png` HOLDS ALL 124,091 COVEY TILES, VERIFIED (every file's size
against COVEY's `find -printf`, one truncated file from a killed stream caught and re-fetched;
42-file md5 sample identical) — pulled zoom by zoom by `~/tiles-master/pull_covey.py` (tar
stream then rsync to fill gaps; the link is 185-1,000 KB/s; NM power save set to disable on
`SmithWifi`); phone inventories `~/tiles-master/inv_p1.tsv`/`inv_p2.tsv` (serial `ls` walk,
`scratchpad/inv.py`, ~2 s a folder, 21 + 52 min), COVEY's list `inv_covey.txt`; `plan.py`
prints the per-zoom union table and writes `only_pN.txt` / `missing_pN.txt`. THE DRIVER:
`tools/cardday.sh phone1|phone2 pull|push /Volumes/<card>` + `cardday.sh covey push`
(`~/tiles-master/cardday.sh` is a symlink to it; log `~/tiles-master/cardday.log`;
`CARDDAY_MASTER`/`CARDDAY_CARDS` override the paths for a rehearsal). Rehearsed end to end on
FAT32 `hdiutil` images — a phone-only tile came back byte-identical on the "new card", zero
sidecars, and the pull found exactly the 15 planted tiles the master lacked. `push` refuses a
card that is not FAT32.
CARD DAY (docs/maps.md "Pooling the tiles of several devices"): old card in →
`tools/card_clone.sh pull /Volumes/<card> ~/wiphone-cards/phoneN` → `tools/tiles_565_to_png.py
<backup>/maps/<area> ~/tiles-master/<area>` (`--jpeg` for usgs-img) → new card in →
`card_clone.sh push` the backup → `tools/convert_tiles.py ~/tiles-master/<src>
/Volumes/<new>/maps/<src>` per source (~300 tiles/s now: numpy + `--jobs`; the card is the
limit, budget ~1 h) → `diskutil eject` → phone → Maps → Menu → Rescan card. COVEY:
`rsync -a --ignore-existing --rsync-path='sudo rsync' ~/tiles-master/<src>/
covey:/root/covey-tiles/<src>/` — its map reads files by path, no index, no restart. ⚠ The new
32 GB SanDisk cards ship FAT32 (the phone reads no exFAT); a card over 32 GB needs
`diskutil eraseDisk FAT32 WIPHONE MBRFormat diskN`. Tools committed `327c1bf` (tools only, no
firmware; `check_convert_tiles` 22 checks holds 565→PNG→565 through both scripts). Bench: a
200 MB FAT32 `hdiutil` image — clone both ways byte-identical, 751 tiles onto it in 4.8 s, no
sidecars. Phone 1's aerial download finished: 4,408 + 8,452 skipped of 12,864, 4 failed, 126 MB
in 2.2 h, card busy 292 times. Phone 2 is STILL MUTED. Commits `327c1bf` (tools) + handoff:
NOT pushed — push on {user}'s word. ⚠ zsh: `for f in $list` does NOT word-split an unquoted
variable — use a `while read` loop (a 42-file md5 loop ran once as one 1,300-character name).

🔇 **2026-09-21 MID-MORNING: 0.9.75 — MASTER MUTE + THE GAME BOY'S RAM FLOOR.** {user} (at work,
"no sounds"): "a master 'mute' button within the settings... make sure it also works for the
Gameboy". Settings → Mute all sounds (`MuteApp`, menu id 51, `GUI_APP_MUTE`; `configs.ini
[audio] mute`; `guiSetMuted()` is the one path for the screen and serial `mute on|off`);
`Audio::setMuted` → every codec volume write goes through `Audio::applyVolume` (outputs at
floor + `codec.mute()` DACMU re-asserted after `codec.setVolume()`, which clears it; amp off) —
ON THE LOUDSPEAKER ROUTE ONLY: earpiece (a call) and headphones play, the ring/pop/music/GBC
all route to the loudspeaker;
header crossed-speaker `GUI::drawMuteIcon`. THE GBC FINDING: first game under 0.9.74's layout →
`wram=internal vram=internal speed=101%` but in-game HEALTH 4.8 KB free / largest 2.3 KB (every
earlier session: 35-38 KB ⇒ WRAM had always been in PSRAM). `gnuboy/hw.c gb_alloc_pref_internal`
now keeps `GB_INTERNAL_KEEP` 20 KB free → `wram=PSRAM vram=internal speed=98%`, in-game 37.9 KB.
⚠ BUILD TRAP: an edit to WiPhone.ino did NOT recompile on the next `pio run` (the flashed phone
still printed the old arg order); check for `Compiling .pio/build/wiphone/src/WiPhone.ino.cpp.o`
in the build output after touching the .ino. Bench with the mute: `mute on` → `open gbc` →
`key ok` → `gbc` (placement, speed) → `gbc autosave`; `key end` from the cable did NOT pause the
game this time (paused=0) — use `gbc autosave` to park it. README: SD card recommendation
(SanDisk High Endurance 32 GB, with the 1-in-8 measurement). REVIEW OF 0.9.74 (4 lenses, 11
verified): 🔑 THE RESERVE SURVIVED BECAUSE OF ONE DEAD `btStop()` in `Networks::disable()` —
it links esp32-hal-bt.c whose strong `btInUse()` returns true, and `initArduino()` (arduino-esp32
is 1.0.6, not 1.0.5) skips its own `esp_bt_controller_mem_release` on that test. Removed; `btInUse`
is weak (`W`) in the ELF now, the core releases before setup(). The released regions are added at
the HEAD of the heap list (tried FIRST, absorb boot allocations, the main region's 67 KB block
stays whole) — my first comment said the opposite. `~GbcApp` now calls `gbcReleaseEmulator()`
(stacks/VRAM/audio/WRAM were resident until reboot after a game; measured 69.0 → 37.7 in game →
68.2 KB after quit). tile_fetch: bar = exact 14/10 KB, `TF_MAX_RAM_FAILS` own counter,
`lastErr[96]`, shorter message. Body cap 96 → 256 KB (USGS aerial tiles over 96 KB failed
on every run). Card retry measured: 9 refused, 9 written on try 2, 0 lost. **STATE: 0.9.74 +
0.9.75 PUSHED (`91afadb`) AND PUBLISHED at {user}'s ask ("Go for it"), live manifest 0.9.75, live
image sha `60c15189…` = the stage; BOTH PHONES ON IT. Phone 2 was left MUTED (`mute off` or
Settings → Mute all sounds → Sounds ON). Phone 1 is running {user}'s aerial download
(`maps dl 1 47.2528 -121.4054 20 16`, 12864 tiles, ~2.5 h from ~11:00).**

🧠 **2026-09-21 MORNING: 0.9.74 — THE BLUETOOTH RESERVE IS RELEASED AT BOOT: +40 KB INTERNAL
RAM ON BOTH PHONES.** {user}: phone 1's map download "waiting for memory... eventually stops due to
ram problems." `maps dl`: 13.2-13.7 KB free with Maps open, under the 14 KB handshake bar, never
connected. The 2 KB phone 1 lacks vs phone 2 is SIP (phone 1 registered, `sip=1`, 3,402
REGISTERs in its log vs 57; 25,960 vs 28,228 B at up=1min). THE FIX: `CONFIG_BT_RESERVE_DRAM
0xdb5c` — 56 KB reserved for a BT controller nothing starts; the GBC's `reclaimInternalRam()`
has released it on first launch for months. `setup()` now calls
`esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` first thing: `BOOT: BT reserve released:
internal free 171600 -> 211924`; idle heap 28,228 → 68,816 / largest 24,704 → 67,564 (phone 2),
26,000 → 66,320 (phone 1); a 20-tile HTTPS run: min-ever 48,012 (was 5,280). ⚠ EVERY "internal
heap is ~16 KB" NOTE BELOW THIS BLOCK IS HISTORY NOW — the gates and the reasoning stand, the
numbers moved. Downloader: `TileJobStatus.ramFree/ramLargest/ramNeed*`, screen row `Needs 14 KB
free (has 13.6) and a 10 KB block (has 10.5)`, stop after 3 no-RAM fails (90 s) with the
numbers, `maps dl` prints a `job:` spec line. {user}'s run restarted on the cable
(`maps dl 1 47.2528 -121.4054 20 16` — centre recovered from `ls /maps/usgs-img/16` columns +
one column's y range; `12864 tiles` = his exact total). PHONE 1's CARD refuses ~1 write in 8 (35 second-try
saves + 7 lost in 311 tiles): the tile write now retries after 200 ms / 1 s / 3 s and counts
`cardRetries` (`maps dl` "card busy N times"; the screen's last-run note). `gbc` prints
`wram=/vram=` placement — NOT yet checked under the new layout (no sound allowed on the bench today; run a game when {user}
is home and read `gbc`). Review: workflow `wiphone-btram-review` (4 lenses). **State: see the
git log — committed/flashed/staged per the usual rule; push/publish on {user}'s word.**

🎮 **2026-09-20 LATE: 0.9.73 — THE MAP'S HOLD RIDES THROUGH THE KEYPAD CHIP'S RELEASE-BLIP, AND
SWEEPS BY TIME.** {user}: "hold to scroll... works for one to 3 or 4 jumps before it just stops."
`keys raw` on phone 2 while he held RIGHT: heartbeats every ~58 ms, then `+124ms 0x00 r`
(a code-0 RELEASE) and `+4ms 0x5C P` (the same key down again) — under a thumb that never
moved, at the first re-report slot (123-125 ms) or 2-4 repeats in. The attributed release
cleared `uiKeyDown`; the map's poll read "up"; the re-press was bounce-killed (rightly). Fix in
the MAP: `MAP_PAN_HOLD_BLIP_MS` = 40 — a release younger than that keeps the hold
(`panHoldBlipMs`, next tick re-asks), and the same arrow as a keypress inside the window is a
continuation. WiPhone.ino: `uiKeyUpAgeMs(mask)` (read-only) + the bench blip in
`uiKeyBenchHold(mask, ms, blipAt)`. Then the choppiness: repeats came every 125-170 ms, not
100 — `mapPanHoldMove(run, dt, &carry)` moves by TIME (curve = px per 100 ms, dt capped 250;
`test_maptiles` +7): 1.2 s = 478 px (curve 480), 3 s = 2,214 (2,267). Also `keys raw` printed
12 of 64 lines (`say()` is 192 bytes; one line per call now). Bench: `maps hold <dir> <ms>
[blip]`; log lines `MAPS: hold continues past a Nms release blip (K repeats in)` and `MAPS:
hold ended after N repeats, T ms, P px`. By hand on phone 2: "Worked" ×2, a real blip ridden
through 3 repeats in. ⚠ The 2026-08-22 memo says the heartbeat is ~109 ms; today it measured
58 — both are the chip's; nothing depends on the number except the 350 ms sweep, which is
clear of both. FRAME RATE: a map frame was 107 ms → 77 (void fill skipped when the blits cover
the band; `WiPhoneApp::drewInsideBand()` → GUI pushes only the band between header and footer;
the 100 ms repeat gate on the 50 ms timer removed — every tick moves by time): 3 s hold = 14 →
29-31 frames, distance on the curve. What is left in a frame: ~18 ms tile rows PSRAM→PSRAM
(a pre-swapped memcpy was tried: no gain), ~10 ms overlays, ~45 ms SPI push at 40 MHz — DMA
or a faster bus is the next lever, not tried. REVIEW (4 lenses; verifiers died on the Fable
credit limit, hand-checked): the blip is judged on DRAIN-time stamps, so the tick that sees a
fresh release now returns at once (no tile piece that pass — a 25-60 ms read made the 4 ms
re-press read as 25+ ms, past the bounce filter, and a fresh tap); F2/digit/`#` hold trackers
use `uiKeyDownOrBlip()`; `maps hold` injects before arming and validates `[blip]`; `keys raw`
prints per entry (`keypadTraceLine`). NOT done: Game Boy single-direction blip = one dropped
frame (1 in 4 blips; measure first); phone 1's heartbeat untraced. {user}, on the 12 fps build: "Way way better." **PUSHED (`0ab6f4c`) AND PUBLISHED at {user}'s ask,
live manifest 0.9.73, live image sha `eecce197…` = the stage; BOTH PHONES ON IT.**

📁 **2026-09-20 NIGHT: 0.9.72 — THE FILE BROWSER COPIES, MOVES AND DELETES FOLDERS.** {user}:
"no way to select a folder and copy/paste/delete... trying to delete 'home' in the maps
folder." `[ This folder... ]` inside any non-root folder → Copy / Move / Delete for the folder
you stand in. Move = one rename; Copy and Delete are JOBS (`FilesApp::Job`, placement-new in
PSRAM, `stepJob()` 80 ms per 30 ms app-timer tick, directory handles kept open per level ≤ 6,
POSIX unlink/rmdir, Back stops). Delete confirms with a live COUNT. `files_paths.h` +
`test_filepaths` (22). Reviewed by 20 agents (3 lenses); the real ones — the running tile
download's area (refused), confirm keys 101/102 vs browse 1/2, uint64 bytes, first error kept,
readdir errors via errno, audio DMA fed inside the slice — all in. Measured on phone 2:
/maps/home (271 tiles, 33 MB) copied in 199 s, deleted in 16 s, no LOOP STALL from the job.
**PUSHED (`e1bcd16`) AND PUBLISHED, live sha `b1c639e5…` verified; BOTH PHONES ON IT.**
`/maps/home` is still on both cards — {user} deletes it himself now, from the phone. Bench: `open
files`; `tools/wiphone_send.py --app maps --tree <dir>` plants a scratch tree; `shot` sees the
browser; a `key back` from a folder clears the note, which shifts the row numbers by one.

🗺️ **2026-09-20 EVENING: 0.9.71 — the map says WHERE each area's tiles are.** {user}: "what is the
'home' map? I don't have any tiles for it." `home` = the hand-converted North Bend/Snoqualmie
tiles from the first maps release (z11-15, ~47.28-47.52 N / 121.99-121.64 W — the pins are in
it), 35 km from the hunt area; the downloader only writes its own three areas. Now
`scanAreaExtent()` reads each area's box at scan time (x folders at zMin + y files of the first
and last), `setArea` says "its tiles are 35km NW of here", the Map area rows end `here` / "35km
NW", serial `maps` prints the boxes. **PUSHED (`6415d63`) AND PUBLISHED, live sha `3417e3b4…`
verified; BOTH PHONES ON IT.** No on-phone way to delete an area (the computer does that).

🗺️ **2026-09-20, LATER: 0.9.70 — the map strip's message wraps to two rows when it needs them
(`noteRows` via `menu_wrap.h`; the no-fix explanation was the one cut off), and the FOURTH side
button cycles the map areas (`setArea((areaSel+1) % areaCount)`; Menu → Map area is the same
switch). Controls guide: button 4 is drawn `map type` — eight characters, its row shares the
line with the D-pad's "arrows scroll" caption. PUSHED (`e4e7d70`) AND PUBLISHED, live sha
`c77eedd9…` verified; BOTH PHONES ON IT.** {user}, on 0.9.69: "Otherwise so far it is working well."

🧲 **2026-09-20 AFTERNOON: 0.9.69 — THE MAP'S SNAP IS A LANDING RULE.** {user}: *"make the snap
less aggressive... I still want to be able to scroll around them, but if I just land near one
while scrolling (as near as I possibly can) then I want it to snap... make each d-pad click
move less since it already will accelerate as you hold it."* A tap is a fixed 12 px nudge
(taps no longer accelerate; a hold still climbs 24→48→72→96); the map snaps only when the
crosshair LANDS within 10 px of a pin/place/node — after a tap or where a hold stops
(`snapLanding`). The path-stop and the marker-to-marker arrow walk are gone (`7`/`9` still walk
pins). `MAP_PAN_NUDGE_PX` / `MAP_SNAP_RADIUS_PX` in map_tiles.h; `test_maptiles` pins radius <
nudge (one tap steps clear) and radius ≥ nudge/2·√2 (any marker reachable). Proven on phone 1
(phone 2 was unplugged) with a bench pin, removed after — see CHANGELOG 0.9.69. 🛑 A 32-bit `long` overflow in `snapNear`'s
distance (a far node "within 10 px") was caught on phone 2 before shipping — the pin picker
already guards it; any new distance test on view coordinates must reject BY AXIS before it
squares. **PUSHED (`c118cd0`) AND PUBLISHED at {user}'s ask, live sha `09dc8239…` verified; BOTH PHONES ON IT.**
⚠ **`ver`'s build stamp does NOT tell two builds of one version apart**: `__DATE__/__TIME__`
are baked into the ONE translation unit that prints them, and an incremental `pio run` that
recompiles only app_maps.cpp relinks with the old stamp — both phones said `13:43:13` after
the 15:0x rebuild. Proof that a phone runs a given binary is esptool's `Hash of data verified`
line (flash.sh prints it — do not `tail -1` it away) or the behaviour itself. Bench notes: the `maps hold` tap is dequeued
0-200 ms after the command, so a hold's repeat count is ±1 — plan release-snap tests so
either count lands within the radius; OK off a marker DROPS A PIN (the Rename dialog), so
screenshot before any OK on the map.

🗺️ **2026-09-20 MORNING: 0.9.68 — THE DOWNLOAD SCREEN WRAPS; A REFUSED TILE WRITE IS RETRIED.**
{user}: *"On the maps download screen there is still text that can get cut off... the status of
the previous download attempt and the file size"* and *"something about card refused... idk
what that means."* `MenuWidget::addNoteWrapped()` (breaking in `menu_wrap.h`, `test_wrap` 42
checks) — every note on that screen and the Maps app's other variable notes wrap; the fetcher's
last error shows as `Last problem: …` + "Start again fetches only the missing tiles"; a short
`f.write()` on a tile ("card refused the write" — one SD busy timeout; phone 2 lost 4 of 12,853
tiles to it) is tried once more before it counts. **PUSHED (`c35cd4b`) AND PUBLISHED, live sha
`d4c0706e…` verified; BOTH PHONES ON 0.9.68.** Bench note: on the map, the left soft key is
`key select` (opens the menu); Back from Help lands on the MENU, not the map.

🎮 **2026-09-19 NIGHT: 0.9.67 — THE GAME BOY RESUMES WHERE YOU LEFT IT.** {user}: *"save state
upon exit and open last state upon entrance. That's how covey does it. I still want to be able to
clear state from the in-game END menu, and manually save state from the same menu. Default screen
'fill'."* Built, reviewed by 22 agents (4 lenses + refute-or-confirm; the confirmed findings are
all in — the destructor-deletes-the-blit-task-mid-write hang, the bounded park wait, the menu box
past the Fill frame, the state keyed by file name only, NVS at power-off), and proven on BOTH
phones (phone 1 on an earlier test build, phone 2 on the final). `CHANGELOG.md` 0.9.67 has the
design, the numbers and the proof lines. **PUSHED (`f5e9103`) AND PUBLISHED at {user}'s ask, 2026-09-19 ~21:30: the live flasher serves
0.9.67 and its image sha matches the stage byte for byte (`2386808a…`).** BOTH PHONES RUN IT
(phone 1 flashed ~21:45 once {user} pressed its button; its earlier `uCity.auto` was adopted into
the cart-id name and resumed, 350 ms; its Screen pref put back to Fill after my bench toggle).

Shape: two files per ROM under `/gbc/`, named `<clean>-<cartid>.auto` (resume point, written by
Quit and by both power-off paths) and `…state` (the manual bookmark); `Clear state` (asks) wipes
both + hard reset; all card work on the BLIT task (`runAction`), the main thread parks the
emulator (`autoSaveNow`: paused → wait emuIdle → job → poll); writes go `.tmp` → size check →
remove → rename; loads size-check first, hard-reset on a failed read of an existing file; pre-0.9.67
`<clean>.state` files are adopted once. Serial: `open gbc`, `gbc`, `gbc autosave`.

🛑 **FOUND ON THE WAY AND FIXED: THE PHONE COULD POWER ITSELF OFF DURING A GAME.** Phone 1 shut
down three seconds into a launch on the bench (the log showed the power-off path running — it
saved the game first). `SX1509::digitalRead()` answers LOW for a pin it could not read; the power
button is active-LOW; `gpioExtenderServiceInterrupt()` latched it as pressed; 2.5 s later, with no
release edge to clear it, the hold handler pulled the latch. Now `readPinChecked()` + a
confirming re-read before power-off (WiPhone.ino, `POWER:` log lines). (Phone 1 was off until {user}
pressed its button; it now runs the published 0.9.67.)

⚠ Bench notes for the Game Boy: `shot` shows NEITHER the game NOR the picker (both draw direct to
the LCD) — `gbc` (status) and the `GBC:` log lines are the eyes; the picker reopens on row 2
(uCity) every time, so a specific ROM is N × `key down` with `gbc` to confirm the row; after
`gbc autosave` the game is parked under the pause menu with Resume selected — `key ok` resumes.

🔧 **2026-09-19: {user}'S SEVEN ASKS, ALL FIXED, REVIEWED, AND PROVEN ON BOTH PHONES. `main` holds
0.9.66 with the webflasher stage regenerated from THIS binary (version read out of the .bin,
per the release rule; 0.9.65 was staged an hour earlier and retired unpushed when the snap grew
to places and nodes — one version string, one binary). PUSHED (`d9d2976`) AND PUBLISHED at {user}'s ask, 2026-09-19 ~18:10: the live
flasher serves 0.9.66 and its image sha matches the stage byte for byte (`c999b864…`).** The reports, verbatim: (1) "Any menu or line
that is too long just cuts off, it should either scroll or wrap text when highlighting over it."
(2) "The wiphone lets me send messages that are too long on meshtastic, and they just won't get
through to anybody else." (3) "Vibrate length of time is very inconsistent... sometimes the motor
just barely starts. Also when sound is enabled for notifications, sometimes sound will play, but
sometimes it won't." (4) "For the game boy emulator, I believe the sound plays through the
earpiece instead of the loudspeaker." Full accounts with the numbers: `CHANGELOG.md` "Unreleased".

**Root causes, each read out of the code BEFORE any fix, then measured:**
1. 📜 **Every list draws through `MenuWidget` → `guiDrawEllipsized()`**; one marquee there covers
   all four row kinds and every app. The selected row scrolls by whole glyphs (800 ms hold / 200
   ms per glyph / 1 s end hold / wrap) on its own tick (`GUI::marqueeTick`), NOT the app timer.
   `menu_marquee.h` + `test_marquee` (50). Seen on phone 2: Books' Leviathan row scrolls to
   `...1.epub` and wraps; short-row screens unchanged; no new stall lines.
2. 📡 **`MeshPhy::send()` gave up after a flat 2000 ms while a full LongFast frame is 2157 ms on
   air** (SF11/250k/CR4-5/preamble 16, AN1200.13 — `mesh_airtime.cpp`, `test_airtime` 46) — the
   radio was forced back to RX with the PA keyed, and `MESH_COMPOSE` ignored the `false`. The
   wait now follows the frame; the compose cap is 200 (Android/iOS `MESSAGE_CHARACTER_LIMIT_BYTES`;
   the wire holds 232 channel / 220 PKI DM); a strip shows `N/200`; a refused send stays put with
   the reason. **On air: 200 and 232 B from phone 1 heard back and read on phone 2, 233 refused.**
3. 📳 **The arrival was stamped with the loop-top `now` from inside the `mesh` block, after the
   DB save's 0.5-1.5 s stall and a full repaint** — the next pass judged the buzz and the chirp
   already over. Stamps are `millis()` now; the chirp plays from the in-flash `pop_pcm[]` (start
   19-21 ms, was a 0.5-1.6 s SPIFFS open); the SX1509 extender write is checked. **Phone 1
   `notify` ×3: buzz off 651/650/653 ms (target 650).** A real arrival behind a 1,062 ms `mesh`
   stall buzzed 869 ms — the save that FOLLOWS an arrival can hold the off-switch late (long,
   never short). ⚠ **The board's extender is an SX1509 (`GPIO_EXTENDER 1509`), not the SN7325
   block that sits first in Hardware.cpp** — read the `#if` before trusting either.
4. 🎮 **`startGame()` chose no output**, so `Audio::start()` powered up on whatever routing was
   last — the earpiece. Loudspeaker unless headphones, restored in the destructor on its own
   flag (`soundOn` is cleared by a starved I2S). **Phone 2: `route=loudspeaker gbc=1` in
   MICROCITY, `route=earpiece gbc=0` after quitting through the game's menu.**

**Also found on the way:** a message with a newline drew its second line over the sender in the
thread and Chats lists (flattened); a refused serial `send` said "watch for MESH RECEIPT"; the
console's `open <app>` entered an app UNDER a running Game Boy (now refused). Still truncating,
deliberately: header titles, `LabelWidget`, `ChoiceWidget`, sliders, text inputs.

**Bench recipe that worked today** (see `docs/maps.md` too): both bridges up
(`tools/panicwatch.py <serial>`; phone 1 = `025A3EAF` → `/tmp/wiphone.cmd`, phone 2 = `025A3F65`
→ `/tmp/wiphone-025A3F65.cmd`); flash with `pkill -f "panicwatch.py <serial>"` (port-specific!)
→ esptool 230400 → restart that bridge; `hold on`, unlock `key ok` + `key *`, `open <app>`, one
`key` per second, `tools/bridge_shot.py`. `send <i> <text>` / `notify` / `audio` are the proofs.

5. 🕹️ **Later the same afternoon, two more: a held arrow did a single nudge (the keypad path
   hides holds from every app on purpose — the map now polls `uiKeyStillHeld()` on its own
   timer and repeats for itself; nothing else changed, per {user}'s "only edit maps"), and a
   tap flew past pins (Snap to pins, menu toggle, on by default: a tap stops on a pin in its
   way; on one, taps walk them in a 45° cone — pins, mesh places AND nodes with a position,
   at {user}'s follow-up). Both seen on phone 2; `maps hold <dir> <ms>` is the bench.** ⚠ **BRIDGE RACE: the bridge can sit up to 1 s in a serial read before
   it looks at the command file; two writes under ~1.3 s apart LOSE THE FIRST** — that is how
   `hold on` went missing and phone 2 "locked itself" mid-test. Space commands ≥ 1.4 s.

**Review (2026-09-19, two rounds, 26 agents):** round 1 — marquee and mesh clean apart from two
low findings on the marquee (a 160 B buffer on the common row path; the tick timed from the
pass's `now`), fixed in `5802882`. Round 2 — buzz, Game Boy and pan/snap: **ten confirmed, none
refuted, all fixed in `836f4be`**, the ones worth remembering: the pop's first chunk was not in
the DMA before the mesh block could stall (pump it at start); a game started WITH headphones
dropped to the earpiece on unplug (the guard was the bug — set the flag unconditionally, the
precedence lives in `Audio::start`); serial `notify` reinstalled I2S under the emulator; the
memory pop latched `playbackEof` and a failed ringtone open then re-opened SPIFFS every pass
with the motor never buzzing; a held arrow was "idle" to the DB-save gate; the hold delay sat
inside the 350 ms staleness window (phantom step); a snapped tap seeded a run; a finished hold
did not end its run. Each re-proven on the phones after the fix (`notify` 650/662 ms; game
`route=loudspeaker` → `earpiece` on quit; `notify`/`open` refused over a game with the reason).


🗺️ **2026-09-18 → 19, overnight with {user}'s standing permission: THE MAPS APP DOWNLOADS ITS
OWN TILES, AND IT IS ALL ON HARDWARE (phone 2 only).** {user} asked, then went to bed: "a maps
app with downloadable maps (to SD) from places like USGS topo; all the features COVEY has
unless unreasonable; arrows pan; top user buttons zoom; OK sets a pin, OK on a pin edits;
pins share to a chosen mesh channel; SELECT opens a menu (download / go to pin, coords, node);
the one user button centres on me; the view saved on exit; check RAM." The analysis is
`docs/maps-brief.md` (written first, before any code); the build followed it.

**What was done, in order, each step proven on WiPhone 2 before the next:**
1. **PR #1 fast-forwarded onto main** (`b76ee2c`, 0.9.64 as first built, never on hardware).
2. 🔴 **Its tiles would have been byte-swapped — and so is every raw-565 row this firmware
   ever pushed.** `TFT_eSprite` stores pixels swapped; `pushImage(uint16_t*)` does not swap
   unless told; nothing ever told it. **Measured**: a red/green/grey BMP in Photos on 0.9.63
   rendered (0,28,197)/(230,0,57)/(16,16,32); after the fix (`lcdNativePixels`, GUI.h, applied
   in app_maps, app_photos ×2, app_books) it reads (255,0,0)/(0,255,0)/(131,129,131). Photos'
   BMPs and greyscale JPEGs and Books' greyscale pictures were wrong since they shipped.
3. **Tree upload** — `up on maps` + `tools/wiphone_send.py --app maps --tree <dir>` pushes a
   converted area over WiFi (`chunkSafeTreeName`, 18 host checks). 271 tiles / 34 MB in 10
   min. Used to prove the viewer: pan, zoom, boundaries, labels, heap flat.
4. 🔑 **HTTPS.** On 0.9.63 an HTTPS `/fetch` of a USGS tile FAILED (`start_ssl_client():207
   -32512`, internal min-ever 2,676). Cause: `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y` (two
   16,717 B record buffers from a ~25 KB largest block) AND the handshake runs on the caller's
   8 KB loop stack. Escape, MEASURED with the new `tlstest` bench: `mbedtls_platform_set_calloc_free`
   → PSRAM, from a task with a 10 KB static internal stack. **70/70 kept-alive GETs, first
   1.5 s (handshake), then ~250 ms/tile, PSRAM back to the byte, task stack floor ~6 KB.**
5. **The engine** (`tile_fetch.{h,cpp}`): sources USGS Topo / USGS Aerial / OpenTopoMap /
   a serial-set custom template; ±radius square z11..zMax; skip-by-size; temp+rename; sniff;
   decode; pause for calls/WiFi loss; stop; status with heap floor + stack floor; a start
   gate (largest ≥ 14 KB, 24 KB before the stack exists). **Decoders** (`tile_decode.cpp`:
   ROM TJpgDec with private callbacks; `tile_png.cpp`: ROM tinfl + 300 lines, 41 host checks).
   **Measured**: 21 USGS tiles in 24 s; 271 in 260 s from the panel (0 failed); 33 OTM PNGs
   at ~4 s/tile (server-bound); `usgs-img` 21 in 20 s. The DB save is deferred while tiles
   are written (its ~55 small writes queued behind 32 KB pieces = 0.5–0.9 s 'mesh' stalls).
6. **The UI**: F1/F2 zoom, F3 centre (`gMapsActive` exempts the map from the music transport);
   SELECT = menu (tested BEFORE `LOGIC_BUTTON_OK`); OK = drop pin / open pin; `Menu → Download
   maps...` (source/radius/detail/estimate/Start-Stop/progress, holds the screen, gated on
   WiFi + USB-or-3.8 V); `Go to coordinates` (two fields: digits, `*` point, `#` minus);
   channel picker on share (beacon's rules; default = beacon channel; channel NAME stored on
   the pin and in `pins.txt`'s new fifth field; update/unshare go to the same channel; rows
   say "Update it on hunt-group" / "Share on another channel..."); `mapsSaveOpenView()` at
   both power-off sites; `MAPS_SELF_POOR_GPS` (fresh but <4 sats / HDOP>10 ranks below the
   pin, grey, "me? 3 sats"); the `NEW_MESSAGE_EVENT` redraw result is used. **Every one of
   these was driven on the panel over serial and screenshotted** (share + unshare seen on air
   on 'hunt-group').
7. **Bench tools that made it possible**: `open <app>` (enterApp from the console — key
   walks from an unknown screen misroute), `hold on|off` (🛑 injected keys do NOT reset the
   30 s sleep timer: the phone locked mid-walk and ate presses all evening; the `key` injector
   also drops some repeated keys — use single keys with a shot after each).

**RAM, honestly — and the soak that changed the design.** Viewer unchanged (+72 B static,
768 KB PSRAM). Downloader: an internal task stack permanently from the first download, and
every TLS handshake — the first, and each reconnect after the server closes keep-alive (~every
100 requests) — dips internal by ~12 KB for a second: lwIP's TCP send/receive buffers, not
shrinkable from the sketch (mbedTLS itself is in PSRAM). A **10 km z15 soak (863 tiles,
16.3 MB, 14 min, 0 failed) with the map open took min-ever to 952 bytes** — no reboot, but one
more allocation from it. Two changes after that: the task stack went **10 → 8 KB** (measured
floor 4,996–6,264 of 10,240 across four runs = 5.2 KB used; the report prints the floor every
run, put it back up if it ever reads under ~1,500), and **every reconnect waits for room**
(internal free ≥ 16 KB and largest ≥ 12 KB, up to 30 s, "Paused" on the screen) before it
handshakes — a start gate does the same for the first one. Expected floor now ≈ 5 KB with the
app closed, ≈ 3 KB open; the fresh-boot acceptance run's numbers are in the commit after this. **⚠ Phone 1 (SIP registered)
has 2–3 KB less headroom and has NOT run this build** — flash it, run one small download,
read `heap` min-ever. Loop-task stack floor after a fresh boot is ~3 KB (the 408 B seen on
0.9.63 was after 22 h).

**Host suite green (21 groups, +test_tilepng 41 checks, +chunkSafeTreeName 18, pins format
+5).** Firmware RAM 88,220 / flash 39.7 %. 🛑 **Not published**: no push, no webflasher
(`tools/publish_webflasher.sh`) — {user}'s call after phone 1. Left on phone 2: `/maps/home`,
`/maps/usgs-topo`, `/maps/usgs-img`, `/maps/otm` areas; pins "Pin 1", "Pin 2" (local,
unshared); `/photos/{usgs_tile.jpg,download.jpg,swaptest.bmp}`.

🔎 **THE MORNING'S REVIEW (09-19, `db0700a`): seven lenses were launched; three finished
before a usage limit and every one of their findings is fixed and re-proven on phone 2.**
The one that mattered most: **a task that creates itself per run and deletes itself RACES
ITS OWN STATIC TCB on this FreeRTOS (8.2)**, and `eTaskGetState()` reads a cleaned-up static
task as `eReady` (the NULL-list → eDeleted clause is newer) — my first fix refused every second
run "not exited yet", measured. **The downloader is now ONE persistent worker on a semaphore**
(`workerTask`), never deleted; three back-to-back runs, heap flat. Also fixed: `HTTPClient`
destructed by a normal return before the worker idles (its Strings leaked internal RAM per run);
error bodies drained or the socket dropped (a 404's tail would read as the next tile); no
Content-Length fails at once (was an 8 s stall per tile); over-cap drops the socket; `waitReady()`
merges pause + RAM-wait (never handshake below free 16 KB / largest 12 KB; 30 s → fail the tile;
10 min paused → end the run; Stop honoured everywhere); card write failures count toward giving
up + free-space check before start; a pin whose channel left the phone is not "retracted"
elsewhere; tRNS on grey/RGB; no internal-heap fallback in any decoder/TLS alloc; stack 8 KB.
⚠ **Correction to the night's claim:** the 0.5–0.9 s `'mesh'` stalls during downloads are the
PRE-EXISTING ones (09-04 handoff, median 644 ms with nothing running) — the DB-save deferral
(now capped at 5 min) did not remove them. That thread is still open and is not the map's.
✅ **OpenTopoMap tiles seen on the panel** (contours, forest, rivers, right colours) — the
PNG path is now eye-proven, not just host-tested. **Second review round** (the four lenses
that never ran + a lens over the fixes) launched at ~03:10; its survivors, if any, are the
commit after `db0700a`.

🗺️ **~03:00–03:40, after the limit reset: COVEY parity + review round two, all on the panel.**
`872f715` nearest-first Pins/Places/Nodes lists with distance + bearing ("6.2km N Clifford") and
**Follow me** (latched, 1 s poll, any scroll stops it, tracked by the fix's arrival stamp);
`f7bddab` the **ruler** (Menu → Measure from here: dashed line to the crosshair, "Ruler: 1.1km SE
of the anchor" on the strip's second row — row 1 could not fit it beside a 1 km bar); `afb630b`
round two's fixes: 🛑 **the places-news redraw was STILL lost** — my first fix OR'd into a per-pass
local the pass had already consumed (now acts on the result directly, like the mirror path);
`gMapsActive` no longer grabs F-keys while LOCKED with the map underneath; a pin whose channel
LEFT the phone is refused by `sharePin()` (rename/move used to re-send it on the automatic
channel — an unretractable copy), its options say so, and delete keeps it; the download form
shows a refused Start and rebuilds after a call ate its ticks; `maps dl` validates
radius/zoom/centre (a negative radius read as an antimeridian crossing = the whole world);
`maps dlurl` validates; `open` refuses during a call (enterApp deletes the CallApp);
🔴 **FilesApp's field-by-field `XferConfig` never set `tree`** (PSRAM is not zeroed → the
general uploader ran in tree mode on some boots); tree segments cap at 31 (the viewer's
area-name limit); `wiphone_send.py --replace`. Final build on phone 2: 19/19 tiles, 0 failed,
min-ever 6,308 app closed. **Round two finished 04:15: 28 agents, 16 findings held (13 of
them already fixed by the time the skeptics read the tree), 7 refuted, 0 unverified.** The last
three real ones are in `HEAD`: the consecutive-failure counter was reset on HTTP 200 BEFORE
decode/write, so a refusing card could never reach the give-up (now reset only on a written
tile, and undecodable bodies count); the "no SD card" refusal tested a value `cardFreeBytes()`
never returns (now `SD.totalBytes()==0`); every refusal in `tileFetchStart` now happens before
the job record is cleared, so a refused Start keeps the previous "Last run" line; and `open`
refuses on the SIP state (the CALLER's CallApp lives inside DialingApp — `callApp` is only the
callee's). Final build: 21/21 tiles, 0 failed, min-ever 6,736; suite green.

📱 **09:50–11:30, {user} plugged phone 1 in ("I'll look after I go out"), and asked for Maps on
the main screen with an icon, then a key of what each button does.** Both done and on both
phones: `4753ffb` the icon (`tools/make_icon_maps.py`, a folded map + route + pin, the Music
icon's pipeline) and the row under parent 1 between Music and Tools; `05755ef` **"What the
buttons do..."** is the map menu's FIRST row — a drawn key (side buttons down the left edge as
on the case, D-pad with OK as the pin, soft keys, shortcuts; More → features + colours).
🛑 **Help rows must be ≤ ~34 chars at AKROBAT_BOLD_16**: a longer row is not clipped at the
right — the smooth-font renderer cuts BOTH ends (measured: "Hold an arrow" lost its H).
**Phone 1's measurements changed the downloader twice:** (1) its first run (21/21, SIP stayed
registered) hit **min-ever 2,572** in the handshake — the start gate, calibrated on phone 2,
had refused it outright ("21 KB block"); the bar is now the reconnect bar + the stack
(largest ≥ 10 KB / free ≥ 14 KB after the stack), which is what phone 1 has with SIP up.
(2) A **per-run dynamic task** (stack freed after each download, to give phone 1 its 8 KB
back) was tried and REJECTED on evidence: 26.3 KB largest before a run → **11.3 KB twenty
minutes after**, free total recovered — fragmentation, the phone's known killer — so the
**persistent worker is back** (fixed 8 KB from the first download; phone 1 settled five
minutes after its run at free 17.3 KB / largest 12.3 KB, from 25.8 / 20.5 KB before —
predictably, and honestly thin). (3) **A download that loses WiFi lets go**: phone 1 dropped WiFi during
a run's first handshake (it did that after every reflash this morning — the deaf-scan spell;
✅ **the 0.9.31 self-bounce FIRED and rejoined, hands-free, twice — first time seen in a real
wedge**) and the paused job sat holding TLS + buffers while the rescue ran against largest
9 KB. Now: pause → `client.stop()` at once; > 20 s without WiFi → the run ends ("start again
to continue"; skip-by-size resumes); a call still pauses ≤ 10 min. Proven on phone 2 with
`wifi drop` mid-run: 1 reconnect, 1 failed tile, run continued. Phone 1's soak on the final
build is the commit after this one.

⏳ **OPEN / OWED:** (a) push + `tools/publish_webflasher.sh` — {user}'s call;
(c) OTM's 4 s/tile makes a 10 km OTM area a 1 h job — the screen says so;
(d) COVEY parity not done: ruler, follow-me latch, N/F toggles, nearest-first GO picker
(the PR's lists are by identity); (e) `docs/maps-brief.md` §10 decisions were taken as
recommended (F3 = centre; raw .565 kept; per-share picker; in-app download that outlives the
screen); {user} may want to move "the one user button" to F4.

✅ **2026-09-14 — 0.9.64: MAPS. Built, host-tested and compiled; NOT YET ON HARDWARE and no
tile has ever been on a card.** Menu → Tools → Maps. Everything it draws was already in the
phone since 0.9.7 — waypoints, node positions, the reference place — rendered as a sentence.
This renders it as a picture, which is the shape of the question people actually ask.
🔑 **THE TILES ARE RAW AND THAT IS THE DESIGN, NOT A SHORTCUT.**
`/maps/<area>/<z>/<x>/<y>.565`, 256×256 RGB565 little-endian, **131072 bytes exactly**, ordinary
slippy numbering. There is no PNG decoder in this firmware and the ROM JPEG decoder refuses
greyscale outright (the reason `jpeg_grey.cpp` exists), so compressed tiles would work until the
day they met a tile of the wrong flavour, in the woods. Raw costs 8× the card (~85 MB for 20×20 km
at z12–z15) and has exactly one failure mode: the wrong length — which is also how a PNG copied
across unconverted gets refused instead of drawn as confetti. `tools/convert_tiles.py` makes them
on the Mac from a z/x/y tree or an `.mbtiles`, with nothing to install (Pillow if present, `sips`
if not).
⚠ **A 128 KB TILE READ IS 100–250 ms AND EVERYTHING SHARES ONE TASK**, so a tile arrives in 32 KB
pieces one per 25 ms tick, opened/seeked/read/**closed** per piece (no `File` alive across a
teardown or a pulled card), with the hole drawn as a marked grey square meanwhile. 250 ms is
exactly what 0.9.58's stall detector was built to complain about; four tiles the obvious way is a
second of frozen phone *per pan*. The timer is disarmed once the screen is painted.
🔑 **PINS ARE PRIVATE UNTIL YOU SAY OTHERWISE.** 64 in a text file on the card, nothing on the air
until "Share it on the mesh" — which prefers a private channel, locks the waypoint to this node,
and **says whether the radio actually sent it**. Rename/move re-send; "Take it off the mesh" sends
the positionless deletion marker (the only way to remove one; `expire == 0` means never).
📐 **MEASURED: +72 bytes of internal RAM, +37 KB flash**, by building the tree with and without it
(87532→87604 RAM, 2544759→2582199 flash). Working set is 768 KB of PSRAM, only while open.
✅ **PROVEN BEFORE THE PHONE, because a map one tile out looks exactly like a right one**:
`tests/test_maptiles.cpp`, **110 checks**, Web Mercator against independently computed values and
the blit rectangles checked *structurally* — every viewport pixel covered exactly once, from the
right pixel of the right tile, across the antimeridian, with a world shorter than the screen, at
z19 after a two-billion-pixel pan. `test_pos.cpp` gained the Waypoint builder's bytes.
🔎 **AND BECAUSE A SCREENSHOT CANNOT TELL RIGHT GROUND FROM WRONG GROUND EITHER:** `maps` prints
what the card holds, the saved view and how the pins file parses; `maps goto <lat> <lon> [z]
[area]` writes the same saved view the app writes — so a known coordinate goes in over USB and the
screen is compared against a map on the computer. That is the FIRST thing to do on hardware.
🔬 **AN ADVERSARIAL AUDIT RAN OVER IT — 9 lenses, each finding re-read by a skeptic whose job was
to refute it. 47 confirmed, 17 thrown out.** Every confirmed one is fixed. The eight that would
have been found only by a thumb, in the woods:
⛔ **THE TEXT DATUM.** `textdatum` is a plain member of the ONE page sprite everybody draws into,
and GUI paints the app first and the footer after — so from the second frame onward every string
on the map inherited the footer's `MR_DATUM`: lifted 8 px (the top chip smearing into the header)
and slammed to x=0, detached from the black box it belongs to. One line, `setTextDatum(TL_DATUM)`,
and it is the kind of thing that no test and no amount of reading the app alone would ever show.
⛔ **A 40 Hz BUSY LOOP ON THE ORDINARY ACT OF PANNING TO THE EDGE OF COVERAGE.** A tile with no
file blacklisted itself, but the blacklist is only consulted in drawMap(), and a failed piece
returns DO_NOTHING so drawMap never ran — forty failed five-level SD path walks a second, forever,
each one evicting a real cached tile.
⛔ **`mapPinPickNearest` SQUARED 32-BIT.** Projected coordinates run to ±134 M at z19; the product
wraps NEGATIVE, and negative beats every real distance — a pin on the far side of the world would
answer "the pin under the crosshair".
⛔ **LIST ROWS WERE ARRAY INDICES.** The waypoint table compacts (`waypoints[i] = waypoints[--n]`)
and getNode() re-sorts, both under an open screen: the row said Camp and OK centred on the truck.
⛔ **savePins()'s "did it work" test was `f.size() > 0`** — satisfied by the header comment alone,
so a card that failed every line after the first still got renamed over the real file.
⛔ **A FAILED RETRACTION FORGOT THE WAYPOINT ID**, stranding a camp on everyone else's map with
nothing left on this phone able to retract it.
⛔ **THE HELP SCREEN CUT 8 OF ITS 22 ROWS** — the whole colour key — with no scroll and no hint.
⛔ **saveView() wrote 32 bytes of NVS of which the tail was uninitialised stack.**

⛔ **WHAT HAS NOT HAPPENED:** no phone, no card, no tile, no measured SD read, no measured pan
latency, no screenshot. The 32 KB chunk and the 25 ms tick are reasoned from the 0.9.58 stall
threshold, **not measured**. Six 128 KB PSRAM slots is a choice, not a measurement.

✅ **2026-09-09 ~14:30 — 0.9.63: the sync card holds the screen, and it is MEASURED, not assumed.
Both phones flashed, flasher republished, every device back on its own page.**
The last finding from the 0.9.61 review: the card was the only Books screen that never called
`holdScreenAwake(true)` — and it is the one screen that exists to be READ before you answer it.
On the library→openBook path (the hold is released entering BOOKS_LIB and BOOKS_READ never runs)
it dimmed at 20 s and slept at 30 s. ⛔ **The DIM BAND is the dangerous part: a keypress is still
delivered to the app until brightness reaches 0, and by then `millis() - syncCardMs` is ~20000,
far past the 600 ms arm — so a press meant only to bring the screen back RAN `applyPending()` and
took the jump.** The 0.9.61 accident, reached by a different door. ⚠ Pre-existing (`4870c7a` has
the same holdless case), but 0.9.61 made it matter by asking the reader to spend time there.
🔑 **PROVEN ON THE PANEL WITH `scr=`, NOT WITH A SCREENSHOT.** A framebuffer capture cannot show
backlight — dimming changes PWM, not pixels, so the card looks identical dimmed or not. The
HEALTH line's `scr=` IS the brightness. Sat on a live card for 110 s: **`scr=40` at t=12, 24, 42,
54, 73, 85, 98 s — flat.** Pre-fix that reads 15 at 20 s and 0 at 30 s.
🔎 **A READER ASYMMETRY WORTH KNOWING, found while restoring the devices:** phone 1's Leviathan
came back from a driving session at `pageStart 8149` instead of `8118`. Not drift — it was stable
across open/close/reopen — and not a save fault. **`prevPage()` with `histN == 0` recomputes the
previous boundary by laying out BACKWARDS, and that need not match the boundary the forward pass
produced**, so a forward-then-back round trip can land tens of bytes off. 31 bytes ≈ five words
here. ✅ **Restored EXACTLY by syncing from phone 2 and accepting** — `applyPending()` goes through
`epubLocate(fraction)`, which reproduced `8118` to the byte — which also re-proved the card path
on 0.9.63.
✅ **FINAL STATE, every value re-read after the last flash:** both phones `firmware 0.9.63`,
`Hash of data verified` ×4 each; phone 1 Leviathan `spine 8 / 8118`, phone 2 Leviathan
`spine 8 / 8118`, phone 2 Ghosts `spine 24 / 2382`; COVEY untouched all day (`books.json` mtime
still 08:42:37). Flasher live: `nikguy321.github.io/wiphone-meshtastic` serves **0.9.63**, served
sha256 `b762b7d96544b45f…` = staged. Host suite green, RAM 26.7% / Flash 38.8%.


🔎 **2026-09-09 ~14:00 — 0.9.62: REVIEWING 0.9.61 FOUND THE SAFETY NET EATING THE THING IT SAVED.
BOTH PHONES FLASHED AND VERIFIED; FLASHER REPUBLISHED.** A 45-agent adversarial review of the
same morning's changes. ⚠ **Only the `card` and `undo` dimensions got their verifiers — the
`save`/`store`/`power` verifiers died to a usage limit, so those were re-checked BY HAND against
source before anything was changed.** Five defects, **four introduced by 0.9.61 itself**:
⛔ **`posStore` removed `.bak` UNCONDITIONALLY before renaming the live file into it.** Fine every
ordinary time and catastrophic in the ONE state the mechanism exists for: a store left `.bak`-only
by a torn write has no real file, so the remove deleted the sole readable copy and the rename then
had nothing to move. **The recovery path destroyed what it was recovering.** Now it only vacates
`.bak` when `SD.exists(BOOKS_POS_FILE)`.
⛔ **`posLoad` fell back to `.bak` only when the real file would not OPEN** — so a zero-length real
file, exactly what a half-finished swap leaves, shadowed a good backup and every book read as
unsaved. It now falls back on anything shorter than the magic line.
⛔ **The `POWER_OFF_EVENT` arm added in 0.9.61 COULD NEVER WORK.** All three dispatchers
(`WiPhone.ino:1195, :3557, :3606`) call `powerOff()` **before** `processEvent`, so it wrote against
a rail already dropping — and duplicated the pre-latch write on the low-battery path. Removed, with
a comment saying why it looks right and is not. 🔑 **The everyday hold-END power-off had NO
pre-latch save at all; it has one now.**
⛔ **"Undo the jump" never expired** — armed for the whole open-book session, sitting in the reader
menu an hour later one press from discarding everything read since, on the row Sync settings /
Close book normally occupy. **Five-minute TTL** (`BOOKS_UNDO_TTL_MS`, `undoOffered()`).
⛔ **`savePosition()` reported SUCCESS when `store` was null** — hiding the exact class of silent
failure the return value was added to expose — and `saveFailed` was never cleared on open, so a red
`SAVE FAILED` carried to an unrelated book. Both fixed.
⏳ **STILL OPEN from the same review, deliberately not acted on yet:** the sync card is the only
Books screen that does not `holdScreenAwake(true)` (`app_books.cpp` BOOKS_SYNCCARD case), so on the
library→openBook path it dims at ~20 s and sleeps at ~30 s — **and in the 20-30 s DIM band a
keypress meant only to wake the screen is still delivered to the app** (`GUI.cpp:1450` swallows it
only when brightness is 0), by which time the 600 ms arm has long expired, so the wake press takes
the jump. ⚠ **Pre-existing, not introduced by 0.9.61** — `git show 4870c7a:WiPhone/app_books.cpp`
has the same holdless case — but 0.9.61 makes it matter because the arming window asks the reader
to spend time on that screen. Recoverable via the undo. **Fix: `holdScreenAwake(true)` as the first
line of the BOOKS_SYNCCARD case, and re-stamp `syncCardMs` on a wake.**
✅ **Host suite green (19 groups), 0.9.62 verified in the ELF, both phones flashed
(`firmware 0.9.62, built Sep 9 2026 13:46:36`, `Hash of data verified` ×4 each), flasher live
(`nikguy321.github.io/...` serves 0.9.62, served sha256 `c629b52d038ec583…` = staged).**
✅ **EVERY DEVICE BACK ON ITS OWN PAGE, re-read after the flash:** phone 1 Leviathan `spine 8 /
8118`, phone 2 Leviathan `spine 8 / 8118`, phone 2 Ghosts `spine 24 / 2382`; COVEY untouched
(`books.json` mtime still 08:42:37, Leviathan `sp8 off7390`, Ghosts `sp24 off1911`).
🛑 **METHOD NOTE WORTH KEEPING: the review found by READING what a green host suite and a
successful hardware test both missed.** 0.9.61 was suite-green, built, flashed, and proven
end-to-end on two phones — and still shipped a data-loss bug in the very function written to
prevent data loss. **Neither tests nor a live demo substitute for someone adversarial reading the
diff.**


📖 **2026-09-09 — {user}'S LOST NIGHT OF READING: IT WAS THE SYNC CARD TAKING THE KEYPRESS THAT
OPENED THE BOOK, NOT A FAILED WRITE. Fixed in 0.9.61, BUILT AND TESTED, 🛑 NOT FLASHED.**
{user}: *"I read a good amount and I believe pushed sync place and then closed the book. But this
morning, both wiphones were at the place I started at last night."* 🔑 **His own guess was right
about the WHAT and wrong about the WHEN: an older sync place was taken, on OPEN, not on close.**
`app_books.cpp:1796` — opening a book is OK in the library, and a parked position lands you on
the CARD instead of the page, with **"Go there" on that same OK**. The reflex second press (or
one queued in the keypad buffer during the open) took a position nobody had read the offer for;
`applyPending()` writes it to the card at once and `dropParkedForThisBook()` retires every
offer, **so nothing is left to show it happened and "Parked positions" reads 0 either way** —
⚠ a 0 count is CONSISTENT with the trap having fired, not proof it did not. COVEY was up until
**20:15** that evening holding a stale `sp:8 fr:0.120272 t:0`, and is where the packet came from.
⚖️ **Not a fluke — the trigger is the normal workflow:** sync while the book is shut, then open
it. It re-arms every time a position parks; it only HURTS when that position is stale.

🔬 **HOW IT WAS PINNED, and the method is the reusable part.** `health.log` has no timestamps,
only `up=Nmin` — **back-calculate from the uptime at the moment you probe** (249 min at a 09:57
probe ⇒ that boot was 05:48). **`psram=` is an app-open tracer**: the reader drops 3.49 MB →
2.79 MB while a chapter is loaded, so open/close cycles are readable minute by minute. That gave
four Books sessions on 09-08 — **17:02-17:27, 17:54-17:56, 19:21-19:45, 21:22-21:33** — and
proved the book **CLOSED CLEANLY** at 21:33 (psram back up, cpu 240→80 MHz, screen off). The
`BOOT reset_reason=1` at 05:48 was at **soc=77% v=3.96** — nowhere near the 3.3 V cutoff, a
spontaneous hard power cycle, and a **red herring**. `wifi=0` all session ⇒ no NTP ⇒ every
`turnedAt` stamped 0, which is exactly the `t:0` COVEY holds.
🛑 **THREE OF MY OWN THEORIES DIED, each to a measurement, and the order matters:**
1. **"The SD write failed silently"** — the leading theory for an hour, and a subagent harness
   reproduced the symptom byte-for-byte from a refusing store. **Killed by asking {user} to jump a
   CHAPTER and reopen: Book info read "ch 19 26%", green.** ⚠ **A harness that reproduces a
   symptom proves the mechanism is SUFFICIENT, never that it is what happened.**
2. **"COVEY has no position for this book"** — I had read `~/.covey/books.json`. **covey-ui runs
   as ROOT: the live store is `/root/.covey/books.json`.** The `covey` user's copy is a dead
   orphan from Aug 6. **Check the unit's `User=` before trusting any dotfile path on COVEY.**
3. The 16 KB `bsAlloc` and the `nIds<=0` early return — both refuted (`ps_malloc`, 3.49 MB free;
   the `fp:` fingerprint id is always emitted).
⚠ **Book info's percentage is a ROUNDED int** (`:1472`), so a test that moves you WITHIN a
chapter proves nothing — {user} caught this himself. Use the **chapter number**, or `bookpage`
over serial, which prints `spine=` and `pageStart=` raw.

✅ **WHAT 0.9.61 CHANGES.** (1) **"Go there" is armed 600 ms after the card appears** — both
routes onto it, the open and the one-second tick while reading; "Stay" answers at once. (2) **A
BACKWARD jump says so**, on the button and in red. (3) **One step of undo** in the reading menu
for the place a jump overwrote. **Four durability defects fixed alongside, none of them the
cause:** both flush sites **discarded `saveIfDirty()`'s bool** — `savePosition()` now returns it,
logs `BOOK SAVE FAILED` and Book info shows it (a card refusing writes was indistinguishable,
all session, from one that works); **`posStore()` removed the live file BEFORE the rename**, so
a failed rename or a dropped rail took EVERY book's place — the opposite of what its own comment
and `bookstore.h` both promised — now the outgoing file is kept as `.bak` and `posLoad` falls
back to it; **nothing saved on power-off** (`bookstore.h` had asked for a low-battery caller
since it was written — `booksSaveOpenPosition()` is it, called before `powerOff()` drops the
latch); **`prevPage()` never advanced `turnsSinceSave`**, so BOOKS_SAVE_EVERY's "at most three
turns" did not hold backwards. 🔑 **`tests/test_bookstore.cpp` gains the case the suite never
had — an io whose `store()` and `load()` FAIL, which is exactly where this class of bug lives:
57 → 111 checks.** Host suite green (19 groups, 0 failed), firmware builds, **RAM 26.7% / Flash
38.8%, unchanged**, `0.9.61` verified present in the ELF (3 hits, no `0.9.60`).
✅ **PUSHED AND PUBLISHED, 2026-09-09 ~11:20.** `main` is at `13ad81a`; the webflasher was staged
BEFORE the release commit and the version read **out of the merged binary** (2 hits of `0.9.61`,
no older string) rather than off the manifest — the 0.9.57 lesson. **Verified against the LIVE
site, not the push:** `nikguy321.github.io/wiphone-meshtastic/manifest.json` serves `0.9.61` and
the served `wiphone-merged.bin` is sha256 `861bd05e2073e6a0…`, byte-identical to the staged one.
✅ **BOTH PHONES FLASHED TO 0.9.61 AND VERIFIED, ~11:30-11:46.** `Hash of data verified` on both
over the cable, and **the version read back off each device** (`ver` → `firmware 0.9.61, built
Sep 9 2026 11:13:45`), not taken from the flasher. Phone 1 100% / radio off as {user} left it;
phone 2 100%, `wifi=3`, `sip=1`.
✅ **AND THE REFACTORED FLUSH PATH IS PROVEN ON HARDWARE, not just compiled.** 0.9.61 rewrites
`nextPage`/`prevPage` to route their flush through `savePosition(++turnsSinceSave >= …)`, so a
mistake there would silently break saving — the exact fault just fixed. Driven over USB on phone 2
with `key`/`bookpage` (which prints `spine=`/`pageStart=` RAW, so no rounded-percentage ambiguity),
using **Ghosts_of_Timkovichi, not Leviathan**, so {user}'s live read was untouched:
`pageStart 2382 → 4461` (6 turns) → close → **reopened at 4461** → `→ 6275` → close → **reopened
at 6275**; and 4461 also survived a REBOOT between runs. Backward paging then walked it exactly
back to **2382** and a reopen read 2382 off the card — which also exercises the new
`prevPage` counter. No `SAVE FAILED` at any point. **Book restored to where it was found.**
✅ **AND THE SYNC-CARD FIX IS NOW PROVEN ON HARDWARE TOO, phone-to-phone over LoRa (~12:30).**
Staged with both ports held open at once — 🔑 **the inbox is RAM-ONLY, so opening phone 1's port
after the packet lands WIPES the parked offer**; the send and the open must happen in one session.
Phone 1 moved 8118→11936 and closed; phone 2 (behind, at 8118) sent `Sent: ch 9, 12%`; phone 1
then opened Leviathan and **the card came up instead of the page**, reading `WiPhone-{user} says: /
they are at 12% / you are at 12%` + **`this moves you BACKWARDS` in RED**, `OK: go BACK to their
place`, and a footer of **`Go BACK` / `Stay`** — all three display changes live.
⛔ **THE ARMING WINDOW WAS TESTED WITH THE EXACT KEYSTROKE THAT CAUSED THE BUG:** `key ok ok` in
ONE injection, so the second press drains on the loop pass right after `openBook` raises the card.
**Result: `pageStart` stayed 11936 — SWALLOWED.** A deliberate press two seconds later applied the
jump (`→ 8118`), and the menu then offered **`Undo the jump (back to 12%)`** at row 4.
⚠ **The undo row was seen but NOT executed** — phone 1 was by then sitting exactly on its baseline
and running it would have displaced the device again.
🔑 **A REAL UX WART THE TEST EXPOSED, worth fixing:** both percentages printed **12%** while the
banner said BACKWARDS, because 8118→11936 stays inside chapter 9 and the card prints a ROUNDED
int. The direction logic is right (it compares the raw `pending.fraction < fractionHere()`), but
the numbers on screen contradict the warning. Show a decimal, or the chapter, when the rounded
values tie.
✅ **EVERYTHING PUT BACK, verified by reopening each book after a reboot:** phone 1 Leviathan
`spine 8 / 8118`, phone 2 Leviathan `spine 8 / 8118`, phone 2 Ghosts `spine 24 / 2382`, all
`pending=0`. **COVEY was never driven and is byte-unchanged** — `/root/.covey/books.json` mtime is
still 08:42:37 (this morning's boot), Leviathan `sp8 off7390 fr0.120272 t0`, Ghosts `sp24 off1911`.
⚠ Driving lesson: **screenshot BEFORE injecting keys.** The unlock is two keys (`ok` then `*`) and
sending it to a phone that is NOT locked opens the dialer and types `+` — which then puts the
left softkey on "Call". Also **`holdScreenAwake(true)` in BOOKS_READ is why a phone parked in the
reader never locks** while one on the home screen does; that asymmetry is the app, not the phone.
🔑 **Book open is 1.6 s, not the 12.4-12.6 s the comment at `app_books.cpp:592` still quotes** —
measured four times today (`BOOK OPEN 1600-1609 ms [epubOpen≈1230 chapter≈362]`). That comment is
stale; the WiFi-wedge reasoning built on it (a 12 s window with the superloop frozen) is now a
~1.6 s window.
⚠ Phone 1 was rebooted once by my serial probe at ~09:57 (read-only: `ver`, `ls`, `health all`).


🛠️ **2026-09-07 ~16:20 — 0.9.60: the WiFi auto-join failure {user} hit today, root-caused and
fixed in the tree.** {user}: *"I had to manually join."* Phone 1's radio had been off ~22 h;
switching it back on produced two auto-switch scans that completed `n=0` **while six APs were on
the air** — a manual `wifi scan` on that same radio minutes earlier listed all six, SmithWifi
strongest at −59 dBm — then `[wifi] join skipped: last scan saw no saved network (1 in a row)`,
and he joined by hand 7 s later. **Root cause: the dry-spell clock counts the time the radio is
deliberately OFF.** `autoSwitchTick()` maintains `_drySpellStartMs` before its own gates and
`connected` is false with the radio powered down; it was cleared in exactly one place, on a
successful connection. So the phone came back already 22 h into a "long dry spell" and all four
readers of `inLongDrySpell()` misfired in the same pass — deaf-radio cure disabled
(`Networks.cpp:671`), join gate stopped failing open (`:809`), scan cadence 5 min not 2 (`:838`),
join retry 10 min (`WiPhone.ino:3207`) — while the radio sat in the documented mid-connect deaf
state, each empty scan stamping fresh "the air is empty" evidence that suppressed the join. New
`Networks::forgetDrySpell()` clears the clock, the deaf accounting and the stale air evidence on
all three radio-ON paths via `resumeReconnect()`. It also clears `_dryBounced`, which survived
off/on round trips and would otherwise certify a new radio's first empty scan as proof. **Easing
untouched** — the reset fires only on a deliberate radio-on, never on a drop, so a phone genuinely
out of range is back on the eased cadence 5 min later and 0.9.50/0.9.51's battery win re-engages
on the same schedule. Webflasher rebuilt and published at 0.9.60, version verified inside the
merged binary, sha256 `bc847f58af54…`.

✅ **VERIFIED ON HARDWARE, same session ~16:38.** Phone 1 flashed to 0.9.60 (built Sep 7
16:18:13) and the afternoon's failure re-run deliberately: WiFi switched **off at 16:30:00**
(`radio switched OFF by the user`, gates `ud=1 rec=0`), left off **7 minutes** — well past
`WIFI_DRY_SPELL_LONG_MS`, so the dry clock accrued exactly as it did over the 22 h that caused
the bug — then switched back **on at 16:38:10**. Result:

```
WIFI: radio switched ON by the user (persisted)
[autosw] ud=0 rec=1 en=1 conn=0 scanning=0 sinceScan=600s   <- forgetDrySpell backdated _msLastScan: scan due NOW
[autosw] ud=0 rec=1 en=1 conn=1 scanning=0 sinceScan=604s   <- ASSOCIATED ~4 s after the toggle
[autosw] scan started (wifi status 3)                        <- CONNECTED, not status 1 mid-connect
[autosw] scan done: n=12                                     <- twelve networks, not zero
[autosw] seen 'SmithWifi' -61dBm saved=1 dis=false
```

**No `[wifi] join skipped` line anywhere** — the join gate failed open as intended. Ended
`wifi=3 joins=12/0`, clock synced (lock screen `16:38 / 7 Sep 2026`, not `00:00 / waiting NTP`).
Against 0.9.59 the same afternoon on the same phone and AP: two scans, both `n=0` at `wifi status
1`, `join skipped`, and no association until {user} joined by hand. Note the deaf state did not
arise at all this time — joining first meant the radio never entered the mid-connect churn that
produces empty scans, so the fast-path cure was never needed. That is the fix working one layer
earlier than expected, but it does leave the repaired cure path itself still unexercised in the
field.

🩺 **2026-09-07 ~12:50 — RAM/health status check (read-only except as noted).** Phone 2
(`025A3F65`) answered over the cable: **firmware 0.9.59 (built Sep 6 07:06/07:09 — the 3c55f76
build), so the "NOT FLASHED" lines below are stale for phone 2**; phone 1's version was not
read. **Internal heap is flat, not leaking:** its retained `/health.log` (1,006 one-minute
samples, ~17 h) shows a 13.4 h idle run on battery with `heap` 26,224 → 25,980 B (−18 B/h,
two ~120 B steps at app opens, otherwise identical for hours), `largest` 25,248 B unchanged
the whole run, PSRAM flat at 3.49 MB, zero `DROP` records. WiFi scans dip `largest` to
~18.5–20 KB and recover. That run ended at `soc=0% v=3.30` — **phone 2 ran its battery flat
overnight (100 % → 0 % in 13.4 h idle, wifi=3 sip=1)** and came back on the charger ~09:27
(`reset_reason=1`), reaching 100 % by ~12:30. **Mesh stalls over the same 17 h: 362 `STALL`
records = 21.6/h, mean 709 ms, 29 over 1 s, 9 over 1.5 s, max 1,985 ms, 357 of 362 `mesh`** —
the thread below, now worse than the 1,505 ms max it quotes. ⚠ **I RESET PHONE 2 TWICE
(12:40 and 12:47) by opening its port** — DTR/RTS pre-set low did not prevent it (3/3 on this
adapter). The two `BOOT reset_reason=1 … build=Sep 6 2026 07:09:49` lines at the end of its
health.log are mine, not spontaneous. It recovered each time in ~15 s (WiFi, SIP REGISTERED,
radio ready, hears phone 1 at 12 dB). Phone 1 was left untouched: COVEY logged its hourly
`[nbr]` announces at 11:37 and 12:37 (5 neighbours), so it is alive; its heap history is the
Sep 3 `backups/health-*-p1-*` logs (9.3 h at −88 B/h, all 13 boots `reset_reason=1` = bench).
✅ **Same session: 0.9.59 webflasher published.** Rebuilt from HEAD (3c55f76; incremental, nothing recompiled — the binary phone 2 has run for 17 h IS this tree), staged with `make_webflasher.sh`, version read out of the merged binary (`0.9.59`, sha256 `53abc854d87a…`), pushed to main and gh-pages. Phone 1's version was not read (port open = reset); the flasher will offer it 0.9.59.

🔎 **2026-09-05 — {user}'S STACKED-BOOKSYNC REPORT IS FOUR SEPARATE FAULTS, ALL FIXED IN THE
WORKING TREE, NONE FLASHED YET.** *"when I sync my place on a device and then do that multiple
times without checking it each time on the wiphone (stacking sync messages) it sometimes gets
confused on which one of the places to sync to. It doesn't happen all the time though."*

Every one of these needs MORE THAN ONE packet parked, which is exactly why `testNewestWins()` —
two packets, stamps 1000 vs 5000, cleanly ordered — never saw any of them. Each was reproduced
on the host against the shipping sources before it was touched.

1. 🛑 **THE TIE-BREAK PICKED THE OLDEST.** `bookSyncInboxFindFor()` kept its candidate on a
   STRICT `>` while walking an insertion-ordered (oldest-first) array, so equal `turnedAt`
   resolved to the FIRST parked. ⚠ **And ties are the normal case, not an edge case:**
   pressing Sync again on COVEY mints a new nonce but NOT a new `turnedAt` — `_turned_at` only
   moves on a page turn — so a stack of taps is a stack of ties. **`booksync_inbox.h` claimed
   the opposite** ("its own turnedAt and nonce"); that sentence was wrong and is corrected.
2. 🔑 **AND `turnedAt` IS 0 WHENEVER THE SENDER HAS NO CLOCK — THIS IS THE "SOMETIMES".**
   `sendMyPlace()` stamps `ntpClock.isTimeKnown() ? getExactUtcTime() : 0`, and **NTP over WiFi
   is the only clock this phone has** — no RTC, no GPS time, one flag set in one place once NTP
   replies. Off-grid, which is what LoRa reading IS, every packet a WiPhone sends carries 0, the
   whole stack ties, and fault 1 fires every time. On WiFi it behaves. Same phone, same books,
   bug present or absent depending on whether that sender has had NTP since boot.
   ✅ Both fixed: compare stamps only when both sides carry one and they differ, otherwise fall
   back to arrival order — the one clock this device can always trust.
3. 🛑 **`pendingIdx` WAS A BARE ARRAY INDEX HELD ACROSS A WAIT FOR A HUMAN KEYPRESS.** Parking a
   packet evicts by SHIFTING every entry down one, `meshService.loop()` runs from the same
   superloop (`WiPhone.ino:4172`) on every one of the thousands of passes while the card sits on
   screen, and nothing re-validates — the `syncSeqSeen` re-check lives only under `BOOKS_READ`.
   The jump itself was always right (`pending` is a by-value copy); it was the REMOVAL that hit
   the wrong packet, destroying the newest place unseen and leaving the accepted one parked to
   be offered again. ✅ Parked packets now carry a stable `id` and are removed by that.
4. 🛑 **ACCEPTING ONE POSITION LEFT THE OLDER ONES PARKED, SO EACH OK WALKED YOU BACKWARDS.**
   Remove bumps the sequence, the reading tick looks again, finds the second-newest for the same
   book, and raises a card indistinguishable from the one just accepted: sync 10/50/80%, take
   80%, then get offered 50%, then 10%. ✅ Acting on a book now retires every offer parked for
   that book — accept and decline alike.
5. 🛑 **AND THE DECLINE CAME STRAIGHT BACK ANYWAY, because COVEY SENDS EVERY RECORD TWICE, 7 s
   APART, BYTE-IDENTICAL** (`books.py` retransmits while `_mesh_sends < 2`; `MESH_REPEAT_S =
   7.0`). The dedup only ever scanned what was CURRENTLY parked, so once a packet was retired
   its twin was "new" again. ✅ The inbox now remembers the last 8 retired packets by hash.
   ⚠ Cleared **only** in `bookSyncInboxInit()`, which **nothing in the firmware calls** — it is
   a test reset, and the 🛑 comment at `app_books.cpp:312` says not to add a caller back.

✅ **Host suite green (`test_inbox` 122 checks, six new groups) and the firmware BUILDS** — RAM
26.7%, Flash 38.8%. 🛑 **NOT FLASHED AND NOT SEEN ON HARDWARE.** {user} was asleep; flashing needs
panicwatch stopped and he may be carrying phone 1.

✅ **2026-09-06 — AND THE THREE DEFECTS BELOW ARE NOW FIXED TOO, at {user}'s go-ahead.** Two of
them needed BOTH ends changing, which is why they waited.
* **The quantiser now rounds UP** (`ceil` in `booksync.cpp`, `math.ceil` in COVEY's
  `booksync.py`), so a chapter start no longer falls into the previous chapter. Rounding to
  nearest would only have made it a coin toss. ⚠ **`tests/vectors_booksync.h` was regenerated
  from COVEY's own module** with `tools/gen_booksync_vectors.py` — the two expressions have to
  move together, and the differential suite is what proves they did (330 checks).
  🔑 **Interoperability is NOT affected**, which is what made this safe: a receiver verifies the
  mac over the record it rebuilds from the packet and never re-quantises anything of its own.
* **Long ids match again.** Both matchers now truncate BOTH sides to the wire's 40 bytes before
  comparing, so a 67-byte `ta:` id stops being invisible to sync. No wire change.
* **COVEY no longer launders a `turnedAt` of 0 into `time.time()`** — `int(turned_at or …)` was
  truthiness, not a None check, and 0 is a real value on this wire.
✅ Host suite green, firmware builds, COVEY 60/60 on both machines and deployed. 🛑 **The WiPhone
is still NOT FLASHED** — that is the one step left, and it wants panicwatch stopped.

⚠ **ORIGINALLY LEFT ALONE ON 2026-09-05, for the record of why:** — each changes the
wire or the other device, which is not a thing to do unattended:
* **The fraction quantiser truncates downward**, so a position at the very start of a chapter
  lands at the END OF THE PREVIOUS ONE. Reproduced end to end through the real pack/unpack.
* **The wire carries only the FIRST book id, cut to 40 bytes, but both ends match it against
  their own UNtruncated ids** — ids run to 67 bytes. For an unlucky book, sync silently does
  nothing for ever, which looks exactly like a wrong passcode.
* **COVEY launders a `turnedAt` of 0 into `time.time()`** — `booksync.py:287` is
  `int(turned_at or time.time())`, and 0 is a real value on this wire, not an absent one. A
  clockless WiPhone position becomes "just now" and then propagates.
⚠ **Refuted and NOT changed, listed so nobody re-opens them:** `sendMyPlace()` stamping send-time
rather than the page turn (inert on the only path that reaches it), the unseeded `rand()` nonce,
and the bookstore's tied-timestamp eviction.

**Where things are.** Both phones run **0.9.58** (flashed this evening, `Hash of data verified`
on both, radios detected, `ver` reports it). ✅ **`main` IS PUSHED AND THE FLASHER IS LIVE AND
VERIFIED** — nikguy321.github.io serves manifest `0.9.58` and a binary whose sha256
(`42d6edc2…`) matches the committed one byte for byte. Host suite **all green, and there is a new
one**: `test_gbrom`, 24 checks. Phone 1 = `usbserial-025A3EAF` = `!00449040` (no plate); phone 2
= `025A3F65` = `!00449334` (woods backplate — battery experiments happen on PHONE 1 ONLY).

🔎 **0.9.58's headline: THE `LOOP STALL` LINE NOW NAMES THE BLOCK THAT STALLED, and it earned
itself within a minute of flashing.** Each of the superloop's 26 major blocks names itself on the
way in; the detector reports the slowest one of the pass that just ended. Measured on phone 1
straight after the flash:
| command | stall | worst block |
|---|---|---|
| `pki` | 313 ms | `serial-cmd` 312 ms |
| `health` | 503 ms | `serial-cmd` 501 ms |
| `nbr` | **644 ms** | 🔑 **`mesh` 644 ms — NOT the printing** |
▶ **AND IT IS NOT ABOUT `nbr` — IT IS THE MESH SERVICE IN ORDINARY USE.** Phone 2's retained
`/health.log`, read over the cable at 22:10 with `health all`, holds **84 STALL records: 83 of
them `mesh`, one `battery`, none `serial-cmd`.** Median **644 ms**, p90 925 ms, **max 1,505 ms**,
six over a second. Nobody was driving it — this is what the phone does while it sits there. So
the `nbr` reading was not a quirk of a big serial dump; it is the same block, and it is the
dominant cost in the whole loop.
🔑 **THE NEXT THREAD, and it is now a measurement rather than a guess:** the mesh DB save is
documented to block ~1.5 s (which matches the 1,505 ms maximum almost exactly) and is held off
while the phone is being USED via `setUiIdle` — so the suspicion is that it fires whenever the
phone is idle, which is most of the time in a pocket. Read it yourself with `health all` on
either phone; the names are in `/health.log`, so a freeze in the woods is diagnosable afterwards.

**Also in 0.9.58, both silent until now:** a **truncated ROM** no longer runs uninitialised PSRAM
(gnuboy checked only `size < 0x200` and took the bank count from the file's own header, so a
buffer under one 16 KB bank left the bank table NULL and the emulated CPU executed whatever was in
the malloc'd bank — no panic, just a dead game); the arithmetic moved to `gnuboy/gb_romsize.h` so
the host suite tests the shipping code. And **`chunkSafeName()` now refuses every leading-dot
name**, so a Mac client can no longer upload the `._` sidecars that 0.9.57 had to hide.

⚠ **THE RELEASE LESSON FROM THIS MORNING, now followed:** regenerate the webflasher stage
**BEFORE** the release commit and **read the version string out of the BINARY** — 0.9.57 shipped a
0.9.57 manifest wrapped around a 0.9.56 image because the stage was stale, and only the manifest
is human-readable. Done and checked for this release.

▶ **THE NEXT STEP IS STILL {user}'S 10-MINUTE METER SESSION, and the tooling is PROVEN.**
`tools/power_bench.py <port>` flips one consumer at a time in the handoff's order, restores every
switch in a `finally`, and asks for the reading at each step. ✅ Rehearsed end to end on phone 1
with no meter: `lcd sleep/wake`, `i2s stop/start` and `lora sleep/rx` each flipped and came back
(the phone reports them as `SLPIN(bench)`, `stopped(bench)`, `SLEEP(bench)`), light sleep returned
in **5,001 ms**, nothing left flipped. **Phone 1 is staged and passes every preflight the script
makes** — `wifi=OFF lora=rx audio=off gbc=0`, `soc=100% v=4.20 chg=1`. **Step 0 is still phone
ON − OFF, the one reading that decides cell-versus-board.**

📡 **THE MESH WAS TESTED END TO END TODAY AND IT WORKS.** `send 2 'link check …'` from phone 1
arrived in COVEY's `hunt-group` conversation as `WiPhone-{user}`. ⚠ **A phone that has just
rebooted transmits into nothing** — the first attempt vanished because opening the serial port
resets the phone and its radio had no neighbours yet. **Give it ~70 s and check `nbr` shows
neighbours before trusting a send.** Phone 1 then had two: `!00449334` at 12 dB and `!62b8d2fd`
at 9 dB. The phone confirms its own transmit with `MESH RECEIPT: '<text>' -> in mesh`.

🔋 **THE DRAIN VERDICT (six lenses, 15/15 skeptic verdicts upheld, critic run): NO SINGLE THING.**
ESP32 never light-sleeping (~20 mA typ) and the SX1276 in RX-continuous (~11 typ) are the two
big rows; the typical idle sum is 35-45 mA against **91-95 mA implied for a healthy 900 mAh cell
to the FIRMWARE'S OWN 3.30 V power-off** (WiPhone.ino:3549 `v <= 3.3` → `powerOff()`; the run did
NOT end in a brown-out). **⇒ 45-60 mA unexplained at 100 % cell health, 25-40 at 80 %.** {user}
confirmed the cell is 900 mAh nominal. Corrections that landed: WiFi OFF vs ASSOCIATED saves
**0-20 %**, not 28 % (the headline rested on one pre-`aud=` run) — the woods rule is "avoid
HUNTING"; the chip floor is 3.56 V / 1.42 h / 15.5 %; **`esp_pm`/auto light sleep is compiled
OUT of this core (objdump-proven)** — manual light sleep with WiFi off is the 15-18 mA ceiling.

**What 0.9.57 is.** (1) Phone 2's "duplicate ROMs starting with `_`" were twelve macOS `._`
AppleDouble sidecars; the Game Boy picker was the one lister that did not skip dotfiles — fixed,
and the sidecars are hidden everywhere now (left on the card, harmless). (2) Serial `ls <dir>`
and `rm </path>` — the card has never been listable over the cable before. (3) Serial `power`:
the USB-power-meter bench (`lcd sleep|wake`, `i2s stop|start`, `lora sleep|rx`, `sleep <secs>`),
every switch round-tripped on phone 1, light sleep returned in 5,001 ms. (4) `gb_hw_updatemap()`
lost its `IRAM_ATTR` to fit — **watch for Game Boy stutter on bank-switch-heavy games**, none expected.

**Smaller open items** (three of the old five were BUILT in 0.9.58 — the STALL caller's name,
the gnuboy truncated-ROM guard and `chunkSafeName`'s leading-dot rule — and are described above):
GPIO38 is both `RFM95_INT` and `USER_SERIAL_RX`, so a DIO0 interrupt wake is impossible with the
woods plate fitted; and the 3.30 V cutoff itself is a small lever (+10-25 min, measure first).

# 🔋 2026-09-03 afternoon — WHERE PHONE 1'S 80-100 mA GOES: NO SINGLE THING IS "MOSTLY
# RESPONSIBLE", THE TWO BIGGEST ROWS ARE THE ESP32 AND THE LORA RADIO, AND 45-60 mA IS UNACCOUNTED
# FOR UNTIL THE METER SESSION ABOVE IS RUN
# ⚠ THE FIGURE IN THIS HEADING WAS "10-50 mA" AND IS SUPERSEDED. The reconciled answer — see
# "THE ONE LINE TO QUOTE" below — is 45-60 mA unexplained at 100 % cell health, 25-40 at 80 %,
# against a typical idle sum of 35-45 mA. Every other figure in this section is a pre-critic draft.

{user}: "take a look at wiphone 1's curves and drain log. do a check to see if any one thing is
mostly responsible for the battery drain and look for optimization opportunities."
Six-lens investigation over the fixtures, the raw dumps and the firmware (log data, idle-state
audit, CPU/tasks, hardware budget, prior measurements, plus the ROM dupes), each high finding
adversarially checked: **14 of 14 skeptic verdicts upheld** (every cited line re-read, every
number recomputed; corrections were refinements, folded in below). Every number below is
**typical (datasheet)** unless it says measured.

**1. The answer to "one thing?" is NO.** In the run's state (WiFi off, screen off, 80 MHz, LoRa
listening, codec off) the ranked idle budget is:
| row | state the firmware leaves it in | mA (typ) | confidence |
|---|---|---|---|
| ESP32-WROVER, 80 MHz, both cores `waiti` between a 1 kHz tick + 5 ms loop + 10 ms LoRa poll | never light-sleeps (`esp_light_sleep_start` only in a debug key + the new bench command) | **20-31** | datasheet row, duty unmeasured |
| SX1276 RX-continuous, LnaBoost off | from init until a TX; never standby/sleep | **10.8** | datasheet, high |
| ST7789 panel controller, SLPOUT/DISPON behind a dark backlight | screen-off only writes backlight `ION=0` (GUI.cpp sleepScreen); SLPIN never sent | 1-8 | guess |
| I2S module + APLL + MCLK on GPIO0 | installed in `Audio::Audio()` with `use_apll`, never `i2s_stop`ped on a boot that never played (watchdog exits on `!isOn()`) | 1-3 | guess |
| microSD idle, one append per minute | mounted at boot, no power pin | 0.2-3 | card-dependent |
| SX1509 2 MHz osc (for a PWM that is at 0), CW2015, SN7326, PSRAM standby, regulator Iq, CP2104 (USB-powered — 0) | | < 1 each | |
| **sum** | | **~35-45 typical** (the table's raw span is 35-69; the critic's reconciliation is 35-45) | |
| **implied by the run** | 900 mAh / 9.12 h; 720 mAh at 80 % health | **99 / 79** | measured hours, unknown capacity; 🛑 **the run did NOT end in a brown-out — the FIRMWARE powered the phone off**: `if (v <= 3.3 && now >= 30000) powerOff();` in the battery tick (WiPhone.ino:3549; boot rule `< 3.1 V` at :1193), which drops the SX1509 latch — exactly why the next boot read `reset_reason=1` POWERON. Capacity "delivered" here is capacity TO 3.30 V ≈ 92-96 % of a to-3.0 V rating (typical LCO tail), so **a healthy 900 mAh cell delivers ~830-865 mAh and implies 91-95 mA** |

**⇒ 45-60 mA is unaccounted for at 100 % cell health, 25-40 at 80 %** (the reconciled figure — the earlier 10-50 / 10-60 drafts elsewhere in this file are SUPERSEDED), and it is EITHER the cell's health (a 2023 cell at ~60-70 %
would close the gap by itself) OR an unlisted consumer (an amp whose shutdown polarity is
assumed, a hungry SD card, a linear regulator, the panel). **Step 0 of the meter session — phone
ON minus phone OFF — is the one reading that decides which.** Do not tune anything against the
table above; the 08-22 downclock was exactly that mistake.
✅ **{user}, 2026-09-03 afternoon: "the cells in the wiphone internal are 900mah."** Nominal is
settled (and agrees with the label photo), so **99 mA is the number for a HEALTHY cell**, and
the only open variable in the gap is delivered capacity — health, not spec. The meter answers
that as well: see step 8 in THE POWER BENCH below (charge from flat through the meter's mAh
counter).
🔑 **THE ONE LINE TO QUOTE (the critic's reconciliation of four lenses' ranges):** a healthy
900 mAh cell to the firmware's 3.30 V cutoff ⇒ **91-95 mA average**; typical idle sum **35-45 mA**
(ESP32 ~20 — no datasheet row exists for "PLL on, both cores in `waiti`", the 20 mA low end is
CPU-lightly-loaded, so the core may sit BELOW 20 and the light-sleep ceiling nearer 12-18 —
SX1276 11, panel 2-8, I2S/APLL 1-3, SD 0.2-3, rest < 2); **unexplained 45-60 mA at 100 % health,
25-40 mA at 80 %.**
🔧 **The cutoff itself is a small lever nobody listed:** the tail 3.40→3.30 V took 14 min at
430-600 mV/h, so the cell was already on its cliff; lowering the power-off to 3.10-3.20 V is worth
an estimated +10-25 min (+2-4 %). The rail is the **MIC5219 LDO** (dropout ~0.1-0.2 V at ~100 mA):
a 3.1 V cell gives ~2.9-3.0 V at the rail, above flash/PSRAM minimums (~2.7 V) but with less
margin under a LoRa TX peak. Measure before changing; the boot rule at 3.1 V means a phone
plugged in below that powers itself off and charges dark (harmless).

**2. WiFi: the saving from OFF was overstated (measured, log-data lens; band widened by the
critic).** Over the SAME voltage window, this WiFi-off run took 1.17-1.21× as long as the 09-01
associated run, which reads as **OFF vs ASSOCIATED (modem sleep) ≈ 15-17 %** — but ⚠ **that
09-01 run (07:20-11:20) was on 0.9.46, BEFORE `aud=` existed and before the codec-leak fix**
(0.9.47 was flashed 11:28 that day), so 5-15 mA of invisible audio leak may sit inside its
64 mV/h; its raw rows are not retained. The 0.9.47 run's within-run "associated ≈ 60 mV/h" slope
(aud=0/0 in 225/225) is like-for-like with this run's plateau **−59.2 mV/h over 4.00-3.80 V
(R²=0.993)** ⇒ ~0-10 %. **Honest band: 0-20 %.** The "40-50 mV/h" hourly numbers were 10 mV-
quantised endpoints. OFF vs HUNTING (~106 mV/h) roughly HALVES the drain. So: **"for the woods,
WiFi off beats doubling the battery" is true against a SEARCHING radio; against an associated
one it may be worth almost nothing — the woods advice is "avoid HUNTING".** Unmeasured lever for
the associated case: `WIFI_PS_MAX_MODEM` + `wifi_sta_config_t.listen_interval` (Networks.cpp:
165-177 declines MAX_MODEM by comment — "can miss beacons" — with no measurement). Retire the
"~11.5 h WiFi off" figure (HANDOFF further down); the measured number is 9h16m. Any future
WiFi-on comparison must be quoted over the same voltage window — or read off the meter: **add a
WiFi-ON associated reading to the session (menu WiFi: on, wait for `wifi=3`, read; the cable does
not disturb association).**

**3. The gauge floor, corrected (measured):** first `soc=0` at **up=471 / 3.56 V**, so **85 min =
1.42 h = 15.5 %** of the battery run reported as empty (not 77 min / "an eighth"). Fixed in
`battery_curve.h` and the 09-03 morning block. Knee at 3.69-3.70 V (curve 27 %) with ~2.5 h
left; below 3.40 V (curve 2 %) there are 14 minutes.

**4. THE LEVERS, ranked by expected mA — ALL UNMEASURED until the `power` session runs:**
| lever | ceiling (typ) | how to measure | risk / why not yet |
|---|---|---|---|
| **Manual light sleep at idle, WiFi OFF only** | **15-18 mA typical (skeptic's range 15-28) ≈ 15-20 % ≈ 9.1 → 10.7-11.5 h** | `power sleep 60` = the exact ceiling in one reading | medium-high: UART console bytes lost while asleep; ≤200 ms windows so a LongFast packet is never missed (SX1276 latches RxDone); wake via `gpio_wakeup_enable(35/39, LOW)`; must gate on `!busy && radioOff() && !audio->isOn() && !gGbcActive && !battCharged` |
| **Automatic light sleep / esp_pm** | — | — | **IMPOSSIBLE on this core**: `objdump` of libesp32.a shows `esp_pm_configure` = `movi a2,0x106; retw` (ESP_ERR_NOT_SUPPORTED); tickless idle not compiled. Needs an arduino-esp32 2.x/3.x port. Do not spend time on `CONFIG_PM_ENABLE` here. |
| A persisted **"Mesh: off"** switch (model: `Networks::setRadioOff`) | 10.8 mA ≈ 11-14 % ≈ +1.1-1.5 h | `power lora sleep` (also calibrates the meter chain: expected value known to ±0.7 mA) | for when the mesh is NOT wanted; in the woods it is the point. CAD duty-cycling (6-9 mA) is a protocol change — don't. |
| **ST7789 SLPIN on screen-off** | 2-8 mA? | `power lcd sleep` | low: +120 ms on wake (SLPOUT settle); frame memory kept; exclude while `gGbcActive` |
| `i2s_stop()` at the end of `Audio::Audio()`; APLL needs an uninstall | 1-3 mA? | `power i2s stop` (module only; APLL stays) | stop = trivial and safe (`start()` calls `i2s_start`); **uninstall reallocates 16 KB of DMA RAM = the fragmentation panic class** — idle-only if ever |
| stretch `vTaskDelay(5)` / the 10 ms poll | < 0.5 mA | — | already in the noise (est. 2-4 % CPU duty at idle); the 1 kHz tick stays regardless |
| 40 MHz APB | 4-8 mA? | — | breaks the IDF UART0 console baud, WiFi, SPI dividers; subsumed by light sleep |
| SX1509 osc off, CW2015 sleep, SD unmount | ≤ 0.5 mA each | — | not worth the risk |

**4b. The woods framing (prior-measurements lens):** the woods state IS the WiFi-off state, so the
entire 0.9.47-0.9.51 WiFi lineage — the biggest drain this project ever measured — is already
irrelevant to it; the target is the 9h07m floor itself, whose composition has NEVER been
measured on this hardware (no component current, ever — only mV/h). A "Mesh: off" switch is a
NON-woods lever (listening is the point of the trip). The 2026-08-31 22 %/h morning stays
unexplained and outside every retained log; it is now instrumented (`aud=`, `ps=`, `joins=`,
`wifi=` once a minute) so a recurrence is diagnosable from `/health.log` — do not spend a
session on it unless it recurs, and do not attribute it to the cell.

**5. Constraints already paid for (prior-measurements lens + code):** never call
`setCpuFrequencyMhz()` from the input path (08-22: dropped keys); the GPS UART at 115200 makes
ANY clock switch deadlock (`gGpsNmea` is a `busy` term — phone 2 runs pinned at 240 with the
plate, and is fine on its bigger pack); `Audio::restore()` reinstalls I2S at most once; the
health line's `ps=1` means nothing when `wifi=255` (driver inited but stopped).

**6. Two curiosities, neither a power item:** (a) **62 `LOOP STALL`s of ~527 ms in the run** —
every 5 min for 3.4 h, then ~30 min, then hourly (up=359/419/479/539, exact 60-min steps) —
38 s total, so < 0.2 mA-equivalent; 62 is a LOWER bound (STALL records are rate-limited to one a
minute). NOT the mirror poller (it gates on `WL_CONNECTED`), NOT the NodeInfo beacon (3 h). The
critic's identification: **the hourly series (up=59,119,…,539) is phase-locked to boot at exactly
one period = phone 1's OWN neighbour-info announce (the only own 1 h timer, `nbr on`); the 5-min
series is NOT phase-locked = relays of an EXTERNAL 300 s position beacon** (phone 2 reports every
300 s; COVEY every 5 min) — phone 1 cannot beacon a position itself (needs a GPS fix). Each is
one LoRa TX (send() polls TxDone synchronously, ~0.5 s at LongFast). **Add the caller's name to
the STALL line (WiPhone.ino ~2494) and the next run answers it for certain.** (b) **GPIO38 is both
`RFM95_INT` (DIO0) and `USER_SERIAL_RX`** (Hardware.h:148 vs :160): the plate's GPS TX drives
the LoRa DIO0 pin. Harmless today because `mesh_phy` polls and never uses DIO0 — but it means a
DIO0 interrupt wake for light sleep is impossible with the plate fitted, and phone 1 begins
UART2 at 9600 on a pin nothing drives.

**6b. What the completeness critic added (13 items, the ones that change anything):**
- **The TRIP phone is phone 2, and its state was analysed by nobody:** GPS at 115200 pins it at
  240 MHz (`gGpsNmea` ∈ `busy`/`hardBusy`: no idle tick stretch, no light-sleep window ever) and
  the M100 GPS itself is ~30 mA typical (docs/woods-backplate.md:134) — **~40-70 mA above the run
  analysed here, and it outranks every phone-1 lever for September.** Nobody priced the GPS
  module's standby/low-rate modes or the 9600-baud exit ("ships at 115200, silent at 9600",
  Hardware.h:165-180, untested), or the plate's TPS63020 Iq. The backplate pack pays for it today
  ({user}: phone 2's battery is fine), but it is the biggest unpriced number in the woods.
- **Light-sleep design residuals:** (a) the SN7326 INT is a 10 ms self-clearing pulse — a press
  landing inside the ~0.5-1 ms sleep-entry window is lost until the next key, so every TIMER wake
  must also poll INT-low / the SN7326 status, not just "set keypadToRead on GPIO wake";
  (b) `gpio_wakeup_enable()` on GPIO35/39 does NOT overwrite the FALLING ISR type — objdump shows it
  calls `rtc_gpio_wakeup_enable` and returns for RTC pads — so no level-ISR storm hazard;
  (c) the bench `power sleep` arms ext0 on GPIO35 only, so the POWER BUTTON (GPIO39) is dead
  during a bench sleep — fine for 60 s, must not be copied into an idle loop; (d) IRAM is full to
  within 16 B; any ISR-side wake code needs another eviction.
- **A latent Game Boy dead-end behind the sidecar fix:** gnuboy accepts any buffer ≥ 512 B and
  maps banks only while `size - pos >= 16 KB`, so a `.gbc` shorter than one bank (a 4,096 B
  sidecar on 0.9.56, or any truncated upload > 512 B) "loads", `gb_hw_updatemap()` mallocs an
  UNINITIALISED PSRAM bank, and the emulator runs garbage — no panic, just a dead game. Hidden now,
  reachable by a truncated ROM. Guard: `if (!cart.romFile && size < cart.romsize * BANK_SIZE)
  return -4;` in gnuboy.c + a host vector. Not built.
- **Parts the repo never named, from wiphone.io's spec sheet (fetched 2026-09-03):** amp AW8733A,
  LDO MIC5219-3.3BM5 (the ONE 3.3 V regulator listed), codec WM8750BL, 1000 mA USB fuse, **stock
  battery "700 mAh LiPo"** — ⚠ {user}'s phones carry the 900 mAh UFX 603048A (label + {user}); do
  not "correct" 900 to 700. Charger IC still unnamed. Sleep current "TBD" on their sheet too.
- Amp and codec are verifiably OFF from boot (`rmtTxInit(false)`; `codec.shutDown()` writes
  POWER1/2 = 0); I2S + APLL + MCLK verifiably RUN from boot (libdriver literal pools:
  `i2s_driver_install → i2s_set_clk → i2s_start + rtc_clk_apll_enable`).

**7. What I would do, in order, once the meter has spoken:** if ON−OFF ≈ 40-50 mA the cell is
the story (a 1000 mAh same-footprint cell + light sleep ≈ +35 %); if ON−OFF ≈ 85-100 mA, hunt
the 45-60 mA with the four toggles and the CP2104 check, THEN build light sleep gated exactly
as above (with the SN7326 pulse rule and the GPIO39 wake), THEN SLPIN, THEN `i2s_stop` in the
constructor, THEN consider the 3.30 → 3.15 V cutoff — each one shipped only with its meter
delta written next to it. For the TRIP, price phone 2's GPS + 240 MHz first; it dwarfs all of
these.

# 🔌 2026-09-03 afternoon — THE POWER BENCH: serial `power` turns {user}'s USB meter into the
# instrument this project never had, and phone 1 is sitting in the exact state it needs

**Why.** Phone 1's full-to-empty run implies **~80-100 mA average with WiFi off** (900 mAh over
9.12 h = 99 mA; 79 mA at 80 % cell health — the cell's health is unknown, so the range is real).
Everything the firmware leaves powered at idle, summed at datasheet typicals, reaches only
**~35-45 mA reconciled** (this paragraph's own raw span was ~35-58): ESP32 at 80 MHz never light-sleeping 20-31, SX1276 RX-continuous ~11, ST7789
awake behind a dark backlight 1-8 (guess), I2S+APLL running from boot 1-3 (guess), SD idle
0.2-1, everything else < 1. **45-60 mA is unaccounted for at 100 % cell health, 25-40 at 80 %** (this line said 10-60; superseded) (regulator topology, panel, codec,
cell health — none measured). The cell is soldered; a drain run resolves 10 mA only after hours
at matched voltage. **The USB meter resolves 1 mA in seconds — IF the cell is full**, so the
charger has tapered and the meter reads the board and not the charge.

**The instrument (0.9.57, both phones):**
| command | flips | expected (typical) |
|---|---|---|
| `power` | nothing — prints every switch and the method | |
| `power lora sleep` / `rx` | SX1276 SLEEP vs RX-continuous (mesh DEAF meanwhile) | ~11 mA — also calibrates the method |
| `power lcd sleep` / `wake` | ST7789 DISPOFF+SLPIN vs normal (backlight untouched) | 1-8 mA, unknown |
| `power i2s stop` / `start` | I2S module + DMA (APLL stays: Audio.cpp:111-121) | 1-3 mA, unknown |
| `power sleep <secs>` | light-sleeps the ESP32 (timer + keypad wake; WiFi must be OFF) | everything EXCEPT the core = the floor |

✅ Smoke-tested on phone 1 over the cable: every toggle round-trips; `power lora sleep` survived
the 5 s radio health check (healthCheck() accepts the parked state); `power sleep 5` came back
after **5,001 ms by timer**, `millis()` compensated, phone healthy after. ⚠ `esp_light_sleep_start()`
overflowed IRAM by 16 bytes — `gb_hw_updatemap()` in gnuboy/hw.c gave up its `IRAM_ATTR` (per
bank switch, not per bus access). Watch for Game Boy stutter; none expected.

📋 **THE SESSION FOR {user} (10 minutes, phone 1, USB meter inline):**
0. **The absolute number first, no serial needed:** meter inline, phone FULL, screen asleep,
   read → then hold END to power OFF (the latch drops, the 3.3 V rail reads 0 V) and read again.
   **ON − OFF = the whole board's idle current**, charger and CP2104 cancelled. If it lands near
   40-50 mA, the "gap" is the cell's health, not a hidden consumer; if it is 80-100 mA, something
   on the board is eating 30-50 mA that no datasheet row explains. This one reading decides
   which problem we have.
1. Phone 1 is WiFi OFF (`power` reads `wifi=OFF`). It rode home in the bag (16:10: `v=4.15
   chg=1 soc=88%`) and **re-topped by 18:14: `soc=100% est=100% v=4.20 chg=1 wifi=255 cpu=80MHz
   scr=0`** — the exact starting state. Put the meter between the cable and the phone. ⚠ **Open the serial terminal FIRST**
   (500000 baud) — opening the port reboots the phone — then wait for the screen to sleep
   (`scr=0` in the health line, or triple-tap). Let the reading settle 30 s: that is **I0**.
2. `power lora sleep` → read **I1**; `power lora rx`. I0−I1 = the radio (~11 mA expected; if it
   reads 0, the meter or the method is wrong, stop there).
3. `power lcd sleep` → **I2**; `power lcd wake`. I0−I2 = the panel controller.
4. `power i2s stop` → **I3**; `power i2s start`. I0−I3 = I2S/DMA.
5. `power sleep 60` → read during the minute → **I4**. I0−I4 = the ESP32 core = the most any
   CPU-sleep work could ever save.
6. `power lora sleep` then `power sleep 60` → **I5** = the absolute floor: charger overhead +
   panel + SD + regulators + everything nobody has a switch for. **I5 is the number that says
   whether the gap is in the ESP32 or in the board.**
7. `gps on` → **I6**; `gps off`. Phone 1 has no GPS, so this is a PURE 80 → 240 MHz step
   (`gGpsNmea` is a `busy` term): the cost of the clock alone, which the 08-22 downclock
   experiment never measured.
8. **Cell health, another day:** run phone 1 flat (the 09-02 way — it powers itself off at
   3.30 V), then charge it THROUGH THE METER with its mAh counter zeroed, until `chg` has been 1 at
   4.19 V for an hour (~3.5 h), WiFi off and screen asleep throughout. The counter includes the
   charger's quiescent and the CP2104 for the whole charge, so **subtract the ABSOLUTE phone-ON
   float reading I0 × charge hours (NOT the ON−OFF delta)**; apply the linear/switching factor
   from the LoRa calibration; the result is capacity TO 3.30 V (≈ 92-96 % of a to-3.0 V rating).
   {user} says the cell is 900 mAh nominal; **~830-865 means healthy** and the ~91-95 mA average is
   real board draw; near 500-650 means the cell has aged and a same-footprint 1000 mAh
   replacement is the biggest single win available.
Quote deltas in USB mA. ⚠ **The USB→battery factor is UNKNOWN until calibrated**: if the
MIC5219 LDO is the sole 3.3 V rail (wiphone.io names exactly one LDO) and the charger is linear
(STAT open-drain + 196 min of float is consistent with linear AND with many switchers), USB mA ≈
battery mA 1:1; a switcher makes it ≈ /1.15. **`power lora sleep` is the calibration**: the
radio hangs off that same rail, so a delta of ~11 mA proves 1:1 and ~9.5-10 says switcher.
Photograph the charger IC marking while the case is open. Absolute readings include the charger
IC and the CP2104 (USB-powered, a constant offset) — only deltas mean anything. ⚠ A linear
charger feeding a 50-100 mA load at 4.2 V may terminate and restart at its recharge threshold;
take each A/B/A within one minute and watch `v=` stay flat. ⚠ **Read the meter's decimal places
first** — its model is unrecorded; at 0.01 A resolution only LoRa (~11) and light sleep (~15-20)
are readable directly, and the panel/I2S/SD deltas need the mAh counter over ≥ 60 min per state.
Hands-on checks if the numbers come out strange: the CP2104 being back-powered through UART0 TX
idling high — invisible to ON−OFF because VBUS is present in both readings — (meter the CP2104's
VDD pin with USB out; anything above ~0.5 V is the ESP32 feeding it; or a firmware A/B that
parks GPIO1 as INPUT when `usbConnected==0`, over a matched 4.00-3.80 V band). The amplifier
polarity question is ANSWERED from wiphone.io: the part is an **AW8733A** (pulse-count CTRL gain
modes matching Hardware.cpp:172, CTRL-low = shutdown), and `rmtTxInit(AMPLIFIER_SHUTDOWN, false)`
at boot idles it LOW — off from the first frame.

# 🎮 2026-09-03 afternoon — 0.9.57: PHONE 2'S "DUPLICATE ROMS STARTING WITH _" WERE MACOS SIDECAR
# FILES, AND THE GAME BOY PICKER WAS THE ONE LISTER THAT SHOWED DOTFILES

{user}: "wiphone 2 (the one with gps) has duplicate ROMs. the duplicates start with '_'. I can't
find the dupes in the file browser."

**Proven on the card, not inferred.** New serial `ls /roms` on phone 2 (0.9.57):
**24 entries, 12 hidden** — every ROM has a `._<same name>` twin of exactly **4,096 bytes**. The
root also holds `.Spotlight-V100/` and `.fseventsd/`. That is Finder: it writes an AppleDouble
resource-fork sidecar beside every file it copies onto a FAT card. Not a failed upload, not a
real duplicate — 48 KB of Mac metadata. (The names read to a person as "_Zelda…" because in
OpenSans CondBold 20 the '.' is a 4×3 px blob at the baseline touching the 7×2 px '_' one pixel
below it with no gap — one ~12 px smudge that reads as an underscore. Measured from the font
tables by the review lens; FatFs LFN is compiled in, so it is the real long name, not an 8.3
alias like `_ZELDA~1.GBC`.)

**Why only the picker showed them:** `FilesApp::scanDir()` (app_files.cpp:115), `BooksApp::scanBooks()`,
`PhotosApp`, `musicIsPlayable()` (music_lib.cpp:65 — with a comment naming the macOS resource
forks) and serial `wallpaper list` all skip a basename starting with `.`. **`GbcApp::scanRoms()`
did not** — anything with `.gb`/`.gbc` was listed. So the sidecar had the extension, appeared in
the picker, and was invisible everywhere a person could delete it. The August bug class in
filter form: one consumer inherited a rule it never set. **Fixed with the same `base[0] != '.'`
test, loud comment in place (app_gbc.cpp scanRoms).**

🔎 **Two instruments that did not exist and now do — `ls [<dir>]` and `rm </path>` on serial.**
`ls` prints every entry with size, marks hidden ones `*`, folders `/`. `rm` deletes ONE file by
full path, refuses folders and relative names. ⚠ **`shot` CANNOT show the Game Boy picker**: it
reads the GUI page sprite (GUI.cpp screenshotToSerial) and the picker draws straight to `lcd` —
two runs of `shot.py … key down down down select` returned the Games menu with the app already
open (`APP id=16427` in the log). Anything drawn direct-to-LCD is invisible to `shot`; `ls` was
the way in.

✅ **BOTH PHONES ARE ON 0.9.57** (`ver` → `firmware 0.9.57, built Sep 3 2026 13:21:11` on each,
esptool `Hash of data verified` ×2, 230400 baud, app partition only). Phone 1's `/roms` has **no
sidecars** (its ROMs came through the WiFi uploader, never through Finder) — but it does hold three
**0-byte EPUB stubs** (`1_/2_/4_The_Dominion_Civil_War_N.epub`, 0 bytes), the shape `chunkOpenFor()`
leaves when an upload opens at offset 0 and never continues (uploader-bench leftovers). Books
scans `/roms`, so those may list as books that cannot open; `rm /roms/<name>` clears them. Not
touched. The 12 sidecars are LEFT ON
THE CARD on purpose (harmless, hidden now; `rm /roms/._<name>` removes one; Finder re-creates them
on the next copy). ⚠ **Trade-off, made knowingly:** on 0.9.56 the picker's own *Back → OK* delete
was the ONE on-phone way to remove a twin (SD.remove on an LFN dotfile works, then rescan); on
0.9.57 hidden rows cannot be selected, so serial `rm` is the only path. If that ever matters, the
Files app could grow a "Show hidden" toggle or a "Remove Mac sidecar files" action — not built.
Optional hardening the lens suggested, also not built: `chunkSafeName()` (chunk_proto.h:71-73)
refuses only `.` and `..`, so a WiFi client that sent `._X.gbc` would be accepted; refusing any
leading-dot basename there is a one-line change with a vector in `tests/test_chunk.cpp`.
CHANGELOG 0.9.57 written, `webflasher/manifest.json` bumped; **NOT committed, NOT published** —
the release commit waits for {user}.

📇 Phone 2's `/books` for the record: the four Dominion Civil War EPUBs, Ghosts_of_Timkovichi,
`positions.cbs` (149 B), and three `chrome_*.bin` test uploads (2.9 MB of leftovers from the
uploader bench — deletable via Files or `rm`).

# 🔋 2026-09-03 — 0.9.56: THE BATTERY % IS NOW PHONE 1'S MEASURED CURVE, NOT THE CHIP'S GUESS

{user}: "make the calibration curve to phone one as you think makes the most sense... then we will
just make that the standard for the firmware. phone 2 and the other people's phones can just use
our best guess." Done, flashed to both phones, pushed, flasher serving it.

**What shipped.** `WiPhone/battery_curve.{h,cpp}` — `batterySocFromVoltage(v)`, an 18-breakpoint
table (50 mV) derived from the run below: 10 mV bins of TIME-LINEAR SOC (100 % at the unplug, 0 %
at the last sample; constant load ⇒ charge linear in time), 50 mV bin means. The raw run is
committed as **`tests/fixtures/p1_discharge_2026-09-03.tsv`** (557 rows: up, chip soc, v, chg) so
the curve can be regenerated and re-scored forever. `tests/test_battery.cpp` (20 checks) scores
the table AND the chip against the run: **ours mean 0.54 pt / max 2; chip mean 5.7 / max 16**,
and pins the fault itself — 3.54 V must read ≥ 12 %. Mutation-tested: a breakpoint zeroed to the
chip's cliff, two transposed, interpolation removed, the top clamp moved — all caught.

**The display rule, and the measurement that forced it:**
| state | shown | why |
|---|---|---|
| **off the charger** | the curve | the number that matters — runtime left in the field — and what the curve was measured for |
| **on the charger** | the chip | **first flash, 45 min into a charge from empty: curve 61-65 %, chip 20-25 %, v 3.89-3.91.** Charge current lifts terminal voltage ~100 mV; a DISCHARGE curve read against it over-reports 30-40 pts, the optimistic direction |

⚠ **`est=` in the health line is ALWAYS the curve**, never the displayed value — on the charger
the display shows the chip, and logging that twice would throw away the one number a future
charge-side correction needs. (First build got this wrong; fixed before it left the desk.)
⚠ **The selection runs AFTER the tick refreshes `battCharged`, and `battCharged` is seeded in
`setup()` beside `cardPresent`.** The first build chose on the PREVIOUS tick's flag, and a boot on
the charger showed the curve's **66 %** for ~30 s before dropping to the chip's 25 % — caught on a
`shot.py` screenshot, exactly the jump that makes a person stop believing the number. Third build:
**screenshot reads 34 %, health line says `soc=34% est=69%` — the chip, from the first frame.**

✅ **VERIFIED ON BOTH PHONES (final build, `Hash of data verified` ×2):** phone 1 `firmware 0.9.56`,
display = chip on the charger (34 %), `est=` = curve (69 %) at 3.94 V; phone 2 `firmware 0.9.56`,
`soc=100 est=100 v=4.21 chg=0` — the backplate signature, curve and chip agreeing — and `nbr`
hearing **`!00449040` (phone 1) at 11 dB**. Both phones on the same build, hearing each other.
⚠ **Phone 2 shows the curve on the cable too**, because its `battCharged` reads 0 on USB (the
backplate holds the rail). Its internal cell IS full then, so the curve's 100 % is right anyway.
⚠ **The old `if (soc > 0.0)` guard FROZE the display at its last non-zero chip value** once the
chip hit 0 % — so the phone showed "1 %" for its final 1.3 h. Gone.

**Honest limits, all in `battery_curve.h`:** it is phone 1's cell at an idle, WiFi-off load — the
woods case; under heavier load it UNDER-reports (conservative, the right way to be wrong); it is
fitted to one run and tested against that run, which proves it reproduces the curve, not that the
curve holds on another day; a second phone-1 discharge would settle that.

## ✅ THE CHARGE CURVE IS CAPTURED TOO — phone 1, empty to full, 2026-09-03

Read off phone 1 at 12:49 after {user}'s dentist trip, 233 KB and under the 256 KB cap, so nothing
had trimmed. Stitched across six boot segments (the first 42 min on 0.9.53 with `soc=` only, then
the 0.9.56 flashes with `est=`): **`tests/fixtures/p1_charge_2026-09-03.tsv`**, 371 rows, `chg=1`
in every one.

| phase | | |
|---|---|---|
| plug-in | 06:43, `v=3.50` (the dead cell had relaxed from 3.30) | chip 0 % |
| CC bulk | 3.50 → 4.10 V in **111 min, 324 mV/h** | |
| CV taper | 4.10 → 4.19 V in 57 min | |
| **full** | cell at 4.19 V at **2.8 h**; chip reads 100 % at **2.9 h** (175 min) | then 3 h of float at 4.19 |

🔑 **A full charge from empty is ~2.9 hours.** And the curve and the chip AGREE at the top (both
100 at 4.19 V), so unplugging a full phone produces no jump.

📈 **The charge-side over-read of the discharge curve is now MEASURED as a function of voltage** —
the thing a charge-mode table would be built from, and the reason the display shows the chip on
the charger:

| v (charging) | curve − chip | | v | curve − chip |
|---|---|---|---|---|
| 3.91 | **+39** | | 4.10 | +18 |
| 3.95 | +33 | | 4.14 | +10 |
| 4.00 | +25 | | 4.16 | +5 |
| 4.05 | +23 | | 4.19 | **0** |

Smooth and monotonic. ⚠ One charge, at whatever current the WiPhone's charger IC delivers — a
different charger or a warmer cell shifts it. **Not built into the firmware; the chip is shown
while charging and that is correct enough.** If someone wants the display to use the curve on the
charger too, subtract this gap — it is all in the fixture's `est − soc` column.

📁 **WHERE THE LOGS ARE, for the next session:**
- `tests/fixtures/p1_discharge_2026-09-03.tsv` — the drain, 557 rows (committed; `test_battery` loads it)
- `tests/fixtures/p1_charge_2026-09-03.tsv` — the charge, 371 rows (committed)
- `backups/health-2026-09-03-p1-drain-raw.log`, `…-p1-charge-raw.log`, `…-p1-drain-run-only.log` —
  the raw `health all` dumps (gitignored, LOCAL ONLY — `backups/` is where device dumps live)
- Segmenting a raw dump: `awk '/^BOOT reset_reason/{c++} {print c"\t"NR"\t"$0}'` — `up=` resets
  every boot and nothing carries a wall clock.

# 🔋 2026-09-03 — PHONE 1'S FULL-TO-EMPTY RUN: 9h16m, AND ITS GAUGE FAILS THE OPPOSITE WAY FROM
# PHONE 2'S

**Unplugged 11:52 PDT 2026-09-02, found dead and replugged ~06:43 PDT 2026-09-03. Read from
`health all` before it went on the charger for the day, per the plan below — retained window was
never at risk.** Closes open item #3: phone 1 now has its own real full-to-empty curve, not the
old ~11.5 h extrapolation from one 61-minute block.

**The run, isolated from four boot segments in the retained log** (boundary = `BOOT
reset_reason=1` lines; this one is the 557-sample segment starting at a fresh `up=0`):

| up= | event |
|---|---|
| 0 min | boot: `soc=100% v=4.19 wifi=255 chg=1` — still on the cable for the pre-flight check |
| 9 min | `chg` flips 1→0 — **the unplug**, matching {user}'s "11:52" almost exactly |
| 449 min (7.5h) | `soc=6% v=3.61` |
| 479 min (8.0h) | `soc=0% v=3.54` — gauge hits its floor, phone keeps running |
| **556 min (9.27h)** | **`soc=0% v=3.30`** — last sample; death within the next minute |

**Total uptime 9h16m; genuinely on battery 547 min = 9h07m** (from the `chg=0` unplug to the last
sample). Death worked out to ≈ 20:59 PDT that evening.

✅ **Textbook-clean load, confirmed by grepping every sample, not just the endpoints:**
`wifi=255` in 557/557, `scr=0`+`cpu=80MHz` in 556/557 (the exception is the boot instant itself),
`chg=0` in all 548 samples after the unplug, zero heap fragmentation (`largest` never left
31,368–32,760), no stalls/panics/reboots inside the run.

🛑 **CORRECTION FROM {user}, 2026-09-03 morning — I HAD PHONE 2's "DEAD ZONE" WRONG, AND SO DID
THE 09-02 SESSION.** Phone 2 wears the woods backplate. **Its 4.6 h at `soc=100%` was the
BACKPLATE CELL feeding the internal cell** — the internal pack genuinely sat at full until the
backplate ran down, then took over and tracked 100→0 over 7.9 h. That is two cells in sequence,
not a gauge fault, and it means phone 2's 12.5 h is backplate + internal, NOT "a bigger cell".
⚠ **BATTERY EXPERIMENTS HAPPEN ON PHONE 1.** Phone 2's backplate confounds every reading —
`chg=`, `soc=`, `v=`, duration — and nothing in the log can tell the two cells apart.

**So the ONLY genuine gauge error we have measured is phone 1's:** the CW2015 hits `soc=0%` at
3.54 V while the cell actually runs to 3.30 V — **1.3 h of real runtime reported as empty**
(up=479→556min). ✏️ *Corrected 2026-09-03 afternoon by re-reading the fixture: the FIRST `soc=0`
row is up=471 at **3.56 V** (up=470 reads 1 % at 3.56), so it is **85 min = 1.42 h = 15.5 % of the
547-min battery run**, not 77 min; `3.54 V` is the up=479 sample. `battery_curve.h` says so now.*
Root cause: the BATINFO profile is never programmed
([[wiphone-battery-measured]]), so the chip runs a generic model whose floor is ~240 mV too high
for this cell. Conservative direction, but it wastes an eighth of the pack on a trip.

📉 **Rate: 94.3 mV/h, endpoint to endpoint from the unplug** (4.16 V at up=9min → 3.30 V at
up=556min). Softer than the 60-64 mV/h "associated, WiFi-off" figures from shorter runs, but
those never included the steep terminal drop this run captured (soc=0%→3.30V alone cost
187 mV/h over its last 77 minutes) — not a contradiction, a difference in what each run measured.

**Retired by this run:** the ~11.5 h WiFi-off estimate that rested on one 61-minute block from
2026-08-23 on an old build. **9h16m is now the number to quote for phone 1, WiFi off, full to
empty**, alongside phone 2's 12.53 h on its bigger backplate pack.

⚠ **Analysis method, worth keeping:** the retained `/health.log` on this read held FOUR separate
boot segments (an old unrelated charging session whose own boot marker had trimmed away, plus
three more), because `up=` resets to 0 every boot and nothing in the file carries a wall clock.
Isolate segments with `awk '/^BOOT reset_reason/{c++} {print c"\t"NR"\t"$0}'` before trusting
any curve pulled from `health all` — the file is not one continuous run by default.


# ▶▶ STATE NOW (2026-09-03 midday, {user} starting a fresh session) — READ THIS BLOCK AND YOU ARE CAUGHT UP

**Both phones are on 0.9.56. `main` is pushed. The web flasher serves 0.9.56.** Host suite: 0 failures.
Phone 1 = `usbserial-025A3EAF` = `!00449040` (no GPS). Phone 2 = `usbserial-025A3F65` =
`!00449334` (GPS on, woods backplate, bigger cell). ⚠ Identify from the phone (`gps` — only
phone 2's reader is ON), never from the port.

**What was found and PROVEN on hardware this week, newest first:**
- **0.9.56 (2026-09-03) — the battery % comes from the VOLTAGE off phone 1's measured curve when
  off the charger** (`battery_curve.{h,cpp}`, 18 breakpoints, mean error 0.5 pt vs the chip's 5.7);
  the chip's number is shown ON the charger, because a discharge curve read under charge current
  over-reports by ~40 points (measured: curve 65 %, chip 25 % at 3.91 V). Health line now logs
  `soc=` (chip) AND `est=` (curve). {user}'s decision: phone 1's curve is the standard for every phone.
- **2026-09-03 — phone 1's full-to-empty run is DONE: 9h16m, WiFi off, textbook-clean load.**
  The chip reports `soc=0%` for its last 1.3 h (3.54→3.30 V). Retires the old ~11.5 h estimate.
  🛑 **{user}'s correction: phone 2's 4.6 h at 100% was its BACKPLATE feeding the internal cell,
  not a gauge fault — battery experiments happen on phone 1 only. Phone 2 wears the backplate
  unless he says otherwise; it is now in memory ([[wiphone-phone2-backplate]]) so it stops being
  re-learned.**
- **0.9.55 — Position, Waypoint and the channel hash pinned too (`gen_pos_vectors.py`,
  `mesh_hash.{h,cpp}`), and every varint reader hardened after UBSan caught a real UB on a
  ten-byte varint (any negative int32 — `altitude` below sea level is one).** Proven on air:
  COVEY's own RAK Position decoded at `up=2min` on the new parser. The CallApp `configs.ini`
  wipe hazard is closed (unverifiable without SIP). **Only the 16-byte on-air header is now
  unpinned, and that is by nature.** {user} was hands-off; done in limit-safe checkpoints
  `8b93e72` → `398d6c4` → release.
- **0.9.54 — the Meshtastic wire format is pinned against upstream's own protobufs**
  (`mesh_wire.cpp` + `test_wire.cpp`, 84 assertions) **and proven on air both ways**: a text
  from phone 2 was decoded by COVEY's independent stack on the right channel with the right
  name. **Phone 2 is on 0.9.54; phone 1 is still on 0.9.53** (on its battery run).
- 0.9.52/53 — **Settings ▸ Notifications ▸ Buzz** (50-650 ms, default 200, clamped on load). Phone 2 left at 400.
- 0.9.50/51 — **the WiFi join is gated on what the last scan saw**; proven on a real 50-min commute
  (`joins` frozen for 28 min, rejoined on arrival). 0.9.51 fixed the gate engaging at 25 min instead of 5.
- 0.9.49 — **one persisted WiFi switch** that four re-enable paths respect; proven both directions.
- 0.9.47 — **the audio codec/amp/I2S were left powered by pausing music**; caught firing and released.
- **WiFi SEARCHING is the biggest measured drain** (~106 vs ~60 mV/h associated). Out-of-range
  radio time went 604 → 49 s/hour across 0.9.47-0.9.50. **For the woods: WiFi OFF is worth more
  than doubling the battery.**
- **The CW2015 gauge holds 100 % for the first 4.6 h** — quote **mV/h**, never %/h. Phone 2's
  full-to-empty curve (4.21 → 3.30 V) is recorded.
- **Phone 2's backplate cell is bigger — measured** (12.5 h at 240 MHz + GPS vs phone 1's ~9-10 h).
- The backplate wire was unseated overnight (10 h at `chg=0`, one 1-min flicker); reseated, charging.

**Claims of mine that were DISPROVEN — do not re-trust:** WiFi power-save being lost on a station
restart (`wifi bounce` held `ps=1`; so did 83 samples of hunting); the codec explaining the 08-31
drain (that run had `aud=0/0` throughout — it was WiFi searching).

**Open, all needing something physical from {user}:**
1. **The safety valve** (blind join every 4th skip) has never fired — needs >40 min out of range.
2. **Backplate field endurance** — both OFF mains, backplate on its own cell topping the phone up.
3. ✅ ~~One full-to-empty run on phone 1~~ — **done 2026-09-03: 9h16m, and its charge from empty too (2.9 h to 100 %).** Both are fixtures in `tests/fixtures/`.
4. ✅ ~~CallApp in-call volume handler rewriting `configs.ini`~~ — **fixed in 0.9.55** (bails
   instead of storing on a failed load). Still needs a completed SIP call to *verify*.
5. The music **crackle** — explicitly deferred to its own session.

**Instruments, all in the once-a-minute health line:** `aud=on/moving` (`1/0` = the leak),
`ps=` (modem sleep), `joins=tried/skipped`, `wifi=` (`255` = radio deliberately off), `chg=`.
Read the card log with `cmd.py <port> "health all" 30 150 out.txt` — ⚠ shorter waits truncate,
and opening a port REBOOTS the phone. `tools/shot.py` drives the UI; send one `--cmd` per app
boundary, the switch into Settings eats queued keys.


⚠ **KNOWN HAZARD, PRE-EXISTING, found by the 0.9.52 review and NOT fixed (out of scope, needs a
completed SIP call to reach):** the **CallApp in-call volume handler** can rewrite `configs.ini`
with ONLY its three volume keys if its own `load()` fails — silently dropping every other
setting in the file (`notify_vibro_ms`, `notify_vol`, the notify modes, screen config…). Every
other writer load-modify-stores the whole file correctly. Fix when SIP calls exist: make that
handler bail rather than store on a failed load. Search GUI.cpp for the in-call volume path.


# 📍 2026-09-02 EVENING — 0.9.55: POSITION, WAYPOINT AND THE CHANNEL HASH ARE PINNED, AND
# UBSAN CAUGHT A REAL BUG IN THE FIRST FIVE MINUTES

{user} picked three items and went hands-off: pin Position/Waypoint the way 0.9.54 pinned Data/
User, split the channel hash out so it can be pinned too, and close the CallApp `configs.ini`
hazard. All three shipped. **The detail is in CHANGELOG.md under 0.9.55**; this block is the
state and the traps.

## What is pinned now — the whole wire format bar one thing

| surface | pinned against upstream? |
|---|---|
| Data / User protobufs (0.9.54) | ✅ |
| **Position encode + decode, incl. a stock-shaped broadcast with every field a RAK sends** | ✅ `tools/gen_pos_vectors.py` → `tests/vectors_pos.h` |
| **Waypoint decode** — full, id-only deletion marker, long name, UTF-8, unknown 7/8, no-id refused | ✅ |
| **Channel hash** — 7 (name, key) pairs vs `meshtastic.util.generate_channel_hash` | ✅ `mesh_hash.{h,cpp}`, in `test_wire` |
| NeighborInfo, PKC DM crypto (pre-existing) | ✅ |
| **The 16-byte on-air PacketHeader** | ❌ **by nature** — firmware-internal, not in the protobufs; structural test only |

🔑 **`mesh_hash.{h,cpp}` is the new home of `meshXorHash`/`meshChannelHash`/the default key.**
`mesh_crypto.h` re-includes it, so nothing else changed. It exists only because `mesh_crypto.cpp`
needs mbedtls and cannot build on a host.

## 🐛 UBSAN FOUND UNDEFINED BEHAVIOUR THE FIRST TIME AN UPSTREAM VECTOR RAN

`mesh_pos.cpp:16: shift exponent 35 is too large for 32-bit type`. **protobuf encodes a negative
`int32` sign-extended to 64 bits — a TEN-BYTE varint** — and the stock-shaped vector carries
`timestamp_millis_adjust = -250`. `rdVarint` shifted a `uint32_t` by 35 then 63. On Xtensa a shift
count masks to 5 bits, so it silently became `<< 3`; the walk still advanced correctly (it follows
the continuation bit) and every field that file reads is `fixed32`, so **nothing was ever
misread** — but `altitude` is `int32` and a fix below sea level sends exactly this. `mesh_wire.cpp`
had the same pattern in three readers. **All four now discard bits past 32.** Pinned by a
below-sea-level Position vector and a hand-crafted ten-byte-varint Data case; removing the guard
makes UBSan complain again. ⚠ **`./tests/run_tests.sh` builds with `-fsanitize=undefined` — read
its output for `runtime error`, not just the pass/fail line. That is how this was found and the
pass/fail line alone would have missed it.**

## Verified on phone 2, on air

| check | result |
|---|---|
| boot | `reset_reason=1`, `node=!00449334`, `firmware 0.9.55`, `MESH RADIO READY`, `ANNOUNCE SENT`, DB 200 nodes / 47 msgs intact |
| TX → independent stack | `send 1 wire test 0.9.55` → COVEY `/root/.covey/messages.json` ch:1 gained it, sender `WiPhone-{user}2` |
| **Position RX on the hardened parser** | `pos` at `up=2min`: **`!1ec2bca2 '{user} Howe Rak' <its full-precision position> (0m ago)`** — COVEY's own RAK, full precision — plus two public nodes at `precision_bits`-quantized coordinates, i.e. exactly the stock-shaped packet the vector models |
| Position TX | ⏸ **not provable right now** — phone 2's GPS has no current fix indoors (`sats=0`), and the beacon refuses under `MESH_POS_MIN_SATS`. Needs sky. |

⚠ **The CallApp fix cannot be hardware-verified** (needs a completed SIP call). It is strictly
safer than what it replaced: a failed load now applies the volume and persists nothing, at
`log_e`, instead of writing a three-key `configs.ini`.

## Every assertion was mutation-tested — assume nothing here is real until you have watched it go red

Swapped lat/lon tags, time always emitted, time from the wrong field, waypoint accepted without an
id, waypoint lat from the wrong field, the old single-byte tag read, XOR→OR in the hash, the name
dropped from the hash: **all caught.** `test_wire` 128 → 139; `test_pos` gained ~30.

# 🔌 2026-09-02 — THE MESHTASTIC WIRE FORMAT IS NOW PINNED AGAINST UPSTREAM'S OWN PROTOBUFS

**Question from {user}: if Meshtastic changes their core files, can we keep up?** Answer, now
written into the test suite rather than into a paragraph: **mostly yes, automatically, and the
part that isn't automatic is now measured.**

## THE SHAPE OF THE PROBLEM

This firmware is a **reimplementation, not a fork** — no `.proto` files, no nanopb, no upstream
source in the tree. The Data and User protobufs are hand-rolled as literal tag bytes (`0x08` =
field 1 varint, `0x48` = field 9). So there is **no "merge upstream" path**; there is only "does
our byte still equal their byte".

🔑 **THE REASON THIS IS SAFE AT ALL: every parser here walks tag/wire-type pairs and SKIPS
fields it does not know.** Upstream adds fields constantly, and this phone steps straight over
them. That is the normal case and it needs no action from anybody. Only a change to an EXISTING
field's number or meaning can hurt — and that is now what the suite watches.

## WHAT WAS BUILT

* **`WiPhone/mesh_wire.{h,cpp}`** — `meshBuildData` / `meshBuildUser` / `meshFillHeader` /
  `meshParseData` / `meshParseUserName` / `meshPbBytes` / `meshPbString`, plus the `MESH_PORT_*`
  values. 🛑 **They were file-statics inside `meshtastic_service.cpp`, which means NOTHING COULD
  TEST THEM** — that file pulls in the ESP32 SDK and cannot be built on a host. Moved verbatim.
* **`tools/gen_wire_vectors.py`** — emits `tests/vectors_wire.h` from the **real Meshtastic
  python protobufs** (`meshtastic` 2.7.11) plus `meshtastic.util` for the channel hash.
* **`tests/test_wire.cpp`** — **84 assertions, all passing.** Whole suite green, firmware builds
  (RAM 26.7 %, flash 38.6 %).

## WHAT IS AND IS NOT PINNED — read this before trusting it

| surface | pinned against upstream? |
|---|---|
| Data protobuf **encode** (11 cases incl. the 127/128/234-byte varint boundary) | ✅ byte equality |
| Data protobuf **decode**, incl. messages carrying fields we don't know | ✅ |
| User/NODEINFO **encode** (5 cases, incl. the `"!%08x"` id spelling and the PKI key) | ✅ |
| User **decode**, incl. long-name preference and unknown fields | ✅ |
| **PortNum values** (TEXT/POSITION/NODEINFO/ROUTING/WAYPOINT/NEIGHBORINFO) | ✅ vs `portnums.proto` |
| NeighborInfo encode | ✅ (pre-existing, `test_neighbor`) |
| PKC DM crypto | ✅ (pre-existing, `test_pki`) |
| **The 16-byte on-air PacketHeader** | ❌ **not in the protobufs at all** |
| **The channel hash** | ❌ not compiled — see below |

⚠ **THE HEADER IS THE REAL GAP AND IT IS NOT FIXABLE FROM PYTHON.** Meshtastic's on-air
`PacketHeader` is firmware-internal (`src/mesh/RadioInterface.h`); the python package never sees
it, because the python API talks to a radio over serial/BLE and the *radio* does the framing.
`test_wire.cpp` therefore checks it **structurally** — 16 bytes, field offsets 0/4/8/12/13/14/15,
hop_limit in the low 3 bits and hop_start in the top 3, `relayNode` = sender's low byte. That
catches a padding change or a bad shift on OUR side. **It cannot catch upstream changing the
header; that still means reading their C++.**

⚠ **THE CHANNEL HASH IS ONE `mbedtls` AWAY FROM BEING PINNED.** Upstream exposes it
(`meshtastic.util.generate_channel_hash`) and it **matches `meshChannelHash()` exactly — checked
2026-09-02, `'LongFast'` → `0x08`, which is what the boot line prints.** It is not compiled into
the test only because it lives in `mesh_crypto.cpp`, which needs `mbedtls/aes.h` and will not
build on a Mac without a new brew dependency. The reference values are emitted as comments in
`vectors_wire.h`. **Splitting `meshXorHash`/`meshChannelHash` out of `mesh_crypto.cpp` into a
header would make this a real test for about twenty minutes of work.** Worth doing.

## 🐛 THE TEST FOUND A REAL DIVERGENCE ON THE DAY IT WAS WRITTEN

`meshBuildData()` **always emitted Data field 2**, even for a zero-length payload — `12 00` where
upstream emits nothing at all (proto3 omits a field holding its default).

```
ours:   08 01 12 00 48 00
theirs: 08 01       48 00
```

⚠ **This never misread a packet** — an absent `bytes` field decodes as empty, so the two are
semantically identical — and it is **unreachable in shipping code**: `meshTxText()` rejects
`textLen == 0` outright, and every other sender passes a fixed struct or an encoder that always
emits something. **Fixed anyway**, because it was two bytes of LoRa airtime and a permanent diff
against every other node's encoding. This is the value of byte-equality over "it round-trips".

## ✅ THE DRILL WAS REHEARSED ACROSS A REAL VERSION GAP, AND THE FORMAT HELD

COVEY runs **meshtastic 2.7.10**; this Mac has **2.7.11**. The generator was run on COVEY over
ssh and its output diffed against the committed vectors: **byte-identical for all 11 Data cases,
5 User cases and 6 decode cases — zero differences outside the version string.**

That is worth more than it looks. It proves the mechanism works on a second machine with a
different upstream version, and it is one real data point that **a minor Meshtastic bump changes
none of the bytes this phone speaks** — which is exactly the forward-compatibility claim the
whole design rests on. ⚠ One version pair is not a trend; it is a rehearsal that passed.

## 🔎 THE REVIEW FOUND FOUR REAL DEFECTS IN THE NEW CODE — ALL FIXED, ALL MUTATION-TESTED

A five-lens adversarial review was run over the change before it shipped. It was worth it.

| defect | why it mattered | reachable today? |
|---|---|---|
| **`portnum` written as ONE RAW BYTE, not a varint** | portnum 256 (`PRIVATE_APP`) encoded as `08 00` — a Data addressed to portnum **ZERO**, silently | **No** — every portnum we speak is < 128. A landmine for whoever adds the next one. |
| **`meshBuildUser` emitted empty names as `12 00`/`1a 00`** | proto3 omits a default string; same defect as the empty payload, one field over | Yes, for any node with a blank name |
| **`meshParseUserName` preferred a present-but-empty `long_name`** | the node rendered as a bare number with a good `short_name` two bytes away, unread | Yes — **this firmware itself sent that form until 0.9.54** |
| **Generator emitted greedy C hex escapes** | `"Caf\xc3\xa9a"` lexes `\xa9a` as one escape; committed cases were safe *by luck* | Not yet — it would have broken on the next non-ASCII name |

🔑 **AND THE REVIEW'S BEST CATCH WAS ABOUT THE TESTS THEMSELVES.** The original
`parse_unknown_fields` vector was **vacuous**: protobuf serialises in field-number order, so its
unknown fields (4, 5, 7, 8) all landed *after* everything the test asserted — a completely broken
skip could not have changed the result. There is now a case with `request_id` (6) sitting
*between* the unknown 4/5 and 7/8, so reading it proves the parser really walked over them.

⚠ **EVERY FIX WAS MUTATION-TESTED — revert it, the suite must go red.** That discipline caught a
second vacuous test: the generated short-public-key vector passed 125/125 with the
`l == MESH_PKI_KEY_LEN` guard **deleted**, because the separate `i + 32 <= dlen` bounds check
happened to reject it anyway. Rewritten by hand with padding behind the key so only the length
guard can reject it. **A test that cannot fail is worse than no test**; assume nothing here is
real until you have watched it go red. **84 assertions → 128.**

⚠ Also closed: decode vectors now cover a **multi-byte length varint** (every earlier decode
payload was under 128 bytes, so that path was never exercised on receive), an **absent field 2**,
and hand-written **`fixed64` skip** / **present-but-empty `long_name`** cases — the two shapes
upstream's own runtime cannot emit, so no generated vector can ever cover them.

## ▶ THE DRILL WHEN MESHTASTIC SHIPS A RELEASE

```
pip install -U meshtastic
python3 tools/gen_wire_vectors.py --out tests/vectors_wire.h
python3 tools/gen_neighbor_vectors.py --out tests/vectors_neighbor.h
./tests/run_tests.sh
```

🔑 **READ THE `git diff` ON THE VECTORS BEFORE RE-RUNNING THE SUITE — the diff IS the protocol
drift report, and regenerating is what makes the failure disappear.** A clean diff means they
changed nothing we speak. A changed byte means read their release notes before touching anything.

## ✅ VERIFIED ON HARDWARE — 0.9.54 FLASHED TO PHONE 2 AND PROVEN ON AIR IN BOTH DIRECTIONS

{user} was in a meeting and said "test it, then push to another update level and flash and
push/commit/update GitHub/webflasher if it checks out". It checked out. `FIRMWARE_VERSION` →
**0.9.54**, built, flashed to phone 2 over the cable at 230400 (`Hash of data verified`).

⚠ **Identified from the phone first, never the port:** `gps: reader ON` — only phone 2's reader
is on. Phone 1 was off the cable on its calibration run and was never touched.

| check | result |
|---|---|
| boot | `reset_reason=1`, `MESH IDENTITY: node=!00449334 'WiPhone-{user}2'`, `firmware 0.9.54` |
| data survived the flash | `MESH DB: loaded 200 nodes, 45 msgs, 1 places from SD` |
| radio | `MESH RADIO READY: 906875000 Hz`; no `SX1276 not found`, no `radio init FAILED` |
| channel hash | boot prints `'LongFast' hash=0x08` — the same byte `meshtastic.util` produced for the vectors |
| **TX** | `MESH ANNOUNCE SENT` — a NodeInfo built by the moved `meshBuildUser`/`meshBuildData`/`meshFillHeader` went on the air |
| **RX** | `nbr`: `!62b8d2fd  snr 10 dB  heard 0m ago`, direct — a real packet decoded by the moved `meshParseData` |
| **TX decoded by an INDEPENDENT stack** | `send 1 ...` → COVEY's RAK → RP2040 → Pi; `/root/.covey/messages.json` ch:1 gained `sender 'WiPhone-{user}2'` with the exact text — right channel, right name. **Done TWICE: once on the first build, and again on the corrected one after the review's fixes changed the encoder.** |

🔑 **That last row is the one that matters.** COVEY is nanopb + the python `meshtastic` package,
none of our code, and it decoded our header, channel hash, AES-CTR payload, Data protobuf and text,
AND had our User protobuf already (it knew the name). Every byte the refactor touches was read
correctly by something we did not write.

🔎 **`covey-ui` RUNS AS ROOT, so its store is `/root/.covey/messages.json`, NOT
`/home/covey/.covey/`.** The first look in `~covey/.covey/` found nothing and nearly read as
"COVEY has no history"; {user} corrected it — the RAK receives, the RP2040 holds it if the Pi is
off, and the Pi persists it. Look under `/root`.

🔎 **Phone 2 read `chg=0` at `v=4.21` while on the USB cable, and that is NOT a fault.** I first
called it the 09-01 backplate-wire-unseated signature; **{user} corrected it and his reading is the
better one**: the backplate was plugged in *as well as* serial, and a full backplate cell holds the
rail at 4.21 V, so the charger IC has nothing to do and leaves STAT released. `chg=0` here means
"not charging", which is right. ⚠ **So `chg=` is only diagnostic on a phone with NO backplate**;
with one attached, a healthy full pack and a disconnected wire look identical from that flag.



# 📳 2026-09-02 — 0.9.52/0.9.53: A BUZZ SLIDER IN SETTINGS ▸ NOTIFICATIONS

✅ **0.9.53 = 0.9.52 + the review's one hardening**: the loaded `notify_vibro_ms` is now
`constrain()`ed to 50..650 at BOTH load sites (boot and screen-open). A hand-edited `-1` had
meant a 65 s motor run per notification, during which `audioDeviceBusy()`'s vibro term held the
audio device awake. Review found **zero defects** in the slider itself (4 lanes: widget grid,
pixel layout from the font tables, persistence path, timer sites).

{user} asked for a vibration-duration slider to tune the pocket feel. **Settings ▸ Notifications ▸
Buzz**, 50-650 ms in 50 ms steps, default 200 (nearest on-grid to the old hard-coded 180).
`ControlState::notifyVibroMs`, persisted as `[audio] notify_vibro_ms`, loaded at boot in
`GUI::loadSettings()`. Both pulse-timing sites in WiPhone.ino read it via
`#define MESH_VIBRO_MS (gui.state.notifyVibroMs)`.

⚠ **Notification buzz ONLY.** The call ring pattern (`vibroOnPeriodMs`/`vibroOffPeriodMs`) is a
separate mechanism and untouched.
⚠ **The screen was full (277/280 px)**, so BOTH sliders went inline (label left, slider right,
`labelWidth` 110 like Screen config). `FocusableApp(4)` → `(5)`.
✅ **Verified on phone 2:** renders clear of the footer; 4 × `right` stepped 200 → 400; **Save →
reboot → reopen → 400 ms.** Phone 2 was left at **400 ms** by that test.
✅ **Phone 1 flashed to 0.9.53 too** ({user} put it on the cable briefly; `gps: reader OFF,
bytes=0` confirms it was phone 1). **Both phones are on 0.9.53.**
🔎 Navigating there over the cable: the app switch into Settings EATS queued keys, so send the
path as TWO `--cmd`s: `"key menu down down down down down down down select"` then
`"key down down down down down down select"` (Notifications is the 7th child of Settings).


# 🔋 2026-09-02 — PHONE 2 RAN ITSELF FLAT OVERNIGHT AND HANDED US THE CALIBRATION RUN

Phone 2 was left off the charger after the 09-01 session and was found at **`soc=0% v=3.62`**.
Its log holds the thing that has been missing all along: **a complete full-to-empty discharge,
12.53 h, 100 % → 0 %, 4.210 → 3.300 V**, at **240 MHz for 100 % of it** (GPS on), WiFi associated
96 %, screen off 99 %.

## 🔑 TWO RESULTS, AND THE SECOND ONE INVALIDATES A LOT OF ARITHMETIC

**1. PHONE 2'S BACKPLATE CELL IS MATERIALLY BIGGER — now measured, not inferred.**
Phone 2 does **strictly more work** than phone 1 (pinned at 240 MHz, GPS streaming) and still
lasts **12.5 h against phone 1's ~9-10 h**. That settles it, and it retires the old worry that the
GPS/240 MHz pin was quietly ruining that phone: it is not, because the pack absorbs it.

**2. 🛑 THE GAUGE REPORTS 100 % FOR THE FIRST 4.6 HOURS.** The cell fell **110 mV (4.210 → 4.100)
while `soc` never moved off 100**. Then it tracked down at 12.6 %/h over the remaining 7.9 h.

| phase | duration | mV/h | reported %/h |
|---|---|---|---|
| gauge stuck at 100 % | 4.60 h | 23.9 | **0** |
| gauge tracking | 7.93 h | 100.8 | 12.6 |

⚠ **SAME PHONE, SAME LOAD, AND `%/h` DIFFERS BY INFINITY ACROSS THE TWO.** Any `%/h` figure is
meaningless unless you say which phase it sampled. **This is the strongest argument yet for the
standing rule: QUOTE mV/h.** The `CW2015` BATINFO profile is never written (`configure()` only
clears MODE) and this is what that costs.
⚠ **It also means a run that starts at a genuinely full 4.21 V behaves differently from one that
starts at 4.13-4.15 V** — the latter is already past the dead zone. Phone 1's 09-01/09-02 runs all
started at 4.13-4.15, so they were NOT distorted by this; do not go back and "correct" them.

## 📈 THE VOLTAGE↔SOC CURVE IS NOW RECORDED 4.21 → 3.30 V

Full depth, single continuous load. ⚠ **And it is TIGHT — most `soc` values map to a 10-20 mV
band — whereas the same mapping POOLED ACROSS PHONE 1'S RUNS spanned 13 points at 3.85 V.** That
is the rate-compensation effect: a single-load curve is usable, a pooled cross-load one is not. So
a home-grown voltage→SOC table would need to be per-load, or carry ±6 points. Data is in
`scratchpad/p2_deep.txt` if this is ever built.

## Housekeeping

✅ **Phone 2 flashed and verified on 0.9.51** (GPS reader ON with 2,908 sentences — which is also
the identity check; phone 1 has no GPS). All 5 mesh channels intact. Charging now.
⚠ **Phone 1 is now OFF the cable** ({user} swapped them) — so it is on battery and running 0.9.51.
⏸ Still untested: **the safety valve** (blind attempt every 4th skip; the commute reached 3).


# ✅ 2026-09-02 MORNING — THE OUT-OF-RANGE TEST PASSED. 0.9.51 SHIPPED FROM WHAT IT SHOWED.

{user} ran the 50-minute commute test. **Both halves of the 0.9.50 scan-gated join worked**, and the
log exposed one real defect which 0.9.51 fixes.

| | |
|---|---|
| `up745  wifi=1  joins=12/0` | lost WiFi setting off |
| `up760  wifi=1  joins=18/0` | last blind attempt |
| **`up770  wifi=1  joins=18/1`** | **FIRST SKIP — the gate engaged** |
| `up778  wifi=1  joins=18/2` | |
| `up788  chg=1   joins=18/3` | plugged in at work |
| **`up789  wifi=3  joins=19/3`** | **REJOINED** — one attempt, connected |

✅ **`tried` frozen at 18 for 28 minutes**, zero join attempts out of range. ✅ Rejoined the moment
a scan saw a saved network. Nothing stranded, nothing needed a human.
📉 **Drain over the gated stretch: ~38 mV/h** (least squares on the voltage trace) against
**106 mV/h** measured hunting on 0.9.46/47. ⚠ **MAGNITUDE IS SOFT AND MUST BE QUOTED THAT WAY**:
27-minute window, 10 mV gauge quantisation (±20 mV/h over that span), R²=0.54. Direction strong,
number not pinned. **A longer plateau run would settle it.**

🐛 **THE DEFECT THE LOG FOUND: the gate engaged 25 min into the drive, not 5** — half the trip
still burned blind joins. `worthAttemptingJoin()` compared `_savedSeenMs` against a **20-minute**
lookback, so a network seen just before setting off counted as "recent evidence" for twenty
minutes after it was gone. **Scans run every 5 min in a dry spell, so that window could only ever
delay the gate.** 0.9.51 replaces it with `_savedSeenLastScan` — *did the MOST RECENT completed
scan see a saved network?* ⚠ Cleared BEFORE `autoSwitchEvaluate()` and set inside it, so the flag
always describes the scan that just finished rather than leaving a previous answer standing
through an early return. ⚠ **The staleness check stays** (`_scanDoneMs`): no scan in a long time
still fails open. Only the POSITIVE evidence test changed.

⚠ **PHONE 2 IS STILL ON 0.9.50** — it was not on the desk this morning. Flash it when it is.
⏸ **The safety valve (blind attempt every 4th skip) is STILL UNTESTED** — the drive reached 3
skips. The next longer trip exercises it; nothing else does.
🧪 **Worth re-running the commute on 0.9.51** to confirm the gate now engages at ~5-6 min, and to
get a longer gated stretch for a firmer mV/h.


# ▶ TOMORROW (2026-09-02, morning): RUN THE OUT-OF-RANGE TEST. START HERE.

**{user} is driving to work and will do it on the commute. He starts the session once he is at work.**
Everything else from 2026-09-01 is verified; this is the ONE thing 0.9.50 needs.

## What he was asked to do

1. **Unplug phone 1 before setting off** — the drive doubles as a battery measurement.
2. Drive to work (out of range of every saved network). **Longer is better: 45-60 min beats 30.**
3. Arrive at work — the phone should rejoin on its own. **No cable needed during any of it.**
4. Plug in, and say so.

## FIRST ACTIONS when he says he is at work

```
cd /Users/{username}/wiphone
python3 <scratchpad>/cmd.py /dev/cu.usbserial-025A3EAF "health all" 25 90 p1_drive.txt
```
⚠ **Opening the port REBOOTS the phone** — harmless, the card log survives. ⚠ `health all` needs
a ~25 s boot wait and ~90 s of capture; shorter and the dump truncates (it did twice on 09-01).
⚠ **Phone 1 is `usbserial-025A3EAF` = `!00449040`.** Identify from the phone, never the port.

## WHAT THE RESULT MEANS — decide this before looking

| observation | verdict |
|---|---|
| `joins=` **skipped climbs, tried barely moves** while `wifi=1` | ✅ the gate works. The headline. |
| **one blind attempt** after ~4 skips (`tried` ticks up once) | ✅ the safety valve works — the thing that makes this safe to ship |
| `wifi=` 1 → 3 within ~5 min of arriving | ✅ rejoin latency is fine |
| `wifi=` takes >15 min to go 1 → 3 at work | ⚠ the gate is costing too much latency — shorten `WIFI_DRY_SCAN_PERIOD_MS` |
| **skipped stays 0 the whole drive** | ⚠ the gate never engaged. Check `[autosw] scan done: n=…` completed, and that the spell passed 5 min. NOT a pass. |
| never rejoins at work | 🛑 **BACK IT OUT.** `worthAttemptingJoin()` is supposed to fail open; if it stranded the phone, one of its early returns is wrong. |

**Then the battery half:** slope the `wifi=1` stretch in **mV/h** and compare with the
**~106 mV/h** measured while hunting on 0.9.46/47. That is the number that says whether the
604 → 49 s/hour duty reduction is real rather than arithmetic. ⚠ Compare mV/h, never %/h — the
CW2015 profile is never written.

⚠ **If his work network was not saved on phone 1**, the return half did not happen and only the
skip half is valid. Check `[autosw] seen '<ssid>' … saved=1` in the log before concluding anything.

## Everything else is DONE and needs nothing

0.9.47 audio watchdog (leak proven + released on hardware) · 0.9.48 dry-spell easing ·
0.9.49 one persisted WiFi switch, proven both directions · 0.9.50 the scan-gated join.
Both phones flashed, pushed, web flasher published. Phone 2 re-verified: GPS on, 5 mesh channels,
hears phone 1 at 11 dB SNR.

🔎 **Open, not urgent:** calibrating the gauge needs ONE run from full to shutdown (current data
spans only 4.13-3.76 V — the bottom half, where it matters, is missing). The music **crackle** is
still deferred to its own session. A cell swap is NOT the fix (UFX 603048A, 6.0x30x48 mm, 900 mAh,
2023.02; +11 % is the realistic ceiling and the 08-31 event needed 180-225 mA).


# 🧪 2026-09-01 END OF SESSION — 0.9.50 IS ON BOTH PHONES AND ONE TEST IS OWED

**Both phones on 0.9.50**, pushed, web flasher published. Phone 2 re-verified after the reflashes:
GPS still ON (32,062 sentences — `gpsen` survives a flash), audio clean, **all 5 mesh channels
intact, and it hears phone 1 `!00449040` directly at 11 dB SNR.**

## ⏸ THE ONE THING WAITING ON {user} — the out-of-range test

**0.9.50 gates the WiFi join on what the last scan actually saw** (`worthAttemptingJoin()`), and
**the out-of-range half has never run.** Everything else in it is verified.

**What he does:** take phone 1 somewhere with NONE of his saved networks for **~30 minutes**,
come back, leave it alone a few minutes, then plug in. ⚠ **No cable needed during the test** —
`joins=` is in the health log. 30 min is the floor: the dry spell starts at 5 min, scans run every
5, and the gate only engages once a COMPLETED scan has positively reported no saved network.
(Turning off every saved AP for 30 min does the same job — it must be all of them, the phone will
hop to any.)

**What to read out of the log afterwards:**
- out of range: `joins=tried/skipped` — `tried` should barely move, `skipped` should climb
- on return: `wifi=` 1 → 3, and how many minutes it took
- 🔑 if `skipped` stays 0 the whole time, the gate never engaged — check the scans were completing
  (`[autosw] scan done: n=…`) and that the spell really passed 5 minutes.

## What 0.9.50 actually changed, and the mistake it corrected

🛑 **0.9.48/49 HAD THE CADENCES THE WRONG WAY ROUND.** A scan is ~350 ms and is the SENSOR; a
failed join holds the radio associating up to 30 s and is the COST. Stretching the scan to 15 min
alongside the retry throttled the cheap thing and pushed worst-case rejoin latency to 15 minutes.
**Scan is back to 5 min; the join is gated instead.**

| | radio time per hour, out of range |
|---|---|
| 0.9.47 | 604 s — 16.8 % duty |
| 0.9.49 | 181 s — 5.0 % |
| **0.9.50** | **49 s — 1.4 %** — and better return latency than either |

⚠ **`worthAttemptingJoin()` FAILS OPEN EVERYWHERE** — no scan evidence, stale evidence, or not yet
in a long dry spell all return true. It may only skip a join it has POSITIVE, RECENT evidence is
pointless. **Plus a safety valve: one blind attempt every 4 skips**, so a hidden SSID or a deaf
radio whose scans lie costs a slower rejoin and never a permanent one. **Do not remove either
property without a replacement.**

✅ **Verified in the normal case:** phone 1 rebooted, scanned, saw 'SmithWifi', switched and
connected, `joins=17/0` — the gate correctly never blocked while a saved network was on air.

## Everything shipped today, in order

0.9.47 audio idle watchdog + `aud=`/`ps=` + the `audio` command (leak PROVEN and released on
hardware) · 0.9.48 the 10-min dry-spell easing · 0.9.49 one persisted WiFi switch that four paths
now respect (PROVEN both directions: off → reboot → `wifi=255`; on → reboot → `wifi=3`) + the
threshold moved to {user}'s 5 min · 0.9.50 the scan-gated join.


# ✅ 2026-09-01 LATE — 0.9.49: ONE WIFI SWITCH THAT ACTUALLY MEANS IT. PROVED BOTH DIRECTIONS.

**Both phones on 0.9.49**, pushed, web flasher published. {user} asked for a single global WiFi-off
toggle in Settings replacing the old one, and for the dry-spell threshold to be **5 min** (he
argued it down from my 10 himself, and he is right — the screen-wake hatch forces an immediate
scan AND retry, so a long back-off costs far less responsiveness than it looks like it should).

🛑 **"WiFi: off" WAS NOT OFF, AND DID NOT STAY OFF.** The toggle stopped the PHY, but:
- the WiFi list screen's 5 s rescan restarted it (`scanNetworks()` opens with `enableSTA(true)`);
- the uploader's stop path and the Game Boy's exit both did an unconditional `mode(WIFI_STA)`;
- **boot never applied a stored OFF at all** — `Networks::init()` does `WiFi.mode(WIFI_STA)` and
  `setup()` only called `disable()` when there was NO saved SSID. ⚠ **Any naive
  "just persist the toggle" would have inherited exactly that bug**;
- and `wifiOn` was a bare global the toggle set and those three paths did not, so the label
  could disagree with the radio.

✅ `Networks::setRadioOff()` flips + persists (`Preferences wpwifi/radiooff`) + applies, in one
call. `Networks::radioOff()` is the ONE predicate all four paths consult. **`wifiOn` is deleted**
and `#define`d to `!wifiState.radioOff()` — the label cannot drift any more.

⚠ **The original "NOT PERSISTED" comment gave a REAL reason** ("a setting you can forget you set,
and the failure mode is a phone that silently never connects again"). **Answered, not ignored:**
the row now carries a subtitle and boot prints `WIFI: radio is OFF (your setting, kept across
restarts)` at log_e. If persistence is ever revisited, keep both of those.

✅ **PROVED ON HARDWARE, BOTH DIRECTIONS** (screenshots + serial): off → row reads
`WiFi: off / saves power - survives restarts`, status bar clears → reboot → **`wifi=255`**
(WL_NO_SHIELD, station not started). On → reboot → **`wifi=3`** associated.
🔎 **`wifi=255` in the health line is the clean "radio deliberately off" signature.**

📋 **Settings menu path for the cable: `key menu down down down down down down down select`**
(main menu order: Phonebook, Messages, Meshtastic, Books, Music, Tools, Games, **Settings**,
Reboot), then `down select` toggles WiFi.

## 💡 THE NEXT IDEA, DELIBERATELY NOT SHIPPED

The retry still calls `connectToPreferred()` **blind**, and a failed join holds the radio
associating for up to 30 s. **Scan first (~350 ms) and only attempt a join when a known SSID is
actually on the air** — that removes almost all of the remaining out-of-range cost and is the
biggest lever left. ⚠ **NOT in 0.9.49 because it cannot be tested from a desk**: it needs a drive
away and back, hidden SSIDs do not appear in scans, and shipping it untested before a three-day
gap risks a phone that will not rejoin. First thing to try next session, with a real trip.


# 📉 2026-09-01 EVENING — WIFI *SEARCHING* IS THE BIGGEST MEASURED DRAIN, AND IT PROBABLY EXPLAINS
# THE 08-31 EVENT. THE AUDIO LEAK IS REAL BUT IS LIKELY NOT WHAT HAPPENED THAT MORNING.

{user} ran phone 1 down again on 0.9.47 (up 79→303 min, 3.73 h, 100 % → 45 %, v 4.120 → 3.780).
Steady state with the first 20 min discarded: **13.53 %/h, 73.5 mV/h ⇒ 7.4 h from full.** WiFi was
associated for only **58 %** of it.

🔑 **THREE RUNS SURVIVE IN THE RETAINED LOG AND THEY LINE UP ON ONE VARIABLE — HOW MUCH OF THE RUN
THE PHONE SPENT ASSOCIATED RATHER THAN SEARCHING:**

| assoc | rate | life | |
|---|---|---|---|
| 27 % | 17.56 %/h | 5.7 h | |
| 58 % | 13.53 %/h | 7.4 h | this run, 0.9.47 |
| 97 % | 10.24 %/h | 9.8 h | the 07:20-11:20 run |

Least squares: `rate = -0.1037 × assoc% + 20.07`, R² = 0.985.
⇒ **extrapolated to a phone that NEVER finds a network: 20.1 %/h. The 08-31 event measured 22 %/h**,
and ~1 h of it was in the car with no WiFi by {user}'s own account.

✅ **CONFIRMED WITHIN THE SINGLE RUN TOO**, so it is not only a between-runs artefact. Voltage
slope over contiguous stretches, first block dropped as surface charge: **associated ≈ 60 mV/h,
searching ≈ 106 mV/h — about 75 % more.** ⚠ The curve-shape objection cuts the RIGHT way: the
slowest block of all (54.5 mV/h) sits at the LOWEST voltage, below the searching stretch, so a
steeper region of the discharge curve is not producing the difference.

⚠ **THREE POINTS IS NOT A STUDY.** Different days, different conditions, and R² on three points is
nearly meaningless as statistics. The claim is only that the relationship is monotonic, holds
within a single run as well as across runs, and lands on 08-31's number at the extrapolated end.

🔑 **CONSEQUENCE — READ THIS BEFORE RE-LITIGATING THE AUDIO LEAK.** The codec leak is REAL and was
caught firing and being released on hardware (see above). But **it is probably not what happened on
08-31**: this run had `aud=0/0` in all 225 samples and still drained at 13.53 %/h purely from
hunting. Two separate faults; the WiFi one is the bigger number.

## PHONE 1 HEALTH ON 0.9.47 — ALL 225 SAMPLES, EVERYTHING CLEAN

| instrument | reading | verdict |
|---|---|---|
| `aud=` | `0/0` in 225/225 | no audio leak |
| `ps=` | `1` in 225/225, **including 83 samples spent searching and 8 with the radio down** | modem sleep held |
| `sip=` | `1` (Idle) in 225/225 — never the stuck `HangUp` | clean |
| `cpu=` | 80 MHz in 216/225; the 9 at 240 line up with screen-on | governor correct |
| `largest` | **33712 → 33712, min == max** | **ZERO fragmentation drift over 3.7 h** |
| psram | 3.441 MB free, varying 336 B across the run | stable |

🔑 **`ps=1` HELD THROUGH 83 SAMPLES OF HUNTING AND 8 WITH THE RADIO DOWN** — that is a second,
stronger disconfirmation of the WiFi-power-save hypothesis, on top of the `wifi bounce` test.
Modem sleep is NOT being lost. The cost of hunting is the scanning itself, not a lost PS setting.

📊 **Chart + full write-up published as an artifact** (voltage trace with the searching bands, SOC,
and the three-run relationship).

## REVISED PREDICTED IDLE LIFE, PHONE 1

| condition | rate | from full |
|---|---|---|
| WiFi **off** entirely | 8.6 %/h | **~11.5 h** |
| associated, in range | ~10 %/h | ~10 h |
| mixed, in and out of range | 13.5 %/h | 7.4 h |
| mostly searching | 17.6 %/h | 5.7 h |
| never finds a network | ~20 %/h | ~5 h |

🌲 **FOR THE WOODS THIS IS THE WHOLE GAME: turning the radio OFF is worth more than doubling the
battery.** A phone left hunting for a network that does not exist is the ~5 h row. This makes the
unbuilt "woods mode" (persist WiFi-off across a reboot + the three escape hatches) the highest-value
remaining battery work by a wide margin — far more than any cell swap.


# 🛑 2026-09-01 CORRECTION — THE WIFI POWER-SAVE CLAIM IS DISPROVEN FOR THE PATH THAT MATTERED

Earlier this session I called lost WiFi modem sleep *"bigger than the codec"* and named
`Networks::bounceRadio()` — the AUTOMATIC deaf-radio cure — as silently doubling idle radio draw
for the rest of a boot. **That was inference from code plus an in-file comment, and the first
direct test on hardware does not support it.**

**MEASURED (phone 1, 0.9.47, `wifi bounce` on the cable):** the bounce genuinely ran —
`radio cycled off/on`, `wifi=6`, `conn=0`, then reassociated to `wifi=3` and SIP re-registered.
**`ps=1` throughout, and the `WIFI: modem sleep had been reset ...` repair line NEVER PRINTED**,
against a check that runs every 5 s and would have caught any drop lasting more than that.

⇒ **`esp_wifi_start()` appears to PRESERVE the configured power-save mode on this SDK**, contrary
to what the comment at Networks.cpp:171-172 implies ("the driver resets the power-save mode when
the station starts").

**What still stands:** `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` really is called in exactly one place
(Networks.cpp:174, inside `connectToWiFi()`, one caller) and six other paths really do start the
station without it. That is a code fact. **What does NOT stand is the consequence I drew from
it.** The one path I could test held the setting.

⚠ **STILL UNTESTED, and they are not the same path:** the Settings WiFi toggle OFF→ON
(`WiFi.mode(WIFI_OFF)` then a bare `esp_wifi_start()`, GUI.cpp:1839), the Game Boy exit
(app_gbc.cpp:328) and the uploader stop (app_gbc_xfer.cpp:1766). Those do a fuller stop/start
than `bounceRadio()` and could still differ. **Do not assume either way — `ps=` answers it in
one glance now.**

🔑 **THE CONSEQUENCE FOR THE 08-31 EVENT: the audio leak is now the ONLY PROVEN drain, and the
WiFi hypothesis is substantially weakened.** The `ps=` field and the invariant both stay — they
cost nothing and will catch it if it ever does happen — but the leading explanation for that
morning is the codec, not the radio.


# ✅ 2026-09-01 LATE MORNING — 0.9.47 IS PROVEN ON HARDWARE. THE AUDIO LEAK IS REAL, CAUGHT IN
# ORDINARY USE, AND THE WATCHDOG RELEASES IT.

**Both phones are on 0.9.47** (flashed 11:28, both `Hash of data verified`), pushed, and the web
flasher is published. `ver` reads `firmware 0.9.47, built Sep  1 2026 11:28:29`.

🎉 **{user} PLAYED A TRACK FOR SIX SECONDS AND PAUSED IT. THE WHOLE MECHANISM THEN PLAYED OUT ON
THE WIRE, UNAMBIGUOUSLY** (serial capture, `scratchpad/p1_proof.log`):

| t | line | meaning |
|---|---|---|
| +31 s | `aud=1/1  scr=45` | playing: powered AND moving samples |
| +46 s | `aud=1/0  scr=45` | **paused — THE LEAK.** Powered, nothing moving |
| +61 s | `aud=1/0  scr=45` | watchdog correctly HOLDS OFF for the lit screen |
| +71 s | `CPU 80MHz (idle)` | screen slept |
| +76, +90 s | `aud=1/0  scr=0` | the 30 s clock running |
| **+101 s** | **`AUDIO: device was left powered with nothing using it - released after 30 s`** | **fired** |
| +105 s | `aud=0/0  scr=0` | released |

🔑 **`aud=1/0` IS THE LEAK SIGNATURE AND IT IS NOW A ONE-GLANCE TEST.** An earlier, accidental
capture had already caught it in a single health sample minutes after flashing — the instrument
found the bug in ordinary use before anybody went looking.

⚠ **The 30 s clock only starts when the screen sleeps.** `screenBrightness > 0` is a deliberate
term of `audioDeviceBusy()` — several consumers (the mic-level meters at GUI.cpp:8988, 9076,
9974, 11800) acquire the device with no non-sticky flag saying so, and enumerating them one by
one is the game the watchdog exists to stop playing. A first attempt to prove this on the bench
failed for exactly this reason and it was NOT a fault: the screen was lit.

## The battery number, from the cleanest run this project has

Today, phone 1, 3.75 h off the cable: **100 % → 57 %, v 4.130 → 3.830**, screen off 99 %, WiFi
associated 97 %, CPU at 80 MHz 99 %, `sip=1` throughout. Steady state with the first 20 min
discarded: **10.2 %/h, 64 mV/h** (hourly blocks 8.1 / 11.2 / 10.7).

| condition | idle life |
|---|---|
| WiFi on and associated | **~9 h** |
| WiFi off (woods) | ~11 h (from the 8.6 %/h WiFi-off figure) |
| **something leaking** (2026-08-31) | **~4.5 h** |

⇒ ~90 mA against 900 mAh nominal, or ~72 mA if the 3.5-year-old pack is at 80 % health.
🔑 **0.9.47 does not make the good case better — it stops the bad case.** The 9 h was always
there when nothing had leaked.

⚠ **TODAY DID NOT ISOLATE THE CAUSE OF THE 08-31 EVENT, AND {user} SPOTTED WHY: two variables
changed.** Thursday had ~1 h in the car with no WiFi; today was 97 % associated throughout. So
the audio leak and the WiFi-power-save loss are BOTH still live explanations for that morning.
Weak supporting signal only: the previous run was 71 % associated at 77 mV/h, today 97 % at
64 mV/h, over near-identical voltage ranges. Two runs is not a measurement.

## The cell, settled from a photo of the label

**UFX 603048A, 3.7 V, 900 mAh, 3.33 Wh, made 2023.02.09** — so **6.0 × 30 × 48 mm**, and 3.5
years old, not the ~6 assumed earlier. **Soldered directly to the board**, no connector; the pack
carries its own protection FETs (docs/woods-backplate.md:191 records them being destroyed once).
Same-footprint options: 900 mAh standard, **1000 mAh widely available with PCM** (DNK Power, FP
Battery, eBay retail), 1100 mAh from a manufacturer with likely MOQ. ⇒ **+11 % realistic.**
⚠ **The arithmetic says a new cell is not the fix**: 900 mAh at the measured ~78 mA idle is
~11.5 h, and even at 80 % health ~9.2 h. The 08-31 event needed 180-225 mA. **A pack cannot
explain it; something was drawing ~100 mA extra.**
⚠ The `603048` code is the BARE cell — the PCM adds length. Measure the TOTAL installed length
against a vendor's overall figure, not the code.

## Still open for Thursday

- ✅ **DONE, AND IT CAME BACK NEGATIVE — see the correction section at the top.** `wifi bounce`
  held `ps=1` throughout and never triggered the repair, so the bounce does NOT lose modem sleep.
  The three fuller stop/start paths remain untested.
- The residual: which of the two leaks caused 08-31. `aud=` and `ps=` now answer it from ordinary
  use — no staged test needed.
- {user}'s music **crackle** is untouched and explicitly deferred to its own session.
- 📋 Assessed and rejected, with numbers, so they are not re-litigated: a no-relay toggle
  (~0.016 mAh/h), Bluetooth (nothing runs it), turning SIP off (idle SIP holds nothing).
- A "woods mode" is still unbuilt. Its real content is NOT just persistence: boot never applies a
  persisted WiFi-off (Networks.cpp:324 vs WiPhone.ino:1423), and three paths restart the radio
  without clearing the toggle (GUI.cpp:7167/7322 rescan, app_gbc_xfer.cpp:1731 softAP,
  app_gbc.cpp:328 GBC exit).


✅ **BOTH PHONES ARE ON 0.9.47** (2026-09-01; the 0.9.46 line below is history) (flashed 2026-08-29 15:24 from one build dir, both
`Hash of data verified`). ⚠ `ver` build-time still cannot distinguish same-day builds. Both
associated, phone 1 SIP-registered; panicwatch running on both. Host suite **1,161
assertions, 0 failures** across eight suites (the old "841" figure in memory is stale).

🎉 **BOOK SYNC IS NO LONGER A STOPWATCH (0.9.46) — PROVEN BOTH DIRECTIONS ON HARDWARE.**
{user}'s report was that a position only landed if he opened the book first, sent second, then
closed and reopened the book. All three steps were working around faults:

- 🛑 **`BooksApp`'s constructor called `bookSyncInboxInit()`.** The app is `new`ed on entry to
  Books and `delete`d on exit (GUI.cpp `GUI_APP_BOOKS`), so **opening the Books app emptied
  the parking spot** — at the exact moment the reader was about to look in it. That is the
  whole reason a position had to arrive with Books ALREADY OPEN. The inbox is a file-static
  array the C runtime zeroes before `setup()`; it never needed initialising. **The call is
  gone and there is a 🛑 comment where it was — it reads like tidy housekeeping, so do not
  put it back.**
- 🐛 **`checkForPending()` ran on book open and after a passcode edit, and nowhere else** —
  hence the close-and-reopen. `BOOKS_READ` now ticks at 1 Hz and re-checks when (and only
  when) `bookSyncInboxSeq()` has moved.
- 🐛 **Even when a position WAS found, the only surfacing was a row inside `Menu`.** Opening a
  book with one waiting now goes **straight to the sync card**, which is COVEY's behaviour and
  what {user} asked for.

**MEASURED ON BOTH PHONES, 2026-08-29 (screenshots at each step, no hand on the receiver):**

| test | result |
|---|---|
| sent with the receiver's Books app **shut**, then Books opened | card on book open: *"WiPhone-9040 says: they are at 66% / you are at 21%"* |
| sent with the receiver **sitting on the page** | card appeared **by itself in <10 s**, no input: *"they are at 82% / you are at 66%"* |
| declined with Back | lands on the page; **still gone 20 s later** — no nag |
| sent while the receiver was in the reading **menu** | menu undisturbed; card raised on `Back` to the page |
| reverse direction (phone 2 → phone 1) | *"WiPhone-{user} says: they are at 66% / you are at 82%"*, accepted, both phones at 66% |

Heap after all of it: phone 1 `largest=23,364`, phone 2 `largest=26,884` with `min == heap`
(never dipped). The two Guru Meditations in `/tmp/wiphone-serial.log` are at lines 122170 and
324400 — **before** the 15:27:02 watcher restart, i.e. historical, not this build.

⚠ **Still RAM-only: a reboot discards anything parked.** Persisting the inbox was deliberately
not attempted — SPIFFS on this phone is pathologically slow and the inbox is four entries.
⚠ **You still do not need to tap Sync on both devices** (D-089) — the prompt wording is the
open item there, not the transport.
📇 **Phone 1's sync device name is still the default `WiPhone-9040`**; phone 2 is
`WiPhone-{user}`. Books ▸ menu ▸ Sync settings ▸ This device, if {user} wants it friendlier.

# 🔋 2026-09-01 — TWO PHONES READ OVER THE CABLE. THE GPS FINDING IS REAL AND IS ON **PHONE 2**.
# PHONE 1'S 22 %/h IS STILL UNEXPLAINED.

{user}: phone 1 unplugged ~06:00 on 08-31, mostly idle, **12 % by 10:00 ≈ 22 %/h** — against the
8.6 %/h idle and ~10 %/h mixed this project has measured.

🛑 **I FIRST ATTRIBUTED THIS TO THE WRONG PHONE.** The inference was "memory says the NEW phone is
on port 025A3EAF, therefore EAF is phone 2" — wrong, and {user} caught it. **Identify a phone from
the phone, never from the port.**

✅ **THE IDENTIFICATION, from `Meshtastic ▸ Status` on each screen via `tools/shot.py`:**

| port | `Node:` | GPS | wallpaper | = |
|---|---|---|---|---|
| `usbserial-025A3EAF` | **`!00449040`** ⇒ default name `WiPhone-9040`, and HANDOFF already records that as phone 1's | none; reader never on, `bytes=0`, `sats -1` | mech on a barren plain | **PHONE 1** |
| `usbserial-025A3F65` | **`!00449334`** | **live module**, 1,047 sentences / 30 s, **0 bad checksums** | tank / battle scene | **PHONE 2** (woods backplate, expanded battery) |

🔑 **`tools/shot.py <port> out.png --cmd "key menu down down select down down down down select"`
lands on Meshtastic ▸ Status, which prints `Node: !xxxxxxxx`. That is the identity check** — no
serial command prints our own node number (`serial_cmd.cpp` never calls `getMyNodeNum()` for self).

## The GPS → 240 MHz pin: REAL, REPRODUCED BOTH WAYS, but it is on PHONE 2

| | phone 2 (`F65`, GPS on) | phone 1 (`EAF`, no GPS) |
|---|---|---|
| cpu=240MHz | **1,071 / 1,071 samples** (1,048 with `scr=0`) | 23, all screen-**on** |
| cpu=80MHz, screen off | **0** | **1,176 / 1,176** |

`gGpsNmea` is a term in the `busy` predicate (WiPhone.ino:4111) — there for **correctness, not
speed**: the comment above it records a measured hard deadlock where `setCpuFrequencyMhz()`'s
`uart_on_apb_change()` callback never exits if NMEA arrives faster than 1 byte/ms (115200 =
11.5 B/ms; 9600 = 0.96 B/ms converges). The same predicate also gates `idleTickStretch`.

✅ **Proved in both directions on phone 2, live:** `gps off` → `CPU 80MHz (idle)` immediately (the
first 80 MHz in that phone's whole retained log); `gps on` → `CPU 240MHz (busy)` immediately.
**Left ON — that is how it was found, and that phone has the expanded battery and no complaint.**

⚠ **This is now a WOODS-TRIP WARNING, not an explanation of phone 1.** Phone 2 runs pinned at
240 MHz with a GPS streaming and {user} reports its battery is fine — which is *evidence against*
the pin being catastrophic on its own, and/or evidence that the backplate battery masks it. The
exit the code's own arithmetic points at: run the module at **9600**, then `gGpsNmea` can leave
`busy` (`gps baud 9600` exists; the M100 Mini ships at 115200 and needs a UBX/PMTK rate command).

## ⚠ PHONE 1'S DRAIN IS NOT IN EITHER LOG, AND `soc=` IS ON AN UNPROGRAMMED BATTERY MODEL

- **Retained windows:** phone 1 = 19.8 h continuous (`up` 2616→3805, one BOOT, no gaps), lowest
  SOC **49 %**; phone 2 = 17.8 h across three boots, **soc 100 % throughout** (on the charger).
  {user}'s event was ~28-34 h before the read. **It is outside both windows. Neither log saw it.**
- Phone 1's own measured discharge, GPS off and CPU at 80 MHz, in 1-h blocks:
  **9.2 / 7.1 / 14.2 / 11.2 %/h** — normal-ish, and nothing like 22.
- 🔑 **`CW2015::configure()` (src/drivers/CW2015.h:76-88) ONLY CLEARS THE MODE REGISTER. THE
  BATINFO BATTERY PROFILE (0x10-0x3F) IS NEVER WRITTEN — nothing in this repo touches it.**
  The gauge estimates SOC from whatever model is in its RAM. So **`soc=` is a soft number**, its
  linearity is unvalidated, and **it is not comparable between two phones with different packs**.
  `readVoltage()` is a direct VCELL ADC read and is the trustworthy signal.
  ⇒ **Quote mV/h, not %/h**, and never compare %/h across the two phones.
- ⇒ **"22 %/h" conflates draw with pack capacity and with gauge error.** Phone 1 is the older
  phone; a degraded pack produces exactly this signature at an unchanged current. **Untested.**
- 🧪 **THE EXPERIMENT THAT SEPARATES THEM:** both phones off the charger from full, side by side,
  screen off, **same** settings (WiFi off both, GPS off both, mesh on both), 2 h, then read
  `health` on each and compare **mV/h**. Identical firmware state ⇒ any difference is the pack.

## What the same read says about the four ideas {user} asked about

1. **Toggle off relaying other nodes' traffic — will NOT save measurable battery.** Measured
   `MARK mesh-relay`: **25 relays in 20 h** (phone 1), **9 in 18 h** (phone 2) = 0.5-1.25/h.
   At ~0.5 s on air (the code's own "~518 ms") and ~90 mA at PA_BOOST +17 dBm that is
   **~0.016 mAh/h** — ~0.01 % of the budget. Even 100 relays/h would be ~1 %. Worth adding as
   mesh etiquette, never as a battery feature. Insert point:
   `meshtastic_service.cpp:1838-1851` (the CLIENT-role relay block), row in `buildMyNode()`,
   persist beside `hoplim` in the `wpmesh` Preferences.
2. **Bluetooth: nothing is running.** No `BluetoothSerial`/`BLEDevice`/`NimBLE` anywhere;
   `btStop()`/`esp_bt_controller_disable()` appear only in `Networks::disable()` and
   `app_gbc.cpp`. arduino-esp32 does not start the controller unless asked. **No power to save —
   but there is RAM:** `app_gbc.cpp:342` calls `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)`
   only when the emulator starts. Doing it once in `setup()` returns the BT ROM regions
   permanently; `heap: internal free=29448`. One line, worth **measuring**.
3. **Unchecking "Make primary" saves ~nothing.** `sipNeedsFullSpeed()` (WiPhone.ino:2091)
   excludes every non-call state, and phone 1 sits at 80 MHz with `sip=1`. With no WiFi there is
   nothing to poll and nothing to re-REGISTER. **The cost is the WiFi radio, not SIP.**
4. Also seen: **`sip=6` (`CallState::HangUp`)** in 1,066/1,071 samples on phone 2 and 295 on
   phone 1 — a teardown that never returns to Idle. No evidence it costs power; stability list.

# 🔎 2026-08-27 LATE AFTERNOON — {user}'S BOOKSYNC REPORT: THE TRIGGER IS UNREPRODUCED,
# THE *NEVER-RECOVERS* HALF IS FIXED (0.9.31)

{user}: *"I generally read ebooks on wiphone 1, and then when I'm done reading I hit the
booksync. I noticed when I did that the wifi cut and refused to go back on … walked out of
range and came back, it didn't want to reconnect automatically. A reboot fixed it. So I
think clicking 'sync my place' killed the wifi's ability to auto reconnect. This might
possibly have been the issue the whole time."*

**The honest split: I could NOT reproduce the trigger, and I DID reproduce, measure and fix
the part that actually hurt — the phone never coming back on its own.**

- ❌ **`Sync my place` does not touch WiFi.** `sendMyPlace()` (app_books.cpp:916) →
  `sendChannelMessage()` → `meshTxText` → `meshPhy.send()`: no WiFi/esp_wifi call anywhere
  on the chain, no filesystem write beyond the ordinary. Six live open→sync→close cycles on
  the bench (bridge-driven, screenshots at each step, "Sent: ch 19, 21%" confirmed, receipt
  `-> in mesh`): **WiFi held every time.** The 0.9.30 flag work is not the culprit either —
  `ud=0 rec=1 en=1` throughout {user}'s wedge.
- ✅ **What IS real on that path, measured on phone 1 this session:** opening a book =
  **12.4-12.6 s of frozen superloop, every time** (four measurements, 12419/12433/12534/
  12571 ms) — during which the WiFi *driver* runs but every piece of RESCUE machinery
  (the retry at ino:2777, autoSwitchTick, the scan-result poll, the bounce) is dead, because
  all of it is polled from loop(). The sync press adds ~1.5 s more (the LoRa TX busy-waits
  TxDone at mesh_phy.cpp:268-277; SF11/250k airtime for a booksync frame is ~1.2-1.5 s).
  🔑 So a drop that lands in that window gets a ~14 s head start on the phone's rescue.
- 🔑 **THE MECHANISM OF "REFUSED TO GO BACK ON", now understood and cited:** a failed
  join hands the radio to the **framework's event-task auto-reconnect**, which retries
  `disconnect()+begin()` **capless and gapless** after every reason≥200 (WiFiGeneric.cpp
  :404-410) — and **esp_wifi documents that a scan started while the station is connecting
  is ineffective or aborted** (esp_wifi.h:322-330). A radio held perpetually mid-connect
  therefore completes **every** scan 0 or -2. That is the whole "deaf radio": not broken
  hardware, not a broken flag — *the scan never got a quiet radio to run on.*
- 🛑 **AND 87ddfbc (08-24) made it worse without knowing.** Removing `connectToWiFi`'s
  `disconnect(true)` killed a real 5 s freeze — but that call was also an **accidental radio
  cycle every 20 s**, which is exactly the cure for this state. The freeze fix quietly
  removed the phone's only automatic escape. Same shape as the "rescan" ritual being an
  accidental bounce.
- ✅ **STAGED THE WEDGE LIVE ON 0.9.30 AND WATCHED IT ALL THE WAY THROUGH** (`wifi drop`
  collided with a book open): the phone went to `wifi=1`, `ud=0 rec=1 en=1 conn=0`, scans
  completing n=0 and a blocking `wifi scan` reading **-2** — the exact signature of {user}'s
  report — sat wedged, and then **the 0.9.29 self-bounce FIRED for the first time ever
  observed**: `[autosw] 2 consecutive empty scans: restarting the radio before this one` →
  scan n=7 → `switching to 'SmithWifi' (-57 dBm)` → `conn=1` → `SIP REGISTRATION ->
  REGISTERED`, hands-free. ⏳ **That closes the 0.9.29 "first live firing UNPROVEN" item.**
  The catch: it took **~10 minutes**, because the eased cadence and the `n<0` blind spot
  both delay it — and {user} reboots long before ten minutes.
- ✅ **SHIPPED (0.9.31, phone 1 flashed, host suite exit 0):** four changes, all aimed at
  "recovers by itself, fast":
  1. **`n<0` completions count as dry evidence** (Networks.cpp) — the mid-connect churn
     alternates 0 and -2, and counting only the zeros roughly doubled time-to-bounce.
  2. **Fast confirm cadence:** with dry evidence and no bounce tried yet this spell, the
     disconnected scan interval drops from 2-5 min to 30 s, so the bounce lands in ~90 s
     instead of ~10 min. Capped to ONE bounce per spell (`_dryBounced`), so a phone
     genuinely out of range all day still gets the battery easing.
  3. **The retry no longer aborts its own rescue:** a scan is skipped while a join is
     younger than 10 s. The wake path was deterministic self-sabotage — the reconnect block
     runs BEFORE autoSwitchTick, so on a screen wake the retry started a join and the
     wake-forced scan's pre-scan disconnect killed it in the SAME loop pass. The wake wish
     is now sticky (backdated `_msLastScan`) instead of a flag that guard would eat.
  4. **The 30 s post-failure quiesce arms after EVERY failed attempt, not only the 5th** —
     the first ~100 s of every drop used to run the framework churn unquenched, which is
     precisely the window {user}'s book-open stall opens.
  Plus `[autosw] scan done: n=… dry=N` now prints the counter, so the next field wedge is
  readable from the log without inference.
- 🔎 **Circumstantial support for {user}'s "the issue the whole time":** in the current logs
  phone 1 (the book phone) has **25** empty scans and 18 book/sync events; phone 2 (same
  desk, same firmware, no books at all) has **1** empty scan and 0 book events. Phone 1's
  log is ~4× longer, so the 25:1 ratio is real but not a controlled experiment.

# ✅ 2026-08-27 EVENING — THE BOOK-OPEN FREEZE IS FIXED: 11.9 s → 1.2 s (0.9.32)

The enabler above is closed, and the measurement corrected a plausible wrong theory.

- 🔑 **INSTRUMENT FIRST. The code-reading theory was wrong.** The audit blamed "no zip
  directory cache — every `zipFind` re-walks the central directory", and predicted
  `navTitles` and the per-image walks. Two permanent log lines (`BOOK OPEN` in
  `openBook`, `epubOpen` in `epub_parse.cpp`, plus SD-transaction counters) said otherwise
  on the first run: `fingerprint=228 findContainer=93 readOpf=11 **manifest=10908**
  nav=270`, and **34,332 SD reads / 7.4 MB** for one open. The zipFinds are 5 and 279 reads
  — cheap. `navTitles` is 270 ms. Nearly all of it was in one place nobody suspected.
- 🛑 **THE CULPRIT: `zipHasName(src, it->href)` in the spine-build loop.** It is a full
  `zipForEach` — a 64 KB EOCD re-scan plus two small SD reads per central-directory entry —
  and it was called **once per spine item**. A 90-chapter book therefore walked the same
  archive **ninety times** to answer ninety yes/no questions.
- ✅ **THE FIX: one presence pass.** `presenceVisit`/`PresenceCtx` walk the directory ONCE
  and mark `OpfItem.present` for every manifest item; the spine loop then tests a bool.
  `zipHasName`/`nameVisit`/`NameListCtx` are DELETED rather than left lying around, because
  the next per-item caller would silently reintroduce the freeze.
- ⚠ **THREE defects caught before this shipped — all of them "a chapter disappears and
  nobody is told", which is this repo's signature failure:**
  1. *Self-review:* the first cut had `if (!present && strcmp(...)) { present = true; break; }`.
     Two manifest items sharing one href — malformed but real — would leave the second
     unmarked and its chapter gone. The old per-item `zipHasName` answered true for both.
     The `break` and the guard are gone; every match is marked.
  2. *Adversarial review:* **the pass discarded `zipForEach`'s return value.** That walk
     abandons itself and returns false on a short read from the card (`epub_parse.cpp:176`,
     `:191`), and because ONE walk now serves every item, a glitch part-way would leave
     every later item `present=false` — the tail of the book vanishing silently. The old
     code degraded gently: one bad walk cost one chapter. **It now fails OPEN** — a false
     return means only "the walk did not finish", so the manifest is trusted instead of a
     half-read directory. A genuinely missing chapter then fails visibly at read time.
  3. *Adversarial review:* the pass ran even for `nItems == 0` / `nSpineIds == 0`, adding a
     full directory walk to a malformed book that was going to fail anyway. Now guarded.
- ✅ **MEASURED ON PHONE 1 (0.9.32, flashed):** `BOOK OPEN 11903 ms → 1285 ms`, SD reads
  **34,332 → 1,493**, bytes **7.4 MB → 506 KB**, and the `LOOP STALL` the open produces
  **12,025 ms → 1,301 ms**. Correctness checked, not just speed: same book opens at the same
  place (ch 19/90, 21 %, identical text) with the **same spine count of 90**, and a
  second, differently-built book (Dominion 1, 115 items) opens in 1,106 ms with its 41
  chapters and the empty-cover skip still working.
- 🔑 **Why this is a WiFi fix too:** that stall is time with the superloop stopped, and every
  piece of the WiFi rescue machinery is polled from it. **The window in which a drop gets a
  free head start shrank from ~12.5 s to ~1.3 s.**
- 📌 The `BOOK OPEN` / `epubOpen` timing lines STAY IN, like `SLOW WIFI` and `SLOW STEP`
  before them: the next person should not have to re-derive where the time goes. On the host
  build `EB_NOW`/`EB_TIMING` compile to nothing.

## ▶▶ WHAT IS OWED ON THIS (next session)

1. ~~Phone 2 is still on 0.9.30~~ ✅ **DONE — both phones on 0.9.32, both associated.**
2. ⏳ **The fast-bounce has NOT been seen firing in the wedge state** (the desk AP is
   too strong: three staged drop-during-book-open collisions all rejoined in ~2 s
   without ever going dry). The next natural wedge is the test — grep the panicwatch log for
   `dry=` and `restarting the radio`, and check the gap between them is ~90 s, not ~10 min.
3. 🟡 **The open is now 1.2 s, and ~750 ms of that is still SD.** If it ever matters:
   `findOpf` alone is 279 reads (one full directory walk) and the presence pass is another,
   so a single cached central directory per open would remove both. Not urgent at 1.2 s.
4. ~~🟡 **NEW P2, found by the same audit, adjacent to the incident:** `MeshPhy::send`'s
   **2000 ms TX timeout is shorter than a maximum-length frame's airtime.**~~ ✅ **FIXED
   2026-09-19 (unproven on hardware as of writing):** the wait now follows the frame —
   `meshLoraTxTimeoutMs()` in `mesh_airtime.{h,cpp}` = airtime + 25 % + 300 ms, floor 1 s,
   with the AN1200.13 airtime pinned by `tests/test_airtime.cpp` (254 B → 2157 ms, 37 B →
   519 ms). The compose field's cap dropped from 233 (the RECEIVE buffer) to
   `meshComposeCap()` = min(wire budget 232 channel / 220 PKI DM, the phone apps' 200) = 200,
   with a live `N/200` counter, and a refused Send now stays on the compose screen and says
   why (`lastSendError()`), instead of dropping into the thread with nothing sent. Original
   finding: at SF11/250k a
   254 B frame (the max `MESH_TEXT_LEN` 234 allows) is 2157 ms, so it is declared
   "TX timeout", the IRQ flags cleared and the radio forced back to RX **while the PA is
   still transmitting** — truncated on air, `send()` returns false, and the ~2 s of loop
   stall was paid anyway. A max-length channel text can therefore never be sent. Booksync
   frames are ~130-165 B so they are safe; this is a latent bug for long chat messages.
5. 🟡 **P2, unchanged:** Remove-network and Disconnect still call full `disable()` (radio +
   BT off), heavier than either action means.

# ✅ 2026-08-27 EARLY AFTERNOON — THE PEEK WART IS CLOSED (0.9.30), AT {user}'S ASK

The wart the 0.9.29 block below filed as P2 ("peeking at the WiFi list silently kills
auto-rejoin") is FIXED and proven, plus two relatives found while holding the screen:

- `NetworksApp`'s destructor restores auto-rejoin on the way out
  (`Networks::resumeReconnect()`), **gated on `userDisabled()`** so an explicit
  Disconnect/Remove/WIFI-OFF is never overruled by merely leaving the screen.
- `disable()` now sets `_userDisabled` LIVE (before, only boot-time `loadPreferred()` ever
  set it, so a mid-session Disconnect left the flag stale-false); a deliberate
  `connectTo()` clears it, mirroring the 0.9.28 `reconnect` restore and the join path's
  own `disabled=false` INI write.
- **Both WIFI-ON toggles (Settings row and edit screen) now call `resumeReconnect()`** —
  they used to only `esp_wifi_start()`, so an innocent OFF→ON cycle left auto-rejoin dead
  until a join or reboot. The Settings toggle's own comment promised "the main loop's
  reconnect and auto-switch own that" while those loops gated on the flag it never set.
- ✅ **All three scenarios proven live on phone 1, driven entirely over the panicwatch
  bridge** (`key` + the new bridge-shot trick: the `shot` serial command's base64 lands in
  the panicwatch log and decodes to a PNG — no port open, no reset risk): (1) peek & back
  out → `rec` 0→1 on exit, autosw scan, `switching to '{user}-wifi'`, `conn=1`, SIP
  re-registered, hands-free; (2) explicit WIFI-OFF + peek + back out → **stays off**
  (wifi=255, 50 s of autosw silence); (3) WIFI-ON toggle → `ud=0 rec=1` at the toggle,
  hands-free rejoin + `SIP REGISTRATION -> REGISTERED`. Host suite exit 0. Both flashed.
- ⚠ Still deliberately NOT touched: Remove-network and the Disconnect button calling full
  `disable()` (radio + BT off — heavier than either action means). Same P2 note as
  before; their SEMANTICS are now at least consistent (they set the live flag).

# 🔎 2026-08-27 MIDDAY — {user}'S IDLE-DROP REPORT IS DIAGNOSED LIVE: THE DEAF-SCAN STATE,
# AND 0.9.29 MAKES THE PHONE CURE IT ITSELF

{user}: *"wiphone 1 was idle for a while and it dropped wifi connection (wiphone 2 didn't) …
if I go and rescan, in the past it brings it right back up, but I'm leaving it wifi
unconnected for you to see first."* That preserved wedge was measured end to end:

- 🔑 **THE WEDGE, OBSERVED LIVE (phone 2 = on-air control, associated, hearing
  {user}-wifi at −50 dBm):** phone 1's whole rescue machinery was RUNNING — `rec=1 en=1`,
  retry loop attempting, auto-switch scanning — but **every scan completed empty**: autosw
  async n=0 over and over, serial blocking scan 0 and −2, screen-wake scan at 240 MHz n=0.
  The n=0 branch honestly believes "out of range" and backs off, so the phone stays down
  forever. The 0.9.28 reconnect-flag fix is NOT the culprit and held throughout.
- 🔑 **THE CURE, ISOLATED: a radio off/on, nothing else.** A reboot mid-session (see trap
  below) came up hearing 6 networks, auto-joined and SIP-registered inside a minute — same
  radio that scanned empty seconds earlier. **And "open the WiFi screen and rescan brings
  it back" was never the scan: `NetworksApp`'s constructor calls
  `wifiState.disconnect()` = `WiFi.disconnect(true, true)` = radio OFF, and its first scan
  turns it back on.** {user}'s ritual was an accidental radio bounce all along. (It also
  erases the driver's remembered AP and clears `reconnect` — see the wart below.)
- ✅ **SHIPPED (0.9.29):** the auto-switcher counts consecutive completed-empty
  disconnected scans (`_dryScans`) and at 2 **bounces the radio itself** before the next
  scan — log line `[autosw] N consecutive empty scans: restarting the radio`. Plus a new
  bench command **`wifi bounce`** (`Networks::bounceRadio()`): the cure on demand without
  a reboot, proven live: bounce → immediate scan n=1 ({user}-wifi −36 dBm) → rejoin in
  9 s → `SIP REGISTRATION -> REGISTERED`. Host suite exit 0. Both phones flashed.
- 🛑 **THE BOUNCE-V1 LESSON, hit live and fixed the same hour:** calling
  `WiFi.disconnect(true)` on an ASSOCIATED phone swallows the STA_DISCONNECTED event —
  `Networks::connected` stays TRUE, and **both** rescue loops gate on it: the phone sat
  half-down with no retries and no scans, and even `wifi drop` couldn't wake it (a plain
  disconnect on an already-down driver fires no event). `bounceRadio()` therefore delivers
  a plain disconnect FIRST (event → flag), sets `connected = false` explicitly, THEN
  cycles the radio, then stamps the next autosw scan due immediately. **Any future code
  that turns the radio off must tell the state layer first.**
- ⚠ **HONEST LIMIT:** the `_dryScans >= 2` trigger cannot be fabricated at the desk (AP
  present → scans see it), so its first live firing is UNPROVEN — the next natural
  idle-drop is the test. panicwatch is running on both phones; grep the log for the
  `restarting the radio` line when it happens.
- ~~🔴 **NEW WART FOR THE P2 LIST:** opening the WiFi settings screen and backing out
  WITHOUT joining leaves `reconnect = false` … peeking at the network list silently kills
  auto-rejoin until the next join or reboot.~~ ✅ **CLOSED THE SAME DAY (0.9.30, {user}'s
  ask) — see the block above.** Kept for the record of when it was found.
- ⚠ **BENCH TRAP AMENDED:** "phone 1's adapter never resets on port open" held for DOZENS
  of spaced opens (2026-08-26) but a RAPID close→reopen (two `shot.py` runs seconds apart)
  DID hard-reset it (`BOOT: reset_reason=1`) and destroyed the preserved wedge state.
  Space port opens by a few seconds, or route through the panicwatch cmd bridge.
- 🔎 Context recovered from the log: the drop itself happened OFF the bench — the USB
  port vanished 09:26→11:02 while phone 1 ran on battery (soc 100→79 %), so the
  disconnect moment is unrecorded; the wedge state it came back in is what was measured.

# 🌙 2026-08-26 NIGHT — THE UPLOADER IS REBUILT AND THE PAGE NO LONGER DIES (0.9.28)

{user}'s overnight ask: *"the wifi uploaders … are unstable and unusable (ram leaking and pages
not wanting to load on browsers) … goal is to be able to (somehow and conveniently) upload
files to the device without opening the case, without it panic booting or losing connection."*

🔑 **ROOT CAUSE, MEASURED (docs/uploader-bench-2026-08-26.md):** the framework WebServer costs
~10 KB of internal-heap transient per fresh CONNECTION — page bytes irrelevant; switching the
page to `sendContent_P` moved nothing — and from the 13-15 K idle largest a SIP phone runs,
that bottoms at 1.6-3 K: the breaker's instant tier. So nearly every browser VISIT (page +
favicon + probe) tripped the breaker and the NEXT visitor got "site cannot be loaded": 2 of
10 page GETs answered, with 90→360→600 s backoff. The "RAM leak" was the breaker's own
allocate/free churn (its timer-resume replanted a ~6 KB server into a 4.5 K-largest heap
every 90-180 s) plus ESPmDNS's genuine leak (one WiFi-event callback per begin/end cycle,
never removed — and the breaker cycled it on every pause).

**SHIPPED (0.9.28, commit 3b90a62):** the RAW single-connection pump (4ff2a18's transport)
now listens on **:80 as well as :8081** and serves the PAGE, /favicon, /fetch (shared
`xferFetchUrl`), and a /log redirect — keep-alive, no Strings, per-request heap ~zero. The
WebServer lives on **:8080** for what genuinely needs it (multipart `/upload` for no-JS +
`curl -F`, /log streaming) — its trips can no longer take the page down. The raw-served page
skips the 8081 probe (`RAWPAGE` marker): same-origin IS the raw transport. Breaker reworked
on the night's measurements: the 6144 trip line must hold for **1.5 s** (one request's
transient is not the flood; **<3072 stays instant** — the PHY-abort guard); raw traffic
inhibits trips like chunk pacing already did; resume is **recovery-only at largest ≥ 10240**
(the same bar xferStart demands — the timer-resume was the flap). mDNS begins once per boot,
never ends. `up` prints both URLs.

✅ **PROVEN ON PHONE 1:** page 19-20/20 under a 20-GET hammer (was 2/10); **3× 4-book
batches = 12/12 files byte-verified (~52 MB) at 48-69 KB/s, ZERO breaker events, zero
panics, no wedge**; resumed tails 156-249 KB/s. 🛠 **NEW `tools/wiphone_send.py`** — the
convenient no-browser path: `python3 tools/wiphone_send.py --app books f1.epub f2.epub`
(finds the phone via the serial bridge or wiphone.local, starts `up on books` itself,
keep-alive chunks, resume, 507-with-patience, verifies held==size). `tools/chunkup.py`
still works unchanged (:8081 is still listening).

# 🛑 RETRACTED, SAME MORNING, BY {user}'S THUMB: THE RADIO IS FINE. READ THE CORRECTION BELOW
# THE ORIGINAL VERDICT, WHICH FOLLOWS UNEDITED AS A SPECIMEN OF A CONFOUNDED CONTROL.

**2026-08-27, ~30 min after the "hardware failure" verdict was pushed: {user} opened
Settings > WiFi on phone 1, scanned, immediately saw the hotspot, joined it.** Minutes
later the SAME serial `wifi scan` that had read 0/0/-2 all morning listed **6 networks,
{user}-wifi at -59 dBm**, and SIP registered. Nothing was repaired in between.

🔑 **THE REAL MECHANISM: a scan issued while the firmware's connect-RETRY loop is
mid-attempt fails (-2) or reads empty — and phone 1 was in that state for EVERY deaf
reading** (at work it had no joinable saved network, so the retry never stopped). Phone 2
"as control" was ASSOCIATED, so its scans ran clean — it controlled for the air and the
firmware but NOT for the WiFi state, which is the variable that mattered. The softAP wedge
is most plausibly the same collision (mode switch during a connect attempt), now unproven
either way. `wifi calreset` / `wifi restore` remain useful tools; neither was the cure
because nothing was broken. ⚠ **`wifi restore` DID wipe the driver's remembered network**
— harmless only because {user} re-joined by hand right after.

✅ **AND THE AUTO-CONNECT BUG IS FOUND, FIXED AND PROVEN, SAME SESSION — it was a
ONE-WAY FLAG, and the screen that flips it is the WiFi settings screen itself.**
`Networks::reconnect` is set true in the CONSTRUCTOR and false in
`disconnect()`/`disable()` — and nothing anywhere ever set it back. The WiFi settings
screen's SAVE path calls `disconnect()` (GUI.cpp:6096) before rejoining, so **editing or
saving any network left the phone unable to auto-rejoin until reboot** — and since
`disconnect()` is `WiFi.disconnect(true, true)`, it also turned the radio OFF and ERASED
the driver's stored AP, which is the state my morning scans were reading as "hardware-dead"
and which reboots alone could not fully unwind. Phone 1 is simply the phone whose networks
get EDITED (SIP phone, most fiddled-with) — that is the whole phone-1-vs-phone-2
difference. Not the radio, not SIP's code, not the GPS plate, not build differences.
**Fix (Networks.cpp, connectTo): a deliberate join restores `reconnect = true`** — every
join path (settings screen, connectToPreferred, the auto-switch hop) flows through it.
**Proven at the work desk:** fresh boot → associated to the hotspot HANDS-FREE; then
`wifi drop` → rejoined itself in ~2 s (`[autosw] conn=0` → `conn=1`, rec=1) — the exact
test that failed thirty minutes earlier with rec=0. Both phones flashed.
⚠ Two related warts, seen and deliberately NOT fixed in the same change: REMOVING a saved
network calls `disable()` (radio full-off, GUI.cpp:6122) and the Disconnect button does
too (:6135) — heavier than either action means. P2 candidates; touch them with the
settings screen in hand, not blind.

# ~~🔴 2026-08-27 MORNING — PHONE 1'S WIFI RADIO IS A HARDWARE FAILURE~~ (WRONG — above)

**The evidence chain, every link measured at {user}'s work desk with both phones side by side
on the same air:**
- Phone 2: `wifi scan` → 3 networks incl. {user}-wifi at **-60 dBm ch6**, associated, served
  the uploader flawlessly. Phone 1, same desk: **0 / 0 / -2** (-2 = the scan API itself
  failing), cannot associate, and `up on books`'s softAP fallback **wedged the chip silent**.
- Ruled out one at a time: heap (fresh boots at largest 20-26 K, still deaf), the air
  (phone 2), firmware (identical 0.9.28), **RF calibration** (`wifi calreset` — new serial
  command — erased it, PHY recalibrated, still -2), **WiFi driver NVS state** (`wifi
  restore` — new serial command, esp_wifi_restore() — still -2).
- History fits a DEGRADING radio, not a config: weeks of "phone 1 keeps losing association"
  (the old downclock hunt), 9-for-9 clean on a router days ago, 52 MB of batches served
  fine last night, dead by morning.
🛑 **RETRACTION (mine, from last night's notes): "reboot cures the deaf radio" and "heap
fragmentation causes it" are both WRONG** — today it boots deaf with a pristine heap. The
post-batch fragmentation tail (largest 3-5 K for minutes) was REAL and measured, but its
blamed consequences (SIP no-register, the deaf episode) are now confounded by the dying
radio — treat them as unattributed until phone 2 shows or doesn't show the same.
**Consequences:** phone 1 = the SIP phone, and SIP rides WiFi → **phone 1 cannot make calls
until the radio is fixed.** Mesh (LoRa), screen, SD, serial all fine. Hardware options are
{user}'s call: reflow/reseat or replace the ESP32 module, inspect the antenna matching
network, or swap roles and load the SIP account on phone 2 (`data/configs.ini`).

✅ **AND THE REAL-BROWSER ACCEPTANCE RUN IS CLOSED — on phone 2, over the Android hotspot
(the original crash-day class of link), driven in {user}'s actual Chrome via the extension:**
the page loaded from the raw front door (`RAWPAGE=1`), its own JS uploaded 256 KB + 2 MB +
512 KB **byte-verified by held-count**, a zero-byte file was skipped and said so, label
"fast chunked transport", **zero breaker events all session**. 🔑 **The run caught a real
bug in one shot** — the thing the bench pushers were structurally blind to: the RAWPAGE
branch set the piece size but left `BASE=''`, so `piece()`'s `if(BASE)` routed the raw-served
page into the MULTIPART branch, which the raw server refuses (413: multipart overhead pushes
a 16 K piece past the cap). Fixed (`RAW = BASE||window.RAWPAGE`), reflashed, re-proven.
⏳ Still worth one tap from {user}'s Android sometime: same engine as desktop Chrome, so this
is a formality now, not a gate.

⚠ **HONEST FINDS, ALL MEASURED, STILL OPEN:**
- **The SD threw 507 (write failed) on 3 of 12 files** under sustained load; retries landed
  every byte. Watch this card.
- **After a heavy batch the internal heap stays fragmented (largest 3-5 K) for minutes** —
  measured and real. ⚠ But the consequences this bullet originally blamed on it (SIP unable
  to re-register, the deaf-radio episode, "only a reboot cured that") are **RETRACTED as
  attributions** — see the 08-27 morning section: phone 1's radio was dying, and it confounds
  every WiFi symptom from that night. The `everRegistered=0`-without-`going to init` puzzle
  stands unanswered. Re-observe the fragmentation tail's effects on PHONE 2 before believing
  any of it again.
- The remaining ~1-in-12 page-GET failure under rapid-fire curl is the single-slot takeover
  race; real browsers (2-3 connections) should not see it. ⏳ **The real-browser acceptance
  run is STILL OWED** — attempted overnight but blocked: the sandboxed browser refuses
  plain-HTTP LAN pages and Chrome's extension was signed out. {user} opened the page in Chrome
  on the morning of 08-27; whether an upload landed is in the serial log after this write.
- 🛑 Cosmetic: the raw-served page's status line says "compatible chunked" where it should
  say "fast" (the label keys on BASE, which is '' same-origin). One-word JS fix, not yet made.

🛠 **BENCH FACTS THAT CHANGED:** on phone 1's adapter, **opening the serial port does NOT
reset the phone** (uptime survived dozens of opens) — reset needs an explicit **RTS pulse**
(`setDTR(False); setRTS(True); sleep(0.2); setRTS(False)` — 2 s, vs a 48 s reflash).
⚠ panicwatch instances SURVIVE a plain `pkill -f panicwatch.py` often enough to matter, and
a second reader on the port silently steals bytes (stale-looking logs, identical repeated
HEALTH lines) — `kill -9` + `lsof` the port before trusting a capture. panicwatch stamps its
log in UTC.

🔴 **THE ONE THING OWED THAT NEEDS {user}'S HANDS:** bench the SIP re-init (0.9.26). With a real
account, **drop WiFi mid-call, restore it, and watch for `SIP is going to init`** followed by
registration returning rather than flapping. `WiPhone.ino:3202` was `==` where `=` was meant
since the initial commit, so that path had **never once run**; it is the only change of
2026-08-26 not proven on hardware, and reverting it is one line.

⚠ **THE DAY'S METHOD LESSON, because it cost hours: THREE TIMES THE INSTRUMENT WAS THE FAULT,
NOT THE PHONE.** `grep` silently suppressing matches in a binary-ish capture (use `-a`);
**every serial port open RESETS the phone**, restarting the very association race being
measured (use ONE long capture, not repeated probes); and the **Mac entering Idle Sleep
mid-capture**, which read exactly like a 16-minute firmware wedge (run long captures under
`caffeinate -i -s`). And a 2-vs-2 A/B on an INTERMITTENT fault produced a confident wrong root
cause that had to be retracted in the repo. **Count the runs before naming a cause.**

### 🔴 CLOSED 2026-08-26 — A SWEEP FOR UNSWEPT FIXES FOUND FIVE (0.9.25). READ THE FIRST ONE.

🛑 **EVERY TIMESTAMP THIS PHONE PUT ON THE AIR WAS EIGHT HOURS WRONG.** `getExactUnixTime()` is
`getExactUtcTime() + timeOffsetSeconds`, and `WiPhone.ino:1479` applies `[time] zone` at boot.
That trap was found and fixed on the REPLAY path — with a comment saying so — and **never swept
to the other seven callers**, every one of which stamps or compares a value that meets COVEY's
real UTC: the **GPS position beacon** (COVEY renders `time.time() - ts`, so every WiPhone
position on its map read 8 h stale), booksync `turnedAt` ×3 (so **anything COVEY turned in the
last ~7.9 h was painted "(their clock looks wrong)"**), waypoint expiry ×2, and the SMS-mirror
"ten minutes of slack" which was really eight hours ten.
⚠ **THE SHIFT IS SEVEN HOURS ON THESE PHONES, NOT EIGHT.** Measured, not read off the shipped
config: `sun 47.6062,-122.3321` → `(local, UTC-7:00)`. The shipped `data/configs.ini` says
`zone=-8`, so a stock phone would see eight; {user}'s two are on PDT and see seven.

🔑 **THAT SETTLES THE "7.7-HOUR-OLD POSITION" OF 2026-08-25.** A just-taken fix would have
arrived reading 7.0 h old. It read 7.7 — so the fix was genuinely ~42 minutes stale, which is
what a phone indoors at `sats in view: 0` should look like. **That session was right about the
cause and wrong about the size: ~7 of the 7.7 hours were this bug, ~42 minutes were the sky.**

🛑 **USE-AFTER-FREE ON TWO LIVE SIP TEARDOWN PATHS.** `wifiTerminateCall()` and `rtpSilent()`
deleted every dialog and then wrote `currentCall->terminated = 1` through the freed pointer —
and `currentCall` was never NULLed anywhere, so `isBusy()` kept reading freed memory **every
superloop pass for the rest of the session**. Callers are ordinary: WiFi drop mid-call, RTP
silence. The sibling `terminateCall()` had the guard and was not copied.

🛑 **A RECEIVED MESSAGE COULD WALK OFF A 256-BYTE STACK BUFFER.** The channel-URL parser clamped
with `i + l <= len`, which wraps in 32-bit; a `0xFFFFFFFE` length passes it. Every other
protobuf walker here clamps by subtraction. ✅ Verified on the handset with the real payload
(`chan Cv7___8P`): 0 channels added, channels intact, no crash.

**Also fixed:** the GPS quality gate guarded only the transmit path, so a fix measured at **20 km
wrong** was refused for the radio and then used for every distance on your own screen; Screen
settings was a **fourth** reader of lock/dim/sleep with the old 0-defaults, and its Save
persisted them (the lock rides on sleep); `data/configs.ini` still shipped `lock_keyboard=0`, so
`uploadfs` handed out a phone that never locks.

🔑 **AND THE SWEEP CAUGHT THIS SESSION MAKING THE SAME MISTAKE TWICE.** (1) The key-0 grep guard
added this morning **would not have caught the bug it was written for** — it misses a named
constant (`ROW_INERT = 0`, exactly what Photos had), a wrapped call, and the two-argument form.
Replaced with `tests/check_menu_keys.py`, proven against all three. (2) 0.9.21 corrected the
SIP-flap myth in `tinySIP.h` and MEMORY.md **by name and never grepped this file** — two more
copies survived here, one headed "READ BEFORE RE-TRYING" and concluding "THE FLAP WAS
LOAD-BEARING", which is an instruction not to re-apply a fix that is shipped and measured.
**A fix is not done until you have grepped for its own spelling.**

- [x] ✅ **FIXED in 0.9.26** — `WiPhone.ino:3202` was `==` where `=` was meant, so the SIP
      re-init after a network-loss teardown had **never once run** since the initial commit.
      Written through `setSipState()`, not a raw `=`, so the ring-armed/missed-call bookkeeping
      is not bypassed. ⚠ **STILL OWED, AND IT IS THE ONE UNPROVEN CHANGE OF THE DAY: bench it.**
      With a real account, drop WiFi mid-call, restore, watch for `SIP is going to init` and
      registration returning rather than flapping. If it misbehaves, that one line is the whole
      change.
- [x] ✅ **FIXED in 0.9.26** — the music pause is idempotent now
      (`sipCallActive() && musicPlayerIsPlaying()`), so the latch cannot stick through a long
      `HangUp`. `sipCallActive()` itself was deliberately left alone.
- [x] ✅ **CLOSED 2026-08-26 — THE WIFI DROPS WERE THE ACCESS POINT, NOT THE PHONE. THE
      DOWNCLOCK IS EXONERATED.**

      🛑 **AN EARLIER COMMIT TODAY (`3535a85`) CLAIMED "the idle downclock is what kills the
      WiFi association". THAT WAS WRONG AND IS WITHDRAWN.** It rested on a 2-vs-2 A/B of an
      INTERMITTENT fault, which is exactly how a small sample lies.

      **What the full data set says.** All the failures happened while the phone was on an
      **iPhone personal hotspot**; everything after {user} got home was on a **standard router**:

      | AP | idle clock | runs | result |
      |---|---|---|---|
      | hotspot | 80 MHz | 2 | both dropped ~48 s in, then `scan done: n=0` forever |
      | router | 80 MHz | **9** (8 x 180 s trials + one 30 min) | **9 OK, downclock fired every time, `empty_scans=0`** |
      | hotspot/router | 160 / 240 MHz | 3 | OK — but every one of these was AFTER conditions changed, so they prove nothing about the clock |

      🔑 **An iPhone hotspot stops advertising when it is not actively serving a client**, and an
      incoming call disrupts it — {user} had one mid-session. So `n=0` meant *the SSID was not on
      the air*, and the phone was behaving correctly. The "wedged radio" reading was the
      investigator's, not the phone's.

      **DO NOT change the CPU idle frequency on this evidence.** 160 MHz was proposed and then
      withdrawn; the shipped 80 MHz idle is fine and is what {user} wants.

      🛠 **`wifi scan` NOW EXISTS ON THE CONSOLE**, and it is the whole lesson made permanent: a
      blocking scan that lists what the radio can hear. `scan done: n=0` has two completely
      different causes — a deaf radio or an absent AP — and nothing on the phone could tell them
      apart, which is what sent an afternoon after the wrong suspect.

      ⚠ **THREE separate times today the INSTRUMENT was the fault, not the phone:** `grep`
      silently suppressing matches in a binary-ish capture; every port open RESETTING the phone
      and restarting the association race being measured; and **the Mac going to Idle Sleep
      mid-capture** (16:36:16 -> 16:53:21 in `pmset -g log`), which read exactly like a 16-minute
      firmware wedge. **Run long captures under `caffeinate -i -s`.**

- [ ] 🔎 **AND SEPARATELY, PHONE 1 DOES NOT ALWAYS JOIN AT BOOT.** ⚠ **An earlier claim in this
      session that "NTP never syncs" was WRONG and is withdrawn** — NTP syncs fine once the
      phone associates (proved: the clock came up and `sun` printed a correct date and
      UTC-7:00). What actually happens is that some boots come up `wifi status 1`
      (`WL_NO_SSID_AVAIL`) with `conn=0` and never join, and then there is nothing for NTP to
      talk to. Seen repeatedly today alongside scans returning `n=0` while this Mac was
      associated to a 2.4 GHz AP in the same room.
      🔑 **AND THE INSTRUMENT WAS PART OF THE ILLUSION: every port open RESETS the phone, so
      each measurement restarted the association race from zero.** A phone left alone gets
      there; one probed every two minutes may never. Measure this with ONE long capture, not
      repeated short ones.
- [x] ✅ **DONE — COVEY's share of the sweep is fixed, deployed and pushed** (`5ab453b`):
      `is_protected()` was condemning every file under `/root/.covey/downloads` so **Delete and
      Move were dead in two of the four roots**; `Prefs.save()` gained the fsync every sibling
      already had; the ScrollList docstring still showed D-110 verbatim; the test recipes
      swallowed every failure; "31 suites" corrected to 47. 46/47 on each machine, covey-ui
      restarted active with NRestarts=0. Original wording:
      ~~COVEY, from the same sweep:~~ `tests/README.md` and two handoff recipes still hand out
      a test loop with no `||`, so **every failure is swallowed** — and one of them has
      `rsync ... covey:covey-ui/` on the very next line; "31 suites" survives in three places
      (it is 47, at 46/47); `widgets.py:628`'s ScrollList docstring still shows the **D-110
      crash verbatim** (`band` where a height belongs) and is that widget's only documentation.

---

### ✅ CLOSED 2026-08-26 — PHOTOS RENAME: THE LAST UNDRIVEN SCREEN, AND IT HAD A HOLE (0.9.22)

The one path in Photos nobody had ever reached. 0.9.18's note said to assume it had something
wrong in it. It did.

🛑 **THERE WAS NO WAY TO DELETE A CHARACTER.** `PHOTOS_RENAME` tested `LOGIC_BUTTON_BACK(event)`
before the event could reach the text widget — and Back IS backspace. The field opens prefilled
with the current filename, so the only possible edit was to APPEND: `BT06.JPG` → `BT06.JPGxyz`
and nothing else. **Renaming a photo was impossible in practice.** Measured before the fix: four
Back presses walked four screens OUT of the app; `key 7` / `key 2 2` typed `p` and `b` fine, so
the field worked and only the erase was missing.

⚠ **Photos was the SINGLE exception in the firmware.** `app_books.cpp:1944` and
`app_meshtastic.cpp:1372/:1396/:1419` all cancel on `WIPHONE_KEY_END` and say why, with the
convention spelled out at `app_meshtastic.cpp:198`. Fixed to match them.

✅ **THE WHOLE PATH IS NOW EXERCISED ON THE HANDSET**, every step photographed, card finished
byte-for-byte as it started: typing (`key 7`→p, `key 2 2`→b, multi-tap strip renders), `#`
toggles case (`ABC2`), `*` opens the punctuation picker, 8× Back clears the field without
exiting, Save with no extension is **refused on screen** ("Keep a .jpg/.jpeg/.bmp ending"),
`BT06.JPG` → `BT06.JPEG` → **"Renamed to BT06.JPEG"** → renamed back → **"Renamed to BT06.JPG"**.
🔑 **Both result lines are messages that could not render at all before 0.9.18** — first time
anyone has seen a Photos rename report its own outcome.

**This closes the last item on the Photos list.** ⚠ Still not driven by a thumb: nothing. The
`key`/`shot` cable path reached every screen in the app.

---

### ✅ ANSWERED 2026-08-26 — THE SIP FLAP IS FIXED, AND TWO STALE SENTENCES SAID OTHERWISE (0.9.21)

{user} remembered it being fixed. **He was right, and it is now MEASURED.** 330 s of console on
phone 1 (the phone that actually holds the account), WiFi up throughout: **8 REGISTERs, ONE
`SIP REGISTRATION -> REGISTERED`, zero "lost"** — seven refreshes in a row all logging
`registered=1`. Before `3d9f329` it read REGISTERED → lost → REGISTERED once a minute.

🛑 **WHAT COST THE TIME WAS NOT THE CODE, IT WAS TWO STALE ENGLISH SENTENCES.** `tinySIP.h:624`
still said *"THE REAL FIX, deliberately NOT shipped tonight"* — directly above a paragraph
describing that same fix as in place — and MEMORY.md said it was "left for a session with {user}
present". Both were written 08-22 and obsolete by 08-24. **Both corrected.** ⚠ When a fix lands,
grep for the comment that said it had not.

🛑 **`rtpSilent()` WAS SETTING `registered = true`.** It is reached when the OTHER PARTY's RTP
goes quiet (`WiPhone.ino:3186`) and has no business touching registration — it reads like a copy
of `wifiTerminateCall()` above it with the boolean flipped. **Cost:** `sipRegistered` gates
CallApp's END/BACK (`GUI.cpp:5575`, `:5592`), so after an RTP timeout on an UNregistered phone,
END stopped being the one press that leaves the call screen. Removed.

🔋 **AND A BATTERY BUG FOUND WHILE WATCHING THAT CONSOLE — the WiFi scan backoff was defeated by
its own retry line.** An empty scan set `_msLastScan = now - AUTO_SCAN_PERIOD_MS + RETRY`, which
means "570 s ago" — correct only while CONNECTED, because disconnected the due-check compares
against 120 s. A stamp older than the period is due IMMEDIATELY.
**MEASURED with the AP gone: 114 scans in 280 s, one every ~2.5 s, against a design of one every
two minutes.** ⚠ **An empty scan IS the out-of-range case**, so the one situation the backoff
existed for was the one it never applied to. Fixed by splitting `n == 0` (out of range → normal
backoff) from `n < 0` (aborted → retry soon), with `currentDiscPeriod()` as the single
definition both the due-check and the retry scheduler use.

- [x] ✅ **VERIFIED ON HARDWARE 2026-08-26** — {user} switched the hotspot off and the same phone
      on the same cable went from **114 scans in ~280 s** (one every ~2.5 s) to **3 in 420 s**
      (one every ~2 min, the designed rate). `sinceScan` reads `0s` after a scan now, not
      `570s`. ~38x less radio-on time out of range, and it still rejoins at once: hotspot back
      on -> `wifi: up`, `registered: yes` on the next boot.
- [ ] 🔎 **Phone 1's WiFi returned 0 networks on 112 consecutive scans** while the Mac was
      associated to a 2.4 GHz AP on channel 6 in the same room. It recovered on its own later.
      Worth one look if it recurs — a scan that sees literally nothing is not the same as
      being out of range.

---

### ✅ CLOSED 2026-08-26 — THE OTHER MENUS WERE DROPPING THEIR ROWS TOO (0.9.20)

0.9.18 fixed `addOption(text, 0, …)` in Photos and **nobody swept for it.** Eight more sites in
Books, Files and Music were still passing 0, so those rows had never once appeared: "(no books
yet)", why a book would not open, what the last booksync send did, **"Channel 'booksync':
MISSING"**, "Parked positions: N", the result of the last file operation, "(more files not
listed)", "(no music yet)", and why a track would not decode.

⚠ **`app_files.cpp:156` still carried the comment `key 0 = inert`** — the exact false belief
0.9.18 was written to kill, sitting one file over from the fix. **That is what an unswept fix
looks like, and it is the reason this entry exists.**

**FIXED** with `MenuWidget::addNote()`, so there is one correct way to write a display-only row.
The widget's refusal now names it: `menu option key is 0 - row DROPPED, not added. For a
display-only row use addNote()`. 🛑 **`MENU_ROW_NOTE` is `0xFFFFFF01` and a range test will EAT
it** — `(int)(sel - ROW_FIRST)` underflows to a NEGATIVE index that sails through
`idx < entryCount` and reads off the front of the array, so Files, Books and Music each reject
it explicitly *before* their range test. A `tests/run_tests.sh` source guard now fails the suite
on any new `addOption(..., 0, ...)` — a grep, not a unit test, because the wrong spelling
compiles, links, runs and shows nothing.

✅ **VERIFIED ON THE HANDSET:** marking a file for copy in Files shows **"Copy marked - open a
folder an.."** as its first row — a line that had never rendered. OK pressed on that row three
times: nothing, no crash, screen unchanged.

✅ **AND THE CHAT-HISTORY ROUND TRIP THAT 0.9.19 OWED IS NOW DONE**, with {user}'s go-ahead for
one transmission: phone 1 `send 2 persist-check 0826` → `MESH RECEIPT: … -> in mesh`, msgs
26→27, card file 9272→9680 B; **phone 1 rebooted and the message is still in the thread**; and
**phone 2 received it over the air and it survived phone 2's reboot too** (`loaded from SD at
boot`, 1 msg, 1704→2032 B). The incoming direction is the one that matters — phone 2 is the
phone that had been losing everything.

✅ **BOTH PHONES ARE ON 0.9.26**, `built Aug 26 2026 08:03:20`, read back with `ver` on each
port rather than assumed from a successful upload.

- [ ] ⚠ **The audit that found the unswept sites lost one of its 14 readers** to an API error,
      so about a quarter of the 08-25 evening transcript was never read. Treat that sweep's
      findings as a FLOOR. The re-runnable script is
      `.claude/…/workflows/scripts/handoff-truth-audit-wf_df33ea35-806.js`.

---

### ✅ CLOSED 2026-08-26 — THE CHAT HISTORY WENT TO THE CARD AND WAS READ BACK FROM FLASH (0.9.19)

{user}: *"after rebooting I lose the chat history in meshtastic chats on the wiphone's."*

🔑 **`meshFs()` ANSWERED "NO CARD" FOR THE WHOLE OF `setup()`.** It reads `s_meshCardIn`, which
was assigned in exactly one place — `MeshtasticService::loop()` — and `loadDb()` /
`loadFavourites()` run in `setup()`, before any loop pass. So the load always got the initial
`false` and read **SPIFFS**, while every save from the first loop pass on wrote the **SD card**.
⚠ And `loadDb()`'s "fallback" was `meshFs()` then `SPIFFS` — **two spellings of the same
filesystem**, so the card's database was unreachable at boot whatever was in it.

**MEASURED over the cable before anything was changed:** `/meshdb.bin` was **9272 bytes on
phone 1's card** and **6352 on phone 2's** (~26 and ~14 messages), while phone 2's chat list
came up with **no messages in any of its five conversations**. Two file sizes side by side were
the whole diagnosis.

🛑 **AND EACH REBOOT ATE MORE OF IT.** The save writes RAM over the card unconditionally, so a
phone that loaded nothing saved nothing over a good file. **Phone 2's stored history is GONE** —
three diagnostic boots took its card database from 6352 bytes to 1704 (20 nodes, zero messages)
before the cause was found. Phone 1 survived only because its SPIFFS copy happened to be nearly
as fresh as its card copy. Not recoverable; recorded so nobody hunts for it.

**FIXED:** `setCardPresent()` is the single writer of that flag and `WiPhone.ino` calls it
**before** `meshService.setup()`; both load paths now name SD and SPIFFS explicitly.
**Retention is now a decision rather than an accident**, at {user}'s request: `MESH_PERSIST_*` —
**40 per chat / 200 total on SD, 12 / 60 on SPIFFS** (the split is the measured 50x speed
difference; a save blocks keypad, screen and WiFi together). In-session scrollback is unchanged
at 150. The rule lives in `mesh_retain.cpp`, host-tested by `tests/test_retain.cpp` (15 checks).

🛠 **NEW INSTRUMENT — `meshdb` on the console.** Which filesystem is in use, which one the boot
actually read, what is in RAM, what the next save keeps, and **the size of `/meshdb.bin` on both
filesystems**. ⚠ The old load line was `log_i`, which this build's log level DROPS — so the one
line that would have shown the wrong file being read never reached the cable anyone was
watching. It is `log_e` now and names the filesystem.

✅ **VERIFIED ON THE HANDSET (both flashed 2026-08-26):** phone 1 boots `loaded from SD at boot`
with 26 msgs where the old build had 3 in LongFast; a node starred over the cable **survived a
reboot** (the favourites file has the same fix); `meshdb` reports the same 26 across successive
reboots.

- [ ] ⚠ **STILL OWED: the round trip with a NEW message.** Everything above proves the *load*
      path and proves a *star* surviving. Watching a freshly received text survive a reboot
      needs one transmission on a private channel — ask {user} before putting anything on the
      air, then: `send <i> persist-check`, wait 15 s, reopen the port, look at the thread.
- [ ] **Phone 2 has an empty history to refill.** Its chats will stay blank until it hears
      something; that is now expected, not a fault.

---

### 🔴 P0 — PHONE 2 AND THE WOODS-PLATE GPS: ONE BUG FOUND AND FIXED, A SECOND STILL OPEN

✅ **BUG 1 — THE BOOT-LOOP PANIC — IS ROOT-CAUSED AND FIXED.** 🔑 **The plate's GPS starts
transmitting the moment it has POWER; it does not wait for the firmware.** So when `setup()`
called `userSerial.begin(115200, ...)` it was attaching the UART RX interrupt to an ALREADY
SATURATED line, and the ISR fired inside the attach's own critical section:

```
Guru Meditation Error: Core 1 panic'ed (Interrupt wdt timeout on CPU1)
setup() WiPhone.ino:1289 -> HardwareSerial::begin -> uartBegin -> uartAttachRx
   -> uartEnableInterrupt -> esp_intr_alloc -> vTaskExitCritical -> _xt_lowint1 -> _uart_isr
```

It then rebooted into `setup()` and did it again — 🛑 **a BOOT LOOP, which presents as a dark
screen and dead buttons, i.e. exactly "it won't respond to button presses".** Reproduced on
**2 of 3 boots** with a byte-identical backtrace. ⚠ **The second backtrace in that dump
(`esp_pm_impl_waiti` / `prvIdleTask`, `EXCCAUSE 0x6`) is the OTHER core's idle task and is
MEANINGLESS** — it cost an hour and nearly sent this hunt after a divide-by-zero that does not
exist. On an INT_WDT dump, read the FIRST backtrace.

**The fix (committed):** open the port at `USER_SERIAL_BAUD` always, and retune to `gGpsBaud`
from `loop()`'s first pass. The line is still live at 9600, but interrupts arrive ~12x less
often and that attach has always survived — it is what every `gpsen=false` boot already did.
The retune calls `updateBaudRate()`, which never re-enters `esp_intr_alloc`, so it cannot
reproduce the fault. 🔑 **That is also the mechanism behind the asymmetry that led here:
toggling `gps on` by hand always worked, booting with it on did not.** The old comment at that
call said opening at USER_SERIAL_BAUD and retuning "would work too" but dirtied the reader's
counters — `gGpsReader.reset()` at the retune keeps them clean, so nothing is lost.
**VERIFIED:** with the fix flashed, boots are clean, no Guru, `retuned from loop()` in the log,
and a GPS fix arrives normally.

✅ **BUG 2 — THE IN-USE WEDGE — IS ALSO ROOT-CAUSED AND FIXED (2026-08-25).**

🔑 **IT IS A DEADLOCK IN THE ARDUINO CORE, REACHED BY ARITHMETIC.** Every `HardwareSerial`
registers `uart_on_apb_change()` (`esp32-hal-uart.c:225`). On `APB_BEFORE_CHANGE` that callback
switches the UART RX interrupt OFF and drains the hardware FIFO into the 256-byte RX queue using
`xQueueSend(..., 1)` — a **one-tick block**, and `CONFIG_FREERTOS_HZ` is 1000, so **1 ms per
byte** once that queue is full. The queue's only consumer is the main loop, which is at that
moment *inside the callback*. Nothing can free a slot.

```
115200 -> 11.52 bytes arrive per ms, 1 drains per ms -> +10.5/ms, exit condition UNREACHABLE
  9600 ->  0.96 bytes arrive per ms, 1 drains per ms -> converges, exits, always has
```
🛑 **So ANY `setCpuFrequencyMhz()` call parks the loop task forever while the plate's GPS is
streaming.** ⚠ And `esp32-hal-cpu.c:190-196` fires that callback on ANY frequency change, even
80<->240 where APB does not actually move — the guard there covers only the register write.

**MEASURED, and it is what finally identified it:** subscribing the loop task to the task
watchdog (it never was — see below) reported `loopTask (CPU 1)` stalled with **`CPU 1: IDLE1`**.
The loop **BLOCKS, it does not spin** — which killed every "the UART storm starves the loop"
theory at once. No watchdog had ever fired because only core 0's idle task is checked.

⚠ **Why it read as "mostly on screen unlock":** the wake REPAINT (~30.7 ms of SPI) runs BEFORE
the clock is raised, so ~354 bytes pile into a 256-byte queue — over threshold every time.
Screen-OFF is preceded by a backlight write, not a repaint, so that pass is short and usually
survives. That is why the downclock kept surviving in tests and sent the hunt after the wrong
transition.

🔑 **THE PREDICTION THAT CONFIRMED IT, testable against logs already captured:** the wedge lands
*between* `setCpuFrequencyMhz()` and the `CPU %luMHz` log line that follows it, so **a wedged log
never contains that line.** Checked against 5 wedge captures: **5 of 5 have no `CPU nnnMHz` line
at all**, while every surviving run prints one.

**THE FIX:** `gGpsNmea` is now a term in the `busy` (and `hardBusy`) predicate, so the clock is
pinned at 240 MHz whenever the GPS owns the UART and the transition never happens. Costs idle
power while GPS is on; buys a phone that answers its buttons. **VERIFIED: 380 s with GPS enabled
at boot, healthy throughout, where the same build died at ~40 s before.**

⚠ **The SD/smsmirror lines were a RED HERRING** — they are simply what logs shortly before the
clock gate evaluates. Good thing that was hedged rather than fixed.

✅ **ALSO ADDED, AND WORTH KEEPING: the loop task is now subscribed to the task watchdog**
(20 s, print-not-panic). It never was, which is exactly why a permanent stall was silent — and
note the existing `LOOP STALL` detector CANNOT see this class of fault, because it measures the
gap between two passes and so only reports a stall the loop RECOVERED from.

---

#### (superseded) Bug 2 as it stood before the cause was found With bug 1 fixed the phone boots and
runs, then **silently stops ~30-40 s in, with GPS on.** ⚠ **It is a DIFFERENT SHAPE from bug 1:
no panic, no backtrace, no reboot — the loop task simply stops** while other tasks keep
printing. Not "interrupts off too long"; something blocks and never returns.

Where it stops, on 3 of 3 wedges, is the last two lines logged:
```
[E][vfs_api.cpp:64] open(): /sd/smsmirror.txt does not exist
[E][vfs_api.cpp:64] open(): /sd/roms/smsmirror.txt does not exist
```
That is `smsMirrorLoadConfig()`'s 30-second retry (`sms_mirror_poll.cpp:102`) re-reading a file
that never exists. ⚠ **BE CAREFUL WITH THIS CLUE:** those lines come from the VFS layer, not
from the poller (which logs only on its FIRST miss), and the loop plainly continues past them —
so this is *where logging stops*, NOT proof that `SD.open()` is what hangs. It is always the
SECOND retry, never the first. GPS off survives these same polls indefinitely.

Next step is instrumentation, not more staring: a breadcrumb of where the loop last was that
survives the hang (the existing `LOOP STALL` detector at `WiPhone.ino:2105` only fires if the
loop RESUMES, so it cannot see a permanent stop), and subscribing the loop task to the task
watchdog so a stall REBOOTS with a decodable reason instead of hanging silently.

---

#### Original P0 notes (kept — the symptom description and the ruled-out theories)

{user}, 2026-08-25, from work: *"was working good but after I turned on the gps it wanted to lock
up every so often mostly on screen unlock… now its screen is off and it won't respond to button
presses."*

🔑 **WHAT THE WEDGE ACTUALLY IS, MEASURED — the main loop stops while the phone stays powered.**
It is NOT a crash-reboot and NOT a dead chip. In the wedged state:
  * **zero `HEALTH` / `KEYS` / `DROP` lines** — those come from `loop()`, and `loop()` has stopped;
  * `[E][Networks.cpp:262] resolveDomain()` errors **keep arriving**, because that is a separate
    FreeRTOS task and it is still running — 🛑 **so "the phone is still printing" does NOT mean
    the phone is alive**, and this is the trap that will fool the next person;
  * serial commands are typed but never answered (the reader is in `loop()`);
  * `esptool` talks to it perfectly — ⚠ **and that proves nothing**, because it resets into the
    ROM bootloader. `ESP32-D0WDQ6-V3 rev 3, MAC 4c:eb:d6:44:93:34` was read out of a phone that
    was, as far as the user was concerned, dead.
  * ⚠ The `/dev/cu.usbserial-*` node proves nothing either — that is the **CP2104**, which is
    USB-powered and enumerates whether or not the ESP32 is running.

⚠ **One boot came up `reset_reason=5` = INT_WDT**, so at least one wedge ended in an interrupt
watchdog reset — "something blocked with interrupts off". The panic dump that came with it
decodes to `esp_pm_impl_waiti` / `prvIdleTask` and its `EXCCAUSE 0x6` is **meaningless**: an
INT_WDT dump shows the *interrupted* context, which was the idle task. 🛑 **Do not go hunting a
divide-by-zero. There isn't one.** (Both divisions near the GPS fix log are guarded, and
`sats=-1 hdop=-1.0` is cosmetic — `gpsSats` only updates when a GGA supplies a count.)

✅ **WORKAROUND, APPLIED AND PERSISTED: `gps off`.** Sent over serial; it writes
`wpmesh/gpsen=false`, so it survives a reboot — confirmed by rebooting and seeing no
`NMEA reader ON` line. The phone then ran healthily through the screen-off transition and the
240→80 MHz downclock, `up=3min` and counting, `heap=24300 largest=21076` flat.

🛑 **THE CAUSE IS NOT FOUND, AND ONE PLAUSIBLE STORY WAS TESTED AND KILLED.** The obvious
suspect was the PLL switch: `setCpuFrequencyMhz()` 240→80 at screen-off, with GPS holding the
shared user UART at **115200** where the GUI path uses 9600 — and `WiPhone.ino:3715` already
records that switch breaking the *input* path on 2026-08-22 (*"The level was never the problem;
the SWITCHING was"*). **It does not reproduce.** With GPS turned on AFTER boot, the phone sailed
through `CPU 80MHz (idle)` at t=40.8 s and stayed healthy for 135 s. So the downclock alone is
not it.

What is left, in the order worth testing:
  * ⚠ **GPS on AT BOOT vs toggled later** — every wedge so far booted with `gpsen=true`
    (`userSerial.begin(gGpsBaud…)` in `setup()`); the run that survived called
    `updateBaudRate()` on an already-begun UART. That asymmetry is real and untested.
  * ⚠ **Plain intermittency** — {user} said "every so often". 135 s of survival proves little.
  * ⚠ **Screen UNLOCK specifically**, which is where he saw it and which serial cannot exercise.
    This needs a human with a thumb.

⚠ **`panicwatch` was suspected and CLEARED**: the wedge began minutes after it attached, but a
control run with panicwatch attached and GPS off gave 6 `HEALTH` lines in 100 s. The instrument
is not the disease.

- [ ] **Leave GPS off until this is understood.** Turning it back on is one serial command, and
      it is how you reproduce the bug — do that deliberately, with `tools/panicwatch.py 025A3F65`
      already attached, not by accident.
- [ ] **Next capture wants the `Guru Meditation` HEADER line**, which was filtered out of the
      first capture and is the one thing that would name the panic class outright.
- [ ] ⚠ **Unrelated but visible throughout: `resolveDomain(): errno=210` for `pool.ntp.org` on
      every boot.** WiFi associates (`wifi=1`) and DNS does not resolve. Not the wedge, but it is
      the same shape as the fault Jake documented on his handheld, and the clock depends on it.

### ✅ CLOSED 2026-08-25 — THE WALLPAPER, AND THE INSTRUMENT THAT FOUND IT (0.9.13)

{user}: *"I tried to apply a background from a photo but nothing changed."* **Fixed, flashed to
BOTH phones, and verified by screenshot.**

🛑 **`gui.init()` READ THE WALLPAPER ~50 LINES BEFORE `SD.begin()` MOUNTED THE CARD.** Photos
writes `/background.jpg` to the SD card; the loader asked an unmounted filesystem and got
`false`, on every boot since the feature existed. The fingerprint was in every boot log —
`[E][vfs_api.cpp:72] exists(): File system is not mounted`. 🔑 **Phone 2's card still had
`/background.jpg` at 31,440 bytes — the exact size of `BT09.JPG`.** {user}'s copy had always
worked; nothing ever read it. ⚠ **Do NOT "fix" this by moving `SD.begin()` earlier** — it
shares SPI with the screen and that order is deliberate. `GUI::loadWallpaper()` is called
again *after* the mount instead.

🔑 **WHY IT LOOKED LIKE NOTHING AT ALL:** a rejected override falls into the SAME path as "none
chosen", and this phone ships a `/background.jpg` **in SPIFFS** — the dark texture everyone
reads as the default. Something always loaded. Just never the user's picture.

Also closed with it: **two different size ceilings on one photo** (viewer 2 MB, loader 1 MB —
a photo in between viewed, said "Wallpaper set", and vanished at the next boot; both are 2 MB
now); **setAsWallpaper() now verifies** by asking the loader and rolling back on refusal;
**wallpapers FILL the screen** (TJpgDec only halves, so every 480x270 photo on the card was a
240x135 band across the top — now resampled to cover, cropped to centre).

🔬 **NEW INSTRUMENT, AND IT IS THE POINT: `shot` + `tools/shot.py`** — the live frame as a PNG
on the host. **The reason Photos shipped broken is that a cable cannot press keys**, so no GUI
claim in this repo was ever checked. Now it can be:

```bash
tools/shot.py /dev/cu.usbserial-025A3F65 out.png --wait 16 --cmd "wallpaper set BT01.JPG"
```

Plus `wallpaper` on the console (what the loader found, from where, at what pixel size, and why
it refused) and `wallpaper reload | list | clear | set <name>` — `set` runs **the same
`photosSetWallpaper()` the menu runs**, so the path is provable over the cable.

- [ ] ⚠ **`/photos/20260823_093939.jpg` on phone 2 is 0 bytes** — a failed upload. Photos lists
      it and cannot open it. Small, and its own job.

### ✅ CLOSED 2026-08-25 — "PHONE 2 NEVER LOCKS" (0.9.16). IT WAS LOCKING; THE FIRST KEY UNDID IT

{user}: *"whiphone two doesn't like its screen to lock ever… I just don't want to be accidentally
pushing buttons while it is in a bag."* **Root-caused, fixed, both phones flashed, before/after
proven with the same script.**

🔑 **`GUI::inCall()` said "yes" for TEARDOWN.** It tested `not NotInited/Idle/Error`, which is
true for `HangUp`, `HangingUp`, `HungUp` and `Decline`. Its one caller is the unlock path, which
reads that as "a call is coming in, let any key answer it" and clears `locked`.

🛑 **One press of END unlocks a locked phone — on BOTH phones.** END sets `HangUp` from anywhere
with no call needed, *before* the same press reaches the unlock check. 🛑 **And with a SIP
account, EVERY key unlocks for as long as SIP sits in teardown** — measured in this repo at 19
min and SIX HOURS at `sip=6` when the proxy is unreachable. **That is the difference between the two
phones: PHONE 1 has the account (registered to voip.ms), PHONE 2 has none.**
⚠ **CORRECTED 2026-08-26 — this sentence had the phones the wrong way round, and line ~908 of
this same file always had it right.** Measured over the cable, `sip` on each:
`025A3EAF` (phone 1) → `account LOADED (sip:100000_sample@seattle1.voip.ms)`;
`025A3F65` (phone 2) → `account NOT LOADED`. The mechanism above is unchanged; only the label
on the phone was wrong.

Fixed by making `inCall()` mean live-or-ringing, using the SAME set `sipNeedsFullSpeed()` and
the WiFi auto-switch gate already use — ⚠ **the third guard here to need this correction**.
Answering a ringing phone on any key still works (`BeingInvited` is in the set).

⚠ **`lock` on the console is the new diagnostic** and it prints the SLEEP gate on purpose: the
lock is not its own timer, it fires inside `SCREEN_SLEEP_EVENT`, so a phone that dims but never
sleeps never locks either — two causes, one symptom.

- [ ] ⚠ **Loaded, not fired:** `lock_keyboard` defaults to **0** when a `[lock]` section exists
      without the key, while a missing SECTION defaults to 1 (`WiPhone.ino`). Same disagreement
      that shipped a phone which never dimmed or slept (2026-08-22 audit). Both phones read 1
      today, so it was not this bug — but it is a live landmine.
- [ ] 🔎 **Not verified by machine: answering a REAL incoming call from the lock screen.** The
      set is the same one two other guards use and `BeingInvited` is in it, but no call has been
      placed to a locked phone. Fold this into the existing "ring the phone" task below.

### ✅ CLOSED 2026-08-25 — MENU CONTRAST OVER A WALLPAPER (0.9.14)

{user}, right after the wallpaper fix: *"the menus have no contrast… can the menu items have a
translucent grey background so I can see the words no matter what background I pick?"*
**Done, both phones flashed, before/after screenshotted from one build.**

🔑 **It got worse the moment the wallpaper started working.** Every phone had been showing the
same near-black SPIFFS texture, and white text sits on that perfectly. The main menu is the ONE
menu built non-opaque (the `false` ending its `MenuWidget` constructor, GUI.cpp) so the picture
shows through — and a real photograph behind it made four of five rows unreadable.

Fixed with a **scrim**: `guiDrawScrim()` lays a translucent grey plate under each non-selected
row via the sprite's existing `pushTransparent()`. ⚠ **Checked as a number, not by eye** — grey
56 at alpha 190 puts the plate between 41.7 (black photo) and 106.7 (white photo), so white
text measures **5.35:1 at worst**, above the 4.5:1 floor for *any* wallpaper. Tune it live with
`scrim <alpha> [hex]` (RAM only; `scrim 0` is the old look).

🔬 **NEW INSTRUMENT: `key` — the cable can press buttons.** With `shot` (0.9.13) this closes the
loop: any screen can now be reached AND seen without a thumb.

```bash
tools/shot.py /dev/cu.usbserial-025A3F65 menu.png --wait 16 --cmd "key menu" --cmd "key down down"
```

⚠ It injects into `keypadBuff`, so it is a REAL press — the wake, drain loop and each app's
`processEvent` run unchanged. Do not add a second dispatch path.

- [x] ✅ **THE CLOCK FACE TOO — ASKED, AND {user} CHOSE THE SCRIM (0.9.15).** `00:00`, the
      date/network line, the missed-call line and the softkey label each get a plate sized to
      the WORDS, not a panel across the screen. 🛑 **One union plate for the clock and the line
      under it** — their bands overlap, and two translucent plates over the same pixel blend
      TWICE and paint a dark seam right where the eye lands. Measured after the fact by reading
      the screenshot back: smooth from y=68 to y=182, median plate 105 → **5.47:1** behind the
      network line, 79 → **8.21:1** behind the softkey.
- [ ] ⚠ **Cosmetic, pre-existing, NOT fixed:** the softkey label's font is whatever the
      missed-call block left set — 20pt with a missed call showing, 24pt without. The scrim
      measures from the current font so it follows either way, but the label itself changes
      size. Its own small job.

### ✅ CLOSED 2026-08-25 — GPS BEACONING: THE CHAIN WAS FINE, THE SLOT WAS BEING WASTED (0.9.17)

{user}: *"covey isn't seeing wiphone 2 on the map."* 🔑 **Nothing was broken.** The RAK had a
real, full-precision position from `WiPhone-{user}2` `!00449334` matching {user}'s RAK and Meteor
Pro to four decimals — **7.7 hours old**, while the node itself was heard every minute. The
phone was indoors: `sats in view: 0`, raw NMEA showing satellites with elevation/azimuth and a
**blank SNR** (knows where to look, hears nothing). Config was correct throughout
(`reporting every 300s to 'hunt-group'`).

Two real improvements came out of it:

- 🔑 **A SLOT IS NOW OWED, NOT SPENT.** Interval 300 s vs `MESH_POS_TX_FRESH_MS` 30 s: the old
  code sent-or-lost in one instant, so under canopy — fix coming and going — a phone that had a
  good fix 40 s ago stayed silent for another five minutes. `posDue` holds the slot open and
  fires on the next fresh fix. ⚠ **Freshness is NOT relaxed**; nothing older than 30 s ever goes
  out. ⚠ One slot max — a flag, not a counter. **Proven on the handset:** nothing owed at t+0,
  `A SLOT IS OWED` after one interval with no fix.
- 🛑 **FIX-QUALITY GATE, and it is measured.** `sats=3 hdop=6.4` gave a position **~20 km** from
  the phone's real location and nothing refused it. Three satellites is a **2D fix** — it assumes
  an altitude and puts the error sideways. `MESH_POS_MIN_SATS=4` (the arithmetic minimum for 3D,
  not a strict bar), `MESH_POS_MAX_HDOP_X10=100` as a backstop. ⚠ −1 = unknown = allowed;
  refusing on silence would break receivers that emit no GGA. Pure `meshPosFixUsable()` in
  `mesh_pos.cpp` with host tests, including the 20 km fix as a named check.

⚠ **METHOD NOTE THAT COST A WRONG "GREEN" TODAY:** grepping `run_tests.sh` output for
`passed|failed` matched a DIFFERENT suite's summary line while `test_pos` was reporting
`1 FAILURE(S)`. **Check the exit code.** `bash tests/run_tests.sh >/dev/null 2>&1; echo $?`

- [x] ✅ **DONE — both phones are on 0.9.20** (`built Aug 26 2026 08:03:20`), read back with
      `ver` on each port, so this is not an assumption about what was flashed. ⚠ The 0-byte
      photo this item mentions no longer exists — it was deleted through the Photos UI on
      2026-08-25. Original wording kept below.
      ~~WiPhone 1 is still on 0.9.16 — it was unplugged for this build.~~
- [ ] 🔎 **The beacon has never been watched end-to-end with a real fix.** Take phone 2 outside,
      then read COVEY's `pos.time` for `!00449334` and confirm it moves.

### 🔴 P1 — TWO THINGS NEED A HUMAN, AND ONE OF THEM IS UNTESTED CODE

⚠ **The wallpaper half of this is now DONE and machine-verified** — see the block above. What
is left below still needs a thumb, but `shot` means you can now SEE the result of every press.

- [x] ✅ **THE PHOTOS APP HAS NOW BEEN RUN — 2026-08-25, over the cable, and it works.** `key` +
      `shot` drove it end to end on phone 1 (Menu > Tools > Photos) and every screenshot is
      clean: the **LIST** (14 files with sizes and the restore-wallpaper row), the **VIEWER**
      (BT01.JPG renders scaled to fit, 2/14), RIGHT walking the folder (⚠ only `key right` was ever sent — LEFT is a separate branch and is still unpressed), and the **OPTIONS**
      menu (Set as wallpaper / Lock / Rename / Delete / Cancel). The empty-file message added in
      0.9.17 was verified on the real 0-byte photo: *"This file is empty (0 bytes) / the upload
      did not finish"*, with Options still reachable so it can still be deleted. **This closes
      the "nobody has ever run it" half of the item.**
### 🛑 CLOSED 2026-08-25 — THE PHOTOS APP COULD NEVER SHOW A MESSAGE (0.9.18)

`ROW_INERT = 0`, and **`MenuWidget::addOption()` refuses a key of 0** — it logs
`menu option key is 0` and adds no row. So the list's result line has **never once appeared**:
"Deleted", "Renamed to X", "Wallpaper set", "Not set - <reason>", "X is locked", the truncation
warning. The constant said `key 0 = not selectable` and did the opposite.

⚠ **IT QUIETLY UNDID THE SAME DAY'S 0.9.13 WALLPAPER WORK** — `setAsWallpaper()` had just been
rewritten to report what the loader actually said, through this exact line. It was only ever
visible over the cable, **which is why the gap survived being "verified". A fix checked through
one channel is not checked.** Fixed with real keys (`ROW_NOTE`/`ROW_TRUNCATED`); the widget is
untouched because other menus may rely on it rejecting 0.

- [x] ✅ **DELETE CONFIRMATION EXERCISED** (2026-08-25, deleting the 0-byte photo at {user}'s
      request). `Cancel` is first and selected by default, so a mis-timed OK destroys nothing.
      Before/after listing: exactly one file gone, the other thirteen byte-identical.
- [x] ✅ **DONE 2026-08-26 — the rename text-entry screen is fully exercised**, and it had a
      real bug (no backspace; see the 0.9.22 block at the top). Driven with `key`/`shot`.
- [ ] ~~🔑 **OPEN THE PHOTOS APP (Menu > Tools > Photos). NOBODY HAS EVER RUN IT.**~~ It is new
      today: it builds, registers, boots clean and passes the duplicate-menu-id check — but
      **every screen in it needs a key press and serial cannot press keys.** The starring
      feature shipped with exactly this gap the same morning and it had a real bug (a
      filesystem write on the key path, `reset_reason=4`). Assume this one has something too.
      First open also creates `/photos`; put a JPEG in there (Files > /photos >
      "[ Upload into this folder ]") and try view / rename / lock / delete / set-as-wallpaper.
- [ ] **Ring the phone and confirm an INBOUND call lands.** The SIP fix is measured (zero flaps
      in four minutes, self-recovered from a real WiFi drop) but "a call actually arrives" is
      the one property the cable cannot prove. It is the last thing between that fix and done.

### ✅ Closed 2026-08-24 — do not redo these

- **The scrolling freeze is FIXED, and it was never the database.** `WiFi.disconnect(true)`
  blocks **5007 ms** (`begin()` next to it: 30 ms) — the argument is `wifioff`, so it stops the
  whole radio to reassociate. One task, so it froze keypad, screen and WiFi together. That is
  why it was rare (needs the hotspot to blip) and why it always came "with WiFi dropping" —
  **the drop was the trigger, not the symptom.** Fix: `WiFi.disconnect(false)`.
- **The mesh database lives on the SD card** (`meshFs()`, SPIFFS fallback per call). Benchmarked:
  SPIFFS 2599–2845 ms for an 8 KB save-shaped write against SD's 48–57 ms.
- **SIP registration no longer flaps**, and the ping starvation that made the 08-22 attempt fail
  is fixed too. **`chg=` is settled** — it was inverted as well as reading the wrong GPIO.
- **Node list 32 → 200 in PSRAM; nodes can be starred** (`*` on the Nodes list; starred sort to
  the top and are evicted last; the star list is its own file).
- **NEW: Photos app** — `/photos` on SD, with lock / rename / delete / set-as-wallpaper and a
  restore-default. ⚠ **CORRECTED 2026-08-26: baseline colour JPEG ONLY — not BMP, and not
  greyscale.** `TFT_eSPI::drawImage()` sniffs `RLE3`, `I256` and `FF D8 FF` and has never had a
  BMP path, and the ROM TJpgDec refuses greyscale and progressive JPEGs. The app has since been
  run on the handset; see the P1 block.

### 🛠 INSTRUMENTS THAT NOW EXIST — reach for these before theorising

- **`LOOP STALL`** — any superloop pass over 250 ms, logged at `log_e` **and into `/health.log`**
  (rate-limited to one a minute) with `scr` / `cpu` / `wifi` state. Persisting it is what cracked
  the freeze: it separated `scr=0 cpu=80` (idle, the database saves behaving) from
  `scr=65 cpu=240 wifi=6` (screen on, phone active, WiFi down) and pointed away from storage.
- **`TIME_STEP(name, call)`** in WiPhone.ino — times one named step and says so past 150 ms.
  Three successive splits with it walked 5296 ms down to the single guilty argument.
- **`bench`** (serial) — SPIFFS vs SD, 8 KB, the same shape a real save has. Re-measure rather
  than trusting these numbers on a different card.
- **`wifi drop`** (serial) — disconnect WITHOUT marking the radio user-disabled, so the field
  retry path runs on demand. A bug needing someone else's access point to misbehave cannot be
  measured otherwise.
- **`star`**, **`send <chan> <text>`**, **`health`** — the other cable-only exercisers. Anything
  reachable only by a thumb cannot be proven from here; that keeps being the lesson.
- Boot lines carry **`build=<compiler timestamp>`** so a run of samples can be attributed to a
  firmware. Without it the `chg=` question was unanswerable from four hours of data.

### ⚠ THREE WRONG ANSWERS FROM TODAY, RECORDED SO THEY ARE NOT RE-RUN

1. *"The freeze is the database save."* It was blocking ~1.5 s and it really is better on SD —
   **but it was not what {user} was feeling.** The symptom named a component and the component was
   innocent.
2. *"Batch the ~55 small writes."* No change. The write was never the cost; SPIFFS `open` is.
3. *"Chunk the write across loop passes."* **Worse — 31 stalls against 12.** SPIFFS is quick
   until a write crosses a block boundary and forces an erase, and an erase is atomic however
   small the write that triggered it. **A stall the filesystem takes in one indivisible piece
   cannot be chunked around.**

### 🖼 COVEY — ✅ SHIPPED, and this heading was stale for two days

- [x] ✅ **DONE — COVEY's photo viewer shipped (D-115…D-122), is deployed, and has been driven
      on the panel twice.** It even grew past the WiPhone's version: an oversize JPEG is
      rescued out of process by `gdk-pixbuf-thumbnailer` and labelled `shown downscaled`.
      **Read the COVEY handoff, not this heading.** What is genuinely still open there is
      Rename/Delete by finger, one real upload over port 8085, and the `.webp`/`.tif` listing
      question. Original wording kept:
      ~~A photo viewer for COVEY, same idea as the WiPhone's… only the WiPhone half is built.~~

### P2 — ⚠ WATCH FOR THIS RECURRING: your own devices getting evicted

**DMs from phone 1 to COVEY had been failing silently, for a while.** The 32-node table evicted
strictly oldest-heard, and the slot wipe takes `pubKey`/`pkiFlags` with it — so on LongFast, a
public channel full of nodes we will never DM, COVEY lost its slot to the 32nd stranger and came
back **keyless**. Keyless = DMs fall back to the pre-2.5 legacy form, which COVEY refuses
(`err=6`). The message appeared in the thread and looked sent. **The delivery receipt is what
made it visible, hours after shipping.**

Fixed: a node whose public key we know is evicted **last**. But the cap is still 32 on a public
mesh, so:

- [ ] **If DMs to COVEY ever go quiet again, run `pki` FIRST.** `NO KEY` for `!62b8d2fd` is the
      whole diagnosis.
- [ ] 🔑 **To make COVEY re-announce its key: `ssh covey` → `sudo systemctl restart covey-ui`.**
      It emits NodeInfo on start and phone 1 logs `MESH PKI: learned key ... DMs unlocked`.
      ⚠ **The phone's own `announce` is NOT reliable for this** — it drew NodeInfo from two
      strangers across three minutes and never from COVEY, because `nbr` shows **0 direct
      neighbours**: COVEY is multi-hop, and its NodeInfo replies do not survive the trip even
      though routed NAKs do.
- [ ] Consider whether 32 is the right cap now that this phone sits on LongFast.

### ✅ `chg=` IS SETTLED (2026-08-24) — it was inverted as well as mis-read

The charger IC's STAT is open-drain: LOW on the charger, released high off it. The read was
`== HIGH`, so the flag was **inverted for the life of the project**, on top of reading the wrong
GPIO until the same morning — which is what hid it. Measured, one boot, one build stamp:

```
up=20  v=4.20  chg=0     on the charger
up=21  v=4.15  chg=1     <- unplugged; voltage starts falling
up=26  v=4.18  chg=0     <- replugged; voltage jumps back
```

⚠ **`chg=1` now means "ON THE CHARGER". It does NOT yet mean "current is flowing"** — plugged in
at soc=100%/4.19 V it still reads 1, which is either a charger topping off or a pin that tracks
USB presence. Settling that needs a run left plugged until charging genuinely terminates.

⚠ **The old warning about mining historical `chg=` still stands** for anything written before the
build stamp existed: pre-fix the flag was reading GPIO 32 (`USER_SERIAL_TX`, idles high), so old
runs record whether the UART was up. Attribute samples by `build=` or do not use them.

### ✅ The SIP flap is FIXED (2026-08-24) — and the 08-22 revert was never its fault

Three changes; the important one is not the obvious one.

1. 🔑 **PING STARVATION — this is why registration could go `lost` and never come back.** The
   caller is an if/else chain: `if (ping due) ping(); else if (register due) registration();`.
   `ping()` wrote `msLastPing` only INSIDE its `connected()` branch, so with the connection down
   the stamp never advanced, the ping branch was taken on *every pass*, and **`registration()`
   was never reached again.** Only a reboot recovered it. ⚠ **That is the "20 minutes later the
   phone went `lost` and never came back" of 08-22 — and it is reachable entirely on its own.
   It has been there since the initial commit.** `ping()` stamps the ATTEMPT now.
2. **The flap:** `requestRegister()` cleared `registered` on every REGISTER, so the phone
   declared itself unregistered for one round trip per refresh, by construction.
3. **`REGISTER_PERIOD_MS` 60000 → 45000.** ⚠ **Neither 2 nor 3 works alone** — with period equal
   to expiry, the expiry check tripped at the instant the refresh fell due, which is exactly why
   the 45 s attempt on its own only moved the flap. The on-air `Expires` header is untouched, so
   a dead phone's binding still clears at the registrar within 60 s.

Unplanned live proof of (1): WiFi genuinely dropped after a reset during testing and
**registration recovered by itself** — precisely what used to require a reboot.

### P4 — hardware, parts in hand

- [ ] **Build plate v2.** ⚠ ORDER: bench ONE TPS63020 off the plate first (set its **3V3 jumper**,
      tie **PS LOW**); meter header **VBAT → GND** (must read the phone cell and TRACK it); meter
      **D1/D2 band orientation, cathodes toward VIN** (D2 backwards = 4.9 V back-charging the cell).
- [ ] **Publish the woods backplate build guide** (gated on assembly).
      📐 The sheet was geometry-audited 2026-08-23 and there is now a **v1→v2 change sheet**
      (`docs/woods-backplate-v1-to-v2.svg`) so the built plate need not be re-checked wire by wire.
      The shared artifact carries both; its source is `docs/woods-backplate-artifact.html`.

### P5 — small and unblocked

- [ ] **Tell people the `*` key exists.** Starring a node on the Nodes list is `*`, and it
      toggles both ways — but the footer there reads "Select / Back" and nothing on screen
      mentions it. The feature is fine; it is simply undiscoverable to anyone who did not build
      it. A footer hint is the whole fix.

### ❌ DECIDED AGAINST (2026-08-24): giving COVEY its own node superset

COVEY has no node store of its own — `node_list()` reads `iface.nodes` straight from the RAK,
so the list size is the radio firmware's compile-time cap and covey-ui cannot raise it. The only
way to show more would be a persistent superset: every node ever seen, merged with the live list.

**{user}'s call, and it is the right one: leave it.** A node the radio has forgotten carries
FROZEN data — its position, battery and hop count are from whenever it was last heard — and
presenting that beside live nodes makes the list *less* trustworthy, not more. "The mesh you can
see right now" is a more honest list than "everything ever heard, some of it fiction". Starring
(shipped the same day) already solved the actual problem, which was finding your own devices in
the noise of a public channel. **Do not re-propose this without a concrete staleness design.**

### P6 — setup, unblocked, quick

- [ ] **Phone 2: flash it to match phone 1**, then set booksync passcode → `2222`.
- [ ] **Phone 2 SIP**: a second free VoIP.ms sub-account + a ring group, so both phones ring.
      ⚠ Never leave both phones primary on the SAME sub-account.
- [ ] **The 2-minute Android-browser upload test**, owed since the upload redesign.

### COVEY, for whoever picks it up

- [ ] **Play-test DEEPFIELD from the Games menu** (not over SSH — different settings file, and the
      SDL focus trap only shows on the launcher path). It is installed, rotated (`screenTurn=90`,
      `controlTurn=0`, `detail=2`), and its d-pad and crosshair bugs are fixed. Confirm the ship
      steers and sound reaches the speaker.
- 📖 **`docs/37_LOVE_Games_On_COVEY.md` in the COVEY repo is the playbook** for any LÖVE game.
      A kit for Jake is in `incoming/deepfield-covey-kit/`.

### Traps worth re-reading before touching anything

- ⚠ **Flashing needs panicwatch STOPPED** — `pkill -9 -f panicwatch.py` FIRST. And beware `&&`
  chains: `grep -c` returning 0 exits non-zero and skips the pkill.
- ⚠ **`cd` into the repo explicitly.** A build "succeeded" in the wrong directory today and esptool
  then flashed nothing, because the shell cwd had been reset between calls.
- ⚠ **Only `log_e` is compiled in.** A `log_d` diagnostic makes a working feature and a broken one
  look identical.
- ⚠ **A scripted multi-edit must write per-edit or not at all.**
- ⚠ **`digitalRead`/`digitalWrite`/`pinMode` DO NOT REACH THE GPIO EXTENDER.** Half this board's
  status pins are `EXTENDER_PIN(n)` == `n + 0x40` on the SX1509, which is out of the ESP32's
  GPIO 0–39 range — and the Arduino core does not reject an out-of-range pin, it silently reads
  a DIFFERENT one (`gpio_get_level()` masks its shift to 5 bits, so pin 64 reads GPIO 32).
  **Always `allDigitalRead` / `allDigitalWrite` / `allPinMode`.** This cost two dead status flags
  for the life of the project. Audited clean 2026-08-24; keep it that way.
- ⚠ **Menu IDs: count UP (48+ is free), never fill the 8/25 gaps.** A duplicate is SILENT;
  `GUI::init()`'s boot check is the only thing that catches it, and it has shipped twice.
- ⚠ **Never restate a constant in a second file.** The `health` trailer did, and was stale within
  the hour. Only the file that owns a constant may print it.
- ⚠ **Reading `/health.log` over HTTP is the dangerous route** — it needs the uploader up, and heap
  has been measured at `largest=10560`, below where an 11 KB allocation has already rebooted this
  phone. Use `health` / `health all` on serial.



## 🔋 2026-08-23: THE FIRST REAL ON-BATTERY RUN — ~6.4 h, NOT ~10 h

{user} was out with the phone all day and asked whether the drain logs were worth reading, warning
that he had charged it in the car. **They were, and the car charge does not spoil them** — it
splits the day into segments, and the discharge segments can be measured on their own. What it
does mean is that `chg=` cannot be used to find the charging periods; they were inferred from the
voltage curve instead.

### THE NUMBER

**3.00 h on battery: soc 99 % → 52 %, v 4.13 → 3.80. That is 15.7 %/h, or ~6.4 h from full to
empty.** The handoff has said **~10 h** since the power work; this is the first measurement taken
**off** USB.

⚠ **"Treat 10 h as unproven from here on" — written here on 08-23 — WAS AN OVERCORRECTION, and
it is this 15.7 %/h run that is the outlier.** The ~10 h figure was recorded (§ "Battery: ~10
hours measured") as *steady state with the first 30 minutes discarded (surface charge)* =
10.0 %/h. That is the same relaxation correction rediscovered from scratch on 08-23 evening,
when a WiFi-off idle hour came in at **8.6 %/h** — within 1.4 %/h of the old number. This 3.0 h
run discarded **nothing** and had the radio hunting for 86 % of it, which inflates it twice over.
**Quote a rate with its conditions attached; stop converting either into an hours figure** until
a run starts an hour after unplugging and continues into the flat part of the curve. No run has
ever watched a pack go full to empty.

**What it was NOT** — both ruled out by the same samples:
- **Not the screen.** Screen was off for **177 of 181** samples.
- **Not the CPU governor.** **177 of 181** samples were at **80 MHz**; only 4 at 240.

So the draw is the radios plus baseline, with the phone sitting in a pocket doing nothing.

### ⚠ LEADING SUSPECT, EXPLICITLY NOT PROVEN

The phone spent **155 of 181 samples in `wifi=1` (WL_NO_SSID_AVAIL)** — i.e. away from any known
network, hunting. `Networks.cpp` scans **every 2 min while disconnected against every 10 min while
connected**, so being away from home is a 5× scan rate all day.

**I could not separate its cost from the curve, and did not pretend to.** Voltage slope is not
linear in SOC, so comparing a hunting block at 3.9 V against a connected block at 4.1 V measures
the battery's chemistry, not the radio. Only one contiguous same-state block was long enough to
fit (105 min, `wifi=1`, 80 mV/h) and there is nothing to compare it against.

▶ **TO SETTLE IT, and it is a cheap experiment:** two runs of an hour each on battery, screen off,
one with WiFi off and one hunting, starting from the same SOC. If hunting is the cause the
difference will be obvious; if it is not, the baseline draw is the finding and the scan rate is
exonerated. **Do not change the scan interval before measuring** — that is how the DFS experiment
went wrong.

### ✅ `chg=` WAS DEAD BECAUSE IT READ THE WRONG CHIP — root-caused and fixed 2026-08-24

It read **0 in all 807 samples**, including two unmistakable charging periods (soc climbing
89 % → 100 %, and 53 % → 86 % during the drive home, voltage rising through both). This document
called it "its own small mystery", then a measured fact. **The cause turned out to be a one-line
bug, found while chasing something else entirely.**

`battCharged` used plain `digitalRead(BATTERY_CHARGING_STATUS_PIN)` — but that pin is
`EXTENDER_PIN(0)` == **64** on the SX1509, and the ESP32 has GPIO 0–39. `digitalRead()` does not
reject an out-of-range pin: `gpio_get_level()` evaluates `(in1.data >> (pin - 32)) & 1` and the
Xtensa shift masks its amount to 5 bits, so `>> 32` became `>> 0`. **It was reading GPIO 32**
(USER_SERIAL_TX / MotorEN), which idles low — hence 0, forever, through every charge.
`cardPresent` had the identical bug and was reading **GPIO 33**, the I2S word-select clock.
Both go through `allDigitalRead()` now.

⚠ **The flag is still not trustworthy, for a different reason.** After the fix it *still* read 0
on USB at soc=96 % with voltage rising. Either the charger had legitimately tapered near full, or
the polarity is inverted (these STAT pins are typically open-drain, **active LOW while charging**,
which makes `== HIGH ? true : false` backwards). **Settle it from a low pack — see P2 — and do
not flip it on the theory.** Until then the voltage curve remains the only honest source.

### 🛠 HOW THE LOG WAS READ, AND WHY NOT OVER WIFI

New serial commands: **`health`** (last 24 KB) and **`health all`**. They stream `/health.log`
straight to UART from a 256-byte stack buffer — no WiFi, no sockets, no heap.

⚠ **The obvious route, `http://wiphone.local/log`, is the dangerous one.** It needs the WiFi
uploader up, and at the moment of reading, internal heap was **free=11448, largest=10560,
min-ever=196** — *below* the ~16 KB baseline at which an 11 KB allocation has already aborted the
WiFi PHY and rebooted this phone once. **The log exists to explain restarts; fetching it must not
cause one.**

### ⚠ THE LOG WAS ONE BOOT FROM BEING HALF-EATEN

It measured **98,453 bytes against a 96 KB cap**. The next boot line would have trimmed it to the
newest 32 KB (~4 h) and taken the start of the run with it — silently. The cap was raised **before**
flashing, which is the only reason the whole run survived.

**`HEALTH_LOG_MAX` 96 K → 256 K, `HEALTH_LOG_KEEP` 32 K → 128 K (~16 h).** The cost is nothing: a
line is ~130 bytes once a minute, so the cap is ~33 h and ~2 MB a month, on a card whose other
occupant is a 5 MB book. The old caution was about a resource that is not scarce, and it had
already cost the reset_reason line for a restart on 2026-08-15.

⚠ **And a self-inflicted one worth remembering:** the first version of the `health` trailer restated
the cap as literals in `serial_cmd.cpp`, and they were **stale within the hour** — it confidently
reported the old 96 K/32 K after the constants had been raised. The trailer is now printed by the
file that owns the constants. A diagnostic that misreports the thing it exists to report is worse
than none.

## ⌨️ 2026-08-22 (late): THE MISSED KEYPRESSES — ROOT CAUSE FOUND BY RAW TRACE

{user}, same evening: *"menu scrolling still misses some inputs but not much"* and — new, and true
**since the beginning** — *"tentap typing, sometimes it will miss a tap and I have to tap again
while scrolling through letter choices."*

### 🔑 THE CAUSE: THE SN7326 DOES NOT SAY WHICH KEY WAS RELEASED

Measured with a raw event trace added for the purpose (`keys raw` on the serial console):

```
+0ms    0x41 P    DOWN pressed
+141ms  0x00 r    ...and the release comes back as key code ZERO
+1047ms 0x54 P    OK pressed
+115ms  0x00 r    ...zero again
+455ms  0x60 P    ASTERISK pressed
+109ms  0x00 r    ...zero again
```

**Key code 0 is CALL in this keymap** (`WIPHONE_KEYBOARD` IS defined — Hardware.h:34 — so the
`#else` MZJ map is what compiles; the stock map in the `#ifndef` branch is dead code and its
code 0 means "1", which is a trap when reading this switch). So **every release the phone has
ever seen was applied to CALL.** The key actually pressed never had its bit cleared, stayed
"held", and was therefore DEAF to its own next press until the 350 ms stale sweep let go.
A person re-taps in 150–250 ms — inside that window. That is both reported symptoms, exactly.

⚠ **THE VENDOR CODE HALF-KNEW.** `newState < keypadState` is commented *"Some buttons were
released silently"* — a workaround for this very behaviour. It re-armed `keypadState` only, so
it worked for as long as `keypadState` was what gated a UI keypress, and stopped mattering the
moment anything consulted `uiKeyDown` instead.

⚠ **IT ALSO CAUSED THE DOUBLE PRESSES** seen mid-session: with `keyLastUpMs` stamped for CALL
instead of the real key, the contact bounce visible in that same trace — **a re-press 6 ms after
a release** — met a bounce filter that was watching the wrong key and sailed through as a second
menu step.

**THE FIX:** a release carrying code 0 is attributed to whatever key is actually down. That
clears the right bit AND arms the right bounce window. ⚠ **Interrupt-driven reads only** — an
empty FIFO reads back as 0x00 as well, and the only thing separating "nothing to report" from
"something was released" is that an interrupt fires only when an event really happened.

### 🛑 TWO THEORIES TRIED AND RETIRED BY MEASUREMENT — do not re-implement them

- **A 40 ms UI poll of the FIFO.** Added on the theory that the chip's 10 ms auto-clearing INT
  pulse was stranding events with no edge left to announce them. **The trace refuted it flatly:
  `drained` stayed at 0 through whole sessions of menu use while `empty` climbed into the
  hundreds.** It recovered nothing, spent I2C every 40 ms doing it, and made the 0x00 byte
  ambiguous — which is the one thing the real fix cannot afford. Retired.
- **A same-batch bounce exemption** (a release and re-press drained together escaping the time
  filter). Written for a FIFO backlog that measurement says never happens; its counter never
  moved. It is precisely what would let that 6 ms bounce through. Now counts only, never exempts.

⚠ **ALSO MEASURED: THE HELD-KEY HEARTBEAT IS ~109 ms, NOT THE 40 ms THE CODE ASSUMES.**
`SN7326.h`'s `LONGPRESS_DELAY(1)` comment claims 40 ms; the trace says otherwise:

```
+567ms 0x60 P    * pressed
+109ms 0x60 P    * pressed AGAIN, no release in between   <- a heartbeat, not a keypress
```

Short taps (~110-140 ms) usually end before the first re-report, which is why an early trace
looked as though there were no heartbeats at all — **that reading was wrong and is corrected here.**

🛑 **THIS COST A REAL BUG.** A "stale hold" rescue keyed on 100 ms — chosen from the 40 ms claim —
sat BELOW the true interval, so every heartbeat of a slightly-long press was promoted into a second
keypress. {user} found it instantly: *"push OK then push the star key to unlock, it immediately
thinks I'm trying to type the star key in the dialer"* — the unlock swallowed the real press and
the heartbeat 109 ms later went through to the dialer. **The rescue now counts and does nothing.**
It had nothing left to save anyway (`swept` = 0 once releases are attributed properly) and one
clear way to do harm. ⚠ **Anything keyed on a held-key gap must sit well above 109 ms with room for
jitter, and below the 350 ms sweep to be worth having. Re-measure before trusting any number here.**

### ⚠ ONE REGRESSION THE FIX ITSELF CREATED — fixed, but know the shape of it

The first cut of the code-0 attribution set `mask = held`, i.e. it blamed the release on EVERY
key believed down. Release one of two held keys and **both** were marked up, so the key still
under a finger left `uiKeyDown` and its next ~109 ms heartbeat read as a brand-new press — a
**phantom keypress**. It reached the Select+Back sleep chord (lift one finger early and the other
key fires; a spurious Back navigates if the chord had not yet completed), rolling two-key typing,
and the Game Boy (releasing A while holding RIGHT drops RIGHT for ~109 ms — `app_gbc.cpp` reads
`keypadState` directly and the `newState < keypadState` repair is skipped in game mode).

**Attribution now requires EXACTLY ONE key down** (`(held & (held - 1)) == 0`). With two or more
down the chip has said something was released and not what, and there is no honest way to pick —
so it does not: the byte stays decoded as CALL as it always was, and the sweep tidies up. Counted
as `relamb`.

🔑 **Worth remembering how it was caught: an adversarial source review, not the trace.** It needs
TWO KEYS AT ONCE, and every hardware trace taken that night was single-key. The trace found what
reading could not; the review found what tracing could not. ⚠ That same review refuted 19 of its
own 24 findings, several because they cited line numbers from a tree being edited underneath them
— **do not run a source review against a tree you are actively changing.**

### ⚠ KEY_BOUNCE_MS WAS NEVER ACTUALLY IN THE PATH UNTIL NOW

While every release went to CALL, the bounce window was armed on the wrong key — so the 40 ms it
had carried since it was written had **never been tested against real typing**. Attributing
releases correctly armed it for the first time, putting an untested 40 ms directly in the path of
fast same-key tapping, which is exactly the tentap case. **Now 25 ms**: measured bounces are 6 ms
and 13 ms, and no human re-taps inside 80 ms.

### Still standing, and worth keeping

- The UI press edge is `uiKeyDown` ALONE. It used to be nested inside the `keypadState` edge,
  which made `keypadState` a second undocumented veto and made the comment claiming otherwise
  false. The stale sweep now clears both masks, and skips a pass that arrives late (it runs
  BEFORE the drain, so its premise "we would have seen a heartbeat" needs us to have been looking).
- `SN7326.h::readReg()` no longer discards a byte it successfully read. It returned the status of
  a **trailing zero-length write** left over from a begin/end pair wrapped around `requestFrom()`;
  any NAK there reported failure for a key already off the wire, and the caller dropped it.
- A press dropped for a full `keypadBuff` is no longer latched into `uiKeyDown`, so nothing is
  needed to retry it.

📊 **THE INSTRUMENT: `keys` and `keys raw`** on the serial console, plus a `KEYS` line into
`/health.log` on the minute tick **only when a counter moves**. `relfix` counts re-attributed
releases and should climb steadily in normal use; `killed` counts bounces correctly filtered;
`gapfix`/`swept` should now stay near zero. **`keys raw` is the tool that solved this** — the
counters said what was wrong, only the raw bytes said why.

📶 ✅ **SUPERSEDED 2026-08-26 — DO NOT ACT ON THIS BLOCK.** The deeper fix is SHIPPED and
MEASURED: 8 REGISTERs, one `REGISTERED`, zero "lost" over 330 s on the phone that holds the
account (see the 0.9.21 block at the top). 🛑 **This block's conclusion — "THE FLAP WAS
LOAD-BEARING" — is wrong, and it is written as an instruction to a future session not to re-try
the fix.** What actually made the 08-22 attempt fail was ping starvation, root-caused separately
on 08-24. Kept only for that reasoning. Original heading:
~~SIP: THE DEEPER FIX WORKED AND WAS STILL REVERTED — READ BEFORE RE-TRYING.~~
Shipped as both halves (drop `registered = false` from `registration()`; refresh at 45 s not 60 s,
because the refresh counts from `msLastRegisterRequest` when we ASKED and expiry from
`msLastRegistered` when the proxy ANSWERED, so a 60 s period put the 200 OK on the exact instant
the expiry term tripped). **It did what it was meant to: 53 flaps in 31 min → 0.**

⚠ **THEN THE PHONE WENT `lost` AND NEVER CAME BACK** — ~20 minutes unregistered on the phone that
owns {user}'s number, ended only by reflashing. Reverted; phone 1 is back on vendor behaviour and
verified `registered: yes`. 🔑 **THE LESSON: THE FLAP WAS LOAD-BEARING.** Clearing and re-setting
`registered` every 60 s meant a run of REGISTER refreshes that never got a 200 OK was INVISIBLE —
the phone re-declared itself REGISTERED the moment any one of them worked. Remove the flap and
the same run shows as a permanent `lost`. **The flap was hiding whether refreshes succeed.**

**RULED OUT, do not re-investigate:** `wifiTerminateCall()` (tinySIP.cpp:1666) does clear the flag
but needs a WiFi drop AND `isBusy()` — neither happened (`wifi=3`, no call); `terminateCall()`
returns at its `!currentCall` guard before touching any flag.
**LEADING SUSPECT, UNPROVEN:** `ping()` (tinySIP.cpp:1567) writes `msLastPing` only **inside**
`if (tcpProxy->connected())`. A failing ping leaves the ping condition permanently true, and
`checkCall()`'s chain is `if (ping due) ping(); else if (register due) registration();` — so
**a failing ping starves registration() forever.**

▶ **NEXT ATTEMPT NEEDS INSTRUMENTATION FIRST, NOT A RETRY.** Log at `log_e`: registration()
actually being CALLED, and the class of each REGISTER response. Until those exist there is no way
to tell "never asked" from "asked and ignored". Do it on **phone 2**, not the phone with the number.

**STATE:** keypad fixes FLASHED to phone 1 (025A3EAF); SIP reverted and re-registered. 1167 host
assertions green. Uncommitted. ⚠ **Phone 2 untouched.**

## ✅ 2026-08-22 (night): EVERYTHING FLASHED AND VERIFIED — plus one change BACKED OUT

**Both phones run commit 1833c4d.** Phone 1: 3+ min clean, zero panics, SIP REGISTERED.

✅ **DUPLICATE TEXTS FIXED AND ROOT-CAUSED ON AIR.** A live text produced two acks with the
IDENTICAL Call-ID and CSeq, and the second was dropped: `SIP MESSAGE retransmit dropped
(Call-ID 0ad4f98c… CSeq 102) - acked, not stored`. Identical Call-ID = retransmission of ONE
transaction, so the guard is the COMPLETE answer and the RFC 3261 18.2.2 "wrong box" hypothesis
is RETIRED — the ack goes to `208.100.60.42:5060`, the same box we register to. It was always
the network (Android hotspot, symmetric NAT).

🛑 **THE 80 MHz MENU EXPERIMENT IS BACKED OUT — `UI_IDLE_DOWNCLOCK 0`.** {user} on hardware: "the
menu is a bit laggy and doesn't pick up every button push." Missed input is a BREAK, not a tuning
problem, so it was NOT dialled to 160 as he offered. 🔑 **The evidence contradicted the obvious
reading:** the new "screen idle" state NEVER FIRED — zero occurrences — so menus were never
running at 80 MHz and the lag was not the low clock. It was `cpuRaiseForUi()` calling
`setCpuFrequencyMhz()` from INSIDE THE KEY-DRAIN LOOP; a PLL switch mid-keypad-read is how a
keypress goes missing. **The level was never the problem, the SWITCHING was — 160 switches just as
hard.** If revisited: raise the clock somewhere that is NOT the input path, and MEASURE the saving
first (never quantified; the backlight likely dominates screen-on draw).

✅ **SUPERSEDED 2026-08-26: THE FIX IS SHIPPED** (3d9f329, 08-24) and measured — see the
0.9.21 block at the top. ⚠ This paragraph said "FIX NOT SHIPPED" for two days after it shipped;
it is the third copy of that stale claim found, after tinySIP.h:624 and MEMORY.md, and it is why
0.9.21's sweep is itself an example of the bug it was fixing: **both of those were corrected BY
NAME and nobody grepped this file.** Original wording:
~~SIP REGISTRATION FLAPS ONCE PER REFRESH — CAUSE FOUND, FIX NOT SHIPPED.~~ `REGISTERED -> lost
-> REGISTERED` every cycle, since the initial commit, invisible until the line was promoted to
log_e. 🔑 NOT the period: `registration()` clears `registered` on EVERY refresh (tinySIP.cpp:1288)
and restores it when the 200 OK lands, so the phone declares itself unregistered for one round
trip, by construction. My first fix (refresh at 45 s) was WRONG and is REVERTED — it just moved
the flap to every 45 s. **The real fix** is to not clear the flag while merely REFRESHING (the
expiry check in `registrationInvalid()` already covers a failed refresh) — deliberately left for a
session with {user} present, because it is core registration state on the phone that owns his number.

✅ Also shipped and verified tonight: uploader idle auto-stop (the battery answer), NTP backoff,
the reference-counted screen-timeout holder, `dimming=0`/`sleeping=0` shipped-config landmine,
the ringtone surviving a WiFi drop, xferStart failing cleanly, and the mesh retry backoff.
⚠ **Ring-init trap, worth remembering:** the patch that should have initialised the retransmit ring
ABORTED on a failed assertion before writing the file, and reported success from the half that
applied — shipping uninitialised members. Saved only because `TinySIP sip` is a global (BSS-zeroed);
`Test.cpp:731` builds one on the stack. Now member-initialised in the class. **A scripted
multi-edit must write per-edit or not at all.**

## 📨 2026-08-22 (evening, {user} out of town): DUPLICATE TEXTS — FIXED IN SOURCE, ⚠ NOT FLASHED

**Symptom:** every inbound text arriving twice or more on the WiPhone.
🔑 **COVEY was POWERED OFF, which rules the SMS mirror out entirely** — both copies came in
over SIP. And the SIP receive path had **no de-duplication of any kind**: `tinySIP.cpp` called
`textMessages.add()` for whatever arrived.

**Mechanism:** a UDP MESSAGE whose 200 OK does not reach the server is RETRANSMITTED by that
server (RFC 3261 timer E/F, ~7 times over ~32 s). We *do* ack correctly per RFC 3428 — the ack
most likely goes UNMATCHED because a symmetric NAT on an unfamiliar network maps the reply to a
different port than the request arrived on. Network property; the phone must tolerate it.

**Fix (commit d3462a5):** de-dup on **Call-ID + CSeq** — the RFC identity of a retransmission,
and exactly what a genuine second text does NOT share (send "ok" twice → two Call-IDs), so it
can never merge two real messages. 8-entry ring. **Still acks every copy** — going quiet on the
second guarantees five more.

⚠ **NOT FLASHED — the affected phone (phone 1, adapter `025A3EAF`) was out of town and only
phone 2 was on the cable.** Flash phone 1 and the duplicates should stop.
⚠ **The fix is also an instrument.** If duplicates SURVIVE it, the new log line names the
dropped Call-ID: duplicates carrying DIFFERENT Call-IDs mean VoIP.ms is re-pushing the queued
text as a FRESH transaction after each REGISTER (we re-register every 60 s), which is a
different fault needing a content+time-window guard instead of a transaction-identity one.
🔑 **TRIPLICATE MEASURED (Android → the SIP number): 3 copies on the WiPhone, 1 on COVEY.**
The COUNT is the evidence: a fixed 2 would mean two delivery PATHS, but a varying 2–3 is the
signature of RETRANSMISSION (VoIP.ms retries at ~500 ms, 1 s, 2 s…), with the phone storing
however many landed. Combined with COVEY's single copy, the diagnosis is settled: one message
upstream, delivered repeatedly over SIP, stored every time.

✅ **PHONE 2 EXONERATED** ({user} asked directly, four independent grounds):
1. **The WiPhone NEVER transmits mirror records** — `smsMirrorPack()` is called nowhere in
   firmware (only its own .cpp/.h and the host test). The SMS mirror is one-way, COVEY → phone.
2. Phone 2 has **no SIP account** (measured: `account NOT LOADED, registered: no`).
3. It was 30 miles away — far outside handheld LoRa, no relay chain.
4. COVEY was OFF during the doubling, so no mesh path was live at all.
📎 **The two-way sync {user} remembered is real but is TWO features carrying DIFFERENT traffic:**
SMS mirror (COVEY → phone, CSM1 on `smsmirror`) carries **SIP texts**; mesh history replay
(phone → COVEY, `RPL?` on `booksync`, D-112) carries **mesh channel texts**. Neither can
duplicate a SIP text.

✅ **Phone 2 FLASHED with the fix 2026-08-22** (it has no SIP account yet, but will).
⚠ **Phone 1 STILL UNFLASHED** — it is out of town with {user}; flash when he is home.

✅ **COVEY shows ONE copy of each text (measured same evening, COVEY powered back on).** It reads
the account history over the REST API, so VoIP.ms holds exactly ONE stored message — which rules
out every upstream explanation (DID delivering to two destinations, `getSMS` window overlap,
account-level duplication) and pins the fault to repeated SIP DELIVERY of that single message.
✅ **WORKAROUND, now that COVEY is on:** phone → SIP Accounts → **Unmake primary**. The phone
stops registering (no SIP delivery, no duplicates) and still receives, because COVEY mirrors to
it over the radio. Cost: no outbound calls/texts from the phone until it is made primary again.
⚠ Turning COVEY on ALONE does not fix it — a mirror record adopts ONE matching copy, so with two
already stored you still see two.
⚠ **No workaround while COVEY is off** — un-registering SIP would mean no texts at
all. With COVEY ON, un-registering the phone (SIP Accounts → Unmake primary) would give single
copies via the mesh mirror.

## 📐 v2 DESIGNED 2026-08-22 (not yet built) — DUAL-SOURCED RAIL, PARTS NOT ORDERED

**{user}'s idea, and it is right:** power the plate from the phone's own VBAT so the radio and GPS
survive a dead external pack. v1 rejected VBAT, but **that objection was about the PART**: a
TLV62569 is a BUCK with a 3.4 V input floor and a 1S cell ends at 3.0 V. A **buck-boost** has no
floor, so the objection dissolves with the regulator swap.

**Shipped design (wiring sheet rev v2, commit 4879c5f):** D1 and D2 Schottky-OR the PowerBoost's
5.2 V and the header's VBAT into one buck-boost. Pack alive → 5.2 V wins, phone's cell untouched
(v1 behaviour kept). Pack dead → VBAT takes over. No logic, no firmware. New wire **W20**
(header VBAT → D2 → the OR node); **W6 gains D1**; W7/W8/W10/W12 keep their nets and just land on
the new module. **EN gate carries over unchanged** — a TPS63020 module and the Pololu S9V11E2F3
both behave like the TLV there, and the S9V11E2F3's 100 kΩ EN pull-up matches the TLV's 99.9 kΩ
exactly, so R1 (4.7 k) + W12 keep their measured margin.

🔑 **v2 REMOVES NO CHARGING PATH — {user} asked, and the answer is written into the sheet.** The
PowerBoost stays: it still charges the phone through W4, still charges its own pack over its own
microUSB, and phone-USB still reaches the plate through D1 (measured today). Deleting the
PowerBoost would delete the external pack; that is NOT what v2 does.

**PARTS — ✅ ORDERED 2026-08-22. {user} has the diodes.**
- ✅ **TPS63020 module, Amazon B0H3KQ1VXJ** (DWEII 6-pack). 26 × 18 mm, 2–5.5 V in.
  Pads per the board photos: **VIN · GND · PS · EN** one edge, **OUT · GND** the other,
  **3V3 / 4V2 / 5V solder-selectable**.
  ⚠ **SET THE 3V3 JUMPER** before wiring — it does not ship set for this job.
  ⚠ **PS is power-save select — tie it LOW** (pulse-skipping at idle; the plate idles ~45 mA).
    Do not leave it floating. Check what the module ties it to before assuming.
  ⚠ **EN thresholds are tighter than the TLV's** (V_IL 0.4 V, V_IH 1.2 V vs the Pololu's 0.7 V),
    so checklist item 4 — phone off, EN must read < 0.4 V — is now load-bearing. Our measured
    0.22 V divider still passes, with less headroom than v1 had.
  ⚠ 6 pieces: bench one (3V3 jumper + EN behaviour) BEFORE it goes on the plate.
  ⚠ **26 × 18 mm and ~4 mm tall** — bigger than the TLV breakout. Case impact.
- Alternative if height/size ever bites: **Pololu S9V11E2F3** (#5712, $4.95, 10.9 × 16.5 × 4.1 mm,
  <0.2 mA quiescent, 100 kΩ EN pull-up matching the old TLV's).
- D1/D2: 1N5819 / SS14 / BAT54 ({user} has these).

**⚠ TWO CHECKS OWED BEFORE THE REWORK:**
1. **Meter header VBAT → GND.** It must read the phone's own cell (3.7–4.2 V) and TRACK it as the
   phone discharges. **That pin has never carried current on this plate** — v1 left it empty.
2. **Meter D1 and D2 band orientation — cathodes toward VIN.** D2 backwards = 4.9 V back-charging
   the phone's cell.

**Known tradeoff, accepted:** energy reaching the plate via the phone (pack → boost → charger →
cell → buck-boost) lands ~66% efficient vs ~83% direct, and with the pack dead the plate draws
~45 mA continuous off the phone's own cell. That is the price of the radio not dying.

## ✅ BENCH STATE, 2026-08-22 (final) — REWORK DONE, WHOLE FEATURE PROVEN ON AIR

**R3–R7 are SOLDERED and every close passed the same day:**
- **Phantom power is dead**: pack out + phone alive → no GPS LED (the rail that floated at
  2.54 V now collapses; the meter probe at C2 is optional — the LED and the Error behaviour
  below are the evidence that matters).
- **The screen tells the truth**: both supplies out → mesh screen **Error** within 5 s
  (witnessed by {user}); power back → **`MESH RADIO RECOVERED — reconfigured, announcing`** in
  the serial log within 10 s, and the boot-style announce went out on air (captured). Radio,
  GPS (8-sat fix through R6), SPI through R3–R5, health check 0 false alarms — all verified
  post-rework, firmware 2d3f85c FLASHED.
- 🔑 **DISCOVERY: USB is a second legitimate supply for the plate.** The phone's 5 V node
  (~4.6–4.7 V on USB) reaches the header 5 V terminal, and the TLV runs from 3.4 V — so a USB
  power bank keeps radio + GPS fully alive with NO woods pack. Ran for hours in exactly this
  state (pack out, USB in) with zero drama — which also soaks the review's
  PowerBoost-back-feed worry (formal current probe into the PB 5 V pad remains optional).
  Consequence: the LOST/RECOVERED test CANNOT run over USB serial — both supplies must be out.

**PLATE SWAP RULES (stock LoRa plate ↔ woods plate), both directions safe anytime:**
1. **Swap COLD** — phone fully off, always (driven SPI into an unpowered radio otherwise).
2. **BT2.0 out whenever the woods plate is off the phone** — the PowerBoost keeps boosting and
   the loose 5 V terminal is hot; the 3.3 V rail is safe (EN pulled low by R1) but 5 V is not.
3. Optional: `gps off` before living on the stock plate (spares a future bench session the
   "bytes=0 = wiring?" confusion). Firmware needs NO change — radio config rebuilds at boot,
   mesh identity lives in the phone, and the health check simply never fires on a plate whose
   rail cannot die independently.

**Next: {user} designs the shell/case.** Constraints already known: BT2.0 reachable (it IS the
off switch) · PowerBoost micro-USB cutout (the pack's charger) · case should leave NO room to
populate the JST (paralleled packs, July 2026) · pigtail strain relief where it crosses the
pack + u.FL clicked-not-resting · antenna/SMA bulkhead placement · GPS antenna sky view ·
PowerBoost LED visible if possible (it is a useful power-state tell).

## 🔧 (superseded 2026-08-22 evening — rework was owed, now done; kept for the record)

**Physical state right now:** the plate is **MOUNTED and fully proven on air** — phone charges
from the pack, plate rail gates correctly, **GPS has a live fix (9 sats, HDOP 1.1, 115200 — NOT
9600, see below)**, and the radio TXed an announce and learned a PKI key back through the new
coax/SMA path. The phone runs the health-check firmware (2d3f85c).

### ⚠ OWED: the R3–R7 resistor rework (parts chosen, sheet updated, NOT yet soldered)
**Why (measured):** pack disconnected + phone alive = the phone's driven pins back-feed the plate
through ESD clamps. Plate 3.3 V floated at **2.54 V** (one diode under 3.3), the 5 V node at
**2.18 V** (backwards through the TLV's high-side body diode). An RFM95W runs at 2.5 V: it
answered REG_VERSION, the mesh screen said **READY**, RX worked, **TX was silently impossible**,
and the GPS LED blinked on a plate with no battery. {user} reproduced the READY lie on screen,
including across a reboot.
**The fix (wiring sheet rev 2026-08-22 has every position):**
- **R3–R6, 4× 1 kΩ series** in the phone-driven lines only: W14 MOSI, W15 SCK, W16 NSS,
  W18 GPS-RX. W13 MISO / W17 GPS-TX stay plain wire (plate-driven; EN gate covers them).
- **R7, 10 kΩ, RFM NSS pad 5 → PLATE 3.3 V — REQUIRED** (adversarial-review find): GPIO 27
  floats during every ESP32 reset; NSS drifting low lets the radio drive MISO = **GPIO 12 =
  MTDI, the flash-voltage boot strap** → phone fails to boot. The stock v2.2 plate fits this
  exact pull-up (its R2 100 k). Referenced to the PLATE rail so it adds no phantom feed.
**After soldering, two closes:** (1) repeat the pack-out probe at C2 — expect **well under 1 V**
(the review rates the collapse threshold computed-not-proven; this measurement closes it);
(2) with the pack out ≥5 s the phone's mesh screen must now say **Error**, and ≤10 s after
plugging back in it must say Ready again (announce fires — that's the firmware below).
**Also owed from review (UNCERTAIN, 2 min):** pack out, phone on USB, plate mounted — measure
current into the PowerBoost 5 V pad / voltage at its BAT pad. Expect ~0 (TPS61090 output body
diode blocks); sustained current = back-feed into the boost, wants a Schottky in W4.

### ✅ Firmware: the radio must keep proving it exists (2d3f85c, flash OWED — port was unplugged)
`MeshPhy::healthCheck()` = REG_VERSION **plus** REG_OP_MODE (version alone is a liar twice over:
phantom power, and a reconnected pack answering 0x12 from POR defaults — present, deaf, mute).
Service checks every 5 s → `MESH_RADIO_ERROR` loudly; every 10 s in ERROR it re-runs the FULL
register config (`begin()` is now a shell over `reinit()`) so a swapped pack rejoins in ≤10 s
with a boot-style announce, no reboot. Boot probe hardened with a SLEEP-readback second opinion
(floating MISO can't fake two registers). ⚠ Detection is only honest once R3–R6 are in.

### ✅ GPS: **115200, NOT 9600** — and COVEY's repo knew (its gps.py, D-062)
The M100 was alive all along; 9600 was COVEY's D-033 PLAN number, never amended after the
implementation moved. Scanned on hardware: only 115200 produces sentences. New serial tools:
**`gps baud <n>`** (persists, wpmesh/gpsbaud, retunes live) and **`gps raw`** (hex+ASCII — tells
wrong-baud from UBX-binary). Default now 115200 (`GPS_SERIAL_BAUD_DEFAULT`). Both repos'
docs corrected; the lesson is written into Hardware.h: **when two repos share a part, the working
code is the authority, not the decision that proposed it.**

### If the pack dies in the field (until the rework is soldered)
The phone itself is fine (5 V pin goes to 0 = the shipped state), but the plate half-lives on
phantom power and the screen LIES about Ready. Field rule for now: pack dead → `gps off` + power
the phone off, or pull the plate. After the rework: the screen tells the truth by itself.

*(Superseded, kept for the reasoning:)* the first-power sequence below exists because a reversed
cell or a solder bridge is unrecoverable on the PowerBoost, and both are cheap to rule out while
the plate is loose.

### As-built deviations from the BOM (all deliberate, all recorded)
- **F1 = GF300** (16 V, 3 A radial PPTC) in the **cell POSITIVE** leg, not the 1 A the BOM
  originally specified. The 1 A was undersized: that leg carries boost INPUT current, ~1.6 A at
  a fresh cell and ~1.75 A once it sags to 3.3 V, and PPTC hold derates ~0.85× at 40 °C.
- **BT2.0 pigtail into the PowerBoost's BAT/GND PADS**, not the JST. The pads are wired in
  parallel with the JST, so this is electrically identical. ⚠ **THE JST MUST STAY EMPTY** —
  a pack on both inputs is paralleled cells, which is exactly what destroyed both packs'
  protection FETs in July 2026.
- Battery leg is 20–22 AWG (BT2.0 stock). The 26–30 AWG in the BOM is for signal/3.3 V runs
  only; it is NOT adequate for the cell leg.

### ✅ Everything measured today (the build guide's owed numbers — all pass)
| measurement | result | meaning |
|---|---|---|
| 5 V header fed at 5.0 V / 200 mA limit, no USB | **phone charges** | the pack-drives-phone-through-the-header topology is now MEASURED, not inferred |
| header 3.3 V pin | **3.3 V phone ON / 0 V OFF** | a true power-state signal, so the TLV EN gating works; also proves `ENABLE_DAUGHTER_33V` does not gate this rail |
| TLV62569 EN→VIN, unpowered | **99.9 kΩ** | the internal pull-up, measured directly (within 0.1% of the assumed 100 k) |
| EN powered, no pull-down | **4.95 V** | without gating the plate's rail would be permanently ON |
| EN with the 4.7 kΩ fitted | **0.22 V** | ✅ PASS vs the 0.4 V guaranteed-off limit; 0.234 V at the PowerBoost's 5.2 V |

### ✅ The stock LoRa plate's resistors — CLOSED, and the plate is a v2.2 (not v2.0)
Vendor design files found and saved to **`~/Downloads/wiphone-lora-v2.1/`** (schematic PDF +
rendered PNG, BOM .xlsx, Eagle .sch/.brd). Published on wiphone.io, NOT GitHub. Authoritative:
- **R1 = R2 = 100 kΩ** pull-ups (RESET and NSS to 3.3 V). {user} measured 98.9 k and 52 k — the
  52 k is an in-circuit parallel-path artifact, not a different part.
- **R6 = 0 Ω**, the series element of an otherwise-unpopulated pi network (**C2, C3 = DNP**).
  Netlist: `ANT = {R6.2, C3.2, U1.ANT}`, `N$9 = {R6.1, C2.2, J1.SIGNAL}`.
  ⇒ **the stock RF path is module ANT → 0 Ω link → U.FL. No matching, no filter, no PA.**
  So soldering coax straight from `ANA` to the SMA pigtail is what the factory does minus one
  joint — the "one genuinely open RF question" is closed.
- **R7 and R11 DO NOT EXIST** (nor R3/R4/R5 — the numbering simply has gaps).

### ✅ FIRST POWER DONE — steps 1–4 PASSED, 2026-08-22 (bench supply AND LiPo)

| measurement | result | meaning |
|---|---|---|
| 5 V rail (PowerBoost out) | **5.15 V** | boost running; PowerBoost-typical, in spec |
| header 3.3 V pin → GND, no phone | **0.22 V** | 🔑 this pin IS the EN node (W12). The divider predicts 5.15 × 4.7 k/(4.7 k + 99.9 k) = **0.231 V**, so 0.22 V is a three-way pass at once: W12 continuous, R1 fitted and correct (missing R1 → ~5.15 V and a permanently-live plate), internal pull-up intact. Well under the 0.4 V guaranteed-off limit. **Far easier to probe as built than the TLV EN pad — use this pin.** |
| plate 3.3 V rail (across C2 and C4) | **0 V** | ✅ THE key check — the EN gate really does collapse the plate rail with no phone attached |
| repeat on the LiPo (step 4) | **same three readings** | no bench-supply artifact |

⚠ **The PowerBoost's LED is on continuously and that is CORRECT** — the 1000C's green
power-good LED lights whenever the boost is enabled (the yellow/red charge LEDs only mean
anything when its own micro-USB is fed). It is already inside the measured few-mA quiescent.

### ▶ MOUNTING (step 5) — mate it COLD, and here is why
**Phone fully shut down (not asleep — the header 3.3 V pin reading 0 V is the proof) AND the
pack unplugged at the BT2.0 before the plate touches the header.** Then: seat squarely and fully
home → plug the pack back in (phone should begin charging while still off) → power the phone on.
Two independent reasons, both about the mate itself:
- **The mesh is live**, so the ESP32 is actively driving SPI (GPIO 12/13/14/27) at 3.3 V. Hot-mated,
  those lines land on an RFM95W whose rail is still down (EN does not rise until the header's 3.3 V
  pin makes contact) and the drive current goes through the RFM's ESD clamps to find its rail.
- **Ten contacts mate in an arbitrary order** by hand. With the pack live, W4 presents 5.2 V at the
  header's 5 V pin, which must not make before GND.

🔋 **The BT2.0 unplug is the real off switch, not the phone's power button.** The EN gate stops the
PLATE RAIL draining the pack when the phone is off, but the 5 V boost keeps feeding the phone's
charger regardless — that is the intended power-bank behaviour, and it is the overnight drain path.

### ▶ FIRST-POWER SEQUENCE — do these in order, plate still OFF the phone
1. **Meter BT2.0 polarity with a DMM. Do not trust wire colour.** Confirm pack + reaches the
   PowerBoost BAT pad. Reverse-connecting a LiPo here is not recoverable.
2. **Confirm the JST is empty.**
3. 🔑 **First power from a BENCH SUPPLY, not the cell** — 3.8 V, **300 mA limit**, into
   BAT+/GND (the PowerBoost cannot tell it from a cell, and a LiPo cannot be current-limited).
   Expect: quiescent draw only; **5 V rail ≈ 5.2 V**; and — the interesting one —
   **the plate's 3.3 V rail should read 0 V**, because EN is gated off the phone's 3.3 V pin
   and no phone is attached. A live 3.3 V rail here means the EN gate is miswired.
4. Only when 3 is clean: swap the bench supply for the LiPo, re-check the same three readings.
5. Then mount on the phone and verify, in order: phone charges · plate 3.3 V comes up with the
   phone · `gps on` then `gps` shows bytes+sentences (bytes rising with sentences 0 = wrong
   baud) · radio still transmits (serial `nbr now`, or watch a mesh send).

⚠ **The PowerBoost will not run at all without a cell (or bench supply) on BAT** — a dark board
with nothing attached is expected, not a fault.

## ✅ 2026-08-21 (afternoon): NEIGHBOUR INFO — who hears whom, on both devices

{user}'s ask, for building a mesh map while hunting. **Phone side:** a RAM table of
nodes heard with NO relay (hop_start == hop_limit; hop_start absent = unknown,
never counted — a relayed node is not a neighbour and an edge to it would be a
road that does not exist), announced as a real Meshtastic NeighborInfo packet
(portnum 71) on the PRIVATE channels only — never the public primary, and never
booksync/smsmirror (machine channels, nobody reading). **My node > Neighbor
info** cycles off / 1h / 4h; serial `nbr` lists the table, `nbr on|4h|off|now`
drives it. The encoder is hand-rolled protobuf pinned BYTE-FOR-BYTE to the real
runtime's output (tools/gen_neighbor_vectors.py → tests/test_neighbor.cpp);
proto3 default-elision is part of that contract (emitting `snr=0` where the
reference emits nothing cost 5 bytes and byte-equality).
🔑 **MEASURED, and it decides the whole feature: the RAK forwards port-71
packets to its clients ONLY while its own NeighborInfo module is enabled.**
Module off → the phone announced twice and COVEY saw nothing; module on →
`[nbr] !00449040 hears 1 node(s): !62b8d2fd` immediately. So COVEY's toggle is
not about its own broadcasting, it is the switch that lets COVEY (and the
Android app) see anyone's neighbours at all.
⚠ The RAK's own module has a documented 4-hour floor and will not transmit over
LoRa on a default-key channel (COVEY's primary is stock LongFast) — so COVEY
contributes its local view, and the PHONE is what actually puts neighbour data
on the air. That inverts {user}'s expectation ("covey more often") and is worth
saying out loud.

## ✅ 2026-08-21 (afternoon): MESH HISTORY REPLAY — the phone is COVEY's memory now

**docs/replay-spec.md is the contract; CHANGELOG has the narrative.** The phone
rings the last 64 channel texts it hears/sends (PSRAM ~13 KB, internal RAM
untouched); COVEY asks `RPL?` on booksync after a blind window (lent radio /
powered off) and gets records back — original UTC timestamps, oldest first, one
packet per 3 s, honest gap marker. **Proven live in the exact bag scenario:**
two messages sent through COVEY's own radio while covey-ui slept came back on
wake AS OUTGOING in its Howe-group history, timestamps within seconds of truth.
Wire format pinned both ways by generated vectors (tools/gen_replay_vectors.py
→ tests/vectors_replay.h; regenerate whenever covey_ui/replay.py changes).
Serial `replay` = ring/pending/last-served. Machine-prefix rule extended: RPL*
never reaches chat on either device (unknown-prefix fallthrough IS chat).
🔑 Clock traps now encoded in the spec: `getExactUtcTime()` not
`getExactUnixTime()` (local-shifted epoch — cost 7 h on the first live run),
and the asker pads t2 by +600 s (a tight window served 0 records with both
sitting in the ring; the no-RTC Pi and the phone WILL disagree).
COVEY's half: covey_ui/replay.py (reference) + replay_glue.py + the
_replay_tick in app.py; its HANDOFF carries D-112.
v2 lines live in the spec: GPS-true time, LAN transport, SD-persisted ring.
✅ **The adversarial review RAN (19 agents) and its two confirmed findings are
FIXED + flashed + verified on air:**
1. 🔒 **PRIVACY (high): legacy channel-encrypted DMs were entering the ring** —
   this phone deliberately accepts that DM form, and it decrypts with the
   CHANNEL key, so replay would have BROADCAST a private message to every
   booksync member. The spec's "DMs are safe, they're PKC" assumption was
   simply false for the legacy form. Ring capture is now gated on
   `toInternal == MESH_BROADCAST_ADDR` — that check IS the DM exclusion.
2. **Gap honesty (medium): a reboot hid its own losses.** Coverage was proxied
   by ring FULLNESS, and the ring is empty after every reboot — the exact case
   the flag exists for. Now a `replayCoverFrom` stamp (first packet after the
   clock locks) is compared against t1. Verified on air: an all-time ask now
   answers GAP where it used to claim completeness.
⚠ Three verify agents were cut by a usage limit; their findings (skewed-window
handling, ASK_MAX-vs-ring interplay) are UNJUDGED — the workflow can be re-run
from the session's workflows/scripts/replay-review-wf_963fee0d-85c.js.
⚠ Capture needs a KNOWN CLOCK: for the first minute or two after a reflash the
phone hears traffic and rings NOTHING (correct — an unstamped record can serve
no window, and it cost a confusing bench minute today).

## ✅ 2026-08-21 (day): the redesign LANDED — chunked protocol + raw transport

**What shipped (see CHANGELOG for the narrative):** chunk_proto.h (verdicts, name
safety, CRC32 — host-tested), `/chunk` on the WebServer (multipart fallback, 4 KB
pieces), **the raw transport on port 8081** (one TCP connection per batch, raw
text/plain bodies read incrementally by a non-blocking state machine in the main
loop, CORS-simple so the page fetches cross-port, 16 KB pieces), the page JS
(probes 8081 → falls back; batch survives per-file failure; native form kept for
fetch-less browsers), durable-bytes-in-RAM (File::size() lies on open files),
chunkPersist-not-finalize on breaker pauses, no breaker trips at all mid-batch
(cycling inside tight heap LEAKED — floors stair-stepped 3328→616 until removed),
serial `heap`, per-file floor telemetry, tools/chunk_push.py (both transports +
fault injection).

**Acceptance vs the brief's bar:**
- ✅ 3 consecutive 4-book batches over the fast link *(the phone-to-phone hotspot —
  the original crash-day link)*, all 12 files byte-verified, **zero** breaker
  events, **zero** panics; 66–97 KB/s, ROUND-TRIP-BOUND on that link (home LAN
  should be several× faster; piece size is the lever, in-flight is window-bound).
- ⚠ **"floor never below 10 KB free" is UNMET AS WRITTEN and appears unmeetable on
  this stack**: worst sampled per-file floor 1,384 free (allocator min-ever 144 B).
  These are sub-millisecond WiFi-task burst allocations during ANY sustained RX —
  invisible to sampling, non-fatal by design (failed RX alloc = dropped frame =
  TCP retransmit), and ~90 MB of hardware transfers today crossed them without a
  single crash. {user} decides whether the bar moves or more mitigation is wanted.
- ⏳ **Real-mobile-browser run still owed**: {user}'s Android on {user}-wifi opening
  http://<phone-ip>/ and uploading any file (a photo works) — 2 minutes, closes
  the bar's last clause. The page JS itself is proven in a real engine against a
  faithful mock (15 % injected failures, CRC corruption, resume) and both
  transports are proven against the real phone via chunk_push.py.

**Traps for whoever touches this next:**
- ⚠ Flashing REQUIRES panicwatch STOPPED (it eats esptool sync; one attempt left
  the phone in download mode). `pkill -f panicwatch` → `pio run -t upload` →
  restart it.
- ⚠ `pieces=` in the done-line telemetry is per-SEGMENT (resets when the batch
  file reopens after a connection drop) — not per-file. The floors are per-segment
  too. sent==size in chunk_push output is the honest per-file check.
- ⚠ The legacy whole-file `/upload` can still wedge the main loop if its client
  vanishes mid-POST (framework parser + flooded RX window). Chunked pieces dodge
  this by being smaller than the TCP window. `/upload` stays for curl + no-JS.
- The `.bin` probe clutter in /books from graded testing is invisible to the
  Books list; harmless.

## 📻 COVEY BT investigation (same day, for the COVEY repo's record too)

{user}: RAK invisible to the Android Meshtastic app. Radio config verified
bluetooth.enabled=True, mode 1, pin 123456 (covey-ui stopped/queried/restarted).
Mac holds a stale `AdaDFU` pairing (DFU-mode identity from the failed BLE-OTA era;
cannot explain app-mode invisibility; blueutil not installed, removal parked).
Android bonds wiped by {user} + Meshtastic app device list cleared — STILL invisible
⇒ the RAK is likely NOT ADVERTISING. Leading theory: covey-ui holds the serial
phone-API 24/7 and nRF52 Meshtastic suppresses BLE while the serial API is in use
(would also date "when it used to work" to before the always-on UI / the 08-14
2.7.26+bootloader reflash, which also wiped bonds). **Decisive 30 s test ready:
stop covey-ui, {user} scans from the app, restart.** Related: outgoing messages sent
from the Android app never appeared in COVEY's UI — investigated, mechanism
narrowed (meshtastic lib discards own-node carbon copies with from==0 BEFORE
publish — but COVEY's journal shows ZERO such drops, so more likely the firmware
never cc'd the serial session at all); definitive answer needs the phone connected,
i.e. blocked on the BT fix. Full agent report should be re-run if lost; key files:
covey_ui/mesh_meshtastic.py:720 (_on_receive), :1004 (send_text local echo),
meshtastic lib mesh_interface.py:1574 (the own-packet drop).

## 🌅 2026-08-21: THE UPLOAD REDESIGN — where it stands, exactly

**The phone RUNS the chunked build** (safe daily firmware: upload paths are inert
until `up on`; serial `heap` command in; WiFi/SIP/mesh untouched). **NOT accepted,
NOT done** — read this before continuing.

**Done + proven:**
- `chunk_proto.h` (verdicts/name-sanitize/hex/CRC32, host-tested), `/chunk` GET+POST
  on the transfer server, page JS chunked sender (4 KB stop-and-wait, CRC32, resync,
  honest retries), `tools/chunk_push.py` (bench pusher: --corrupt/--restart-at/
  --resume/--gap all proven against the mock), serial `heap`, per-request + per-file
  heap telemetry, chunk-aware breaker (8 s non-escalating pauses for chunk-paced
  traffic; resume line 7168 = backpressure line).
- **Mock-proven byte-perfect in a real browser** (scratchpad mock_phone.py): 3-file
  batch through 15% injected failures, CRC-corrupt piece 422+recovery, mid-file
  abandon + true resume (tail-only resend). JS crc32 == zlib == chunk_proto.h.
- Measured (via `heap` + graded probes): request machinery transient ~10 KB; 4 KB
  body costs nothing more, 8 KB dips 4 KB, 16 KB grinds largest to 636 B; each
  request fragments largest ~1.6–2 KB; server recycle heals it; boot largest is a
  stable ~19 K (the "9–10 K boots" were UPTIME fragmentation, mystery closed).

**The hardware verdict that redirects the work:** two real runs (unpaced, and paced
150 ms/piece) both died the same way — hundreds of per-piece CONNECTIONS accumulate
TIME_WAIT pcbs (120 s tail, heap-backed) + per-request String churn, largest pinned
~5 K, breaker flapping, client gives up. **Pacing does not save the WebServer path;
its per-request cost is the killer.** ▶ NEXT: a raw single-connection chunk endpoint
(one TCP connection per file/session, raw bodies read incrementally at OUR pace into
the PSRAM block — in-flight bounded by TCP window 5744 + stop-and-wait, so the flood
is impossible by construction; CORS-simple so the page can use it cross-port;
WebServer `/chunk` stays as the fallback). Sketch: non-blocking state machine pumped
from the main loop, 2 s dead-client deadline, piece cap 32 K.

**Review findings — ✅ ALL FIVE PAGE ITEMS (4a-e) AND ITEMS 1-3 WERE FIXED IN 4ff2a18
("The raw transport lands", same day), verified against its diff on 2026-08-26 night.
This list stood mislabeled "OPEN" for five days — the stale-claim trap this repo keeps
documenting. Kept for the reasoning; nothing below is owed:**
1. 🔑 **CONFIRMED from vfs_api.cpp: `File::size()` on an OPEN written file stats the
   PATH — FatFS dir entry updates only on sync/close — so held arithmetic undercounts
   between flushes.** Fix: track durable bytes in RAM; trust size() only at (re)open
   and on closed files. (Applies to the raw server design too.)
2. `/fetch` can TRUNCATE the open batch file via a second FIL (name collision incl.
   the `download.epub` default) → add `chunkFinalize()` at top of handleFetch.
3. Telemetry floors/pieces init only at off==0 → resumed files log garbage floors;
   pieces double-count on 507 (increment precedes flush verdict).
4. Page JS: (a) give-up on one file ABANDONS the rest of the batch (fail() never
   advances); (b) permanent 4xx retried 24×; (c) fetch-less JS browsers get a dead
   page (unconditional preventDefault kills the native-form fallback); (d) SD-fail
   loop: acks between 507s reset tries so a dying card cycles forever (needs a
   no-progress stall counter); (e) zero-byte files skipped but counted as delivered.
5. Fixed already: stale s_chunkSawPiece verdict after an aborted POST.

**Traps (cost real time):**
- ⚠ **Flashing REQUIRES panicwatch STOPPED** — it eats esptool's sync replies; one
  attempt left the phone sitting in download mode. pkill panicwatch → pio upload →
  restart panicwatch.
- ⚠ A vanished client mid-POST on the LEGACY /upload path can wedge the main loop
  INDEFINITELY (flooded RX window keeps the FIN out; parser spins on connected()).
  Chunked pieces are smaller than the TCP window precisely so this cannot happen.
- ⚠ piece_*.bin probe clutter sits in /books (Books list won't show .bin; harmless).
- Book 2 was left TRUNCATED by the failed runs and was re-pulled + verified 3060655
  on 2026-08-21 morning. The corpus books live on COVEY /home/covey/books/.

**Acceptance bar unchanged** (docs/upload-redesign-brief.md): three consecutive
4-book batches over the fast LAN at full speed, zero breaker trips, internal-heap
floor never below 10 KB, then a REAL mobile browser. Then update README's upload
claims (add `heap` to its serial table too) + CHANGELOG.

## 🌙 2026-08-20 (evening) — the upload saga: fast networks were the killer all along

> **▶ NEXT SESSION STARTS HERE: `docs/upload-redesign-brief.md` — {user} approved the
> token spend for the chunked stop-and-wait redesign. The brief is the cold-start
> document: measured anatomy, agreed design, scouted traps, test corpus, acceptance bar.**

**The bug {user} reported as "books upload locked up + panic" unraveled into a 5-act play,
every act measured on hardware.** (1) The STA reconnect gate stranded WiFi during server
sessions — fixed (softAP-only gates). (2) lwIP starvation killed the listener then the
whole stack — circuit breaker added. (3) Breakers v1/v2 both died of resume-threshold
catch-22s (a paused-but-allocated server pins largest below ANY threshold) — v3 frees the
server outright on pause + timer-backed resume. (4) THE REAL ANATOMY: fast links (home
LAN 3 ms RTT; the crash-day phone-to-phone hotspot link) let TCP flood internal heap with
radio RX buffers faster than SD drains — heap measured 14 KB → 868 B in seconds; the slow
work-hotspot uplink had been accidental flow control all along. v4 = mid-upload
BACKPRESSURE (stop consuming ⇒ window closes ⇒ sender stalls) + breaker defers mid-file
(trips only < 3 KB during transfers). Push now SURVIVES but crawls on fast LANs.
(5) **THE ANSWER IS PULL**: /fetch ("paste a download link") lets the phone read at its
own pace — 5 MB books at FULL SPEED, heap intact. Serial `up on books` added so the bench
can start the Books uploader hands-free; delivery ran: Mac python http.server + POST
/fetch per book, straight into /books.

⚠ Bench follow-ups queued: per-request heap anatomy of the push path (why transients run
7-15 KB); breaker v5 lessons — a timer-resume into sub-threshold heap flaps in 3 s
slivers, and a pause landing right after handleFetch can eat the HTTP response after the
download already succeeded (harmless but reads as failure); WiPhone power button does NOT
reboot (screen-off only — a "restart" over serial is esptool --after hard_reset read_mac).
⚠ smsmirror.txt shows as a "book" in the Books list (config file in the .txt-books net) —
filter queued.

## ☀️ 2026-08-20 (day, {user} at work) — GPS half written, wiring sheet, rf_check's first run

- **🔧 THE WIFI AUTO-SWITCH DEADLOCK, found live and fixed (9ad7a36, FLASHED + PROVEN):**
  the switcher's call-guard blocked on CallState::HangUp — a teardown state the END key
  enters from ANY screen (ino:1857) that STICKS with no proxy (BYE can never send). Leave
  WiFi coverage + one END press = the network-restorer blocked by a state that needs the
  network to end. Measured: sip=6 for SIX HOURS, 7,549 EHOSTUNREACH errors, zero scans,
  the saved hotspot in range. Guard now blocks only the six audio-delicate states AND only
  while CONNECTED (no WiFi ⇒ no audio to glitch ⇒ never block). Proven on hardware: fresh
  boot scanned at 2 min, joined '{user}-wifi' by itself; END presses leave it connected.
  ⚠ HEALTH's wifi= field is RAW WiFi.status() (1=NO_SSID_AVAIL, 3=CONNECTED,
  6=DISCONNECTED) — it was misread as a boolean once today; don't repeat that.
  ⚠ Follow-up question, deliberately not changed: should END enter HangUp from screens
  with no call? It plausibly backs call-reject; the guard fix removes its teeth here.
  ✅ **ANSWERED 2026-09-25 (`9213fc0` + its review repair): NO.** END sets HangUp only when
  `gui.inCall()` — live or ringing, so BeingInvited is included and END still REJECTS a
  ringing call. Outside a call HangUp stopped music, zeroed the volumes and stuck with no
  WiFi/account. The one thing it did by accident — the only way out of CallState::Error —
  is now explicit: END in Error retries SIP once (see STATE NOW, "{user}'S CALL").
  ⚠ The phone now runs the post-0.9.7 build (GPS half dormant + Sun screen + GPS toggle
  + this fix) — TONIGHT'S BENCH FLASH IS ALREADY DONE; after assembly just flip
  My node > GPS receiver.

- **The woods backplate is GO**: parts in hand, bench checks tonight. The build sheet is
  `docs/woods-backplate-wiring.svg` — leg-by-leg, every pad named, W1–W19 run list, the
  before-first-power checklist (5V-accepts-charge test, TLV EN pull-up measure via
  R1-divider: R_pu = 4.7k×(VIN−V_EN)/V_EN, pass = V_EN < 0.4 V). RFM95W pads verified
  against the HopeRF DS Figure 2 itself. Docs reconciled: the buck feeds from the
  PowerBoost's 5.2 V, NOT VBAT (BOM superseded the .md; both now agree).
- **UI-first rule adopted ({user}):** features ship with a UI surface; serial = diagnostics.
  Applied same-day: **Meshtastic > Sun & legal light** (countdown-first screen) and
  **My node > GPS receiver** toggle (off / on (no fix yet) / on (fix), same NVS pref).
- **The GPS firmware half is IN and dormant** (commit fe96b66, NOT flashed — soak): nmea
  reader on the stock USER_SERIAL UART2 (GPIO 38/32 @ 9600 = the plate's exact wiring),
  fix < 2 min old slots into resolveReference between chosen waypoint and pin. After
  assembly: `gps on` once over serial, then `gps` for bytes/sentences/fix (bytes up +
  sentences 0 = wrong baud). **No auto-broadcast, on purpose** — announcing stays manual.
- **Morning incident closed**: dropping a pin crashed covey-ui AND the service stayed dead
  (xinit exits 0 → Restart=on-failure blind). Fixed + regression-proven + unit now
  Restart=always, crash-restart proven live. COVEY repo D-110. 'test pin' heard by the
  phone on air; **'vashon' still needs a re-save on COVEY.** `sun` proven live at the pin.
- **rf_check.py ran on hardware for the first time** (remote, covey-ui auto-restored):
  config 8/8 vs baseline (drift RETIRED), and it was counting the RAK's OWN packets as
  receptions — one printed DIRECT — now filtered (COVEY commit 03f9bc9). Zero external
  packets corroborated by the phone: the mesh was genuinely quiet. Side-by-side vs the
  spare RAK remains the September test.
- **Serial bridge hardened**: panicwatch now repo-tracked (`tools/panicwatch.py`) + types a
  wake newline first — first bytes after DFS idle arrive mangled (`pos` arrived as 40
  chars of junk). Plain `printf 'cmd\n' > /tmp/wiphone.cmd` is reliable now.
- Diagnostic truth-telling: `pos` no longer says "no waypoints heard" above a listed
  waypoint (says "none CHOSEN"); reference stays a deliberate choice (Places > pick).
- **📋 QUEUED for the GPS firmware release ({user}, 2026-08-20): publish the complete woods
  backplate build guide on GitHub** — the repo is PUBLIC (verified: private=false, Pages
  live), so the docs are already visible; what's owed is the FINISHED guide: {user}'s
  measured values folded in (TLV EN pull-up, the 5V charge-in result, terminal count),
  the wiring sheet + BOM + firmware setup (`gps on` / My node toggle) linked from the
  README as a build-this page. **Timing: AFTER {user} meters and assembles** — measured
  numbers, not predictions, go in the public guide.

## 🌙 The overnight block ({user} asleep; everything below flashed + verified + pushed)

- **Waypoint DELETE parity was BROKEN and is fixed**: COVEY's delete is a Waypoint with
  an id and NO position (`waypoints.is_delete` — absence of position is the marker,
  because expire==0 means "never"); the phone's parser rejected exactly that form. Now:
  id-only packet ⇒ delete (owner-checked); local once-a-minute expiry sweep too. ⚠ v3's
  waypoint tail had NO size field in the DB header — DB is now **v4** (every struct size
  recorded; v1/v2/v3 migrate, v3's places re-learn on air). {user} must RE-SAVE 'vashon'.
- **`unread clear` survives reboots now** — Messages' part1/part2 caches were storing
  stale "u" flags back over the direct file writes (`dropPartitionCaches()` guards every
  such write; a kept log line separates failed-write from re-marked). ⚠ My first two
  diagnoses (cache theory applied too narrowly, then a LOG-READING error — awk matched
  the FIRST occurrence in a 38k-line log) were wrong before the fenced reboot test
  proved the fix; fence your greps.
- **Text overflow: 16 sweep findings + 7 review follow-ups, all fixed.** guiDrawEllipsized
  everywhere (UTF-8-safe, returns drawn width); header draws title LAST sized to what
  icons+clock leave; ⚠ MenuOptionIconnedTimed (chats list) has ITS OWN redraw — fixing
  the other two option classes does not touch it. sipThreadIdentity() is the ONE
  grouping rule (letters-only hash tail — hex would feed smsMirrorDigits and break
  idempotency); sipDisplayLabel() the ONE display rule.
- **`sun`** (serial): NOAA solar math in sun_times.{h,cpp}, 21 host checks incl. almanac
  anchors; verified on device (Seattle 08-20: 05:39/06:12/20:13/20:47). ⚠ Times are UTC
  minutes 0..1439 — evening events WRAP to the adjacent UTC date; unwrap for spans.
  Clock-face UI deliberately left for {user}'s daylight taste.
- **COVEY: position broadcasts can ride a chosen channel** (Node & Location row, suite
  41, deployed, covey-ui active). Default UNCHANGED (still public LongFast) until {user}
  flips it — the row says "every radio can read this" so the flip is informed. The
  on/off half already existed (Auto-broadcast off/1m/5m/15m).
- COVEY map ruler (RUL) from earlier tonight: suite 40, deployed.

**▶ MORNING CHECKLIST FOR {user}:** (1) re-save the camp waypoint on COVEY (v4 migration
dropped it — expect it in Places in seconds); (2) flip Position channel to a private one
if you want (Settings > Node & Location); (3) `sun` works from serial now, and lights up
on the phone once a waypoint/pin exists; (4) the overnight soak monitor logged only my
own reflashes. Wishlist still open: Files icon/main-menu, clock-face legal light,
wallpapers, flasher publish after soak.

## 📍 0.9.7 (2026-08-19 evening): positions, places, and the unread-icon truth

**What it does:** the phone (no GPS) learns node positions (port 3) and waypoints
(port 8) off the air; Meshtastic > Places lists them; Nodes shows "1.4km NE" from a
chosen reference; "I'm here (tell the mesh)" pins the phone at a waypoint and announces
one Position (on a PRIVATE channel when one exists). Serial: `pos`, `unread`. DB v3
(v1/v2 migrate in place — verified live: "migrated 32 node(s) from v2", keys kept).
Crypto/geometry host-proven (test_pos, python-checked vectors).

**A 36-agent adversarial review ran BEFORE flash and confirmed 9 findings + resurrected
3 whose verifiers died on a network/limit outage (⚠ lesson: a finding whose verifier
never ran is NOT refuted — read the workflow failures list). All 12 fixed pre-release;
the CHANGELOG's review section lists them. Standouts: protobuf tags are varints (stock
precision_bits broke the one-byte reader), locked-waypoint ownership, node-slot
recycling leaked position+pubkey, serial help() truncated at 192B for releases.**

**Verified on hardware tonight:** boot clean, v2→v3 migration kept 32 nodes + PKC key,
`unread` found and named the white-icon cause (31 real unread from the re-mirrored
history, oldest in the 14255550142 thread — buried past the old 120-message read-marking
cap; thread-open now scans full depth while unread exist), live PKC key-learns from two
more nodes.

**⚠ AWAITING ({user}'s move):** he'll add/re-save a waypoint on COVEY's map — expect
`MESH WAYPOINT: '<name>'` on serial, the place in Places, `pos` showing it. Then pick
reference / "I'm here" and see the phone on COVEY's map. COVEY's position broadcasts
need its GPS to have a fix before Nodes shows "COVEY 1.4km NE".

**🛑 PRIVACY FINDING FOR COVEY (not fixed, {user} to decide):** the RAK's PRIMARY channel
is the stock default LongFast (hash 0x08, well-known key — proven by ACK interop), and
covey-ui's `sendPosition` uses channel index 0 ⇒ **COVEY broadcasts {user}'s GPS position
every 5 min readable by ANY Meshtastic radio in range.** Waypoints are safe (mapview
picks channels deliberately). Fix = covey-ui sends positions on a private channel
(+ the Settings toggle {user} asked about). The phone's own pin announce already prefers
a private channel.

## 🔐 0.9.6 (2026-08-19): Meshtastic PKC — DMs can work again. What was DONE and what is NOT YET PROVEN.

**Why:** Meshtastic 2.5+ requires public-key crypto for DMs BOTH WAYS — the RAK refuses
to send to a keyless phone (`PKI_SEND_FAIL_PUBLIC_KEY`, the 2026-08-19 finding), and —
new finding, from reading 2.7's Router.cpp — it also silently DROPS the legacy
channel-encrypted text DMs this phone was SENDING ("Rejecting legacy DM"). So 0.9.5 DMs
were dead in both directions and only the RAK→phone direction was known.

**The scheme** (every fact verified against meshtastic/firmware master source
2026-08-19, files in CryptoEngine.cpp / Router.cpp / mesh.proto — not from docs):
- session key = SHA-256(X25519(my priv, peer pub)); AES-256-CCM (L=2, M=8, no AAD).
- 13-byte nonce: packetId u32 LE | extraNonce | fromNode | 0x00. Frame:
  ciphertext | tag(8) | extraNonce(4). On-air channel-hash byte = 0x00.
- Keys travel in NodeInfo (User field 8), TOFU. **Stock NEVER overwrites a stored
  key** — hence the phone's keypair is PERMANENT (NVS `wpmesh/pkipriv`): if it is ever
  lost (chip erase!), every peer must delete/re-learn this node or DMs die SILENTLY.
- ACKs (Routing{error_reason} + request_id) are CHANNEL-encrypted on the primary even
  for PKI DMs — stock excludes ROUTING from PKC. The phone now ACKs DMs it accepts and
  re-ACKs retransmissions (dupes of ids in `recentAckIds`).

**Where the code lives:** `mesh_pki.{h,cpp}` (self-contained crypto; vendored
curve25519-donna + tiny-AES-c under `WiPhone/src/crypto/` — provenance headers in each
file), integration in meshtastic_service.{h,cpp}, serial commands in serial_cmd.cpp.
`tests/test_pki.cpp` + `tests/vectors_pki.h` (regen: `tools/gen_pki_vectors.py`, needs
`pip install cryptography`) prove byte-identical frames against Python both directions.

**RAM ({user} asked):** +1,688 B static measured (84,020 → 85,708), ZERO heap. The X25519
derive costs ~3 KB transient STACK, so it only ever runs at superloop depth: RX
key-learn warms a 2-entry session-key cache; a GUI-depth DM with a cold cache is QUEUED
one loop tick (`pendingDm`), never derived inline. On-hardware floor after boot (which
includes one derive): 3,020 B free (`pki` prints it live).

**Verified on hardware tonight:** boots clean, heap unchanged, node DB migrated v1→v2
(names kept, `MESH DB: 8 nodes`), keypair generated + persisted, announce carries the
key (`MESH ANNOUNCE SENT ... pki=in packet`), `pki` command works.

**🎉 PROVEN ON AIR, BOTH DIRECTIONS (2026-08-19 ~12:00, real hardware, no simulation):**
1. The RAK's key arrived with its periodic NodeInfo (~90 min after flash): phone shows
   `!62b8d2fd '{user} H New Device' key HepOEI6R...`. ⚠ Lesson: the RAK does NOT answer
   `announce` want_response requests (stock suppresses replies to recently-heard nodes) —
   the ≤3 h periodic broadcast is the realistic key-exchange path after a fresh flash.
2. **Phone→RAK:** serial `dm !62b8d2fd <text>` was decrypted by the 2.7 RAK, displayed
   in COVEY's Messages, and persisted (verified in `/root/.covey/messages.json` over
   SSH — note covey-ui runs as ROOT, so its state is under /root/.covey, not ~covey).
3. **RAK→phone:** {user} saw it pop and replied "works!" FROM COVEY — the phone logged
   `MESH PKI DM from !62b8d2fd id=0x07b69a66 (6B) ACKed`, stored it in Chats, and its
   ACK closes the loop on COVEY's side. "Meshtastic 2.7 refuses ALL DMs" is CLOSED.
4. Follow-up flashed the same hour: phone-originated DMs now set want_ack, so the RAK's
   `MESH DM ACK ... err=0` line is per-message delivery proof for OUR sends too (the
   first test send got no ACK purely because want_ack was 0 — by design, now changed).

**Still open:** after soak, rebuild the webflasher (`tools/make_webflasher.sh` +
publish) so the page serves 0.9.6.

**Honest limitations, on purpose:** phone-originated DMs don't set want_ack (no
retransmit machinery — ACKs are logged, not shown in UI); DM to a keyless node still
goes legacy (works for pre-2.5 peers, dropped by 2.5+, log_e says so); a >220-char DM is
refused (PKI overhead + the 255 B LoRa frame — stock refuses the same length). A
mismatch-flagged key needs "Clear nodes" (Meshtastic app) or nothing changes.

## ✅ 0.9.5 (2026-08-19): the erase-recovery release
The web flasher is live and DATA-SAFE (esptool-js direct; erase only as an explicit
option — see docs/webflasher-plan.md for the incident that forced that). Files app,
number completion (sipCompleteAddress — sees through sip:/+ prefixes; needs the ACTIVE
account), serial toolbox (chan/chans/sip/bookpage), reader margin fix, booksync PSK
ROTATED (passcode 2222 both sides; the leaked key is dead), book sync re-verified
end-to-end by {user}, missed calls + phonebook confirm. 🔑 Field lesson worth keeping:
"texting dead + completion dead + mirror refusing" = ONE cause, no active SIP account —
the `sip` serial command prints it in one line.

## ✅ 2026-08-19 (day): reader fix proven, Files app, web flasher, courtesies

- **The e-reader margin bug is FIXED and confirmed by {user}** ("works well"). Mechanism:
  SmoothFont::textWidth bills the LAST glyph ink-only; the layouter measured one char at a
  time so EVERY char was "last" → lines packed 1-2 chars over budget → the draw clamped
  them left, beheading the first character. `fontMeasure` (app_books.cpp) now bills every
  glyph its advance via a sentinel space. ⚠ Any NEW measure must reuse it (exported in
  app_books.h) — a second naive measure reintroduces the bug.
- **Files app** (Menu > Tools > Files): browse/view/upload-anywhere + clipboard Copy/Move
  with mark-never-breaks-the-file semantics (rename-else-copy-verify-then-delete, refuse
  overwrite, partial dst removed on failure), delete behind Cancel-first confirm. The
  `bookpage` serial probe cornered the reader bug on its first use.
- **Web flasher**: `webflasher/` prototype (ESP Web Tools, the t-ui pattern) + staging
  script + `docs/webflasher-plan.md`. 🛑 NOT published yet — go-live steps in the plan.
  🔑 Update discovery: the PAGE is the check; the phone-side idea from {user}'s notes cannot
  be a GitHub poll (TLS wall) and must NOT lean on COVEY (only {user} has one — his call,
  2026-08-19). Planned instead: an offline build-age nudge once the URL is live.
- **Phonebook Delete asks first** (confirm menu REBUILT each entry — a reused widget
  remembered its highlight and would have recreated the one-press erase; review catch).
- **Missed calls** show amber on the clock, observed purely inside setSipState (ring arms;
  Call/Accept/DECLINE disarm — declined-counts-as-missed was a review catch too); cleared
  by opening the dialer or phonebook.
- Still open on the wishlist: Files icon + main-menu promotion, COVEY wallpaper, the
  build-age nudge + Firmware-update screen text at flasher go-live.

## ✅ 0.9.4 — THE AUDIT RELEASE (2026-08-19). What changed and what to know.

A 38-agent read-only audit produced 30 verified findings (the full list with change specs
is committed at `docs/audit-2026-08-18-confirmed.json`); the high-value low-risk set is
implemented, adversarially re-reviewed (which caught a major defect in the first draft of
the WiFi backoff — see below), built, flashed, and boot-verified. The CHANGELOG's 0.9.4
section is the user-facing summary. Engineer's notes:

- **tinySIP hardening**: vsnprintf in Connection::print/printf; header-count cap. ⚠ A
  >100-header message is REJECTED WHOLE (the comment says so honestly now).
- **findMessage**: `IniFile*&` out-param — the old `IniFile&` "reseating" shallow-copied
  part2 over part1 (aliased Sections, latent double free). Three call sites bind
  `IniFile& ini = *part` inside the found-guard.
- **WiFi out-of-range backoff** (WiPhone.ino, reconnect block): 5×20 s then 180 s, radio
  quiesced 30 s after each eased attempt, instant retry on screen wake (10 s spacing).
  🔑 **The quiesce consults `lastWifiConnectAttemptMs()`** (stamped inside connectToWiFi,
  i.e. by EVERY join path) — the first draft disconnected joins the auto-switcher or the
  user had just started; the review caught it. Do not "simplify" that check away.
- **Mesh SPI poll** rate-limited to 10 ms (RX_DONE latches; packets are >100 ms on air).
- **Idle tick**: vTaskDelay(5) when the CPU-gate predicate says idle AND
  `smsMirrorPollBusy()` is false (a mid-transfer poll keeps the 1 ms tick).
- **QoL**: Reboot is a confirm submenu (menu[42] — the count must match the rows);
  redial via `controlState.lastDialed` (OK on empty dialer prefills, second OK calls);
  battery % on the clock (transparency-bracketed — opaque smooth-font cells paint black
  boxes over the wallpaper); `#` on the clock opens Messages when the envelope shows
  (`ENTER_MESSAGES_APP` 0x20); Reply returns to the THREAD via `returnState`.

## 🔎 OPEN: the e-reader "characters cut off at margins" report (photo, 2026-08-18)

{user}'s photo shows page 16/90 of the BattleTech book with the first characters of two
continued-paragraph lines missing ("**c**alled", "his **h**ead"). **The layout engine is
exonerated**: a host probe ran the real epub_parse + book_layout over the real book — all
90 chapters, proportional measure — and every non-whitespace byte lands in exactly one
line (structurally, the only byte nextStart can skip is a space). The source bytes are
clean plain ASCII. So the loss happens ON DEVICE between layout and pixels — real font
metrics or the render path, where host tests cannot reach.

**▶ The instrument is already flashed: serial command `bookpage`** dumps the open reader
page — per line: text offset, byte length, measured px, first raw byte, rendered string.
**Next time the reader is open on a damaged page** (the position is saved, so opening the
book lands right there), run it via the watcher: `echo bookpage > /tmp/wiphone.cmd`, read
the serial log. If a line's `off` skips the missing char → device-side layout/measure; if
`off` is right but the rendered string lacks it → bookRenderRun/drawString. One paste
settles it. (Probe plumbing: `booksDebugDumpPage()` in app_books.cpp via a live-instance
pointer; prints a hint if no book is open.)

🎉 **ALL FOUR OPEN FAULTS ARE FIXED AND THE FIX IS ON THE PHONE.** The evening session
below closed everything the morning audit left open — read "WHAT 0.9.3 CHANGED" first.
The morning's fault write-ups are kept beneath it because their reasoning is still the
best map of these subsystems.

## ✅ WHAT 0.9.3 CHANGED (2026-08-18 evening) — all deployed, most verified on hardware

**Fault 1 (COVEY receives nothing) — fixed, and the real mechanism was NEITHER suspect.**
The 45 s timeout was deployed and the poller STILL saw nothing new. 🔑 **`getSMS` applies
a DEFAULT `limit` of 50 and keeps the OLDEST 50 of the date window** — the day the window
outgrew 50 messages, polls "succeeded" with the newest silently absent. Plus the server
stamps dates in the ACCOUNT's timezone (~3 h ahead), so an evening text is dated tomorrow
and a same-day `to=` missed it. Fixed in COVEY's `siptext.py` (`limit=9999`, `to`+1 day).
**Proven end to end: {user}'s mobile → VoIP.ms → COVEY poller → LAN mirror → this phone's
store (`poll ok: 2 new`, `since=110050232`).**

**Fault 2 (texts land mid-thread) — fixed at both layers.**
- **A sentinel-repair pass in `Messages::load()`**: when the clock is known, partitions
  whose index `t2 == ffffffff` get their unknown-time messages stamped with distinct
  ascending times ending at the sync moment (arrival order preserved — `reorderLast`
  files equal keys in arrival order), then the partition re-sorts and t1/t2 recompute.
  ✅ **Fired on hardware first boot: "MSG: repaired 6 unknown-time message(s) in
  partition 2"** — six real messages un-pinned from "newest forever".
- **`msgCompare` never returns 0 now**: ties break on the VoIP.ms id when both carry one
  (which also puts COVEY's clamp-stamped catch-up bursts in true order), then on
  insertion sequence. `threadCompare` ties break on peer. Two openings of one thread can
  no longer disagree.
- `getAckTime()` reads `"a"` (was `"t"`).

**Fault 3 (no vibration on incoming text) — fixed structurally.**
`smsMirrorIngestLine()` latches news flags; the MAIN LOOP is the single announcer for
BOTH transports (`smsMirrorTakeNews()`): immediate buzz for an inbound arrival (Text
messages setting), NEW_MESSAGE_EVENT coalesced to ≥700 ms so a burst is a handful of
snapshot rebuilds, not one per record. The old notify inside meshService.loop() is gone
(it would double-buzz). The motor now fires BEFORE the pop (kills the I2C-disturbance
theory outright), and a latched-`ringing` suppression logs itself.
⚠ **The buzz is the one thing NOT yet verified on hardware** — it needs a genuinely
inbound text arriving via the mirror. Ask {user} to text the number and listen.

**Fault 4 (reboots opening/scrolling conversations) — every ranked cause closed.**
- 🔑 **NanoIni `Section`/`KeyValue` objects now allocate in PSRAM** (operator new, the
  AbstractWidget pattern; MessageData inherits it). The whole-partition parse — ~700
  internal blocks per file — now costs internal heap nothing. This was confirmed live
  first: the LAN mirror's catch-up burst aborted the phone mid-ingest
  (`NanoIni::parse → loadPartition → preload → ingestMirrored`, reset_reason=4, decoded
  by the watcher) minutes after COVEY's fix made the mirror flow again.
- **`preload()` break fix**: a cached part1 no longer drags part2 into RAM for a window
  that fits in one partition (the break sat inside the reload branch); part2's partition
  number is read off part2 (was part1).
- **`MultilineTextWidget::redraw()` allocates nothing** — the two unchecked per-redraw
  strdups (up to 13×/scroll keypress) are in-place char swaps now.
- **`setText()` checks `allocateMore()`** and truncates instead of writing 4 bytes past
  the row-pointer block on OOM; `appendText()` checks its malloc; thread rows and the
  row-pointer array live in PSRAM (`extStrndup`/`extRealloc`).
- **The LAN poller persists its `since=` high-water mark** to `/roms/smsmirror.since`,
  so a reboot no longer re-fetches and re-ingests the entire account history into the
  most fragile minutes of boot. Delete the file to force a full resync on purpose.

**Housekeeping:** ChoiceWidget's per-keypress `log_e` spam (incl. "MESUT") deleted;
`FIRMWARE_VERSION` 0.9.2 → 0.9.3; CHANGELOG released section written.

**The adversarial review round (same evening) — a 20-agent multi-lens review of the diff
confirmed 14 findings, and three were regressions in the fixes themselves. All fixed,
rebuilt, reflashed (`260e39b`). The ones worth remembering:**
- 🛑 **Persisting the since-id would have made refused records PERMANENT silent loss** —
  the mark advanced even when ingest returned -1 (no SIP account, unbuildable URI, db
  load failure), and the RAM-only mark's reboot-refetch was the thing quietly healing
  that. The mark now advances only past accepted records. **The general lesson: before
  persisting any high-water mark, ask what the old full-rescan was silently repairing.**
- The sentinel repair now lets **the index follow the file**: a failed partition store no
  longer clears the ffffffff flag that makes the repair run; a stale flag (power loss
  between the two stores) heals on the next load.
- `msgCompare`'s both-have-vids tiebreak was **intransitive** across mixed vid/no-vid
  equal-time blocks — UB for qsort, the exact nondeterminism it was written to remove.
  Now strictly lexicographic on (has-vid, vid, seq).
- The buzz latch is gated on the record being **recent** (10 min, clock known) so a
  full resync of a fresh store does not run the motor for the whole catch-up.
- The mirror poll's read deadline is a **stall** timeout (re-armed on progress, 120 s
  absolute cap): the first-ever catch-up processed 61 records slower than the old 8 s
  whole-response budget and was being declared "timed out" while healthy.

**Verification state:** clean boot, mesh radio + all 5 channels up, SIP registered,
sentinel repair fired, heap `largest` ~24 KB and recovering post-boot. Still to watch:
the buzz on a real inbound, thread order by eye, and a long soak (the honest window is
hours — see "do not characterise heap behaviour from anything under an hour").

---

## 🗂 THE MORNING AUDIT (kept for the reasoning; every fault below is FIXED as of 0.9.3)

---

## ☎️ THE PHONE IS A PHONE NOW (2026-08-17)

**Calls and texts work, both directions, on a real number.** Proven on hardware against {user}'s
own mobile.

| | |
|---|---|
| **Number** | **425-320-0782** (Everett WA), VoIP.ms |
| **Sub-account** | `100000_sample` · POP **`seattle1.voip.ms`** · UDP-SIP · G.711U only |
| **Outbound call** | ✅ works, audio described as "ok" |
| **Outbound text** | ✅ arrives with the right caller ID |
| **Inbound call** | ✅ rings (and vibrates) |
| **Inbound text** | ✅ arrives |

🔑 **WHAT MADE INBOUND WORK — it was never the firmware.** The DID was still routed to
`[main account]` while the phone registers as the SUB-account, so inbound calls hit an account
with nothing registered on it (**busy signal**) and inbound SMS had nowhere to go (**silence**).
Fixed entirely in the portal: **DID Numbers → Manage DIDs → edit the DID → route SIP/IAX to
`100000_sample`, set POP to Seattle 1, enable SMS and point its delivery at the same
sub-account.** Outbound worked all along because it uses the sub-account's own registration.

⚠ **Phonebook entry that works for BOTH calling and texting:** put the FULL URI in the SIP URI
field — `14255550142@seattle1.voip.ms`. Calling auto-appends the server for a bare number
(`ControlState::setRemoteNameUri`), but **`TinySIP::sendMessage()` uses the address verbatim**,
so a bare number calls fine and silently fails to text.

⚠ **Read the POP hostname off the portal, never infer it.** "Seattle 1" is literally
`seattle1.voip.ms`, NOT the unnumbered `seattle.voip.ms`. That guess cost a correction.

⚠ **Do not add G.722** to the sub-account's codecs. VoIP.ms does not support it between
different POPs, and the firmware only implements PCMU / PCMA / G.722 anyway — G.729a would
connect and give you silence.

### Also shipped 2026-08-17

| | |
|---|---|
| **The 5-second freezes** | Blocking DNS on the main loop — see the section below. Fixed. |
| **Ringer mode** | **Settings > Audio** now has Ring + vibrate / **Vibrate only** / Silent, persisted and loaded at boot. Asked for after a very loud ring at work. |
| **Message icons** | Status bar now says WHICH kind of message waits: **white = SIP**, **green = Meshtastic**, overlapping pair = both. Collapses as you read each kind. ⚠ The white/green mapping was ASSUMED, not specified — swapping is two lines in `GUI::drawMessageIcon()`. |

---

## ▶ PICK UP HERE (2026-08-18) — FOUR OPEN FAULTS, AUDITED

Reported after a day of real use. **Nothing here has been reproduced on a bench.** Every claim
below was read out of the code and then attacked by a second reader; **7 of 12 candidate causes
were REFUTED and are not listed.** What follows survived that. Order matters: fault 1 gates the
others, and several fixes interact.

⚠ **Do the MEASUREMENTS before the fixes.** Each fault below names a check that takes under a
minute and needs no reflash. Four theories were wrong before the last backtrace arrived.

---

### 1. ✅ COVEY RECEIVES NOTHING — CONFIRMED and the fix is STAGED (2026-08-18)
{user} ran **Test Connection from the device**: *"VoIP.ms did not answer in time"* — the read
path timing out at its 12 s default while sends succeed at 45 s. Fix is committed on COVEY's
`main` (read timeout 45 s, an always-visible poll-status strip, a `busy` watchdog), **not yet
deployed** — deploy steps and verify order are in COVEY's `notes/HANDOFF.md`.
⚠ **When it deploys, the mirror starts flowing again — expect fault 4 to fire harder under
the inbound burst, and expect ordering strangeness from the catch-up poll (the `_parse_ts`
clamp stamps old messages with the fetch moment). Neither is new; do not re-diagnose.**

---

### 2. 🟠 A NEW TEXT LANDS MID-THREAD — cause found, and it is NOT what it looks like

🔑 **`saveMessage` stores `0xFFFFFFFF` as the "unknown time" sentinel** (`Storage.cpp:710-712`,
`if (!time) { time--; }`). That is the **MAXIMUM uint32**, so a message with unknown time sorts
as **the newest message in the thread forever**, and every text that arrives afterwards is
placed *above* it — i.e. in the middle. The sentinel was chosen so partition sorting stayed
correct; it is actively wrong as a display order.

⚠ **The phone stores unknown time far more often than you would think.** `everUpdated` is a RAM
flag, so **time is unknown after EVERY reboot until NTP answers** — and stays unknown for a
whole session on any network where NTP is blocked. **Fault 4 manufactures those windows**, and
VoIP.ms pushes queued SMS immediately after REGISTER, i.e. precisely into them.

**MEASUREMENT — free, no reflash, no serial:**
- **Status bar has no `HH:MM`** → the clock is unknown → everything stored this session is
  getting the sentinel. Confirmed on the spot.
- **In the thread, a message drawn with only a name and NO ` · <time>`** → its stored time is in
  the future, i.e. it is a sentinel. `Clock::dateTimeAgo` writes an empty string for any
  future timestamp (`clock.cpp:373-375`), and the header omits the separator when `ago` is
  empty — so the bug hides the very value that identifies it.

**Also true, and worth fixing in the same pass:**
- **`msgCompare` returns 0 for equal timestamps and `qsort` is not stable** — so any block of
  messages sharing a timestamp (the common case) comes out in **arbitrary order that can differ
  between two openings of the same thread**. Needs a deterministic tiebreaker.
- The material for one is already stored and free: `saveMessage` writes the VoIP.ms id under
  `"v"`, but `SipThreadMsg` does not carry it and `addMsg` never reads it.
- 🛑 **Do NOT switch to an id-only sort.** A text received over SIP that COVEY has not yet
  mirrored has **no id at all**, as does every locally-composed send until adoption — an
  id-only key dumps all of them to one end. Time-with-id-tiebreak, or fix the sentinel.
- ⚠ **`buildThread`'s character budget walks backward BY SORT ORDER**, so once the order is
  wrong the genuinely newest text can be pushed behind "*… N older messages not shown*" and
  **vanish from the screen entirely.** Expect that as a second symptom; it is a consequence,
  not a separate bug.

---

### 3. 🟠 NO VIBRATION ON AN INCOMING TEXT — a whole transport is silent

🔑 **The LAN (HTTP) mirror stores an incoming text and announces NOTHING** — no vibration, no
pop, and no `NEW_MESSAGE_EVENT`. It is the only delivery path in the firmware with no
notification at all. The LoRa mirror got one (`meshtastic_service.cpp:682-684`); the LAN poller
never did.

🔑 **And it poisons the other path:** ingest dedups by VoIP.ms id, so **whichever transport
lands a text first wins** — and because LAN is the silent one, a LAN-first delivery
**permanently suppresses the LoRa copy's notification.** That fits the report exactly.

**Fix:** in `sms_mirror_poll.cpp::ingestLine()`, take `bool inbound` from
`smsMirrorIngestLine(line, &inbound)` and call `smsMirrorNotifyArrival()` when `r > 0 &&
inbound`, mirroring the mesh path. Raise `NEW_MESSAGE_EVENT` there too.

⚠ **Confirm from the log you already have before changing anything** — both lines are `log_e`:
`SMSMIRROR poll ok: N new of M` with **N > 0** and **no `NOTIFY: buzz` in the same second** is
the proof.

⚠ **The motor block is gated on `!gui.state.ringing`, and the one log line that would prove it
ran is INSIDE that gate.** Any path leaving `ringing` latched kills every later notification
with no trace. Add an `else` that logs `NOTIFY: suppressed (ringing)`.

⚠ **`NOTIFY: buzz` proves the DECISION, never the motor.** The write goes through the I2C
extender and cannot report failure. **A/B that settles it:** set Text messages to *Vibrate
only* and send one. Buzzes there but not in *Sound + vibrate* → the pop's I2C traffic is
disturbing the extender; fix by driving the motor **before** `playPop`.

---

### 4. 🟠 REBOOTS WHILE SCROLLING / OPENING CONVERSATIONS — my paging fix missed the bulk

🛑 **`SCAN_PAGE = 10` bounds only the `preloaded` DEEP COPIES. The partition FILES those copies
are read out of are still loaded WHOLE**, and every INI section and key-value in them is a
separate `operator new` from the **internal** heap. At `PARTITION_SIZE = 100` that is roughly
**700 live internal blocks per partition**. Paging bounded the wrong thing.

Ranked, all read-and-certain unless noted:

1. **Whole-partition parse** (above). Measure first — the existing `appHeapProbe()` already
   spans it: it runs at the end of `enterApp()`, and `MessagesApp`'s constructor does
   `buildChats()` → `sipThreadsBuild()` → `preload()` → `loadPartition()`. **Reboot, open
   Messages, read `largest` either side in `health.log`.** No code change needed to measure.
2. **`preload()` parses a SECOND partition it never reads**, on every page after the first: the
   `break` for "only one partition needed" sits *inside* the branch that loads part1, so a
   cached part1 falls through and loads part2 unconditionally. **Doubles item 1 for every page.**
   (`Storage.cpp:674` also reads the partition number off the wrong object while you are there.)
3. 🔑 **`MultilineTextWidget::redraw()` calls `strdup()` twice unchecked and dereferences the
   result on the next line** (`GUI.cpp:11416`, `:11430`). In the thread view nearly every row
   ends in `\n`, so this runs **up to 13 times per redraw — on every scroll keypress.** This is
   **the only finding whose trigger is literally "while scrolling"**, which is what {user}
   reported. Guard both, or drop the allocations (one wants to omit a trailing character, the
   other wants a prefix width — a length-limited `textWidth` does both).
4. **`setText()` ignores `allocateMore()`'s return value** and writes `rowsDyn[cursRow]`
   regardless; when the pointer realloc fails, `cursRow == maxRows` and the store goes **4 bytes
   past the end of the block.** An OOM becomes **silent heap corruption** — which is exactly why
   such a reboot would produce no decodable backtrace.
5. *(inference)* The per-row `strndup` load: ~90–115 small internal allocations per thread open
   plus a 10→20→40→80→160 realloc ladder. ⚠ **If item 1 shows the partitions are the bulk, LEAVE
   `THREAD_TEXT_BUDGET` ALONE** — it was already cut once on a wrong diagnosis and restored, and
   cutting it again would repeat that mistake. The right fix is a **PSRAM row allocator**.
6. *(inference)* An inbound burst multiplies all of the above: each `NEW_MESSAGE_EVENT` rebuilds
   both the chats menu and the thread widget, each rebuild re-runs four 120-deep scans. **Fixing
   fault 1 will make fault 4 fire harder.** Coalesce with a dirty flag.

**Ruled out**, so nobody spends a fix there: `getText()`'s large malloc and `appendText()`'s
unchecked malloc are real defects but unreachable from opening or scrolling a conversation.

---

### Found in passing — small, unrelated, worth doing
- 🔊 **`ChoiceWidget` logs at `log_e` on every left/right keypress** (`GUI.cpp:10776/10778/
  10788/10794`, including one that just prints `MESUT`). `log_e` is the only level compiled in,
  so leftover debug spam is flooding the field log. **Delete.**
- **`MessageData::getAckTime()` reads `"t"` instead of `"a"`** — returns the message time, not
  the ack time. Dormant (nothing calls it), one character to fix, a live trap for whoever adds
  delivery receipts.
- **Neither the LAN poll nor the LoRa mirror raises `NEW_MESSAGE_EVENT`**, so a mirrored text
  arriving while a thread is open is invisible until you back out and re-enter — and then it
  appears at whatever position the sort key gives it, which **reads as "it appeared in the
  middle"**. This muddies fault 2's report; fix it with fault 3.

### The tool for 3 and 4
```bash
python3 tools/panicwatch.py       # logs every byte, decodes any Backtrace via addr2line
```
(now repo-tracked; the `/tmp/panicwatch.py` copy this doc used to point at survives no reboot)
⚠ **It owns the serial port. Do not open a second monitor** — that killed the previous version
twice, taking the dump with it. Send console commands by writing a line to `/tmp/wiphone.cmd`
(`up on`, `up off`, `sync`, `mirror`).
⚠ **The FIRST bytes of a write after idle get mangled** — with the phone idling (DFS, 80 MHz)
`pos` once arrived as 40 chars of junk while a command 3 s later was clean (2026-08-20). The
watcher now types a sacrificial newline + 250 ms pause before every command, so `printf
'cmd\n' > /tmp/wiphone.cmd` is reliable as-is (the old leading-`\n` trick in the command file
never survived the watcher's `strip()`).

---

## 🛑 THE CONVERSATION-VIEW ABORT — FOUND, FIXED, AND WORTH READING FIRST

**Opening a conversation rebooted the phone.** Four rounds of theorising got it wrong; the
decoded backtrace got it in two minutes. **Read the crash before theorising about the crash**
— this doc already said that, and it was right again.

```
operator new -> __cxa_allocate_exception -> std::terminate -> abort   (reset_reason=4)
  NanoIni::Section::deepCopy / addKeyValue
  MessageData::MessageData
  Messages::preload
  <caller>
```

🔑 **`Messages::preload(dir, offset, count)` DEEP-COPIES each message's INI section into
INTERNAL RAM.** Asking for a large window means that many live section copies, and the
allocation behind them is `operator new`, which **THROWS** on failure. Nothing catches it, so
it becomes `terminate()` → `abort()`. **Same bug class as the `WiFiUDP::parsePacket` abort
written up further down this file.**

⚠ **THE WHOLE-WINDOW FORM IS A TRAP: it works on a small store and aborts on a big one**, so
it survives every test and dies in use. That is why it presented as "one specific conversation
crashes" — it depends on how much is stored in that DIRECTION, not on the thread.

✅ **All three callers are paged now** — 10 sections live at a time with `clearPreloaded()`
between pages, same depth covered:
`sip_threads.cpp` `scanDirection()` + `collect()`, and `GUI.cpp` `MessagesApp::markThreadRead()`.
**If you add a fourth `preload()` caller, PAGE IT.** `grep -n "\.preload(" WiPhone/*.cpp`.

⚠ **The cost is a visible pause opening a conversation** — 12 passes instead of 1, three times
over (chats list, thread, mark-read). `SCAN_PAGE` could rise, but **measure `largest` at a few
page sizes before picking a number**; 120 aborted and 10 is safe, and nobody has found the
edge between them.

⚠ **A wrong diagnosis shipped on the way** and has been reverted: `THREAD_TEXT_BUDGET` was cut
1800 → 600 blaming `MultilineTextWidget`'s per-row internal allocations. `largest` really was
collapsing, but the cause was the deep copies above. **It is back at 1800.** If thread
rendering ever does look like the culprit, the app-open probe in `GUI::enterApp()` prints
`largest` either side of opening the screen — measure, do not assume.

🔑 **THE TOOL THAT ENDED IT.** A serial watcher that logs every byte and runs `addr2line`
against `firmware.elf` the instant `Backtrace:` appears. ⚠ **The first version DIED whenever
anything else opened the port — and took the dump with it, twice.** It reconnects on any error
now, and commands go through a file (`/tmp/wiphone.cmd`) so nothing ever opens a second handle.

---

## 📨 SMS MIRRORING FROM COVEY (2026-08-17) — COVEY'S HALF IS LIVE, THE PHONE'S IS HALF-BUILT

**The plan in item 0 below could not be built as written, and the reason is worth reading
before anything else here.**

### 🛑 THE PHONE CANNOT TALK TO VOIP.MS. NOT A URL PROBLEM — THE OTA MEMORY WALL AGAIN.

"Let the WiPhone poll the same `getSMS` and merge" is impossible on this build. Re-verified
2026-08-17 from the framework's own headers, not from memory:

| | |
|---|---|
| `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` | **16384** — a ~16 KB IN buffer **and** a ~16 KB OUT buffer |
| `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` | **not set** — so none of it may come from PSRAM |
| free internal heap, whole phone | **~31 KB**, largest block **~20.5 KB** on the plateau |

~33 KB of internal heap does not exist here. ⚠ **And the obvious escape hatch is not
available either: mbedtls is 2.16.7**, which predates `MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH`,
so RFC 6066 max-fragment negotiation would **not** shrink those allocations — in 2.16 the
buffers are sized from `MBEDTLS_SSL_MAX_CONTENT_LEN` regardless of what gets negotiated.
Nor is there a cleartext way round it: **`http://voip.ms/api/v1/rest.php` answers 301 to
https** (measured). **Do not re-plan around the phone reaching VoIP.ms directly.**

### ✅ SO COVEY RELAYS — and that turns out to be the better arrangement
`getSMS` returns the **account's** history, so COVEY already holds every text this phone
sent *and* received. Syncing the phone **from COVEY** therefore does more than close the
original one-way gap: it **back-fills texts that arrived while the phone was off**.

**One record, two transports.** `CSM1 <id> <peer> <dir> <ts> <text...>` — see
[`sms_mirror.h`](../WiPhone/sms_mirror.h) for the full reasoning. `id` is the VoIP.ms message
id and is THE dedup key. `text` is last so it holds spaces with no quoting. Worst realistic
case is **198 bytes against MESH_TEXT_LEN 234**, and a single SMS can never exceed 160
characters, so **one record is always one packet — no chunking, ever.**
⚠ **The LAN transport serves these same lines**, not JSON, deliberately: JSON would mean a
JSON parser on a phone with 20 KB of contiguous heap, and two parsers to keep in step.
⚠ **The text is escaped** (`\\` `\n` `\r`) because one raw newline in a last-position field
splits a record in half on a line-oriented transport and takes the next record down with it.

### What is DONE and PROVEN
- **[`sms_mirror.cpp`](../WiPhone/sms_mirror.cpp) + 185 host assertions**, including
  **interop vectors generated by running COVEY's own `smsmirror.py`**
  (`tools/gen_smsmirror_vectors.py`) — the booksync precedent, so a format drift cannot go
  silent. **Tests are now 1,026 across nine suites.**
- **COVEY's LAN endpoint is LIVE and tested against the real store** — 19 real records
  served, `403` with no token and with a wrong token, `404` on a wrong path, `since=` filter
  correct, a garbage `since` does not 500.
- **The mesh channel exists.** A **new** channel `smsmirror` at **index 4** with a fresh
  random 256-bit PSK. Booksync at index 3 verified **byte-identical afterwards**. Radio
  config backed up to `firmware/meshtastic/backups/` in the COVEY repo.
- **The phone's RX hook and ingest are written and compile** (+8 bytes static RAM).

### 🔑 WHY A NEW CHANNEL AND NOT `booksync` — this is a security finding, not a preference
🛑 **The `booksync` channel's PSK is published on the public internet.**
this doc committed the booksync channel's PSK **and** its passcode in plaintext, in
`Nikguy321/wiphone-meshtastic`, **which is public** (confirmed: an unauthenticated GitHub
API request returns 200). ✅ **Both are redacted as of 2026-08-17** — but they are still in
the git HISTORY, so the old key must be treated as compromised until it is rotated. Anyone who reads the repo can decrypt that
channel. Tolerable for reading positions; **not** somewhere to put the content of texts.
**That PSK and passcode should be rotated and scrubbed regardless of this feature.**

The new channel also cost no firmware work — the mesh layer is already generic
(`importChannelUrl()`, `sendChannelMessage(hash, text)`, 8 slots).

### ▶ WHAT IS LEFT, in the order that gets it working
1. **IMPORT THE `smsmirror` CHANNEL ON THE PHONE.** Nothing arrives until this is done.
   🛑 **THE INVITE URL IS NOT WRITTEN DOWN HERE, AND MUST NOT BE.** A Meshtastic invite
   carries the channel PSK inside its base64 fragment, so pasting one into this file would
   publish the key — which is precisely the mistake that makes the `booksync` channel
   worthless (see above). **It travels device to device instead**, and COVEY gained the
   button for it on 2026-08-17:

   | on | do |
   |---|---|
   | **COVEY** | open the **`smsmirror`** chat → the **dots** → **Share channel link** → tap this phone in the node list. It sends the invite as a **DM** (never a broadcast — the link *is* the key). |
   | **this phone** | **Menu > Meshtastic**, open that message, press **OK** — `app_meshtastic.cpp` `MESH_VIEWMSG` calls `applyChannelUrl()`. |

   ✅ **The pass is the phone answering `Applied: 1 channel(s) added.`** That message is the
   only confirmation you get, and it is worth waiting for: it distinguishes an invite that
   was **APPLIED TO THE RADIO** from one merely sitting in a chat thread. That exact
   distinction is what made book sync look broken for a whole session.
2. **The LAN poller does not exist yet**, and ⚠ **it must NOT be a blocking `HTTPClient`
   GET.** The UI is one task; a 2.5 s call every 60 s is the 5-second-freeze bug rebuilt on
   purpose. It needs a **state machine over `WiFiClient` driven from the main loop** — a
   FreeRTOS task is the other option but its stack cannot live in PSRAM, and internal heap
   is the scarce thing. The ingest entry point it needs already exists and is transport-
   neutral: `smsMirrorIngestLine()` in [`sms_mirror_rx.h`](../WiPhone/sms_mirror_rx.h).
   COVEY's token is in its `prefs.json` under `smsmirror_token`; the endpoint is
   `http://<covey>:8087/sms?since=<id>` with header `X-Covey-Token`.
3. **NOTHING HAS CROSSED BETWEEN THE TWO DEVICES YET.** The wire format is proven against
   COVEY's real encoder on the Mac, and COVEY's server is proven against real data, but no
   mirrored text has reached the phone by either path. **Treat the whole ingest path as
   unproven until one does.**

⚠ **THE LAN PATH IS CLEARTEXT** — it must be, the phone has no TLS. The token stops a port
scanner, not somebody sniffing the wifi you are both on. `smsmirror_lan` in COVEY's prefs
turns it off; the LoRa path *is* encrypted, and the two dedup by id so nothing is lost by
running only one. `cryptography` 43 is on the Pi and the phone has AES-CTR in `mesh_crypto`,
so encrypting the LAN body under the channel key is a real option if it is ever wanted.

⚠ **`ingestMirrored()`'s scan is BOUNDED** to the newest 60 messages in the record's own
direction (an unbounded scan means loading every partition). If a mirrored text pairs with a
copy older than that window it is **stored twice**. That needs a burst of >60 messages
between COVEY sending and the phone hearing — outside normal use, but real.

---

## 🧊 THE 5-SECOND UI FREEZES WERE BLOCKING DNS

**A freeze that recovers is not a crash** — a crash reboots. This was the main loop blocked, and
since the whole UI is one task, blocked means frozen. `resolveDomain()` in `Networks.cpp` is
reached from SIP connect, from **RTP setup during a call**, and from NTP, and it had two blocking
calls in series:

1. **A 500 ms mDNS query run for EVERY name**, including public ones. mDNS only answers for
   `.local`, so that half second was guaranteed waste. Every `X not found on local network` line
   in an old log IS that wasted half second.
2. **`lwip_gethostbyname()`**, which retries internally and blocks for SECONDS when DNS is slow
   or unreachable — and **NTP retries every 500 ms** (`TIME_UPDATE_RETRY_DELAY_MS`). Measured:
   `pool.ntp.org` failed to resolve **seven times in two minutes** on a work WiFi.

Fixed by asking mDNS only about names it could answer for, and caching results — **including
failures**, which is the half that matters, since a negative entry makes an unresolvable name
free for `NEG_TTL_MS` no matter how eagerly the caller retries. Measured after: **7 failures per
2 min → 1 per 3 min**, zero wasted mDNS lookups. ⚠ `.local` answers are deliberately NOT cached
(the `wiphone.local` staleness trap). Cleared while there: the WiFi auto-switch scan is already
async and was innocent.

---

## 🛑 THE RESTART MYSTERY IS SOLVED. READ THIS BEFORE ANYTHING ELSE.

**`WiFiUDP::parsePacket()` was both the abort AND the fragmentation.** One function, two
separate failure modes, and between them they account for everything chased for weeks.

```cpp
char * buf = new char[1460];                    // EVERY call, before it knows if data exists
if(!buf){ return 0; }                           // DEAD CODE: new[] throws, never returns null
if ((len = recvfrom(..., MSG_DONTWAIT, ...)) == -1) { delete[] buf; return 0; }
```

**Fault 1 — the abort.** `new[]` throws `std::bad_alloc` on failure. Nothing catches it, so it
becomes `std::terminate()` → `abort()` → **`reset_reason=4`**. The framework's own null-check
never fires because it was written with `malloc` semantics. Proven by decoding the panic
backtrace against `firmware.elf`:

```
loop -> TinySIP::checkCall -> UDP_SIPConnection::available -> WiFiUDP::parsePacket
     -> operator new[] -> __cxa_throw -> std::terminate -> abort
```

**Fault 2 — the ratchet.** It allocates 1,460 bytes *before* checking whether a packet is
waiting — the overwhelmingly common case — then frees it. `TinySIP::checkCall()` drove that
from the main loop **thousands of times a second**. An allocator running flat out, doing
nothing, chopping the heap to pieces. This is why `largest` slid all day and never recovered
while FREE heap sat perfectly still.

**Both fixed.** `udpParsePacketSafe()` in `helpers.h` fronts all six call sites (guard + catch);
`UDP_SIPConnection::available()` polls at **50 ms**. Measured either side, idle, screen off:

| | before | after |
|---|---|---|
| shape | **continuous** ratchet, step after step | two early steps, then a **plateau** |
| 2 min | 22,492 and still falling | 27,836 |
| 28 min | **3,168**, then panic | 20,528 |
| free heap | 31,056 (never moved) | ~31,000 (never moves) |
| `reset_reason=4` | **every 1–3 min** | **none** |

### ✅ PROVEN OVERNIGHT, 2026-08-16→17 — 5.4 HOURS, ZERO PANICS

Idle, screen off, SIP registered, on USB power. `largest` sampled from the health log:

```
up=  0min  27,988
up= 24min  20,528
up= 78min  20,528
up=160min  20,528
up=241min  20,528
up=295min  20,528     <- four and a half hours without moving
up=322min  20,084
```

**It PLATEAUS.** Two steps in the first half hour, then it settles and stays. This was the open
question — whether the remaining drift was a slow slide toward death — and the answer is no.
**1,330 heap dips over the night, `reset_reason=4` count: ZERO.**

The dip histogram is the mechanism, confirmed: **991 of the 1,330 drops are exactly −1,712** —
the `parsePacket` buffer taken and handed straight back, every one recovering in full. That is
the same churn as before, now happening at 20 Hz instead of ~20 kHz.

⚠ **Two corrections this section has already needed, kept as a warning against short windows.**
It once claimed "the ratchet is gone" (from a 4-minute sample) and later "one discrete step,
then flat" (from 11 minutes). The truth needed six hours: **two** early steps, then a genuine
plateau. **Do not characterise heap behaviour on this phone from anything under an hour.**

🔎 **STILL OPEN, LOW PRIORITY — the two early steps.** ~5 KB then ~2 KB, both inside the first
half hour, never recurring. Free heap barely moves across them, so they still look like
fragmentation rather than plain allocation. Not urgent: the plateau at ~20,500 sits against an
abort threshold of 1,460–3,168. The watchpoint prints app / sip / wifi / screen / uptime at the
instant of each drop, so whoever picks this up starts with the context already logged.

⚠ **NOT claimed:** that any of this explains restarts recorded *before* SIP existed. Nothing else
polls UDP continuously — `USE_VIRTUAL_KEYBOARD` is commented out, NTP polls only inside a
request window — so those had a different cause and remain unexplained.

### 🔑 THE TOOL THAT FOUND IT — use it before theorising
A **ratchet watchpoint** at the top of `loop()` in `WiPhone.ino` samples `largest` every 250 ms
and logs ONLY real drops, with context:

```
DROP largest 27884->22492 (-5392) app=16388 sip=1 wifi=3 scr=0 cpu=80MHz up=109s
```

It found the cause on its first run. The 15 s HEALTH line was far too coarse — it says memory
vanished sometime in the last quarter minute, never during what. **Silent on an idle phone.**
Costs a free-list walk 4×/second; delete it or raise `DROP_THRESH` once this is well trusted.

### 💡 THE LESSON, recorded because it cost hours
**The ESP32 prints a backtrace on panic and I spent half a day on heap arithmetic before
reading it.** `addr2line` against `.pio/build/wiphone/firmware.elf` named the exact function in
two minutes. **Read the crash before theorising about the crash.**

```bash
xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/wiphone/firmware.elf 0x... 0x...
```

---

## ▶ WHERE TO PICK UP (2026-08-16)

**{user}'s stated priorities, in his words: STABILITY and BATTERY LIFE on the WiPhone.** The
audio crackle is explicitly **dropped** — *"Covey does music, so worst case it's just not great
on the wiphone and I live with it."* Do not spend time on it.

### 🧠 THE MEMORY PICTURE — where the internal heap actually went

Free internal heap went from **~9,964 to ~31,300** in one day, and `largest` from ~8,136 to
~27,700. Three finds, all located with `xtensa-esp32-elf-nm --size-sort -S firmware.elf`
(addresses starting `3ffc…` are internal DRAM; `3f4…` is flash and irrelevant):

| what | recovered | how |
|---|---|---|
| **A phantom second `Audio` object** | **12,616 B** | `DiagnosticsApp::processEvent()` held a `static Audio` — 12,604 B of BSS — as a workaround for `DIAGNOSTICS_ONLY` builds. `config.h` defines `WIPHONE_PRODUCTION`, so that mode is **off** and the branch can never run, but `static` reserves the memory unconditionally. Now `#ifdef`-guarded. It also reassigned the **global** `audio` pointer, so opening Diagnostics in production would have swapped the phone's audio device. |
| **All widgets → PSRAM** | ~4,200 B per screen | `operator new` on `AbstractWidget`, the root of the hierarchy, covers all ~163 widget allocations in one place. Settings > SIP accounts went **−4,236 → +0** per open. ⚠ Moving just the app OBJECT (the BooksApp trick) recovered only 460 of those bytes — the object was never the cost, its dozen widgets were. |
| **The UDP poll churn** | the ratchet itself | see the section above |

**What is left in internal DRAM, and why it stays:** `setup()::audio_local` (12,604 B — the real
one), the global `sip` (5,364 B), WiFi's `g_cnxMgr`, `meshService`. ⚠ **The `Audio` object should
NOT be moved to PSRAM** — it is I2S/DMA-adjacent and timing-critical.

⚠ **The Game Boy owns 16 KB of internal RAM that CANNOT be moved:** an 8,192-byte blit task
stack, a 4,096-byte emulator stack, and a 4,096-byte audio buffer. **FreeRTOS stacks physically
cannot run from PSRAM on the ESP32.** The ROM and both framebuffers are already correctly in
PSRAM. This is why **`sipMayPoll()` stops SIP polling while the emulator is on screen** — they
compete for memory neither can give up. A live call outranks the emulator.

### ⚠ A DEFECT LEFT DELIBERATELY UNFIXED
`TFT_eSprite::esp32Calloc()` tries **`calloc()` first and PSRAM only as a fallback** — backwards
on a device with 3.6 MB of PSRAM and ~28 KB of internal heap. It is real, and it is **not**
fixed, because a 14-minute soak with the screen forced permanently awake (`SCREEN_ALWAYS_ON_TEST`
in `config.h`, left in place but off) showed **screen-on alone does not ratchet the heap**:
`largest` drifted −1,792 and then sat flat. The sprite sites are the Ackman game and the design
demo, neither of which gets used. Flipping the priority would slow every sprite write and cost
the emulator frame rate to fix something that is not happening. **If evidence ever points back
here, use a size threshold — big allocations to PSRAM, small hot ones internal — not a blanket
flip.**

### ✅ Shipped 2026-08-16 — typing, honest labels, and OTA switched off at source

**Nothing here is queued work; it is all done, flashed and pushed.** Listed because three of
the four are behaviour changes you will notice in the hand.

| | |
|---|---|
| **Typing** | **The D-pad centre now commits the highlighted multi-tap letter and is consumed doing it** ([`GUI.cpp`](../WiPhone/GUI.cpp), `alphanumericInputEvent`). `AAA` is tap-OK-tap-OK instead of two 2 s waits. The timeout is UNCHANGED — OK is an extra way to commit, not a replacement. Only `WIPHONE_KEY_OK` is taken, so **CALL and the top-left Send still commit AND send in one press**. ⚠ On mesh compose, sending after typing is now two OK presses (commit, then send) or one CALL. |
| **Labels** | Four screens said **"Cancel"** on a key that is **backspace** (mesh compose, edit name, short name, book sync editor). Now **"Clear"**, matching what GUI.cpp already used everywhere. Cancel is **END**, the key below Back. |
| **OTA screen** | **Settings > Firmware update** is now a scrollable USB how-to, not a form. Up/down = line, left/right = page, Back exits. Text is string literals drawn from flash: **zero heap**, deliberately — `MultilineTextWidget` would `strdup` a row per line on every open. **896 B less static RAM, 2.5 KB less flash** than the form it replaced. |
| **OTA at boot** | **The every-boot TLS attempt is gone.** Gated on **`OTA_TRANSPORT_AVAILABLE` (0) in `WiPhone/ota.h`** — flip to 1 only when the TRANSPORT changes. `start_ssl_client: -1` no longer appears in the boot log. |

⚠ **Two traps worth keeping, found while doing this:**
- **`ota.updateExists()` is the NETWORK call** and it sat to the **LEFT** of the cheap local
  predicates, so `&&` short-circuiting never protected it. Fixed underneath the gate for
  whenever OTA is real again.
- **`autoUpdateEnabled()` DEFAULTS TO TRUE** — it returns false only if the ini literally says
  `"no"`. So "just turn auto-update off" was never a fix, and is now impossible anyway because
  the screen that set it is gone. That is why the gate is a compile-time switch, not a setting.

📊 **Measured either side of the OTA-at-boot change:** fresh-boot `largest` went **14,388 →
15,860**. ⚠ **One sample each and NOT under identical conditions** (the second was screen-on at
240 MHz, which normally makes `largest` worse) — the direction is trustworthy, the exact number
is not.

### ▶ THE ACTUAL OPEN ITEMS

0. 🔄 **SUPERSEDED 2026-08-17 — see "SMS MIRRORING FROM COVEY" near the top of this doc.**
   The plan below ("the fix is for the WiPhone to poll the same `getSMS` and merge") **cannot
   be built**: the phone has no working TLS and cannot reach the VoIP.ms API at all. COVEY
   relays instead, over the LAN and over LoRa. COVEY's half is live; the phone's RX hook and
   ingest are written; **the LAN poller and the channel import are still to do.** The
   measured VoIP.ms facts below are all still true and still worth not re-deriving.

   **What COVEY does, and why it does not touch this phone.** COVEY does **NOT** register
   SIP. It uses the VoIP.ms **REST API** (`https://voip.ms/api/v1/rest.php`, `getSMS` /
   `sendSMS`) with the account email + an API password, IP allow-list opened to `0.0.0.0`.
   That was deliberate: VoIP.ms overwrites a registration when the same sub-account is used
   twice, so registering COVEY as `100000_sample` would have **taken the registration off
   this phone** and killed its inbound calls and texts. Nothing in the portal changed —
   the DID still routes SMS to this sub-account, and this phone's SIP is untouched.

   **The gap to close here.** `getSMS` returns the ACCOUNT's history, so COVEY sees both
   directions including everything this phone sends and receives. Nothing pushes COVEY's
   outbound to this phone, so **a text sent from COVEY never appears in this phone's
   threads.** The fix is for the WiPhone to poll the same `getSMS` and merge, which pairs
   naturally with item 1 below — the conversation-style rewrite has to touch the storage
   layer anyway, and merging a second source is much cheaper to design in now than to
   retrofit afterwards.

   ⚠ **THREE THINGS MEASURED ON COVEY THAT WILL BITE HERE TOO — do not re-derive them:**
   - **`type=0` is SENT and `type=1` is RECEIVED**, the opposite of what the name suggests.
     Established against messages whose origin was known independently (the "test fr" /
     "om covey 2" pair typed on COVEY, and a text sent from this phone, are both type=0;
     every reply from the mobile is type=1). Getting it backwards makes the unread count
     permanently zero and draws received texts as if you sent them.
   - **`limit` returns the OLDEST messages, not the newest.** A limit-only query silently
     stops returning new messages once history outgrows it, while still answering
     `"status":"success"`. Bound the query with a `from`/`to` date window instead.
   - **VoIP.ms answers HTTP 403 to a request with no User-Agent header** (Python's default
     `Python-urllib/3.x` is refused outright), before the API is reached, so there is no
     status string to explain it. Send some User-Agent.
   - Timestamps did not reconcile to any single offset on COVEY; it orders by message id
     and clamps display times so nothing shows in the future. Expect the same.

1. 🔜 **NEXT UP, AGREED WITH {user} 2026-08-17: make the SIP texting app CONVERSATION-STYLE**,
   the way the Meshtastic app already is, instead of the current inbox/outbox split.

   **Scouted, so do not re-derive it:**
   - **`MessagesApp` (GUI.h) is built around the split**: states `MAIN / INBOX / OUTBOX /
     COMPOSING`, separate `inboxMenu` and `sentMenu`, and separate `inboxOffset/inboxSelected`
     and `sentOffset/sentSelected`. Nine methods in GUI.cpp, plus `ViewMessageApp` (5) and
     `CreateMessageApp` (11) hanging off it.
   - **`MeshtasticApp` is the working template to copy** — `MESH_CHATS` lists conversations,
     `MESH_THREAD` shows one correspondent with both directions interleaved, `MESH_COMPOSE`
     writes into it. Same widget vocabulary, same app skeleton.
   - ⚠ **The storage layer is the real work, not the UI.** `flash.messages` is queried as two
     flat lists (incoming / sent). Threading needs a grouping pass by correspondent, and the
     SIP URI is the natural key — but check how `Storage::messages` stores and sorts before
     assuming it can group cheaply on a phone with ~20 KB of internal heap.
   - Widgets are all PSRAM-backed now (`AbstractWidget::operator new`), so building a few more
     menus costs contiguous internal RAM roughly nothing — the app-open probe will confirm.

2. 👀 **WATCH FOR CRASHES IN NORMAL USE.** This is the standing task and the only real proof.
   ✅ **Overnight 2026-08-16→17: 5.4 hours idle, SIP registered, ZERO `reset_reason=4`.** But
   that was idle. A day of real use — menus, Books, the Game Boy, calls — is the test that has
   not been run. ⚠ **Check the reason before assuming:** `4` is the abort that was fixed;
   `1` is power-on class and a completely different investigation.

3. 🔎 **ONE UNEXPLAINED PANIC, 2026-08-17.** The phone pa{user}ed once while STARTING an outbound
   call, then the next attempt worked and it has not recurred. Strong suspicion, unproven: a
   call changes the sample rate, which makes `configureI2S()` reinstall the driver, and that
   reallocates **~16 KB of internal DMA-capable RAM** (`dma_buf_count(4) x dma_buf_len(1024) x
   2ch x 2B`) against a plateau of ~20 KB. **No backtrace was captured**, so this is a theory.
   If it happens again, get the backtrace before touching anything — see the lesson below.

4. **Node names should fill in on their own** now the node table evicts instead of freezing.
   Stock firmware beacons every 3 h, so give it hours, not minutes.

5. ✅ **CLOSED, all confirmed by hand on the device.** OK-commits-a-letter typing (*"typing
   seems to work a lot better"*), the **Firmware update** USB how-to page, the four **Clear**
   labels, the **visible cursor**, the **ringer mode**, and **calls and texts in both
   directions** on a real number.

   ⚠ **TWO CORRECTIONS WORTH KEEPING, because both came from trusting a short look.**
   - This doc once said *"`MultilineTextWidget::processEvent` has ZERO references to
     `WIPHONE_KEY_LEFT`/`RIGHT`"* and queued writing cursor movement. **Wrong.** It handles
     both and always has — the scouting grep used a 160-line window and the handler sits ~190
     lines into the function. The real bug was the cursor being drawn in hardcoded
     `WP_COLOR_0` (black) on screens whose theme sets a BLACK background, so movement worked
     all along and was simply invisible. **Bound your grep window; confirm "missing" before
     building on it.**
   - The POP hostname was inferred as `seattle.voip.ms` from VoIP.ms running seattle/2/3.
     It is `seattle1.voip.ms`. **Read it off the portal.**

5. **Nothing else is queued.** Everything below is reference.

### 🔑 THE ONE TOOL THAT MADE TODAY WORK — use it before theorising
`GUI::enterApp()` carries an **app-open heap probe** that writes to `health.log` *and* serial:
```
APP id=16398 heap 15772->12492 largest 12284->9216 (-3068)     <- Books, before the fix
APP id=16398 heap 15732->15316 largest 12208->12068 (-140)     <- after
```
It named the culprit on the first crash after being added, then **validated its own fix in the
same log file** with starting conditions 76 bytes apart. App ids are `GUI_BASE_APP (16384) +
ordinal`: **16388** CLOCK · **16389** SPLASH · **16392** PHONEBOOK · **16394** MESSAGES ·
**16397** MESHTASTIC · **16398** BOOKS · **16399** MUSIC · **16406** DIAGNOSTICS ·
**16409** NETWORKS · **16410** AUDIO_CONFIG · **16426** GBC.

⚠ **`largest` is the number that predicts the crash.** `free` and `min` disagree with it and
neither predicts anything. `min` is flat while idle and steps down only on events.

### ⚠ THE BUG CLASS THAT PRODUCED FOUR OF TODAY'S SEVEN FAULTS
**Component A writes shared state; component B reads it without setting it, so B's behaviour
depends on what A did last.** The menu ID collision, the Game Boy's I2S channel format, the
notification pop's device config, and the CPU gate's shared SIP predicate were all this.
**Every consumer must set what it needs. Anything borrowing the audio device for a one-shot
must `preserve()`/`restore()` around it.** When fixing one caller of a shared predicate, add a
new predicate rather than editing the shared one.

### ⚠ WHERE I WAS WRONG TODAY — recorded so it is not re-trusted
- **OTA was never "ready but for a version bump".** The transport cannot work at all — see the
  memory-wall section. I said otherwise before testing it.
- **The WiFi scan screen was NOT a missing `scanDelete()`.** The framework frees results
  itself. Right fix (5 s tick), wrong reason.
- **`gaps:0` does NOT rule out audio starvation** — only *severe* starvation. A 5 ms dropout is
  invisible to that counter and plainly audible.
- **The mesh frequency was NOT mismatched.** `djb2("LongFast") % 104 = slot 19 = 906.875 MHz`,
  exactly what is hardcoded. I had a tidy theory involving COVEY's 2.7.26 upgrade; it was wrong.

⚠ **`wiphone.local` mDNS goes stale when the phone changes network** (seen at
`192.168.158.33` on the hotspot, then `192.168.1.57` on SmithWifi). It pings but port 80
refuses. **Resolve first** — `dscacheutil -q host -a name wiphone.local` — and use the raw IP
if in doubt.

---

## ✅ WHAT SHIPPED 2026-08-15 (all measured, all on hardware)

| | |
|---|---|
| **Restarts** | WiFi event handler re-registered on **every** connect retry — ~430 copies per car journey, each multiplying a 1,460-byte UDP churn on reconnect. Registered once now. |
| **Restarts** | **Opening Books cost ~3 KB of contiguous internal heap, every time.** `BooksApp::operator new` → PSRAM. −3,068 → −140, validated by the probe. |
| **Restarts** | Notification pops permanently reconfigured the audio device (6 parameters, none restored). `Audio::preserve()`/`restore()` implemented — they had been declared under a bare `// TODO` since the beginning. |
| **Restarts** | `configureI2S()` reinstalled the driver — and its ~16 KB of internal DMA buffers — to change nothing. |
| **Restarts** | The WiFi scan screen rebuilt its whole menu every second (and asked for 750 ms **per channel**, so a scan could never finish). Now 5 s. |
| **Battery** | **CPU pinned at 240 MHz for 19+ min, screen off, out of range**, because `sipCallActive()` counts `HangUp` — a teardown state the phone can rest in forever with no proxy. Separate `sipNeedsFullSpeed()` for the CPU gate. |
| **Game Boy** | Ran at **50%** because it set the I2S sample rate but **inherited** the channel format. Paced by a blocking `i2s_write`, a mono sink drains at half rate. Arithmetic matched to the decimal. |
| **Menus** | Settings > WiFi auto-switch showed the Music icon and **opened Music** — duplicate menu ID. Now checked at boot. |
| **Diagnostics** | `/log?tail=N`; the health log **keeps its newest 32 KB** instead of deleting everything; the app-open probe. |
| **Book sync** | **Proved on air, both directions.** |
| **Mesh** | Periodic NodeInfo, replies to `want_response`, editable short name, node table evicts instead of freezing. |

Read this first to resume. One command tells you the codebase is healthy:

```bash
./tests/run_tests.sh
```

Expect **841 assertions, 0 failures** across eight suites. It compiles the phone's own
sources — including the real MP3 decoder — with the host compiler under ASan, no PlatformIO,
no ESP32, no phone attached.
⚠ Two suites need gitignored fixtures or they skip themselves and say so:
`tools/gen_jpeg_fixtures.sh <book.epub>` and `tools/gen_mp3_fixtures.sh <file.mp3>`.

---

## ▶ PICK UP HERE

### 🔎 1. Why does the phone restart? — **CAUGHT. It is a PANIC, and the heap fragments.**
**2026-08-14: the first crash was captured in `/health.log`.** `BOOT reset_reason=4` — a
PANIC. That rules out the other three outright: **not brownout (9), not the watchdog (6),
not power (1).** The battery was never below 3.99 V and never charging across 173 samples,
so the tired-cell theory is dead too.

**Fragmentation is confirmed, and it is a RATCHET.** From the 112-minute run, which is the
cleanest evidence because nothing unusual was happening to it:

| up | free heap | largest block | |
|---|---|---|---|
| 0 min | 19,148 | **15,508** | idle, screen off |
| 9 min | 15,164 | 11,836 | screen on |
| 33 min | 11,836 | 7,216 | screen on |
| 34–101 min | ~11,700 | **7,216** | screen off — flat for 68 minutes |
| 104 min | 9,104 | 5,296 | screen on |
| 106 min | **16,996** | **6,260** | screen off |

**At 106 minutes free heap is back to 16,996 — near the 19,148 it started with — while
`largest` is 6,260 against a starting 15,508 and never recovers.** Free heap returning while
the largest block does not is the fingerprint of fragmentation rather than a leak, and it
steps down on every screen-on episode and holds. `min` heap slid 19,148 → 9,104 in that run.

**The run that pa{user}ed died at `largest=4512`**, having fallen 15,432 → 11,504 → 7,064 →
4,512 over its final three minutes.

⚠ **THE INSTRUMENT WAS PART OF WHAT IT MEASURED, and that is not a footnote.** The panic
happened while the log was being pulled over HTTP — ~30 whole-file fetches, each a fresh TCP
connection through the `WebServer`'s String parser, because a 20 KB response kept truncating.
`largest` oscillated 3,084 ↔ 6,660 throughout. **So the captured crash was probably provoked,
and it is NOT proof that {user}'s occasional spontaneous restarts are the same event.** What it
does establish is the mechanism class — PANIC via internal-heap exhaustion — and the 112-minute
run above fragments the same way with nobody near it. This is the same trap as the
"Stop the instrument distorting the measurement it takes" commit. Fixed, below.

✅ **`/log?tail=N` now exists** so the next measurement costs ONE request instead of thirty:
```bash
curl "http://wiphone.local/log?tail=2000"     # with any upload screen open
```
It seeks to N bytes from the end and steps forward over the partial line it lands in, so the
response always starts on a whole line. ⚠ `Content-Length` is computed AFTER that resync, never
from `f.size()` — a length that disagrees with the body by one byte hangs the client. No `tail`
argument still sends the whole file. **`app_gbc_xfer.cpp` includes Arduino so it is not
host-testable; this was verified live against the phone instead** (`tail=2000` → header 1918,
body 1918, starting on a whole line).

⚠ **`tail` does not cure the truncation, it makes a SMALL read likely to succeed.** Measured on
the phone: ~1.5 KB succeeded 2/2, 3 KB 2/3, 6 KB 0/1, the whole 21 KB file 0/1. Failures land on
512-byte boundaries (1024, 15872), so it is the transfer dying, not the arithmetic.
**Ask for ~1500–3000 bytes, and check `Content-Length` equals the bytes you received** — that
comparison is the only reliable way to know you got it all.

### 💡 READING THE LOG IS THE BIGGEST SINGLE CONSUMER OF WHAT THE LOG MEASURES
Measured 2026-08-14 on a fresh boot, one minute per row:

| up | largest free block | |
|---|---|---|
| 1 min | 15,456 | idle, screen off |
| 2 min | 12,980 | idle |
| 3 min | **7,480** | the uploader screen opens — **−5,500** |
| 4 min | **4,808** | eight `?tail=` fetches — **−2,672** |

So the uploader app plus the `WebServer` costs ~5.5 KB of contiguous internal RAM, and **each
HTTP request permanently costs a further ~340 bytes that never comes back.** Thirty requests is
~10 KB, which is precisely what takes a healthy 15.5 KB start down to the 4,512 the phone
pa{user}ed at. ⚠ **One sample, so treat ~340 B/request as approximate** — but the shape is not in
doubt, and it means the honest way to read this log is: **one tail request, then close the
screen.** Do not leave the uploader open and poll it.

### 🎯 THE LIKELY ROOT CAUSE, FOUND AND FIXED 2026-08-14 — but PROOF IS TIME WITHOUT A PANIC
**`connectToWiFi()` re-registered the WiFi event handler on EVERY attempt.**
`WiFi.onEvent()` only ever does `cbEventList.push_back()` — **it does not deduplicate, and
`removeEvent()` is called nowhere in this firmware** (verified: zero matches). So the handler
list grew by one entry per connect attempt and never shrank except at reboot.

**That is the retry path.** `WiPhone.ino:1667` calls it every `WIFI_RETRY_PERIOD_MS` = **20 s**
for as long as there is no network. {user}'s **car journey** shows exactly this: the log has
`wifi=1` (WL_NO_SSID_AVAIL) held for the whole run, **143 minutes out of range ≈ 430
registrations.**

Then one `SYSTEM_EVENT_STA_GOT_IP` runs `processWiFiEvent` **N times**, and that handler is not
free — each pass does `delay(100)` plus `udp.begin()` and `udpRtcp.begin()`, and
`WiFiUDP::begin()` does `stop()` (`delete[] tx_buffer`) then **`new char[1460]`**. At N=430 that
is ~43 s of blocking delay and **~1,720 alloc/free cycles of 1,460 internal bytes from a single
reconnect.** The `std::vector` also doubles as it grows, leaving a trail of holes.

⚠ **AND THERE IS NO malloc→PSRAM AUTO-DIVERSION IN THIS BUILD.** The framework `sdkconfig.h`
defines `CONFIG_SPIRAM_USE_CAPS_ALLOC` and **not** `CONFIG_SPIRAM_USE_MALLOC`, so plain
`new`/`malloc` is **internal at every size** — only `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` /
`ps_malloc` reach PSRAM. **`docs/MUSIC.md`'s "the threshold is 16 KB" is wrong for this build:
there is no threshold.** Anything that says otherwise should be corrected, not relied on.

**Why this fits the measured signature:** the growth is **monotonic and never recovers**, which
is the "free heap recovers but `largest` never does" fingerprint exactly; and it is driven by
**WiFi events, not uptime**, which is why a 55-minute idle run with stable WiFi held at
`largest=27,164` while the car run collapsed.

**The fix:** `WiFi.onEvent(processWiFiEvent)` moved out of `connectToWiFi()` into
`Networks::init()` behind a `static bool` latch, so it registers exactly once per boot.
`wifiState.init()` (WiPhone.ino:904) runs before the first `connectToPreferred()` (:916), so the
handler is in place in time. ✅ **Verified on hardware: `[autosw] conn=1` in the boot log, and
that flag is set ONLY from inside `processWiFiEvent` on GOT_IP — so the handler still fires.**

⚠ **CONFIDENCE: this is the best-supported candidate, NOT a proven cure.** Four panics were
captured (`largest` at death: 4,512 / 4,092 / 8,848 / 3,084 — note it is *not* a fixed
threshold, consistent with a probabilistic allocation failure). **The only proof is a long run
across several WiFi drops with no panic.** Check `/health.log` for `reset_reason=4`.

⚠ **A SMALLER INSTANCE OF THE SAME BUG IS LEFT IN ON PURPOSE.** `ESPmDNS`'s
`MDNSResponder::begin()` also calls `WiFi.onEvent()`, and `app_gbc_xfer.cpp:499`
`MDNS.begin("wiphone")` runs on every `xferStart()` while `MDNS.end()` does not remove the
handler — so one permanent entry per uploader session. It was **not** fixed because `MDNS.end()`
is paired with it and breaking that kills `wiphone.local`, which is how the log is read at all.
One per uploader session is negligible beside 430 per car journey.

**What is still open:** naming what takes the ~13 KB of *internal* heap when an app opens. The
22.6 KB of PSRAM that goes with it is already accounted for and is **healthy** — it is exactly
`BooksApp`'s four arrays (`books` 8,112 + `store` 11,912 + `images` 2,400 + `imgBoxes` 96 =
22,520, plus 21 bytes × 4 of allocator header = 22,604 observed). That is the earlier
ps_malloc fix working as designed; PSRAM has 3.6 MB spare and is not the problem.

---

**The reset-reason table, for reading any future log:**

| | |
|---|---|
| **1** POWERON | the switch, a flash, or the power was interrupted |
| **4** PANIC | a crash — null deref, `abort()`, assert |
| **6** TASK_WDT | the main loop stopped feeding the watchdog |
| **9** BROWNOUT | **the supply sagged** — not software at all |

⚠ **Read the number before theorising.** A tired cell under a WiFi transmit peak (9) looks
exactly like a crash from the outside, and chasing it as a memory leak would be chasing the
wrong thing.

**The mechanism, now measured rather than assumed.** The documented failure on this phone is
the WiFi PHY failing to get ~2 KB of *contiguous* internal RAM for RF calibration, at which
point `phy_init` calls `abort()` — and an `abort()` is **reset_reason=4**, which is what was
captured. Free heap looks fine while the largest block is what runs out.

⚠ **`min` being flat does NOT mean the heap is safe, and an earlier version of this doc leaned
on that.** It is flat *while idle* — 84 bytes over 67 idle minutes in the run above — and it
steps down hard every time an app opens. Watch **`largest`**, not `min` and not `free`. The
three disagree, and only `largest` predicts the crash.

### 🎯 THE SECOND CAUSE, FOUND AND FIXED 2026-08-15: **opening Books cost ~3 KB, every time**
**{user}'s hypothesis was that the crashes were WiFi retrying while off-network. The data says
no.** Second run of the car session: **`wifi=1` (out of range) for 16 straight minutes**, retrying
every 20 s and auto-scanning every 2 min, with `largest` flat at ~15,000 — oscillating a few
hundred bytes and never ratcheting. **The off-network path is clean; that is the WiFi handler fix
holding.**

**The app-open probe named the culprit on its first crash.** Every other app measured `+0`;
Books measured, on two separate runs that each pa{user}ed within two minutes of it:
```
APP id=16398 (BOOKS)  largest  8,596 ->  5,732  (-2,864)   ... then 4,692, 3,084, PANIC
APP id=16398 (BOOKS)  largest 12,284 ->  9,216  (-3,068)
```
**Cause:** `new BooksApp` is a single ~3 KB **internal** allocation. `EpubBook book` alone is
**1,584 bytes by value** (measured on the host), plus `BookSyncRecord pending` ~250,
`ids[3][72]` 216, `hist[48]` 192, the sync strings 108, `libNote`+`headerTitle` 160, and the base
classes — landing right on the measured figures.

⚠ **AN EARLIER FIX MOVED THIS APP'S ARRAYS TO PSRAM (22,520 bytes) AND STOPPED THERE, LEAVING
THE OBJECT ITSELF INTERNAL.** That is why the phone kept pa{user}ing after opening a book even
though the arrays were "already fixed". Do not read "Books was fixed" as meaning all of it was.

**Fix:** `BooksApp::operator new` → `ps_malloc`, moving the whole object in one step instead of
converting a dozen members to pointers. Falls back to internal if PSRAM is exhausted; `free()`
is region-agnostic on ESP-IDF so one delete serves both.

✅ **VALIDATED BY THE SAME PROBE, before and after in one log file:**
```
before:  APP id=16398  largest 12,284 -> 9,216   (-3,068)
after:   APP id=16398  largest 12,208 -> 12,068    (-140)
```
Starting conditions **within 76 bytes**, so it is like-for-like. **95% reduction.** The residual
−140 is the widgets, which are separate `new`s registered in `registeredWidgets`.

⚠ **STILL OPEN, smaller:** `largest` fell **17,480 → 13,312 while the NETWORKS screen was open**
— the manual *Scan WiFi networks* page, which is NOT the background auto-switch scan (the flat
16 minutes above exonerates that one). Likely a missing `WiFi.scanDelete()`. Chase it after
confirming Books.

### ✅ FIRST REAL EVIDENCE THE FIX WORKS (2026-08-15, {user} out of the house)
An 85-minute run, **`wifi=1` (WL_NO_SSID_AVAIL — out of range) for essentially all of it**,
which is the exact state that killed it in the car. Out of range, `WiPhone.ino:1667` retries
every `WIFI_RETRY_PERIOD_MS` (20 s), so that window is **roughly 250 connect attempts** — each
of which used to append a handler that was never removed, with the vector doubling as it grew.

| up | largest | wifi |
|---|---|---|
| 29 min | 24,632 | 1 |
| 45 min | 24,632 | 1 |
| 61 min | 24,632 | 1 |
| 77 min | 24,632 | 1 |
| 85 min | **24,632** | 6 |

**Not one byte of movement in 85 minutes, at nearly 3× the headroom the car run had** (which sat
pinned at 8,848 and pa{user}ed at 143 min). ⚠ **HONEST LIMIT: the phone never ASSOCIATED in that
window.** This validates the half where registrations accumulate on retries. It does **not** yet
exercise the amplification half — one `GOT_IP` running the handler N times and churning the
1,460-byte UDP buffers — which needs a successful rejoin after a long time out of range.

**Where to look:** `/health.log` on the card. A line a minute; it survives both the reboot and
being unplugged, so the reset reason of the run that died sits at the top of the next run's
entries.

⚠ **THE LOG USED TO DESTROY ITS OWN EVIDENCE, AND IT COST US ONE (fixed 2026-08-15).** At the
96 KB cap it called `SD.remove()` on the whole file. On 2026-08-15 the phone rebooted while {user}
was out, the log hit the cap 29 minutes later, and **the `BOOT reset_reason=` line for that
reboot was deleted before anyone could read it** — the one number the investigation turns on.
`healthLogTrim()` now copies the last **32 KB** (~4 hours) forward and swaps the file in, so
there is always recent history. Streamed in a 512-byte stack buffer — a 32 KB allocation here
would be self-defeating on this phone. Falls back to deleting if the copy fails, because an
unbounded log is worse than a lost one. Also fixed the ordering: the size check now runs
**before** the boot line is written, so a boot line can no longer be destroyed by the very call
that wrote it. ⚠ **The trim path does not execute until the log next reaches 96 KB (~12 h at a
line a minute), so it has NOT been exercised on hardware yet.**

```bash
curl "http://wiphone.local/log?tail=6000"     # with any upload screen open
```
⚠ **Use `?tail=`.** A whole-file fetch truncates — with modem sleep on and the CPU at 80 MHz
the link is slow — and truncation costs you the END of the file, which is exactly where the
reset reason lives. Retrying instead is what made the reading of the log a cause of the crash.

### 🔋 THE CPU STUCK AT 240 MHz IN A DEAD SIP STATE — found and fixed 2026-08-15
**Measured from the car log:** the phone sat at **`sip=6` (`CallState::HangUp`)** with the
**screen off** and **no network (`wifi=1`)** for **19+ consecutive minutes**, holding the CPU at
**240 MHz** the whole time. At 80 MHz the core draws roughly half as much, so this was about
**double the idle current — out of range, in a car, on battery.**

`sipCallActive()` counts `HangUp` as a call. But `HangUp` and `HangingUp` are **teardown**:
HangUp's own comment says it "can be triggered from any other state", and HangingUp is "waiting
for confirmation of BYE/CANCEL, resending" — **with no proxy reachable that confirmation never
arrives and the phone rests there indefinitely.** Neither has an audio session to protect.

⚠ **THIS IS THE SAME TRAP THE REPO ALREADY HIT AND WROTE UP.** The earlier one was the phone
resting in `CallState::Error` (12) forever while a "not idle" test called it active; the fix
replaced exclusion with a **positive list of eight states**. Correct as far as it went — **but
two of those eight can also stick.** A positive list is not automatically safe; ask of each
member "can the phone rest here forever?"

**Fix:** a separate `sipNeedsFullSpeed()` for the CPU gate — the six states with a live or
imminent audio session. ⚠ **Deliberately NOT an edit to `sipCallActive()`**, which also gates
music-pauses-for-a-call; quietly changing a shared predicate to fix one caller is the exact bug
class that produced three separate faults in this codebase on the same day. Verified on
hardware: `CPU 80MHz (idle)` with the screen off.

### 🔋 2. Battery: ~10 hours measured. Is that good enough?
Measured on the device, steady state with the first 30 minutes discarded (surface charge):

```
94% -> 82% over 1.20 h  =>  10.0 %/h  =>  ~10 h from full
CPU at 80 MHz for 93% of the run, tracking screen state exactly (0 disagreements/103 samples)
```

⚠ **There is no before-measurement**, so the improvement factor is unknown. ~10 h is simply
the number to beat.

**Already done, and audited — do not redo these:** the main loop no longer spins (it was
`taskYIELD()` at 240 MHz forever), the CPU drops to 80 MHz when idle, WiFi modem sleep is on,
and disconnected WiFi scanning backs off. Bluetooth was already off, the keyboard backlight
already off, the screen already dims and sleeps.
**Left on purpose:** the LoRa radio sits in continuous RX (~10–12 mA). That is the cost of
hearing the mesh at all.

### 🎵 3. Music works and has been heard. One thing is still open.
{user}, on headphones: *"sounds alright... a little crackly, but ok"*, and after the fix
*"a bit better, but the songs don't quite skip, but almost."*

The big cause was found and fixed: `Audio::loop()` decoded exactly ONE frame per call — 24 ms
of audio — so any main-loop iteration slower than that drained the DMA with **no way to catch
up**, because the next pass still only produced 24 ms. Something rate-limited to realtime
cannot make up a gap. It now decodes ahead until the DMA refuses more.

⚠ **A residual glitch remains and is NOT diagnosed. THIS IS THE NEXT THING TO DO, and it is a
measurement, not a code change.** {user}, 2026-08-14: *"lets do this later"* — so it is queued,
with everything needed to act on it below.

### ✅ MEASURED BY {user} 2026-08-14 — two facts, and they point the same way
1. **`gaps` never left ZERO**, through general crackle on headphones.
2. 🔑 **"when I let the screen sleep the music cleared up almost perfectly."**

🛑 **2026-09-25: `gaps` IS RETIRED, AND IT WAS WORSE THAN THE CORRECTION BELOW SAYS.** It was blind to
short dropouts (below) AND it counted things that were not dropouts: a 512-byte read against a 627-byte
192 kbps frame ended a pass "starved", 8.6 times a second, with the DMA never dry (phone 2: 8-9/s on every
192k file; a Mac replay of the same feed: 0 true dropouts at passes up to 30 ms). The replacement is
`drops` + `minLead` (serial `music`), checked against a model's ground truth in `tests/test_musicfeed.cpp`;
see the 🎵 block at the top of STATE NOW. The hypothesis below (TFT/SD contention on the shared bus)
was never tested as such; what the screen really did was make passes longer than the ~70 ms ring.

⚠ **FIRST, A CORRECTION: `gaps:0` IS MUCH WEAKER EVIDENCE THAN THIS DOC USED TO CLAIM.** Read
what it actually counts (`Audio.cpp`, the `starvedNow` logic): it only increments if the loop
arrives with the decode buffer empty **and** the DMA then accepts 12 consecutive frames without
filling. The DMA holds ~4,096 frames — about 3.5 MP3 frames — so it fills after ~4 iterations
and clears the flag. **It therefore only ever catches a drought of roughly 313 ms.** A DMA that
empties for 5 ms is invisible to it, and 5 ms of silence is an audible click. **So `gaps:0`
rules out catastrophic starvation, NOT starvation.** The old "if it stays at 0 the cause is
elsewhere" was wrong and cost nothing only because {user} reported the screen fact too.

**▶ LEADING HYPOTHESIS: TFT/SD SPI BUS CONTENTION.** Verified from the pin definitions, not
guessed:

| | pins | speed |
|---|---|---|
| TFT (`src/TFT_eSPI/User_Setup.h`) | SCLK 18, MOSI 23, MISO 19, **CS 5** | 40 MHz |
| SD (`WiPhone.ino:760`, `Hardware.h:110`) | **same bus**, **CS 2** | 15 MHz |

`SD.begin(SD_CARD_CS_PIN, SPI, ...)` puts the card on the very `SPI` object the display uses —
those are the ESP32 VSPI defaults. `SUPPORT_TRANSACTIONS` is force-defined on ESP32
(`TFT_eSPI.cpp:36`), so the bus is properly mutexed: **no corruption, but strict serialisation —
while the screen draws, an SD read waits.** And the now-playing screen sets
`msAppTimerEventPeriod = 1000` (`app_music.cpp:305`), so it wakes and redraws every second.
Screen asleep → no TFT traffic → the card is uncontended → clean. That is {user}'s observation
exactly, and it explains `gaps:0` at the same time, because these stalls are milliseconds.

**▶ THE ONE-PRESS TEST THAT SETTLES IT (do this first, tomorrow).**
Play a track, then **press Back to the Music library list and leave the screen LIT.** Playback
continues by design (the player outlives its app).
- **Crackle CLEARS with the screen still on** → contention, and it is software-fixable. This
  also rules out the backlight electrically **and** rules out CPU frequency, since both are
  unchanged on a static lit screen. That is what makes one button press decisive.
- **Crackle PERSISTS** → electrical coupling from the display/backlight into the audio path,
  and no amount of code will help.

If it is contention, the fix direction is **shrinking what the once-a-second redraw does**, or
topping up the I2S buffer around it — **not** a ring buffer and **not** bigger DMA buffers (see
the red warning below). Do not add a ring buffer on a hunch; decode has ~74% headroom (measured:
5103 µs/frame against a 24000 µs budget).

🔴 **AND THE OBVIOUS FIX WOULD MAKE THE RESTART BUG WORSE — check this before reaching for it.**
I2S is `dma_buf_count = 4`, `dma_buf_len = 1024` ≈ **93 ms** at 44.1 kHz, and the decode-ahead
loop can produce up to 12 frames (~313 ms) in one pass, so it can already refill that buffer
several times over. Those buffers are also roughly **16 KB of internal, DMA-capable RAM — the
exact resource that is running out and pa{user}ing the phone** (§1). **Enlarging them trades the
crackle for the restart.** The two open bugs pull in opposite directions; that is why the
`gaps` reading has to come first.
✅ **(2026-09-25) Resolved without enlarging the bytes:** the ring was made deeper in TIME instead —
mono and half rate — so music's own install is 24 x 512 mono = 24 KB, TX only, 534 ms, against the
boot install's 32 KB (TX + RX, stereo) and 93 ms. And that boot install was ~32 KB, not 16: the RX side
is the same size again. Since 0.9.75 the idle internal heap is ~66 KB, not the ~16 KB this was written
against.

### 🎮 4. Game Boy ran at 50% — FIXED, needs one look to confirm
{user}, 2026-08-14: *"my Metroid II game (probably all the games) is running at 50%"*, and he
remembered older work that shut off WiFi and mesh during play. **Those optimisations are all
still present and were not the problem** (`app_gbc.cpp:187-188` kills WiFi; the main loop gates
`lora.loop()` and `meshService.loop()` on `!gGbcActive`). **It was not CPU speed either** —
`gGbcActive` is in the 240 MHz "busy" list, and `CONFIG_FREERTOS_HZ` is 1000 so the main loop's
`vTaskDelay(1)` is 1 ms, not 10.

**🔑 THE EMULATOR IS PACED BY ITS AUDIO, SO THE I2S CHANNEL FORMAT IS A TIMING PARAMETER.**
The design header says it outright: the blocking `i2s_write` is *"the de facto clock, locked to
the DAC with zero drift"*. `gnuboy_get_audio_count()` returns a **stereo interleaved** int16
count.

`app_gbc.cpp` set the RATE and inherited the FORMAT — `setSampleRate()` only calls
`i2s_set_sample_rates()` and never touches `channel_format`, which `configureI2S()` derives from
`Audio::monoOut`. `monoOut` defaults to false, so for a long time it was stereo **by luck**.
Then the music player began following the headphone jack (stereo on headphones, mono to the
loudspeaker) and started leaving `monoOut = true`. The arithmetic:

| | bytes/second |
|---|---|
| emulator produces | 32264 × 2ch × 2B = **129,056** |
| DAC consumes, **stereo** | 32000 × 2 × 2 = 128,000 (the documented 0.8% surplus) |
| DAC consumes, **mono** | 32000 × 1 × 2 = **64,000** |

Half the sink, so the write blocks twice as long: **50.4%**. {user} measured 50%.

**Fix:** `audio->setMonoOutput(false)` alongside the rate, so it is set rather than inherited.
Flashed 2026-08-14. ⚠ **Confirm two things in-game:** the pause menu's speed readout should be
**green ≥97%** (it is drawn orange below that, `app_gbc.cpp:811-814`), **and game sound should
still work** — forcing stereo also flips `codec.setAudioPath(!mono)`, which is the known-good
historical state but has not been heard by ear since the change.

⚠ **THE TRAP THAT MAKES THIS LOOK LIKE A CPU PROBLEM:** the adaptive frameskip
(`app_gbc.cpp:942`) only drops **display** frames. It cannot speed up a game paced by audio, so
at 50% it simply pins itself at maximum skip and stays there. **A maxed-out skip plus an orange
speed readout reads as "the CPU is too slow" and here it meant the opposite.** The comment at
`app_gbc.cpp:927` already warned about *"wrong rate after some phone sound reconfigured it"* —
audio had throttled games to 13% once before.

### 🎉 5. BOOK SYNC WORKS ON AIR — PROVED 2026-08-15. **{user}: *"works! thanks"***
**The oldest open item in this feature is CLOSED.** A reading position crossed from the WiPhone
to COVEY over LoRa, between two real devices, for the first time. Everything before this was
host tests against generated vectors; this is the real thing.

**🔑 THE TRAP THAT MADE IT LOOK BROKEN — and it will catch the next person too:**
{user} tapped **Sync my place** and reported *"the covey got nothing"*. **Nothing was supposed to
happen.** A LoRa round trip does not fit in the 15 s sync window, so the receiver **parks** the
record and offers it **only when you next OPEN THAT BOOK**. There is no notification, no toast,
no badge — the receiving device looks completely inert until you open the book. Opening it
produced the confirm card immediately, and it had almost certainly worked the first time.
**If sync "does nothing", OPEN THE BOOK ON THE RECEIVER before debugging anything.**

⚠ **CLARIFIED 2026-09-24 ({user} asked): on COVEY the RECEIVER never uses a window** — a `CBS1` packet is parked in the background the moment it lands and offered when the book is opened; the 15 s "Syncing…" countdown belongs to the SENDER's Sync tap only (LoRa at 0 s and 7 s, LAN UDP :8083 for 15 s, then "In sync" / "No other device answered"). The LAN half needs both devices inside their windows at once and the WiPhone never implemented it, so between COVEY and the phones the window carries only that toast — which is misleading for LoRa: the answer is usually parked later. The WiPhone side has always parked in the background (booksync_inbox). {user} rotated the booksync passcode on 2026-09-24.

⚠ **The parked record is RAM-ONLY.** A `covey-ui` restart discards it silently — which happened
during this very debug, when the radio check restarted the service. Re-send after any restart
before concluding it failed.

### ✅ BOTH DIRECTIONS PROVED. **You do NOT need to tap Sync on both devices.**
COVEY → WiPhone was confirmed straight after ({user}: *"it does work! thanks"*), so it is
bidirectional on real hardware.

⚠ **The "tap Sync on the other device" prompt is NOT a requirement, and it reads like one.**
There is no request/response in this protocol (D-089): tapping *Sync my place* **broadcasts your
own position**, and the receiver parks it with **no tap at all**. Tapping on both is only for the
two-way case where you want each device to learn the other's and converge. **For a one-way push,
only the sender taps** — {user} reasonably read the prompt as "you must tap both".

**The receiver is always listening, whatever app is open:** `meshtastic_service.cpp:507` calls
`bookSyncInboxPush()` from the radio receive path, and `app_books.cpp:930` calls
`bookSyncInboxFindFor()` when a book is opened. Nothing needs to be on screen for a packet to be
kept.

### 🔎 THE DIAGNOSTIC SCREEN — **Books > menu > Sync settings**
Built for exactly this and worth reaching for FIRST, because it answers both failure modes:
```
Channel 'booksync': found        <- or MISSING (app_books.cpp:1571)
Parked positions: N              <- N>0 means a record arrived and is waiting (app_books.cpp:1573)
```
⚠ **`Parked positions` is the ONLY visibility you get.** The line logged when a sync packet
arrives (`meshtastic_service.cpp:509`) is a **`log_i`, and only `log_e` is compiled into this
build** — so a packet landing prints *nothing at all* on serial. Do not read serial silence as
"it never arrived"; read the counter.

**The verified-good configuration, for reference when it next misbehaves:**

| | |
|---|---|
| channel | `booksync`, **index 3** on COVEY's radio |
| PSK | **[redacted — this repo is public]** 16 bytes, verified **byte-identical to the stored invite**. Read it off the radio: `meshtastic --port /dev/ttyACM1 --info`, channel index 3. |
| passcode | **[redacted]** — the same on **both** devices; read it from `booksync_pw` in COVEY's `/root/.covey/prefs.json`. It is short and was never rotated after being published; low stakes (it guards page positions only), but do not reuse it for anything else. |
| book | `Ghosts_of_Timkovichi.epub`, **5,059,833 bytes on both** — same size, so the ids agree |

⚠ **An invite STORED in prefs and an invite APPLIED to the radio are different states.** Checking
the radio needs `covey-ui` stopped to free the serial port:
`sudo systemctl stop covey-ui; meshtastic --port /dev/ttyACM1 --info; sudo systemctl start covey-ui`
— **always restart it**, it is easy to leave the device UI-less.

---

## 🔊 THE SHARED-AUDIO-STATE AUDIT (2026-08-15) — 12 findings, 1 fixed, 9 latent

{user} asked for an audit after two bugs in one day turned out to share a root cause. **18
candidates, 12 survived independent adversarial refutation.** The pattern hunted was:
*component A writes shared state, component B reads it without setting it, so B's behaviour
depends on what A did last.*

### 🎯 The headline is a CORRECTION: the music player is not the main offender
**`Audio::playPop()` is, and it predates music.** It runs on every Meshtastic notification and
sets **six** parameters — `setDataChannels(1)`, `setBitsPerSample(16)`, `setSampleRate(8000)`,
`setMonoOutput(true)`, `setHeadphones(false)`, `chooseSpeaker(true)`,
`setVolumes(Max, Max, MaxLoudspeaker)` — and the teardown at `WiPhone.ino` called only
`ceasePlayback()`, **restoring none of them.** One mesh message left the phone at 8 kHz, mono,
loudspeaker-forced, full volume, permanently.

⚠ **That means a mesh notification was almost certainly the REAL cause of the Game Boy 50%
bug**, not the music player — `monoOut = true` is the whole mechanism, and a mesh message is far
more frequent than a music session. The `app_gbc.cpp` fix immunises the emulator; this fixes the
source.

✅ **FIXED 2026-08-15 by implementing `Audio::preserve()` / `Audio::restore()`** — which were
**declared in `Audio.h` from the very beginning under a bare `// TODO` and never implemented.**
The original authors saw this need and every one-shot sound since has leaked device state.
⚠ **`restore()` reinstalls the I2S driver AT MOST ONCE and only if something differs** —
`configureI2S()` reallocates ~16 KB of internal DMA buffers, and fragmenting internal heap is
what causes the panics, so putting values back through the individual setters would have cost
several reinstalls per notification and been worse than the bug.
⚠ **`playback` is deliberately NOT restored** (ceasePlayback closes the file). **A pop still
stops the current track — that is a separate, unfixed issue.**

### ✅ NINE FINDINGS WERE "REAL BUT UNREACHABLE" — REACHABLE SINCE 2026-09-04, FIXED 2026-09-25
✅ **STATUS 2026-09-25: every row below is fixed** — `9213fc0` plus its review-round repair on `kosync`
(LOCAL, unbenched; bench list in STATE NOW, "📞 2026-09-25"). The premise under this heading no longer
holds: phone 2 logged `SIP REGISTRATION -> REGISTERED` on 2026-09-04 and both phones report `sip=1`.
The guards are pinned by `tests/check_call_audio.py` (a revert of any of them fails `run_tests.sh`).
*(History, as written 2026-08-15:)* **Every one of them needs a completed SIP call, and this phone has
never made one.** Measured: **`sip=12` (`CallState::Error`) in 49 of 50 health samples** — with no proxy
reachable the phone rests in Error forever. Fix these *before* SIP is ever made to work:

| | what | status (2026-09-25) |
|---|---|---|
| 🔴 **Live mic RTP leak** | Call teardown never clears `microphoneOn` / `microphoneStreamOut` / `rtpRemoteIP` / `rtpRemotePort`, and `audioOn` is the **only** gate. Anything that turns audio back on — music, **or a mesh pop, which needs no user action at all** — resumes encoding the live microphone and sending RTP to whoever you last called. `microphoneOn` is written exactly ONCE in the whole tree (`= true`); there is no `= false` anywhere and no `rtp.stop()`. | ✅ Fixed at the source 2026-08-16: `Audio::shutdown()` clears `microphoneOn`/`microphoneStreamOut`/`rtpRemotePort`/`rtpRemoteIP` and closes the socket. 0.9.79 adds the class backstop: an RTP session armed with no live call is shut down within 3 s (`AUDIO: RTP session armed with no live call`, held while the motor runs or a pop plays), and the **202## egg that streamed the mic to 192.168.1.15 needs `-DAUDIO_DEBUG_EGGS`. |
| 🔴 **`micEnc[1600]` overflow** | `packetSizeSamples()` is computed from `sampleRate`/`dataChannels`. With music's 44.1 kHz stereo left behind, a G.711 call overruns the buffer by ~164 bytes, landing on the `WiFiUDP rtp` object's socket fd and tx_buffer pointer. | ✅ Guarded 2026-08-16 (an oversized packet is dropped with `mic packet too large for micEnc` at log_e, never written past the buffer); 0.9.79 removes the cause — music can no longer change the rate under a call (F1/F2 gated, `playMusic()` refuses an armed session). |
| 🟠 **Ringtone at music volume** (×4 findings, one root cause) | `startRingtone()` takes `Audio::playback` away from music **before** `musicPlayerPause()` checks `audio->musicPlaying()`, so `restoreCallVolume()` is skipped. The phone rings 18–45 dB quiet and the call stays there. | ✅ 0.9.79: `startRingtone()` finishes a pop, then `musicPlayerYieldForCall()` (levels back even when the ring already took `playback`), then the loudspeaker; the loop's per-pass yield uses `gui.inCall()` and waits for a pop in flight. |
| 🟠 **F1 during a call** | The transport keys were guarded for the emulator and for Books but **not for a live call**. F1 starts a track, steals `Audio::playback` from `RtpStream`, and reinstalls I2S at 44.1 kHz underneath the call. | ✅ 0.9.79: `musicLoaded` and the F2 tracker ask `!gui.inCall()`; `playMusic()` refuses while RTP is armed; `startTrack()` refuses first and KEEPS the paused place. |
| 🟡 **Settings > Audio corruption** (×2) | Opening that screen while music plays persists the **music** volume into `/configs.ini` as the call volume, surviving reboot; and saving a volume there while music plays is silently reverted by music's stale snapshot. | ✅ 0.9.79: seeded from music's stash while music holds the codec, Save goes into the stash; and opening the screen with an unreadable configs.ini no longer stores a three-key file (only Save writes, 8b93e72's rule). |
| 🆕 **N1 — END set HangUp from every screen** (found 2026-09-25) | Outside a call HangUp stopped music (place lost), zeroed the volumes, sent a stray CANCEL on TCP; with no WiFi or no account it STUCK, and the music pause counted it as a call — no music until a reboot. | ✅ END sets HangUp only in `gui.inCall()`. ⚠ It was also the accidental only way out of CallState::Error; that is now an explicit END-in-Error SIP retry — **{user} to confirm** (STATE NOW). |
| 🆕 **N2 — `restore*Vol` globals never written** (found 2026-09-25) | The `getVolumes()` into them was commented out, the four `setVolumes(restore...)` calls were not: every call end and END press set 0/0/0 dB — the loudspeaker to its MAXIMUM for the next ring. | ✅ Globals and all four calls removed; the ring follows Settings > Audio's loudspeaker level. |
| 🆕 **N3 — RTP silence counted for the whole boot** (found 2026-09-25) | `rtpSilentScan` and `rtpSilentCnt` were never reset per call: the second call of a boot was ended at connect by `rtpSilent()`, with no BYE. | ✅ `rtp_watch.h`, begun per call in `Audio::newCall()`; 120 s continuous silence ends a call (`CALL: no RTP from the far end`). `test_rtpwatch` replays the old rule, and shows a strike left at hang-up ending the next call without the per-call begin. |

### 🟢 Cosmetic, worth knowing
**Diagnostics keypad self-test can never register F1/F2** (and F3/F4 while playing) once a track
is loaded — the transport consumes them first, so the four side-button squares stay grey and
read as **dead hardware**. `WiPhone.ino:1495-1515` vs `GUI.cpp:8473-8567`.

### The general lesson
**`Audio` is a device-wide singleton with no ownership discipline.** Every consumer should set
what it needs rather than inherit, and anything that borrows the device for a one-shot should
`preserve()` / `restore()` around it. The `app_gbc.cpp` fix and this one are the same fix applied
at two different levels.

## ⬆️ Firmware updates now come from this repo

**Settings > Firmware settings.** The URL box fills itself in; nothing needs typing.

It never was a dead link — **the pinned certificate expired 2021-04-18**. `/wiphone.pem` in
the factory SPIFFS is the *leaf* for wiphone.io, not a root CA, and it beat everything else,
so every check failed the TLS handshake regardless of the URL. The built-in CA is now ISRG
Root X1 (valid to 2035) and `/wiphone.pem` is ignored; an explicit `/user.pem` still overrides.

**To publish a release:**
```bash
tools/publish_ota.sh 0.9.2      # builds, stages ota/, bumps FIRMWARE_VERSION together
git add -A && git commit -m "Release 0.9.2" && git push
```
For a public repo, **pushing is releasing** — the phone reads straight from `main`.

⚠ **raw.githubusercontent.com, never a github.com release URL.** The manifest is fetched by a
hand-rolled socket in `loadIniFile()` with **no redirect handling**; a release asset answers
302 and would look like an empty file. Cost: ~2 MB of repo growth per release.

⚠ A `serverIni` saved in `/user_ota.ini` BEATS the compiled-in default — that is what a user
override is for. A stored `wiphone.io` URL is now treated as "no preference" so the new
default wins; anything else you type will stick.

⚠ **The boot-time auto-check runs before WiFi has associated** and always fails with
`DNS Failed`. Pre-existing, harmless, and it did the same with wiphone.io. Use the **Check**
button once the phone is on WiFi. Fixing it means deferring that check until WL_CONNECTED.

## 🛑 OTA CANNOT WORK ON THIS BUILD — IT IS A MEMORY WALL, NOT A URL OR A CERTIFICATE
**Established 2026-08-15 by actually trying it.** {user} pressed **Check** on a correctly
published 0.9.2 and got:
```
Dev: 0.9.1   Srv: 0
Error: -301 - Can't connect to ser[ver]
```
`-301` is set at **`ota.cpp:585`** when **`client->connect(hname, 443)`** fails. That is the
**TLS handshake**, before a single byte of HTTP. The URL was right, the manifest was live, and
the binary served was byte-identical to the build — all verified from the Mac.

**The arithmetic, from the framework's own `sdkconfig.h`:**

| | |
|---|---|
| `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` | **16384** — a ~16 KB IN buffer **and** a ~16 KB OUT buffer, so **~33 KB** |
| `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` | **not set** — so none of it may come from PSRAM |
| phone's free internal heap | **~19 KB total** |
| phone's largest contiguous block, **fresh boot** | **~14,900 bytes** |

**It is not close, and it is not a fragmentation problem — there is not enough internal heap
even when perfectly clean.** The handshake cannot succeed on this build, ever. That `[E]
WiFiClientSecure.cpp: start_ssl_client: -1` line in every boot log is this, and it has been
there all along.

⚠ **THIS CORRECTS THIS DOCUMENT.** The section below says the update page "works again" after
the expired-pinned-cert fix. **That fix was real and necessary but it was never the whole
story** — the phone still cannot open the connection. So "**OTA has never actually installed
anything**" was never about caution or bad luck: **it has never been able to.** Do not re-plan
around OTA until the transport is changed.

✅ **ACTED ON 2026-08-16.** The UI is gone (Settings > Firmware update is now a USB how-to) and
the boot-time attempt is **compiled out** behind `OTA_TRANSPORT_AVAILABLE` in `WiPhone/ota.h`.
`start_ssl_client: -1` no longer appears in the boot log. **Flip that macro to 1 only when the
transport itself changes** — the download code is intact and still builds, so it is one line
when there is something for it to talk to. ⚠ Two things learned doing it: `ota.updateExists()`
is the network call and it sat to the **LEFT** of the cheap predicates so `&&` never guarded it
(fixed underneath the macro), and **`autoUpdateEnabled()` defaults to TRUE**, so turning
auto-update "off" was never going to stop the attempt.

**Options, ranked:**
1. **USB.** 55 s, hash-verified, ~10 times in one day without a hitch. This is the working path.
2. **Add plain-HTTP support** and serve `firmware.bin` off the LAN — avoids TLS entirely. Port
   **443 is hardcoded at `ota.cpp:585`** and `https://` is assumed in four places
   (`ota.cpp:187, 207, 241, 260`). A real change, but the only one that could work without
   touching the framework.
3. **Rebuild arduino-esp32** with a smaller `MBEDTLS_SSL_MAX_CONTENT_LEN` or
   `MBEDTLS_EXTERNAL_MEM_ALLOC`. Correct in principle; PlatformIO ships precompiled libs, so
   this is a serious undertaking.

✅ **0.9.2 is published and verified live regardless** — manifest and binary both served from
`raw.githubusercontent.com`, binary byte-identical to the build. If the transport is ever fixed,
the release is already sitting there. **0.9.2 was installed over USB.**

⚠ `app1` has still never been written on this phone.

⚠ **THE PHONE IS RUNNING A DEV BUILD THAT CALLS ITSELF 0.9.1 (2026-08-14).** Two USB flashes —
`/log?tail=` and the menu-ID fix — both without bumping `FIRMWARE_VERSION`, so the firmware
installed is NOT the `ota/firmware.bin` committed as 0.9.1. Nothing breaks (the version only
decides whether an update is offered) but **do not trust the reported version to identify what
is on the phone.**

### ▶ THE AGREED NEXT RELEASE — {user}'s plan, 2026-08-14
Get the `gaps:N` reading → fix the crackle → **cut 0.9.2 and install it OVER THE AIR.** That
single release also clears the version skew above and would be **the first OTA this phone has
ever performed.**

⚠ **{user} asked whether he could OTA at the level he is already on. He cannot** — `publish_ota.sh`
bumps `FIRMWARE_VERSION` and stages `ota/` **together** precisely because a manifest equal to
the installed version is indistinguishable from a broken update check. The phone reports 0.9.1,
so the OTA must be **0.9.2 or higher** to be offered at all.

```bash
tools/publish_ota.sh 0.9.2
git add -A && git commit -m "Release 0.9.2" && git push     # for a public repo, pushing IS releasing
```
✅ **The risk is much lower than it was:** the USB adapter is connected
(`/dev/cu.usbserial-025A3EAF`), so recovery from a failed first OTA is a ~55 s reflash rather
than a problem. Do not attempt the first OTA with no cable to hand.

---

## The reader is DONE and confirmed, and sync is now PROVED ON AIR (2026-08-15)

📖 **{user} read a real bought book on this phone on 2026-08-11 and confirmed it working**:
prose, chapter titles, pictures inline, and his place surviving a power cycle. Menu > Books.
Flash at 230400 (hash verified). That was the goal, and it is met.

**The one thing left needs the other device: proving book sync on air against COVEY.** Every
piece is written and host-tested; not one packet has crossed. The order to check things in is
in the sync section below — and every failure mode there is silent on both devices, so follow
it rather than guessing.

Nothing else is outstanding. Everything below this line is context for whoever picks it up.

### The test book is ALREADY ON THE SD CARD
`Ghosts_of_Timkovichi.epub` (**5,059,833 bytes** — the 5,060,061 written here before was
wrong; the FINGERPRINT is what proves a copy, and it feeds on the size so a match settles it)
was pushed over WiFi on 2026-08-11 and sits at
**`/roms/Ghosts_of_Timkovichi.epub`** — not `/books`. That is not a mistake to correct: the only
uploader the phone was running at the time was the Game Boy one, whose server has **no extension
filter at all** (the `accept=` attribute is a browser hint). The Books app scans `/roms` and the
card root as well as `/books` for exactly this reason, and **Manage** can move it.

**Proof it is the right file:** open it and check **Book info** shows `fp:2f6a8bc9d41ee898`. That
fingerprint is sha1(size + first 64 KB + last 64 KB), computed independently on the Mac, so a
match means the 5 MB arrived byte-for-byte.

### Confirmed by eye on 2026-08-11
{user} read a page of the real book. Two things it found, both fixed and flashed:
- **Akrobat has no typographic punctuation** and its `.notdef` is a narrow bar, so "Arc-Royal’s"
  read as "Arc-Royalls" — a missing glyph that looks like a letter, not like damage.
  `bookRenderRun()` substitutes only what the font cannot draw (it asks, via
  `getUnicodeIndex`) and feeds BOTH measuring and drawing. ⚠ It must never touch `chapText`.
- **`HeaderWidget::setTitle` keeps the pointer and `redraw` applies no width limit**, then
  paints the clock and icons over the title's right-hand end. Every other app gets away with
  it by having a short title; a book title ran straight under the clock.

### ⚠ THE INTERNAL HEAP IS ~16 KB, AND THAT IS THE WHOLE FIRMWARE'S MARGIN
Opening a book rebooted the phone a minute or two later. The symptom named the wrong culprit:
`phy_init: failed to allocate memory for RF calibration data` + `abort()` — the WiFi PHY, on
its periodic auto-switch re-scan, failing to get ~2 KB of **internal** RAM. Nothing in the
backtrace pointed at the reader, and the obvious theory (the new picture decoding) was wrong.

Measured with the heap probe in `app_books.cpp` (compiled out behind `BOOKS_HEAP_DEBUG` —
uncomment it before theorising about memory here):

| | free | largest block | psram |
|---|---|---|---|
| before | 5,112 | 3,084 | 3.6 MB |
| after | 15,436 | 12,252 | 3.6 MB |

`new BooksApp` was taking 11 KB of internal heap, 8 KB of it a plain array of 48 filenames.
PSRAM was never the constraint. **Anything in an app that is more than a few hundred bytes
belongs in `ps_malloc`.** Watch `largest` as much as `free`: this was a fragmentation failure
as much as a volume one.

### Pictures (2026-08-11) — including greyscale, via our own decoder
Inline, captioned `[1] press 1 to enlarge`, and full-screen on that number key. Read the two
bugs in commit fab0502 before touching the layout: a picture too tall for the space left on a
page was silently LOST, and a picture on a one-row page could stop the page advancing at all.
Colour goes through the ROM decoder; greyscale through `jpeg_grey` (see below), which is most
of them. Still out of reach and honestly reported on the picture: progressive JPEG, and PNG.

### If something regresses, check in this order
1. **Menu > Books lists the book.** If the library is empty, the SD scan or the card is the
   problem, not the reader.
2. **Open it.** Give it a few seconds: opening reads the zip directory of 230 entries, the OPF,
   the NCX, and fingerprints 128 KB. Expect the Title Page — spine 0 is a coverless-text cover
   and is skipped deliberately. 90 chapters, real titles from the NCX ("Prologue",
   "1. Monkeys with ’Mechs"), ~11 lines a page, ~34 characters a line at the small size.
3. **Page down and back up.** Nothing should be skipped or repeated at a boundary — that was a
   real bug, found by paging a real chapter backwards on the host, and it is now pinned.
4. **Close the book, reopen it.** It must land on the same page. That is the whole feature.
5. **Leave it sitting on a page for two minutes.** The screen must not sleep.

### ✅ SYNC IS PROVED ON AIR (2026-08-15) — built 2026-08-11, commit 595e838
A position crossed from the WiPhone to COVEY over LoRa on 2026-08-15. {user}: ***"works!"***
Before that, what was proven was 34 host assertions against real COVEY-generated packets:
parking, matching, newest-wins, and that every wrong-passcode and tampered vector is diverted
out of Chats and never matches. **Those held up on real hardware, first time, with no code
changes** — the only thing that ever went wrong was knowing where to look for the result.
See the trap in **PICK UP HERE §5**: the receiver shows nothing until you open the book.

**To test it, in this order:**
1. Put the SAME book file on COVEY (`/home/covey/books`). Byte-identical — that makes all
   three ids agree at once. `Ghosts_of_Timkovichi.epub` is on the WiPhone only.
2. Change COVEY's `booksync_pw` off its `"nnnn"` placeholder and type the same into
   **Books > menu > Sync settings > Passcode**. That screen also says whether the `booksync`
   CHANNEL exists — two separate secrets, both silent when wrong.
3. Import COVEY's `booksync` channel invite (DM the link, then **Apply link** in Meshtastic).
4. Read on one, "Sync my place", open the book on the other. The confirm card should offer
   the other device's position.

⚠ Every failure here is silent on both devices. If nothing happens, check in this order:
channel present → passcode identical → the two books share an id (Book info shows them).

### Then: the rest of sync
All four of the pieces below are BUILT (mesh divert, send by channel name, the confirm card,
the parking spot, passcode/device name in NVS) — see the note above for what is still unproven.
What is genuinely left:

- **Sending on a schedule** rather than only on demand, if that turns out to be wanted.
- **A second device to test any of it against.**

### Greyscale pictures — DONE (2026-08-11, commit dc910cf), unseen on screen
`jpeg_grey.{h,cpp}` is our own baseline greyscale decoder. It exists because the ESP32's
TJpgDec is in ROM, does 3-component YCbCr only, and **33 of the 45 pictures in this book are
1-component**. Proven on the phone first (`load_jpg_at=0`), then written, then checked pixel
for pixel against KNOWN pixels (a quality-100 round trip of a checkerboard: mean error 0.001)
and cross-checked against macOS `sips` on the book's own art.

⚠ If a book image ever disagrees with sips by a mean of ~1 with outliers around 33, that is
NOT a bug — it is sips smoothing a low-quality JPEG, which the ground-truth fixture exists to
prove. Check truth.jpg before suspecting the decoder.

What is still unsupported and now genuinely out of reach: progressive JPEG and PNG.

---

## What the reader does, and the three things worth knowing before changing it

`app_books.{h,cpp}` is a `WindowedApp`: library → reader → menu (chapters / text size / book info
/ close), plus **Add books over WiFi** and **Manage** (delete, asks first). The library scans
`/books`, `/roms` and `/`.

**1. Pagination is a separate, testable module.** `book_layout.{h,cpp}` takes the text and a
width-measuring CALLBACK, so wrapping, paragraph gaps, UTF-8 and page-back are all exercised on a
Mac with a fixed-width stub. The phone supplies `SmoothFont::textWidth`. Keep it that way: the
only thing left in the app is drawing the lines it hands back.

⚠ **What page-back promises is CONTINUITY, not identity.** Greedy wrapping restarted at a
different offset can stay permanently out of phase, so a cold page-back may reflow the lines.
What it guarantees is that the page ENDS where the current one begins and is a full page. Exact
back-paging through pages you just read comes from the app's own history stack, not from
re-deriving them. Do not "fix" this by asserting identity — the algorithm cannot keep that
promise, and an earlier version of the test asserted it and passed only by luck.

**2. Position is (spine, byte offset) and text size does not move it.** Changing the font
reflows the page and keeps your place, which is the same property that lets COVEY understand a
position from here. ⚠ The offset is a BYTE offset into the extracted text; COVEY counts
CHARACTERS. They agree for ASCII and drift on curly quotes and em dashes — pre-existing,
documented in `epub_parse.h`, and absorbed by the whole-book fraction. It is one more reason a
synced jump is confirmed rather than taken silently.

**3. The transfer server is SHARED, not copied.** One `XferConfig` per app, one port 80, one copy
of the two lessons that keep it alive on hardware. `xferStart()` stops and restarts if another
app asks for a different folder. If you add a third uploader, add a config — do not add a server.

### Chapter titles are now in the vectors too
`gen_epub_vectors.py` emits `chapter_title()` per spine item, so the nav/NCX reading is checked
against COVEY's output rather than a second reading of the fixture — `epub3-nav.epub` covers the
EPUB3 form and `epub2-subdir.epub` the EPUB2 NCX. ⚠ Regenerating the vectors also REBUILDS the
fixture zips, so their `fp:` fingerprints change; that is expected, and the vectors and fixtures
must be committed together.

One difference from COVEY is deliberate: for a nested NCX this takes the entry's own label where
COVEY concatenates its children's too. They agree on a flat table of contents, which is what the
fixtures and the test book have. Titles never travel — only the spine INDEX does.

---

## What is done

| Module | State |
|---|---|
| `book_hash.{h,cpp}` | SHA-256, SHA-1, HMAC, base32, UTF-8 truncation |
| `booksync.{h,cpp}` | full wire protocol — **320 assertions** |
| `epub_parse.{h,cpp}` | zip, inflate, OPF, spine, XHTML→text, ids, fraction/locate, nav/NCX titles, pictures — **167** |
| `bookstore.{h,cpp}` | reading positions — **57** |
| `book_layout.{h,cpp}` | pages, wrapping, page-back, picture rows — **113** |
| `html_entities.h` | 2125 entities, generated |
| `app_books.{h,cpp}` | the reader. Compiles and runs nowhere yet — see PICK UP HERE |
| `app_gbc_xfer.{h,cpp}` | the shared upload server, one `XferConfig` per app — now four |
| `music_lib.{h,cpp}` | track list + play order — **96 assertions** with wav_reader |
| `wav_reader.{h,cpp}` | WAV headers, downmix, resample | ″ |
| `mp3_stream.{h,cpp}` | ID3 skip, frame sync, feeding helix — **32**, decodes a real track |
| `src/audio/helix-mp3/` | vendored MP3 decoder (RPSL) + our PSRAM allocator |
| `music_player.{h,cpp}` | library, queue, volume — outlives its app on purpose |
| `app_music.{h,cpp}` | the screen. See `docs/MUSIC.md` |

Crypto is self-contained rather than mbedtls **on purpose**: the host tests have to exercise the
code that actually ships, and an mbedtls backend would mean testing one implementation and
shipping another. ~2 KB of flash.

Interop vectors are **generated by running COVEY's real Python** (`tools/gen_*.py`), so they are
ground truth, not a second reading of the spec. Re-run them if COVEY's `booksync.py` / `epub.py`
ever change — the C tests then fail loudly instead of the two devices drifting apart in silence.

---

## ⚠ Traps — every failure mode in this feature is SILENT

COVEY drops a bad MAC without a word (telling an unauthenticated peer why it failed builds an
oracle), so a wrong implementation and a wrong passcode look identical: nothing logged, nothing
sent back, sync simply does not happen.

1. **The mesh mac is computed over the REDUCED record**, not the full one — first id only, device
   name cut to 12 **bytes**, fraction quantised to 1/65535ths.
2. **`make_record` stores `round(fraction, 6)` and that rounding is not cosmetic** — it feeds the
   quantisation. `int(round(1/3,6)*65535)` is 21844; unrounded it is 21845.
3. **Entities and whitespace are an OFFSET problem.** A position is a character offset, so an
   entity decoded to a different length than COVEY's shifts every offset after it and keeps growing
   to the end of the chapter — too small to trip the "offset past the end" check. Python's
   `str.split()` also treats **U+00A0** (what `&nbsp;` decodes to) as whitespace.
4. **`HEX` is `#define HEX 16` in Arduino's `Print.h`** — fine in a file that does not include
   `Arduino.h`, fatal in one that does.
5. **The href trap:** manifest hrefs resolve against the OPF's directory, not the zip root. Get it
   wrong and you get a book with zero chapters and *no error*.
6. **`_norm` returns the href unchanged when there is no base** — it does not normalise.

## How books are matched — not by filename

`id:<dc:identifier>`, `ta:<title|author slug>`, `fp:<content fingerprint>`, best first; ANY overlap
means the same book. **Only the FIRST id travels over LoRa.** Byte-identical copies on both devices
is the happy path — it makes all three agree at once. The filename only decides for a `.txt` (or an
EPUB with no `dc:title`), where the title comes from the filename stem.

**Two secrets, not one:** the `booksync` Meshtastic channel PSK (transport — COVEY has it at
channel idx 3 and can share it as an invite link the WiPhone can already import via **Apply link**)
AND the booksync passcode, which is the HMAC key.

⚠ **This repo is PUBLIC.** Test vectors embed a passcode *and* its derived key, which is the whole
secret — that is why `tools/gen_booksync_vectors.py` uses invented values. Never put a real
passcode there.

---

## Waiting on {user}

- **Read a chapter.** It is flashed; nobody has looked at the screen.
- Put the same book files on both devices — WiPhone SD `/books`, COVEY `/home/covey/books`.
  Byte-identical copies. `Ghosts_of_Timkovichi.epub` is on the WiPhone (in `/roms`); COVEY does
  not have it yet, and sync needs both.
- Import COVEY's `booksync` channel invite onto the WiPhone (DM the link, then **Apply link**).
  Never broadcast it — it carries the channel PSK.
- Change COVEY's booksync passcode from its `"nnnn"` placeholder, and match it on the WiPhone.

## The phone itself

New unit, flashed 2026-08-10. Port **`/dev/cu.usbserial-025A3EAF`** (per-unit — derive it from
`ls /dev/cu.usbserial-*`, never hardcode). ESP32-D0WDQ6-V3 **rev 3**, 16 MB.

🔴 **Its CP2104 fails above 230400** (`Invalid head of packet` at 460800 and 921600). `platformio.ini`
still says `upload_speed = 921600` and `pio run` has no override flag, so drive esptool directly.
Full command in the `wiphone-flashing` memory. Factory backup at
`backups/newphone-factory-full-16MB.bin`.

**Reading the boot log is inverted:** capture at **500000** baud, only `log_e` is compiled in, and
`MeshPhy: SX1276 detected` is a `log_i` that is compiled out. **Silence about the radio means the
radio is fine**; only failure prints.

---

## Recently fixed — don't re-break these

### 🔁 DUPLICATE MENU IDs ARE SILENT, AND HAVE NOW SHIPPED TWICE (2026-08-14)
{user}: Settings > **WiFi auto-switch** was drawn with the **Music icon**, and selecting it
**opened the Music player**. Both symptoms, one cause: `"Music"` (parent 1) and
`"WiFi auto-switch"` (parent 5) both had **ID 42**.

**`findMenu()` matches on ID ALONE — it never looks at `parent`** — and returns the FIRST row
it finds. Music sits earlier in `menu[]`, so it shadowed the WiFi row completely: `enterMenu(5)`
built the Settings row and asked `findMenuIcons(42)`, which gave Music's icon, and on OK
`findMenu(42)` walked from the top and returned Music, so `enterApp(GUI_APP_MUSIC)` ran.

⚠ **This is the SECOND time.** Commit `3629566` is literally titled *"Fix Music opening Books"* —
the same failure — and it was fixed by moving Music to 42, **which `7abb47f` had already given to
WiFi auto-switch.** The fix relocated the collision instead of removing it. A comment reading
*"⚠ ID must be UNIQUE, not just the action"* was added at that time and did not prevent the
recurrence, so **there is now a real check**: `GUI::init()` walks the table at boot and logs
`MENU: DUPLICATE ID <n> - "A" shadows "B"` at `log_e`. Silence there is the pass condition.

WiFi auto-switch is now **43**. Music keeps 42 because `menuIcons[]` is keyed to it. **The only
duplicate in the whole table was 42** — verified by extracting the real table and running the
same O(n²) pass over it, which reproduces the bug on the committed HEAD and reports clean after.
⚠ **Free IDs are 25 and 44+.** 25 is a gap inside a used range and may have been retired; prefer
counting up from the top. Nothing persists a menu ID (`curMenuId` is runtime-only), so
renumbering needs no migration.

## Recently fixed — don't re-break this one

**Triple-tap-to-sleep is suppressed while editing text**, because the top-right Back key is
BACKSPACE inside a field and three quick corrections were sleeping the phone mid-message.

⚠ The rule is **"the focused text field is NON-EMPTY"**, not "a text field has focus". The
first attempt used focus alone and made triple-tap *near-impossible*: `FocusableApp::setFocus()`
focuses whatever widget a screen lands on, and plenty of screens land on a text field, so every
Back press there reset the counter. **Focused is not typing.** An empty field has nothing to
backspace, so the tap can only have been meant as a triple-tap and is let through.

Also worth knowing for any future work in this area:
- `setFocus` is **pure virtual in `AbstractWidget`** and `TextInputAbstract` overrides it. A hook
  added to `FocusableWidget::setFocus` alone will never run for text widgets — that cost a
  build-and-flash cycle. `TextInputAbstract::setFocus` now delegates to the base instead of
  duplicating its two assignments.
- The tracker is a **pointer to the live widget**, cleared by `~FocusableWidget`, not a bool. A
  flag stuck true would disable triple-tap for the whole session.
- `ControlState::inputType` is unusable as a "typing" signal — a MODE, set ad-hoc, persists
  across screens.

Timing (`BACK_TAP_GAP_MS`, 500 ms between consecutive taps) was deliberately left alone so the
rule change could be judged on its own. Confirmed good on hardware 2026-08-10.

## Also open

- **Stretch goal:** [`woods-backplate.md`](woods-backplate.md) — RFM95W + GPS + 915 MHz whip +
  expanded battery on a screw-terminal plate. Design notes only, nothing built.
- `stash@{0}` holds DIAG battery/gauge instrumentation from the July 2026 power repair. Not needed
  on the new phone; kept in case the old one is revived.

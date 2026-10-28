# WiPhone Meshtastic + Game Boy Firmware

Custom firmware for the **[WiPhone](https://www.wiphone.io/)** (an open-source
ESP32 cell phone) that adds:

- a two-way **[Meshtastic](https://meshtastic.org/)** radio node (the WiPhone's
  built-in LoRa radio speaks Meshtastic's on-air protocol — encrypted channel
  text **and encrypted direct messages with modern Meshtastic devices**, no
  phone, app, or internet required — plus shared map pins, distances to your
  people, and sunrise/sunset for wherever you are),
- an **offline map** — your own tiles off the SD card, **or downloaded on the phone
  itself** (USGS Topo, USGS Aerial, OpenTopoMap) over WiFi, with everyone's last
  position, the mesh's shared places and pins you drop yourself drawn on top of
  them (and shared back to the mesh when you choose to),
- a full-speed **Game Boy / Game Boy Color emulator** with sound, save states,
  and drag-and-drop ROM upload over WiFi,
- a **music player** (MP3 and WAV, stereo, hardware transport buttons, plays on
  while you use the rest of the phone),
- an **e-reader** (EPUB and text, pictures inline, your place saved, and
  position sync to another device over LoRa),
- **T9 predictive text** — five keypresses for "hello" instead of thirteen, with a
  25,000-word dictionary in flash, the old mode cycle on `#`, and room for your own
  vocabulary on the SD card,
- and phone quality-of-life fixes (real battery life, WiFi auto-switching,
  input reliability).

All of the WiPhone's normal phone / SIP / menu features stay intact.
Built with **PlatformIO** (the stock WiPhone firmware is Arduino-IDE only).

---

## 🔌 Install it from your browser — no tools needed

### ➡ **[nikguy321.github.io/wiphone-meshtastic](https://nikguy321.github.io/wiphone-meshtastic/)** ⬅

Plug the WiPhone into a computer with a USB cable, open the link in **Chrome or
Edge**, click **Install**, and pick the serial port that appears. About a
minute; the phone reboots itself when done. Nothing to install on the computer
— no Python, no command line. (Prefer doing it by hand? The PlatformIO route
below still works, and the phone's own **Settings → Firmware update** screen
carries both sets of instructions.)

---

## ⚠ You need an SD card — and a good one

Most of what makes this firmware worth having **lives on the SD card**: photos
and wallpaper, books, music, Game Boy ROMs and save states, chat history, the
`/health.log` the phone writes about itself. The phone boots and calls without
a card, but the apps above will be empty or refuse politely.

- **Format it FAT32.** The phone does not speak exFAT — and cards over 32 GB
  come exFAT from the factory, so a big card that "doesn't work" is almost
  always just this. Cards **up to 32 GB** format to FAT32 out of the box
  (Windows: right-click → Format → FAT32; macOS: Disk Utility → **MS-DOS
  (FAT)**, scheme MBR). For a larger card, use the
  [SD Association's formatter](https://www.sdcard.org/downloads/formatter/) or
  any tool that does FAT32 above 32 GB. No files or folders needed — the phone
  creates `/photos`, `/books`, `/music`, `/roms` and the rest itself.
- **Buy a decent card (a name brand, from a real seller).** The phone writes
  under real constraints — uploads stream to the card while WiFi runs, and the
  power switch is a hard cut, not a shutdown — and a weak or counterfeit card
  shows up as exactly the flakiness you'd blame on the firmware: uploads that
  die with SD errors, 0-byte files, photos that vanish. During 0.9.28's ~52 MB
  upload testing, a tired card threw three write errors that a fresh one
  wouldn't have (the transfer retried through them, but that's the margin
  you're spending).
- **The card to buy: SanDisk High Endurance 32 GB (`SDSQQNR-032G-GN6IA`)** —
  [Amazon listing](https://www.amazon.com/SanDisk-Endurance-microSDHC-Adapter-Monitoring/dp/B07P14QHB7),
  sold by Amazon itself, not a marketplace reseller (the counterfeits are the
  cards that misbehave). It is the dash-cam card: rated for continuous
  recording, which is what the map downloader does to a card for hours, and
  32 GB comes FAT32 from the factory. For scale: the pooled tiles of both phones
  and COVEY (132,826 of them) take 16.2 GB and leave ~13 GB free, and a 20 km
  OpenTopoMap area to z17 is about 6.2 GB more. 32 GB still fits all of that, but
  z17 is a budget, not a default. The "MAX Endurance" variant (`SDSQQVR-032G`) is fine too.
  **Why it matters, measured (0.9.74):** the phone gives a card 500 ms to come
  out of "busy" before each block, and a card that pauses longer than that for
  its own housekeeping fails the write. A generic card in one of the two test
  phones refused **one tile write in eight** during a map download (35 rescued
  on a retry, 7 lost, in 311 tiles); a decent card in the other did it 4 times
  in 12,853. The firmware retries a refused write three times with growing
  pauses now and shows the count on the Download screen ("The card was busy
  N times") — a few is normal, dozens is a card to replace.
- **Swap cards with the phone OFF.** The power switch cuts power outright, so
  off is off — but pulling a card from a running phone can corrupt whatever was
  half-written.

---

## What's new

Full detail in **[CHANGELOG.md](CHANGELOG.md)** — every release, including the
bug fixes and why each one happened. Recent highlights:

- **0.9.81-NH** — **Menu → AI**: ask Google's **Gemini** a question from
  the keypad, on WiFi, with **your own free API key** in `gemini.txt` in the new **API Keys**
  folder on the card. The chat is saved, a topic breaks by itself after two hours, the free
  tier's daily limit falls back to the faster model, and the key is never shown on screen.
  **Weather on the Almanac**: Open-Meteo's forecast (now, the next day every 3 hours, a week of
  days) and the **NWS's active alerts** in their own colours, fetched on WiFi when the Almanac
  opens and **kept on the card for offline** — the Weather screen says how old it is.
- **0.9.80-NH** — **Menu → Almanac**: legal light (Washington's big-game rule by default,
  civil twilight as a setting) with a countdown, sun, moon, solunar periods, your position as
  lat/lon, UTM and MGRS with ground elevation and **magnetic declination** (NOAA WMM2025), and
  the date's details; Left/Right steps a day, and days past a US clock change show the time they
  will have. **Altitude on the map**: the ground under the crosshair against the ground under
  you, from elevation tiles the downloader now fetches with the map. **Metric or US units** for
  distances, heights and speed. COVEY has the same Almanac, checked against this one to the second.
- **0.9.79-NH** — the version now carries **`-NH`** and the splash says whose firmware this is.
  **Reading positions sync over KOSync** (an Xteink X4 running the fork, KOReader, or COVEY as
  the home server): *Sync my place* pushes to home first and then opens a 5-minute window an
  e-reader can pull from or push to, and a place from another device arrives as the same
  *Go there / Stay* card the LoRa sync uses — nothing moves a saved place but *Go there*. A
  KOReader that pulls from the phone gets "No progress found" instead of being sent to page 1.
  **The web installer no longer erases the phone's settings** (mesh key, book-sync passcode,
  KOSync memo; it wrote over NVS through 0.9.78). **Music rebuilt**: mono at half rate through
  its own half-second ring — clean through map pans and page turns, an honest `drops` counter, a
  resume that no longer bursts, titles that stop overlapping, and a notification chirp that lets
  the song carry on. **Phone 1's WiFi drops are gone** (the CPU-clock switch under the radio),
  **mesh transmits never stall the phone**, a Game Boy game gives the heap back whole at quit, the
  notification chirp is audible again (it had been erased since 0.9.66), and `pki forget <node>`
  re-trusts one node's key.
- **0.9.78** — **OpenTopoMap's real z17 is downloadable**, 2 to 20 km, throttled to at least
  2 s between z17 requests, and **built to run for days**: the job survives a WiFi drop, a
  call, a game or a restart, and resumes by itself on the network it started on. Stop takes two
  presses, and the form shows the time as well as the size (`50451 tiles, 6.2 GB, up to about
  36 h`). And **the map never goes grey while a shallower tile exists**: each spot is drawn
  from the most detailed level on the card, stretched, and the chip says which level and by how
  much (`z16-14 stretched 2-8x`). USGS has no z17 (it answers 404), so it still stops at z16.
  COVEY does the same, and now keeps the tiles it only STREAMED apart from the ones it
  downloaded, lists them, and can download a spot properly before card day.
- **0.9.77** — **zoom one level past the tiles**: the map goes a level closer than your card
  holds, stretching those tiles 2x. Bigger, not sharper — and it says so (`z17*` in the
  corner, "z16 tiles stretched 2x" on the status chip), while the scale bar stays honest
  because the view really is at that zoom. COVEY does the same with its own cache.
- **0.9.76** — **hold `#` to mute and unmute**, on any screen where nothing is being typed
  — the clock, the menus, a game — with a short buzz as the answer and the crossed speaker
  in the header (and now on the clock face). A tap of `#` still does what it did; the phone
  waits to see which you meant. Inside a text field `#` stays the field's (shift, the input
  mode, a held capital), and on the map it stays zoom.
- **0.9.75** — **Settings → Mute all sounds**: one switch, applied at the audio chip, that
  silences the loudspeaker — ring, chirps, the mesh pop, music, the Game Boy — while a call
  in the earpiece, headphones and vibrate all still work (a crossed speaker shows in the
  header). And the Game Boy now leaves the rest of the
  phone 20 KB of internal RAM while it runs, instead of taking the block 0.9.74 freed.
- **0.9.74** — **the phone gets 40 KB of internal RAM back**: the SDK reserved 56 KB for a
  Bluetooth controller nothing ever starts; it is released at boot now (idle free heap
  28 KB → 68 KB, largest block 25 KB → 67 KB on both phones). The map downloader on phone 1 had
  been sitting under its 14 KB handshake bar saying "waiting for memory"; it now runs with
  50 KB to spare, and when it does wait it says the numbers.
- **0.9.73** — the map's **hold-to-scroll no longer stops under a held thumb**: the keypad
  chip emits a release-and-re-press pair (4 ms apart) under a finger that never moved, and
  the map now rides through it (so do the F2 music key and the held-digit/`#` typing holds);
  a sweep **moves by time at twice the frame rate** (~12 frames a second, from ~5), so it no
  longer lurches. (`keys raw` also shows all 64 of its entries now, not the oldest 12.)
- **0.9.72** — **Files can copy, move and delete whole folders**: the `[ This folder... ]`
  row inside any folder. A delete counts first, then runs as a job the phone stays usable
  under, with Back to stop. (Deleting `/maps/home` from the phone was the ask.)
- **0.9.71** — the map says **where each map's tiles are**: switching onto one whose tiles
  are elsewhere reads `Map: home (z11-15) - its tiles are 35km NW of here`, and the Map area
  list ends every row with `here` or the distance and bearing. (An area that is not where
  you are looking used to look exactly like an empty one.)
- **0.9.70** — the map's **fourth side button cycles the map types** on the card, and a
  message on the map's strip **wraps onto a second row** instead of ending in `..` (the
  no-GPS-fix explanation was the one that did). The on-phone controls guide has the new key.
- **0.9.69** — the map's **snap is a landing rule now**: a tap is a smaller 12 px nudge (only
  a hold speeds up), and the map snaps onto a pin, place or node only when the crosshair
  **lands within 10 px of it** — you can scroll around markers again; the arrows no longer
  stop on anything in their path or jump between markers.
- **0.9.68** — on the map's **Download** screen every line **wraps** instead of ending in
  `..` (the estimate, the running status, the account of the last run, an error); a tile
  write the card refuses is tried once more before it counts as failed.
- **0.9.67** — the **Game Boy resumes where you left it**, the way COVEY does: Quit
  saves your place, the next launch of that game loads it by itself, and power-off saves
  too. The pause menu keeps a separate manual bookmark (Save/Load state) and gains **Clear
  state** (wipes both and restarts the game, asks first). The screen is **Fill** by
  default; a switch to 1:1 is remembered.
- **0.9.66** — the map's arrows also stop on **places from the mesh and nodes with a
  position**, not only pins; the strip names what you landed on.
- **0.9.65** — **Six things {user} noticed, fixed and measured.** The highlighted row of
  every list **scrolls** when its text does not fit. A Meshtastic message **stops at 200
  characters** with a live `N/200` counter, and one the radio refuses **stays on the screen
  with the reason** — the radio driver used to give up on long frames while they were still
  going out. The notification **buzz and chirp are timed from the clock** (they were stamped
  before a second-long database save) and the chirp starts in 20 ms from flash. The **Game
  Boy plays through the loudspeaker** unless headphones are in. On the map, **a held arrow
  keeps scrolling and speeds up**, and **a tap stops on a pin in its way** — on a pin, taps
  walk the pins (Snap to pins, in the Maps menu).
- **0.9.64** — **Maps**. Menu → Maps: your own tiles off the SD card, with
  everyone's last position, the mesh's shared places and your own pins on top. It opens
  where you closed it — and failing that, on your GPS fix. Pins stay private until you
  choose to share one, and sharing puts it on COVEY's map as a real Meshtastic waypoint.
  Tiles are **raw RGB565**, converted on the computer by `tools/convert_tiles.py`, because
  this phone has no PNG decoder and its JPEG decoder refuses greyscale — see
  [docs/maps.md](docs/maps.md). Costs +72 bytes of internal RAM, measured.
- **0.9.42** — Messages knows **who** you are talking to: conversations are labelled
  with the contact's name where your phonebook has one, and starting a new message to
  someone you have already texted opens that conversation instead of a second one
  beside it. Also: choosing a recipient no longer discards a message you had already
  typed.
- **0.9.33 – 0.9.41** — **T9 predictive text** (below), and the memory work that made
  it possible: the Nodes screen used to reboot the phone once the mesh got big enough,
  because menu rows were the one thing in the firmware still allocating from the ~20 KB
  internal heap rather than PSRAM. Also two latent panics on the typing path, and an
  adversarial review that found fourteen more bugs — including a dictionary that could
  not spell "cat".
- **0.9.28** — **the WiFi uploader is finally solid**: the upload page is served
  by the same lean single-connection engine that moves the file pieces, so
  loading it no longer eats the phone's memory and tripping the "site cannot be
  loaded" breaker — measured going from 2-of-10 page loads answered to 19–20 of
  20, and ~52 MB of test batches transferred with zero incidents. Also: the
  phone **auto-rejoins WiFi properly** (a one-way flag meant that editing any
  saved network quietly disabled auto-reconnect until reboot — found because a
  phone sat 78 minutes next to a hotspot it could join in seconds by hand).
- **0.9.9 – 0.9.27** — texting with delivery receipts, a Photos app + custom
  wallpapers, the e-reader syncing your place over the mesh, GPS/woods-backplate
  support, the screen lock actually locking, and a long tail of
  never-actually-ran code paths found and fixed — the changelog tells each story.
- **0.9.7** — the phone knows where everyone is: **Places** (shared map pins),
  live **distances and bearings** in the node list, **Sun & legal light**, and
  every list that used to run off the screen now wraps or ellipsizes.
- **0.9.6** — **direct messages work again with modern Meshtastic** (2.5+
  encrypts DMs with public-key crypto and silently drops the old form; the
  phone now speaks it natively).
- **0.9.5** — install from a browser, a **Files** app, and type-just-the-number
  texting (the server part of the address fills itself in).
- **Fixed since 0.9.7** (in the repo, riding the next release): a WiFi
  auto-switch deadlock that could leave the phone sitting next to a saved
  hotspot without joining it; GPS/woods-backplate support — and the plate now
  exists, is fitted to a phone and holds a live fix, so there is a
  [build guide](#woods-backplate); Sun and the GPS toggle moved into the menus
  — a feature that only exists as a serial command is not a feature.

---

## Game Boy Color emulator

Main menu → **Games → Game Boy**. Based on the retro-go fork of gnuboy, tuned
until real Game Boy Color games run at full speed on the phone.

- **Full speed with sound** — even heavy GBC titles. Sound plays through the
  **loudspeaker** (since 0.9.65 — it used to come out of the earpiece), or the
  headphones when they are plugged in, and switches by itself if you plug or unplug
  mid-game; the **top two side keys** are volume up/down in-game.
- **Picks up where you left off** (0.9.67) — Quit writes your place to the SD card
  (`/gbc/<game>-<cart>.auto`) and the next launch of that game loads it by itself, the
  way COVEY's RetroArch does; a power-off mid-game saves it too. **Save state / Load
  state** in the pause menu are a separate bookmark (`…state`) that quitting never
  overwrites — a checkpoint before a boss. **Clear state** wipes both (the game's own
  battery save with them) and restarts the game from its title; it asks first. Writes
  go through a temp file and are size-checked, so a battery that dies mid-save or a
  full card leaves the previous save intact rather than a torn one; a state is named
  for the cartridge it came from, so a different ROM uploaded under the same file name
  never resumes into it. Deleting a game from the list deletes its saves.
- **Get games in over WiFi** — pick **Transfer ROMs...** in the game list: the
  phone becomes a tiny website (`wiphone.local`); drag `.gb`/`.gbc` files onto
  it from any computer, **or paste a download link** and the phone fetches the
  file itself. Uploads travel as small CRC-checked pieces, one in flight at a
  time, so a fast network cannot flood the phone's memory and a dropped
  connection resumes where it left off instead of starting over. Multiple files
  at once, per-file progress, and honest retries before complaining. Falls back
  to hosting its own hotspot (`WiPhone-ROMs`) when not on WiFi. If
  `wiphone.local` won't resolve —
  phone-hotspot networks and Android browsers often can't do mDNS — use the
  `http://<ip>` address the phone's screen shows instead.
- **Big carts work** — 4 MB ROMs stream from the SD card on demand.
- **Comes with a game** — uCity (public-domain) is built into the firmware, so
  there is something to play before any SD card or upload.
- **Housekeeping in the list** — Back on a ROM offers to delete it (with a
  confirm); long names scroll so you can read them.
- **Two screen modes** — 1.5× **Fill** (the default since 0.9.67) or crisp 1:1,
  toggled from the pause menu and remembered; the menu also shows the measured game
  speed %.
- **In-app help** — the **Help...** row documents the controls and everything
  else a new user needs.
- Heads-up: WiFi and calls are off while a game runs (they come back when you
  quit) — the radio's RAM is the price of full speed.

Controls: D-pad moves, bottom-right side key = **A**, the key above it = **B**,
Back = **Start**, Select = **Select**, End (hang-up) = **pause menu**.

---

## Music player

- **MP3 and WAV straight from the SD card** — nothing to convert. Menu > Music.
  (Format is detected from the file's content, so a mislabelled file still
  plays; a track that will not decode is skipped with the reason shown in the
  list, instead of ending the album.)
- **Stereo** through the headphone jack, loudspeaker otherwise, and it follows
  the jack live: unplug mid-song and it carries on out of the speaker from the
  same spot.
- **The four side buttons are the transport**, top to bottom: play/pause,
  next (**hold** = previous), volume up, volume down. Hold-for-previous does the
  usual thing — back a track within 3 seconds, restart the current one after.
  On the Now Playing screen, keypad **4/6** and D-pad left/right also skip.
- **Keeps playing when you leave the screen.** Read a book or check the mesh
  with music going; tracks advance on their own. A phone call always wins the
  speaker: music pauses for it and deliberately never barges back in on its own.
- Shuffle, repeat (off / all / one), and its own WiFi uploader (same
  `wiphone.local` page; hosts its own hotspot when off WiFi).
- Volume starts low on every boot and is separate from the call volume, so a
  quiet album can never leave you unable to hear the next call.

Decoding is the **Helix MP3 decoder** (RealNetworks, RPSL — vendored under
`WiPhone/src/audio/helix-mp3/`), with its ~29 KB of working memory placed in PSRAM, because the
internal heap on this phone has nothing like that spare. Measured on the device:
about a quarter of realtime at 48 kHz stereo, so there is plenty of headroom.

## Photos

**Menu → Tools → Photos** — a viewer for `/photos` on the SD card, and the one
place wallpaper is chosen. Rename, delete, and lock live behind the viewer; get
pictures in over WiFi with the phone's upload page (or `up on photos` on the
serial console).

**What it can show — this hardware decodes exactly two formats:**

| format | viewer | wallpaper |
|---|---|---|
| **JPEG, baseline** (what phone cameras produce) | ✅ | ✅ |
| JPEG, baseline **greyscale** | ✅ | ❌ (the wallpaper loader refuses it) |
| JPEG, **progressive** ("optimized" web/editor exports) | ❌ refused with a message | ❌ |
| **BMP, uncompressed 24/32-bit** | ✅ | ❌ (wallpaper is JPEG-only) |
| PNG / GIF / WebP / HEIC | — no decoder on this chip; not listed at all | — |

Photos up to **2 MB** open (decoded in PSRAM, scaled to fit); the same 2 MB
limit applies to wallpaper. In practice: pictures straight off a phone camera
just work; if an editor saved something as progressive JPEG or PNG, re-save it
as a plain (baseline) JPEG. Files the phone can't decode are deliberately not
listed rather than shown as a grey rectangle — and when you set a wallpaper,
the phone tests the real loader on the spot and repeats its verdict instead of
claiming success.

## Maps

**Menu → Maps** — an offline map with everyone on it. Full detail, the tile format
and the Mac-side command are in **[docs/maps.md](docs/maps.md)**.

- **Your own tiles, off the card.** `/maps/<area>/<z>/<x>/<y>.565` — ordinary slippy `z/x/y`
  numbering, so whatever tile tree you already have works. Several areas, and it says which
  one it is showing.
- **Everyone is on it.** The mesh's shared places (COVEY's waypoints) as green gems, other
  people's last positions as cyan dots with their names — **grey, with an age, once the fix
  is over 30 minutes old**, because on foot half an hour is far enough to be somewhere else
  and a map that draws a stale dot like a live one is lying by omission. Your own GPS fix is
  a white ring.
- **It downloads its own maps.** `Menu → Download maps...` fetches an area around the
  crosshair over WiFi — **USGS Topo**, **USGS Aerial** or **OpenTopoMap** — 2 to 20 km, to
  z13–z16, or **z17 for OpenTopoMap** (USGS has none), with the tile count, the size on the
  card and the time shown before you press Start. A 5 km hunt area to z15 is ~270 tiles,
  34 MB and about four and a half minutes from USGS. A 20 km OpenTopoMap area to z17 is ~51,000
  tiles, ~6.2 GB and about 36 h: z17 is throttled to at least 2 s a request (a real run went at
  2.07 s a tile), and a run that long is expected to be stopped by life, so **the job resumes
  by itself** after a WiFi drop, a long call or a restart, on the network it started on. It
  carries on if you leave the screen, only fetches what is missing, and writes each tile under
  a temporary name, so a power-off mid-tile leaves nothing wrong on the card. Needs USB power
  or a battery above 3.8 V; Stop is two presses.
- **It never goes grey while a shallower tile exists.** Each spot on the screen is drawn from
  the most detailed level the card has there, stretched. The corner chip gets a star (`z17*`)
  and the status chip says which level and by how much (`z15 tiles stretched 4x`,
  `z16-14 stretched 2-8x`). You can zoom one level past an area's deepest tiles. The scale
  bar stays honest, because the view really is at that zoom.
- **Pins.** **OK** drops one on the crosshair and asks for a name; OK on a pin opens it to
  rename, move, share or delete. 64 of them, in a text file on the card, and **nothing about
  them reaches the air** until you choose *Share it on the mesh* — which asks **which channel**
  (defaulting to your position beacon's, and making you press a public one twice), remembers
  the answer with the pin so an update or a retraction goes to the same place, locks it to
  this phone so nobody else can move your camp, and tells you honestly whether the radio
  actually sent it.
- **It opens where you closed it** — saved on the way out and on power-off. Then, only if
  there is nothing to remember: your GPS fix, the mesh's reference place, the middle of the
  tiles on the card. `Menu → Go to coordinates` takes a typed latitude and longitude;
  `Menu → Follow me` keeps you centred until you scroll; `Menu → Measure from here` is a
  ruler (scroll away from the anchor, read the distance); the Pins / Places / Nodes lists are
  nearest-first with distance and bearing on every row. **Menu → What the buttons do...**
  draws the phone's buttons with what each one does on the map.
- **Arrows scroll — a tap is a 12 px nudge, a hold keeps scrolling and speeds up** (240 →
  960 px/s over the first 0.6 s, by time, so a slow frame moves further rather than later;
  and a hold survives the keypad chip's release-blip under a steady thumb — since 0.9.73). With
  **Snap to markers** on (it is, by default; the menu turns it off) the map snaps onto a
  pin, a place from the mesh or a node with a position **when you land within 10 px of it**
  — after a tap, or where a hold stops — and the strip says what you landed on: `Camp
  (place from the mesh)`, `{user} H (node, 12m ago)`. Nothing else snaps: a sweep sails past
  markers, and the tap after a snap steps clear, so you can scroll around them (since
  0.9.69; it used to stop on anything in a tap's path). **The top two side buttons zoom**,
  the third **centres on you**, the fourth **cycles the map types** on the card (topo →
  aerial → OpenTopoMap, since 0.9.70) — the map owns the side buttons while it is open, so
  a track loaded in the music player cannot turn zoom into play/pause. `7`/`9` step through
  your pins in order. A message on the strip that needs two rows gets them.
  **Menu → What the buttons do...** is the whole table, on the phone.
- A scale bar, the crosshair's coordinates, and how far the crosshair is from your reference
  place — "1.4km NE Camp" — along the bottom.

The tiles are raw RGB565 on the card (~128 KB each) so the viewer decodes nothing while you
pan; the downloader decodes once, on arrival, with the JPEG and inflate code already in the
ESP32's ROM. You can also convert a tile tree on the Mac and push it over WiFi without
touching the card:

```bash
python3 tools/convert_tiles.py ~/covey-tiles ~/tiles-565/home --zoom 12-15
python3 tools/wiphone_send.py --app maps --tree ~/tiles-565/home
```

⚠ **HTTPS on this phone was impossible until 0.9.64** — the framework builds mbedTLS to
allocate from the internal heap, which sat at ~25 KB — and the downloader is the first thing
on the phone to do it, by moving mbedTLS into PSRAM and running on its own task. The internal
heap dips by ~9 KB during each handshake. The downloader waits for 14 KB free before it opens
a connection and, since 0.9.74, says on screen how much the phone has against that when it
cannot (`Needs 14 KB free (has 13.6)...`); the same release gave the phone 40 KB more internal
RAM, so on both phones a download now runs with ~50 KB to spare. Details and the measured
numbers are in [docs/maps.md](docs/maps.md).

## Almanac

**Menu → Almanac** — sun, moon, solunar tables, where you are and what day it is, all
**offline** (pure arithmetic on the phone; nothing needs a network). The details, the formulas
and the accuracy are in **[docs/almanac.md](docs/almanac.md)**.

- **Today**, on one screen: the countdown a hunting day actually asks (`LEGAL LIGHT: 3h 12m
  left`, `First light in 5h 02m`, `Dark - first light 06:34`), today's legal light and sunrise
  to sunset with the day's length, the moon's phase, the solunar rating and the next period, and
  which place it is all for (your GPS, your pin, or a waypoint chosen in Meshtastic > Places;
  with none of those, the last GPS fix since the phone started — `At last GPS 2h 5m ago`, if it
  had 4+ satellites and HDOP 10 or better, the bar every screen of the phone uses — and then
  where the map was last looking, `At map view`).
  **Left/Right** move a day at a time, a year either way; a day not worked out yet says
  `Computing...` for a moment (the phone works it out a slice at a time, so nothing else
  stalls). A day across a US clock change is given in the clock time that day will have, and
  says so (`Times in UTC-8 (after the Nov 1 change)`) — on Oct 27 at UTC-7, Nov 7's end of
  legal light reads 17:10, as the clock will on Nov 7, not 18:10. On the change day itself
  every day screen says so: with a **time zone** set (Settings → Time zone: Auto or a named
  zone, 0.9.82) the phone changes its own clock and the screens say it did; with a typed offset
  they say to check the Time offset (a fixed offset cannot change itself, and the phone cannot
  know whether you have).
- **Sun**: first and last legal light, sunrise, solar noon, sunset, civil twilight, the day's
  length and how much it changed since yesterday, and where the sun is right now.
- **Moon**: rise, overhead, set and underfoot in time order, phase, illumination, age, and the
  next new moon, quarters and full moon with their dates (each on the clock of its own date,
  across a US clock change too).
- **Solunar**: John Alden Knight's major and minor periods and the day's rating — **labelled as
  the folk tables they are**, not as science.
- **Position & GPS**: latitude/longitude, degrees and minutes, **UTM and MGRS**, the ground's
  height from the elevation tiles on the card, the GPS's altitude, speed and course, satellites,
  and the **magnetic declination** with how to use it (`true = magnetic + 14.8`).
- **Date & seasons**: day of the year, ISO week, the clock and who set it, the next equinox or
  solstice, and a reminder in the three weeks before the US clocks change (`US clocks go back 1 h
  Sun Nov 1 - set Time offset then`; on the day, `US clocks went back 1 h at 2 AM today - check
  Time offset`). It never names an offset: the phone's offset alone cannot say whether the change
  has been made (-8 on Nov 1 is Pacific done or Alaska not yet). Hawaii and Arizona, whose clocks
  never change, set **Almanac → Settings → US daylight saving: no**, which turns off the reminder
  and the clock-change shift above. With a time zone (0.9.82) none of that applies: the zone's
  own rule decides, to the second, and the reminder reads `Clocks go back 1 h Sun Nov 1 - the
  phone changes by itself`.

**Legal light has two rules**, chosen in Almanac → Settings: **30 minutes before sunrise to 30
minutes after sunset** (the default — Washington's big-game rule), or **civil twilight** (the
sun 6° below the horizon; at 47° N that is 31 to 41 minutes, longer than 30 all year). Every screen that
counts it down says which rule it used, and the times are rounded **inward** — first light up,
last light down — so the window shown is never wider than the real one. It is only as good as
the clock and the place, and the screen names both; a clock set from the mesh is flagged. The
Meshtastic "Sun & legal light" screen and the serial `sun` use the same computation and rule.

**Units**: metric or US (feet, miles, mph — and for the weather °F, inHg and inches), one
setting (Almanac → Settings) shared with the map's scale bar, distances and heights.

### Weather

**Almanac → Weather...** — the forecast for the Almanac's place, and the US weather alerts for it.

- **Where it comes from**: [Open-Meteo](https://open-meteo.com)'s free forecast (no account, no
  key; **Weather data by Open-Meteo.com, CC BY 4.0** — the screen says so) and the **US National
  Weather Service**'s active alerts (public domain). The phone sends only the place rounded to
  about a kilometre (and ~10 m for the NWS), the ground height under it from the elevation tiles
  on the card (mountain temperatures depend on it), and a User-Agent naming this project. Both
  are fetched over HTTPS with the Let's Encrypt roots built in.
- **When**: on opening the Almanac (and the Weather screen) with WiFi up, when what is on the card
  is over an hour old or was fetched more than ~5 km away — once the day's tables are done, never
  while music plays, and not again for 10 minutes after a failed try. **Refresh** (the left soft
  key on Weather) fetches now, pausing the music as the AI does. Nothing runs in the background.
- **Offline**: the last forecast is kept on the card (`/wx/weather.txt`) and carried into the
  woods: hours that have passed drop off, *Now* becomes *Forecast for 14:00* an hour after the
  fetch, days are labelled on the phone's own clock, and past the last day it says **No forecast
  after Fri Oct 9 - refresh on Wi-Fi**. Above its credit the Weather screen says **As of 07:10 (3h 05m
  ago) for GPS** (the age only on a clock set from NTP or GPS) and it warns when the place's time zone is not the
  phone's.
- **Alerts come first, in colour** (red for Severe/Extreme, yellow otherwise) — and **only in the
  US**: elsewhere the screen says **Alerts: US only (NWS)**. It never says a bare "No alerts": an
  empty answer is **No alerts at 07:10**; a failed check is **Alerts not checked**, keeping the
  last ones with their own time; a storm day too big for the phone's buffer is **Alerts: 9+ (too
  many to list)**. An alert past its end is hidden offline. TODAY shows the most severe alert in
  force near the top, and a one-line summary: `Weather: 52F, rain 60% this afternoon` within an
  hour of the fetch, `Forecast: ...` after it, and `Forecast 12 km away: ...` read from elsewhere.
- Wind is where it blows **from** (`Wind from NW 14 km/h, gusts 30`). Serial: `wx`, `wx fetch`,
  `wx show`, `wx clear`, `open weather`.

**Where the numbers come from**: the sun and moon are the standard published theories (NOAA /
Meeus; the moon is Meeus's ELP-2000/82 series), checked in the host tests against independent
PyEphem values — sun and moon events to well under a minute (17 s worst, at 71° N). The declination is
NOAA's **World Magnetic Model 2025** (public domain), which is **valid 2025.0 to 2030.0**: after
that the screen says the model has expired instead of showing a number that has quietly drifted.
UTM/MGRS are WGS84, checked against the `utm` and `mgrs` packages. Ground heights come from the
AWS Open Data **Terrain Tiles** (USGS 3DEP/NED, SRTM, GMTED and ETOPO, via Mapzen/Nextzen),
converted into `/maps/elev` on the card on the Mac, or fetched by the phone itself with a map download
("Elevation too", on by default) — see [docs/maps.md](docs/maps.md), "Altitude".

## AI (Gemini)

**Menu → AI** (the speech-bubble icon on the main screen) asks Google's Gemini anything you type
on the keypad, over WiFi. It needs **your own API key**. A key is free, and nothing is built into
the firmware: this repo is public, so the key lives only on your phone's card.

### 1. Get a free key

1. On a computer or phone browser, open **[aistudio.google.com](https://aistudio.google.com)** and
   sign in with a Google account.
2. Choose **Get API key**, then **Create API key**, and copy it. (Keys made since 2025 may look like
   `AQ.Ab8…` rather than the older `AIza…`; both work.)
3. Keep it to yourself, like a password: anyone holding it can use your allowance. If it leaks,
   delete it in AI Studio and make a new one.

The free tier is enough for the phone: about **20 answers a day from Gemini Flash**, then the
phone switches to **Flash-Lite**, which has its own, larger daily allowance. You can see your
exact limits in AI Studio under **Rate limits**.

### 2. Put the key on the phone

Make a plain text file named **`gemini.txt`** containing one line:

```
key=YOUR_KEY
```

and put it in the **`API Keys`** folder on the card. The phone makes that folder every time it
starts. Any one of these works, easiest first:

- **Over WiFi from a browser:** on the phone, **Menu → Tools → Files → API Keys → [ Upload into this folder ]**,
  then open the address the phone shows (or `http://wiphone.local/`) on a computer or phone on the
  same WiFi and pick the file. **AI → Menu → Key info → Add key file over WiFi** does the same.
  Use your home WiFi: if the phone has no network it makes its own hotspot, and that hotspot is
  **open**.
- **With a card reader:** copy `gemini.txt` into `API Keys` on the card.
- **From a computer with this repo (developers):** `up on keys` on the serial console, then
  `tools/wiphone_send.py --app keys --ip <the phone's address> gemini.txt`. The file goes over
  WiFi (the cable only starts the uploader), and a key file is always sent whole, so a replaced
  key of the same length takes.

Then open **AI → Menu → Key info**: it should say **`key: set (N chars)`** (the length only - the
key itself is never shown). The phone reads the file each time the app opens and before every
question, so a new or replaced key works at once, no restart.

The same file works on a T-Deck running the T-UI firmware, which reads `gemini.txt` from the
card's root (the phone looks there too, after `API Keys`).

**Optional lines** in `gemini.txt`: `model=` (default `gemini-flash-latest`), `thinking=0` to
`8192` (how long the model may think before answering; default 1024, `0` = off - faster but
plainer), `fallback=` (default `gemini-flash-lite-latest`, or `none`) and `lookup=off` (no
Wikipedia lookup before each question - see **Current facts** below; on by default). `#` comment
lines, a BOM, Windows line endings, quotes and spaces around `=` are all fine.

### 3. Use it

- **Ask:** press **OK** to open the question box and type. Predictive text works as in Messages:
  **`#`** switches T9 → Abc → ABC → 123, **`*`** gives punctuation, **`0`** is a space, **Up/Down**
  picks another word, **Back** deletes. Send with **Ask** (the left soft key) or **Call**;
  **End** cancels the question.
- **Wait:** the status shows what is happening (`Looking up Wikipedia...`, `Asking Gemini...`,
  `Google is busy - trying again...`) and the seconds so far. The free Flash model has been slow
  lately - **30-90 seconds** for an answer is common, Flash-Lite is quicker - and the phone waits up
  to **2.5 minutes**. If nothing comes in that time (or the connection drops before the answer is
  whole), it asks **once more** (`No answer in time - trying again (2 of 2)...`), and if that fails
  too it asks **Flash-Lite once** (`Asking Flash-Lite instead...`) - the answer then says
  `Flash-Lite` under it. That is the **Retry if it times out** setting in **Menu** (on by default;
  OK on the row turns it off and on, and the phone remembers it). Off, a timeout is reported at
  once. It never retries a refused key, a daily limit or anything you cancelled, and the Wikipedia
  background found for the question is reused, not looked up again. **End** cancels (so does a call coming in);
  **Back** leaves the app and the question carries on - the answer is in the chat when you come
  back. If music is playing it **pauses** for the question (the phone needs its full speed for the
  secure connection); press Play in Music to carry on.
- **Read:** answers are plain text. **Up/Down** pages through the chat; under each answer it says
  which model wrote it (`Flash` or `Flash-Lite`).
- **Current facts:** the model's own knowledge is a year or more old, so every question tells
  Gemini **today's date and time** (only when the phone's clock is set from the network or GPS -
  otherwise none, rather than a wrong one), and before each question the phone **looks it up on
  Wikipedia itself**: it takes the question's key words (`has the new tomb raider legacy of
  atlantis come out` -> `tomb raider legacy atlantis`; `what's` and `baldur's` count as `what` and
  `baldur`; accents and dashes are folded the same way COVEY and the X4 fold them, so
  `navier–stokes` is `navier stokes` and `gauß` is `gauss`; a follow-up like `and when?` or `how long is it?` borrows the words of the newest
  earlier question in the topic that named something), asks Wikipedia's search for the top two
  pages, keeps only those whose **title or description** has one of the key words in it, and gives
  Gemini their first few sentences along with the question, marked as background to use only if it
  fits.
  An answer that came with background ends with a **`Wikipedia:`** line naming the pages
  (`Wikipedia: Tomb Raider: Legacy of Atlantis; Tomb Raider: Catalyst`); with none, the answer is
  from the model's own memory, which can be out of date - it is told to say so rather than guess.
  **Only those key words go to Wikipedia** - never the whole question, the chat or the key - over a
  checked secure connection, once per question (it adds a few seconds). If Wikipedia does not
  answer, or finds nothing, the question is asked without it. `lookup=off` in `gemini.txt` turns
  the lookup off. (Google's own Search grounding is not free on the current models, and Gemini's
  web-page reading tool made the free Flash go silent for minutes, so the phone does the looking
  up.)
- **Follow-ups:** Gemini remembers the **current topic** - your last four questions - so "and how
  long is it?" works. After two hours without a question the next one starts a new topic by
  itself (when the phone's clock is set from the network or GPS); **Menu → New topic** starts one
  now, and **Menu → Clear chat** deletes the whole chat.
- The chat is saved on the card (`/ai/chat.txt`, up to the last 20 questions or 16 KB, whichever
  is less) and survives a restart.

### When something goes wrong

| The phone says | What it means |
|---|---|
| `No key - put gemini.txt in API Keys` | No key file was found - see step 2. |
| `Key rejected` | Google refused the key: check it was copied whole, or make a new one. |
| `No WiFi - join a network first` / `WiFi is off` | Join a network in **Settings → Scan WiFi networks**; `WiFi is off` means turn on **Settings → WiFi** first. |
| `Google is busy - trying again...` | Normal at busy times: it retries twice, then asks Flash-Lite. Any server-side error (500, 502, 503, 504) is treated this way. |
| `Daily free limit for Flash reached - using Flash-Lite until about 17:00` | Flash's 20 a day are used; answers come from Flash-Lite until the time Google gave. |
| `Today's free Gemini limit is used up` | Both models' daily allowances are used; it says when to try again. |
| `Secure connection failed` | The connection to Google did not complete: try again; if it keeps happening, check the WiFi. |
| `Phone low on memory (...) - reboot first` | Too little unbroken memory is left for a secure connection (checked before every one, the Wikipedia lookup's included): restart the phone, then ask again. |
| `No answer in time - trying again (2 of 2)...` / `Asking Flash-Lite instead...` | Flash sent nothing in 2.5 minutes: the phone is asking once more, then Flash-Lite (**Menu → Retry if it times out**). |
| `Gemini did not answer in time` | No answer after the retries (or with **Retry if it times out** off): ask again, or ask for less. |
| `Gemini sent an empty answer - ask again` | It stumbled twice in a row (it is asked again once by itself): ask again. |
| `A call came in - ask again after it` | A call rang while the phone was waiting: the question was stopped for it. |
| No `Wikipedia:` line under an answer | Nothing was found, no page found was about the question's key words, Wikipedia did not answer (the serial `ai` says why), or `lookup=off` is set. |
| `A map download is running` (or a call, a game, the uploader) | One thing at a time: ask again when that is finished. |
| A question marked failed in the chat | It stays there for reference and is never sent back to Gemini. |

### Safety of the key

The phone checks Google's certificate against Google's own root certificates (built in, and
nothing on the card can replace them), and sends the key in a header, never in a web address. The
Wikipedia lookup is checked against the Let's Encrypt roots the phone already carries, and it is
sent nothing but the question's key words (and a User-Agent naming this project, as Wikipedia
asks): no key, no chat, not the whole question. The key
is never printed on the serial console (`ai` shows only its length), never logged and never
drawn: the Files viewer refuses to open anything in `API Keys`, Books never lists `gemini.txt`,
and `tools/card_clone.sh` leaves the folder out of a cloned card. The WiFi uploader only ever
WRITES into `API Keys`: it never reads, lists or resumes a key file (nobody on the network - which
may be the phone's open hotspot - can learn whether a key is there or how long it is), and a key
file is always written whole, so a browser upload of a new key the same length as the old one
takes too. A key pasted on the `model=` line
by mistake is ignored rather than used as a model name, and Key info says so.

## E-reader

- **EPUB and plain text** from the card, with **pictures inline**. Menu > Books.
  Numbered pictures enlarge to full screen when you press that number key.
- **Books arrive over WiFi too** — the library's top row starts the same
  drag-and-drop upload page the emulator uses.
- **Three text sizes**, switched from the reader menu; your place is preserved
  across the change, and a **chapter list** jumps anywhere in the book.
- **Your place is saved** and survives the phone losing power without warning.
- Position is **(chapter, character offset)**, never a page number — so it still
  means something after you change the font size, and on a different device.
- **Greyscale JPEGs decode**, which the ESP32's built-in decoder cannot do at
  all (it handles 3-component colour only, and most book art is 1-component).
- **Book sync over LoRa** — share your reading position with another device on a
  private channel. Jumps are confirmed, never taken silently — and an offer
  whose clock looks wrong says so instead of being trusted. Diagnostics live in
  Books → menu → Sync settings.

## Meshtastic features

- **Two-way Meshtastic text messaging** on the default LongFast channel (US),
  AES-encrypted and interoperable with regular Meshtastic nodes.
- **Encrypted direct messages that modern Meshtastic accepts.** Meshtastic 2.5+
  requires public-key crypto for DMs and silently drops the legacy form — this
  phone speaks the real thing (X25519 + AES-256-CCM, keys learned automatically
  from NodeInfo, incoming DMs acknowledged). Proven against stock 2.7 hardware,
  both directions.
- **Multiple channels** — import custom channels from a Meshtastic share link
  (decoded on-device, no internet). Each channel shows up as its own chat.
- **Places** — waypoints shared on the mesh (camp, the truck, a stand) appear
  in **Meshtastic → Places**, with expiry and owner-locked edits. Pick one as
  your reference and the **Nodes list shows live distance and bearing** to
  everyone — "3.2km E of camp · 4 min ago".
- **"I'm here (announce)"** — declare yourself at a waypoint; the phone
  broadcasts one position so other devices' maps show it (preferring a private
  channel over public LongFast, and admitting it honestly when the send failed).
- **Sun & legal light** — first and last legal light (under the Almanac's rule:
  30 minutes either side of the sun by default, or civil twilight), sunrise,
  sunset and a countdown ("LEGAL LIGHT: 13h 34m left") for the reference place,
  computed offline.
- **GPS** — a receiver on the expansion header gives the phone its own live
  position; the toggle is in My node, and everything above works without it.
  The receiver rides on the **[woods backplate](#woods-backplate)** — built,
  fitted and holding a fix, with a full build guide below.
- **Phone location** (0.9.82). No GPS plate? Join your phone's hotspot and let an app on
  the phone share its GPS. The WiPhone finds it by itself (**My node → Phone location: auto**,
  on by default). It uses it for the map's "me (phone)", distances, the Almanac and, if you
  turned position reporting on, the mesh. The GPS plate still wins when both have a fix. It
  never guesses a location from your internet address.
  - **Android:** **NH Devices Companion**, a small free app in this repo
    ([`android/`](android/README.md),
    [download the APK](https://github.com/Nikguy321/wiphone-meshtastic/raw/main/android/dist/nh-companion.apk)).
    One button sets it up; only devices on your hotspot can read the position, and the GPS is
    on only while one is connected. On Samsung phones, also turn on Developer options → **Force
    full GNSS measurements**: otherwise Samsung switches the GPS off when the phone lies still. Or *gpsdRelay* (F-Droid) / *Share GPS* (Play) as a TCP
    server on port **10110**. gpsdRelay stops when a Samsung phone's screen locks.
  - **iPhone:** *GPS 2 IP* in Socket mode (port **11123**). It is shown on the WiPhone but not
    sent to the mesh, because iOS gives apps no accuracy figure. Other iPhone apps are listed,
    untested, in the guide.
  - **Step-by-step setup for each app, the hotspot settings and what every status line means:
    [docs/phone-location.md](docs/phone-location.md).**
  - Only a fresh fix with 4+ satellites and HDOP 10 or better is ever sent. It is marked as
    coming from a phone (External), and the clock is never set from it.
  - **Who it trusts:** whatever answers on the joined network's gateway. On your own hotspot
    that is your phone. On someone else's WiFi (a hotel, a campground, a boat's NMEA gateway) a
    device serving NMEA could set where the WiPhone thinks it is. Turn Phone location **off**
    on networks you do not control.
  - Upgrading with position reporting switched on but the GPS off (or aimed at a public
    channel)? Phone location starts **off**, so nothing begins going out by itself. Turn it on
    in My node.
- **Wi-Fi location** (0.9.82, **off** by default). With no GPS and no phone fix, the WiPhone can
  ask beaconDB where the WiFi access points around it are (**My node → Wi-Fi location: off / map
  only / map + mesh**). It sends only access point addresses and signal strengths, never names,
  and turns beaconDB's internet-address fallback off. It believes an answer only when two separate
  lookups (the access points split in two halves) agree within 100 m, and drops it the moment a
  scan no longer hears those access points (or the WiFi drops), or when it lands far from your last
  GPS fix. The map shows `me +/-60 m Wi-Fi` with a ring. **map + mesh** (it starts when you leave
  My node) may also send it as a ~182 m cell after two confirming scans. It often finds nothing:
  beaconDB knows few homes and campsites, and that is by design. Details:
  [docs/phone-location.md, Wi-Fi location](docs/phone-location.md#wi-fi-location).
- **Node list** with friendly names (learned from NodeInfo), an **editable node
  name** (long and 4-character short), and a **configurable hop limit**.
- **Mesh client role** — relays/rebroadcasts other nodes' packets to extend the
  mesh's range (flood routing).
- **Messages that fit the air.** The compose screen stops at **200 characters** — what
  the Meshtastic phone apps allow, so it is the longest anyone can send back — with a live
  `N/200` counter, and a message the radio refuses **stays on the screen with the reason**
  instead of vanishing (the radio driver used to give up on long frames while they were
  still going out, so anything near the limit was truncated on the air and never seen).
- **New-message notifications** — a status-bar icon (with unread counts per chat),
  a brief on-screen popup, plus a quiet "pop" sound and a short vibration
  (designed to be unobtrusive — e.g. usable as a communicator while hunting).
  Meshtastic has its own entry in the sound settings: ring+vibrate, vibrate
  only, or silent; the buzz length is a slider under **Settings → Notifications**.
  Since 0.9.65 the buzz and the chirp are **timed from the clock** — they used to be
  stamped before a second-long database save and were sometimes cut short — and the
  chirp plays from flash, so it starts in 20 ms rather than after a file open.
- **Persistent** message history, channels, places and keys across reboots.
- **Low-power green/black UI theme** for the Meshtastic app, and a Meshtastic
  icon on the main menu.
- **Handy extras** — hold **Select + Back** (the top-left and top-right corner
  keys) together for two seconds to turn the screen off.

The normal WiPhone experience (phone calls, SIP, contacts, games, settings) is
untouched.

---

## Woods backplate

The stock WiPhone back cover carries a LoRa radio and a small flexible antenna.
The **woods backplate** is a custom cover that swaps it for what you actually
want out in the trees: the same RFM95W radio but on a **real 915 MHz whip**, a
**GPS receiver** so the phone knows where it is (and so your position and your
Places go out on the mesh), and a **second, bigger battery** that charges the
phone from your pocket. It bolts onto the WiPhone's expansion header — no custom
PCB and no reflow oven, just wire, a soldering iron and an afternoon.

### ➡ **[Start here — the beginner's wiring map](https://github.com/Nikguy321/wiphone-meshtastic/blob/main/docs/woods-backplate-start-here.svg)** ⬅

One page: what every part is for, what plugs into what, and the ways to hurt
something. **Open this one first**, even if you have built things like this
before.

[![The woods backplate start-here diagram — one-page beginner's map](docs/woods-backplate-start-here.svg)](https://github.com/Nikguy321/wiphone-meshtastic/blob/main/docs/woods-backplate-start-here.svg)

*(The preview above is squeezed to README width and will be unreadable on a
phone — click it, or the link above it, to open it full size and zoomable.)*

Then, when you are ready to actually build:

- 🔧 **[The wiring sheet](https://github.com/Nikguy321/wiphone-meshtastic/blob/main/docs/woods-backplate-wiring.svg)** — the leg-by-leg build sheet: every pad,
  every capacitor leg, a numbered run list and a before-first-power checklist.
  **This is what you build from.** The map above is a map.
- 📦 **[The full order list](https://github.com/Nikguy321/wiphone-meshtastic/blob/main/docs/woods-backplate-bom.md)** — every part with prices, links, vendors,
  alternates and the reasoning behind each choice.
- 📐 **[The design doc](https://github.com/Nikguy321/wiphone-meshtastic/blob/main/docs/woods-backplate.md)** — why it is wired this way, and what was measured to
  prove it.

### Three things to get right *before* you order

⚠ **The pack needs its own protection board, and the diodes have a direction.**
This is a bare LiPo cell wired next to a phone that has its own LiPo cell. Buy a
pack with a protection PCB on it — not a naked cell. And when D2 goes in, meter
the band before you apply power: **D2 backwards lets the pack uncontrolled-charge
the phone's own battery**, which is the one mistake here that starts a fire.
Never hard-parallel the two packs onto VBAT either; that destroyed both packs'
protection FETs the last time it was tried. And **meter the JST polarity before
you plug the pack in** — red is *usually* positive, but Adafruit themselves warn
that the shell colours vary, and a reversed LiPo plug vents the cell.

⚠ **It needs a buck-*boost*, and the order list's Adafruit table still sells you
a buck.** Row 3 of that table (Adafruit 4711, TLV62569) is superseded — a buck
has a 3.4 V input floor and a 1S cell ends at 3.0 V. Order the **TPS63020**
module in the table below instead. While you are at it: **the two Schottky
diodes have no row in the order list at all**, so buying strictly off its tables
leaves you without them. They are in the table below.

⚠ **R7, the 10 kΩ pull-up, is not optional — without it the phone may not boot.**
The radio's chip-select pin floats during every ESP32 reset; if it drifts low the
radio drives the MISO line, and MISO is GPIO 12, which is the flash-voltage boot
strap. High at reset means the phone comes up dead-screen. One resistor.

The wiring map and the sheet carry the rest (SMA vs RP-SMA, never keying up
without an antenna fitted, the GPS pad whose silk screen lies, the polyfuse that
must not be 1 A). Read them; do not build from this page.

### What you have to buy

| Part | Why it is there | Where |
|---|---|---|
| **Already on hand** — RFM95W (SX1276 915 MHz) ×2 · HGLRC M100 Mini GPS · 1S LiPo (larger, **must have its own protection PCB**) · WiPhone Header Breakout (screw terminals) | the radio, the GPS, the pack, and the plate they all bolt to | you already have these — do not re-order |
| **SMA ↔ u.FL adapter cable, 15 cm RG178** | pigtail *and* panel-mount SMA bulkhead in one part — drill the cover for its nut | [Adafruit 851](https://www.adafruit.com/product/851) · $3.95 · ⚠ **SMA, not RP-SMA** |
| **PowerBoost 1000C** | the entire battery half: 1 A LiPo charger + 5.2 V boost + load-share + its own microUSB in | [Adafruit 2465](https://www.adafruit.com/product/2465) · $19.95 |
| **TPS63020 buck-boost module, 3.3 V** | the plate's own 3.3 V rail — the phone's 3.3 V pin cannot feed the radio | Amazon ASIN `B0H3KQ1VXJ` (DWEII 6-pack) · ~$13 · alt: Pololu S9V11E2F3 #5712 |
| **2 × Schottky diodes** (1N5819 / SS14 / BAT54) | D1 + D2 OR the pack's 5.2 V and the phone's VBAT into the regulator, so a dead pack cannot kill the radio | any assortment · D2's reverse blocking is safety-critical |
| **915 MHz whip antenna, SMA male** | the actual antenna | Amazon: `915MHz LoRa antenna SMA male 3dBi` · ~$11 · ⚠ **SMA male, not RP-SMA** |
| **Polyfuse, 3 A hold (GF300)** | sits in the cell-positive leg, which carries full boost *input* current (~1.9 A worst case) | Amazon: `PPTC resettable fuse 3A` · ~$8 · ⚠ **not 1 A** |
| **Resistor assortment** | 1 × 4.7 kΩ (EN pull-down) · 4 × 1 kΩ (R3–R6, anti-phantom-power) · 1 × 10 kΩ (**R7 NSS pull-up — required**) · opt. 1 × 10 kΩ (RESET) | Amazon: `resistor assortment kit` · ~$10 |
| **Ceramic capacitor assortment** | 2 × 100 nF + 1 × 10 µF (radio) + 1 × 22–47 µF (GPS) | Amazon: `ceramic capacitor assortment kit` · ~$12 |
| **u.FL / IPEX SMT receptacle** *(optional but recommended)* | lets you A/B the whip against the stock plate's known-good FPC antenna — the best deafness test you have | Amazon: `U.FL IPEX SMT receptacle connector PCB mount` · ~$8 |
| **JST-PH 2-pin pigtails** | so the pack can be split off for separate charging | Amazon · ~$8 |
| **Silicone hookup wire, 26–30 AWG** | thin and flexible — it has to survive the case closing | Amazon · ~$15 |

Two vendors: Adafruit (2 items) and Amazon (everything else). The Amazon rows are
written as **specs to match, not sellers** — those listings rotate constantly.
Prices were live on 2026-08-11; re-check before you total it up. Full 164-line
version, with the reasoning and the alternates, in
**[the order list](https://github.com/Nikguy321/wiphone-meshtastic/blob/main/docs/woods-backplate-bom.md)**.

### Where this stands

The plate is real: one is built, fitted to a phone and proven on air — the phone
charges from the pack, the radio TXes through the whip, and the GPS holds a live
fix (9 satellites, HDOP 1.1). But that built plate is the **v1** rail, and the
wiring sheet and the map above describe **v2**: the revision that adds the
buck-boost and the two diodes so the radio and GPS keep running on the phone's
own cell after the external pack dies. **Build v2** — it is the better design and
the sheet is correct — but do not read the sheet as a record of something already
tested end to end. The firmware half is done and shipping either way; the GPS is
off until you turn it on (**My node**, or `gps on` over serial).

---

## Predictive text (T9)

Five keypresses for "hello" instead of thirteen. Type `d-o-n-t` and get **don't** — the
apostrophe comes free, exactly as it did on the phones this is imitating.

It is **on by default**, with a **Settings → Predictive text** switch that remembers, and
the word being predicted shows in the footer rather than in your message, so nothing is
committed until you accept it.

| Key | What it does |
|---|---|
| **2–9** | build the word; the prediction appears in the footer |
| **UP / DOWN** | step through the other words on those digits — only while a word is pending, so they still move the cursor otherwise |
| **OK** | accept the word |
| **0** | space, and accepts the word first |
| **BACK** | un-types one **keypress**, not one letter |
| **`#`** | cycles **T9 → Abc → ABC → 123** — the classic escape, and 123 is how you type "testing 1 2 3" |
| **hold a number** | types the digit itself, without leaving T9 |
| **hold `#`** | capitalises the next word — for a name in the middle of a sentence |

Sentences capitalise themselves: the first word, and the first word after `.`, `!` or `?`.

Where it works: **message bodies** (Meshtastic and SIP) and the **note page**. Where it
deliberately does not: SIP addresses, WiFi passwords, IP addresses, the four-character
short name. A dictionary in a password field is a bug, not a feature, so every field opts
in explicitly and the default is off.

### Your own words

Jargon your dictionary has never heard of — unit names, callsigns, place names, a team
roster — goes in a plain text file at **`/t9/extra.txt`** on the SD card. It loads at boot
and its words are always offered **after** the built-in English ones, so they are there
when you want them and never in the way.

It is a file on the card and not part of the firmware on purpose: one person's vocabulary
has no business in a stranger's phone, and it means adding words is a file copy rather than
a reflash. `tools/gen_t9_extra.py` builds one from any word list; `up on t9` on the serial
console starts the WiFi uploader pointed at it, and `t9 reload` picks up the new file
without rebooting.

### Why it does not eat the phone

The dictionary is **25,000 words in flash** (`static const`, memory-mapped, read by plain
pointer dereference) and the engine **allocates nothing at all** — every buffer is a fixed
member. That is not tidiness: on this phone an internal-heap allocation failure inside
`new` throws, nothing catches it, and the phone aborts. The only safe amount of allocation
on the keypress path is none.

Cost: **+64 bytes of RAM**, 270 KB of flash, and a worst-case lookup of 25 binary-search
probes — about 0.05% of the 250 ms the firmware treats as a stall. The optional SD
dictionary lives in PSRAM and costs the internal heap nothing either.

---

## Texting

- **Conversations, not an inbox and an outbox.** Messages opens on a list of
  people, newest first, with unread counts on the row. Open one to see the whole
  exchange in order, oldest at the top, your messages headed `You · 2 min ago`.
  Reply is one press and already knows the address; each message also offers
  Reply/Delete when opened.
- **Named, not numbered.** A conversation is labelled with the contact's name
  wherever your phonebook has one — on the row, in the title when it is open, and
  above each message they sent. Only a stranger shows as a number.
- **One person is one conversation**, however the number is written — with the
  country code, without it, punctuated, or as a full SIP address. That includes
  starting one: **New Message → Choose someone you have already texted opens the
  conversation you already have with them**, rather than a second one beside it.
- **Type just the number.** A bare number is completed to
  `number@your-server` automatically, from your active SIP account — in the
  composer and when saving a phonebook contact. (It needs a SIP account to be
  active; without one, sends fail and the serial `sip` command says why.)
- ⚠ The list covers your **recent** conversations, not the entire history. Someone
  you have not texted in a long time may not be shown, and a long thread shows
  its newest messages with an honest "… N older messages not shown" line.

### Sharing a number with a second device

If another device uses the same phone number, the texts it sends can be mirrored
here so the two do not drift apart. Built for COVEY, a Raspberry Pi handheld, but
the wire format is plain text and easy to speak.

- **Over WiFi** you get the full history, including anything that arrived while the
  phone was off. **Over LoRa** you get new texts anywhere in radio range with no
  WiFi at all — as channel messages or encrypted DMs. Run both — a text already
  held is recognised and not shown twice.
- **Setup:** an `smsmirror.txt` on the SD card — the other device's address,
  then its shared token (optional third line: the port; default 8087). Upload it
  with the usual WiFi upload page (the phone also looks in `/roms`, which is
  where that page writes). The file is re-read every 30 seconds, so changes take
  effect without a reboot. No file means the feature is off, and the phone says
  so on serial instead of failing quietly.
- **Notifications behave**: texts you sent from the other device arrive silently,
  and catch-up history older than ten minutes never buzzes — only a genuinely
  new incoming text does.
- If the mirror ever seems stuck, delete `/roms/smsmirror.since` on the card —
  the phone refetches everything and deduplicates.
- ⚠ **Use a dedicated mesh channel.** An invite link contains the channel key, so
  sharing it gives away everything else on that channel.
- ⚠ **The WiFi side is cleartext** — this phone cannot do HTTPS at all
  ([why](#why-there-is-no-over-the-air-update)). The token stops a stray browser, not
  someone watching the network. Turn it off on networks you do not trust; the LoRa
  side is encrypted by the channel key.

---

## Serial console

Plug in USB, open a terminal at **500000 baud**, type `?`:

| command | does |
|---|---|
| `up on` / `up on books\|photos\|t9` / `up off` / `up` | start / stop the WiFi upload page (into `/roms`, `/books`, `/photos` or `/t9`); show its address |
| `t9` / `t9 on\|off` / `t9 reload` | predictive text: state and how many extra words are loaded; switch it; re-read `/t9/extra.txt` without rebooting |
| `heap` | memory truth: internal/DMA/PSRAM free + largest + floor since boot |
| `maps dl [src lat lon km zmax \| stop]` | the tile downloader: status with heap floor, start, stop (`docs/maps.md`) |
| `maps dlurl <template> \| clear` | a plain-HTTP relay as tile source 3 (`{z}/{x}/{y}`) |
| `tlstest <url> [n]` | the TLS bench: n kept-alive GETs from the fetch task, heap and timing |
| `up on maps` | the tile uploader: `tools/wiphone_send.py --app maps --tree <dir>` |
| `up on keys` | the API-key uploader into `/API Keys` (over WiFi): `tools/wiphone_send.py --app keys --ip <addr> gemini.txt` (always sent whole) |
| `ai` / `ai ask <question>` / `ai last` | Menu → AI from the cable: the key file (its length, never the key), the chat, the daily limits, the last question's HTTP code, model, time and stack floor; ask through the app's own path; print the newest answer |
| `ai topic` / `ai clear` / `ai reload` | start a new topic / delete the saved chat / re-read `gemini.txt` |
| `wx` / `wx fetch` / `wx show` / `wx clear` | the Almanac's weather: the cache's age, place source and rounded point, each host's code, bytes and handshake time, the worker's stack and heap floors (never a URL); fetch now (Refresh's path); print the Weather screen's rows; delete the cache. Refused while a game runs (it owns the card) |
| `open <app>` | jump into maps / almanac / weather / ai / photos / books / music / mesh / gbc / clock, whatever screen is up (`gbc` is the ROM picker; `key ok` starts the first game; `weather` is the Almanac opened on Weather) |
| `hold on \| off` | keep the screen awake and unlocked for a scripted bench session |
| `notify [sip]` | fire the real message-arrival announcement (buzz + chirp) from the cable; the log prints `buzz off after N ms`, `pop start took N ms`, `pop stopped after N ms` |
| `maps hold up\|down\|left\|right [ms [blip]]` | press an arrow on the map and hold it for that long — the hold-to-scroll bench; `blip` ms in, fake the chip's release-and-re-press under a held finger |
| `key hold <key> [ms [blip]]` | press any key and hold it — the F2, held-digit and `#` holds from the cable (`key hold # 800` toggles the mute) |
| `mute on\|off` | the master mute (Settings → Mute all sounds) from the cable; `mute` alone reports; `audio` shows `muted=` |
| `keys` / `keys raw` | the keypad's health counters / the last 64 bytes the chip sent, with the gap before each — the trace that found the release-under-a-held-finger (0.9.73) |
| `gbc` / `gbc autosave` | the Game Boy: what is up, which ROM, its two state files — and the power-off save, run from the cable (it leaves the game parked under the pause menu) |
| `send <i> <text>` / `dm <!node> <text>` | a channel text / a direct message; a refusal prints `REFUSED` and the same reason the compose screen shows (`too long for the mesh` past 232 / 220 bytes) |
| `wifi scan` | what the radio can actually hear — deaf radio vs absent AP |
| `wifi calreset` / `wifi restore` | erase the RF calibration / the WiFi driver's stored state, and reboot — the deaf-radio probes (⚠ `restore` forgets the last-used network) |
| `sync` / `mirror` | fetch mirrored texts now; mirror status |
| `sip` | SIP account state — loaded, registered, WiFi — in one line |
| `chan <url>` / `chans` | apply a Meshtastic channel invite link; list channels |
| `dm <!node> <text>` | send a direct message (encrypted when the key is known) |
| `announce` / `pki` | broadcast NodeInfo now; DM crypto state |
| `pos` | the whole positions picture: places, node fixes, pin, reference, and where this phone is (each source: GPS, the hotspot phone, Wi-Fi location) |
| `pos every <secs>\|off` / `pos now` | reporting cadence / one beacon now. From the cable, neither ever goes to a public channel: that is armed on the phone |
| `pos inject gps\|phone <lat> <lon> [sats hdop]` / `pos inject clear` | a bench fix for that source, RAM only: drawn and measured from, never sent by the schedule (`pos now` with one says INJECTED and sends it as MANUAL, not as a measurement). Use dummy coordinates |
| `phonepos` / `phonepos auto\|off` / `phonepos port <n>` | Phone location: state, port, bytes, sentences, stale lines, last fix age (never a coordinate or an address); switch it (`auto` is refused while reporting goes to a public channel); an extra port to try |
| `phonepos at <ip>[:port]` / `phonepos at off` | bench: ask a Mac running `tools/nmea_phone_stub.py` instead of the gateway (RAM only; its fixes are bench fixes) |
| `netloc` / `netloc off\|map` / `netloc now` | Wi-Fi location: the setting, the last scan (counts), the last look's verdict and accuracy, the fix, each half's HTTP and handshake time (never an address or a coordinate); switch it (`map + mesh` only on the phone); look as soon as allowed |
| `netloc answer <A> <B>` | bench: fold two canned beaconDB answers (`{...}`, a 3-digit status, or `-`) through the agreement rule; the fix is a bench fix, never sent by the schedule |
| `tz` / `tz auto\|custom [offset]\|<zone>` / `tz test <lat> <lon>` / `tz forget` | the time zone: what is in force, where it came from, Auto's last look and why, the next DST change (never a coordinate); set it (zones: pacific mountain arizona central eastern atlantic newfoundland alaska hawaii saskatchewan); look a place up without changing anything; drop the zone Auto found |
| `sun` | legal light at the reference place (or `sun <lat>,<lon>`) |
| `gps` / `gps on\|off` | GPS receiver state / route the user UART to it (`on` is refused from the cable while reporting goes to a public channel) |
| `unread` / `unread clear` | recount + repair unread counters; mark all read |
| `bookpage` | dump the open reader page's layout (debugging) |

For the things that otherwise need the phone in your hand — handy when it is on
a bench instead. **No authentication:** whoever holds the cable holds the phone.

---

## Phone improvements

- **Time zone** (Settings, 0.9.82; was "Time offset"). **Auto** sets the zone from where the
  phone *measures* it is - the woods plate's GPS, or your phone's GPS over its hotspot (Phone
  location) - and daylight saving then changes by itself, after a reboot too. It never uses
  your internet address or a bench fix, and when it is not sure (no good fix, near a zone line,
  at sea, outside North America and the Caribbean, or a new zone seen only once) it changes
  nothing: a new zone must be seen on two looks two minutes apart, and a banner says when it
  changes. Or pick **Pacific, Mountain, Arizona, Central, Eastern, Atlantic, Newfoundland,
  Alaska, Hawaii or Saskatchewan** (each with its own DST rule), or **Custom** - the typed
  offset, as before, which Auto also uses until it finds a zone. Going back to Auto from a zone
  picked by hand keeps that zone until a fix finds another. Serial `tz` says what is in force
  and why; `tz test <lat> <lon>` looks a place up. The zone map is
  timezone-boundary-builder's, © OpenStreetMap contributors (ODbL 1.0; see Credits).
- **Mute all sounds** (Settings, 0.9.75) — a master mute at the audio chip: nothing comes
  out of the loudspeaker — ring, message chirp, mesh pop, music, Game Boy — while a call in
  the earpiece, headphones and the vibrate motor keep working, so "Vibrate only" behaves as
  it says. A crossed speaker in the header shows it is on. Serial `mute on|off` flips it
  from the cable. **Hold `#` (0.9.76)** flips it from the keypad wherever `#` is not
  somebody's — not in a text field (the dialer's number included), not on the map — with a
  70 ms buzz to say it took; a short tap of `#` still opens the dialer from the clock or a
  menu, delivered on release.
- **WiFi auto-switch** — the phone quietly scans in the background and hops to
  the strongest *saved* network (with hysteresis, so it doesn't ping-pong);
  waking the screen with no connection triggers an immediate scan+connect, and
  out-of-range scanning backs off to save battery, recovering the moment
  anything connects. Toggle under **Settings → WiFi auto-switch**.
- The **status bar shows the connected WiFi network's name**.
- Fixed the **"Edit current network"** screen freezing on input, and the
  multi-second menu freezes caused by DNS lookups (answers are cached now,
  including the "that name does not resolve" answer that used to freeze the
  phone over and over on restrictive networks).
- **Files: folders can be copied, moved and deleted** (0.9.72). Menu → Tools → Files;
  OK on a folder still enters it, and inside any folder the **`[ This folder... ]`** row
  offers Copy / Move / Delete for the folder you are standing in — that is how a folder is
  "selected". Delete counts what is inside while you read the question (`271 files, 38
  folders, 33 MB`) and then runs as a job with a progress screen, a slice at a time, so the
  phone keeps working (calls, mesh, music) — about 20 files a second on this card; Back
  stops it where it is and says what got done. A folder Move is one rename, instant; a
  folder Copy streams file by file (a 34 MB map area took 3½ minutes). Pasting a folder
  into itself is refused, and so is touching the area a running map download is writing.
  Files: `/maps/home` was the reason.
- **Long names scroll.** In every list on the phone — books, tracks, files, nodes,
  message previews, contacts — the highlighted row **scrolls sideways** when its text does
  not fit, holds at the end, and wraps; unhighlighted rows keep their `..`. Nothing is cut
  off for good any more.
- **Keypad reliability** — fixes for missed taps, stuck buttons, and held keys
  releasing at random (I2C error retry + a 40 ms hardware key heartbeat).
- **The power button can no longer fire by itself** (0.9.67). A failed read of the
  button's expander pin used to count as a press, and 2.5 s later the phone powered off —
  seen on the bench, three seconds into a Game Boy launch, with nobody near it. A failed
  read now keeps the old state, and a 2.5 s hold is confirmed by a fresh read before the
  latch is pulled.
- **Real battery life.** The main loop used to spin flat out at 240 MHz forever
  with the screen off; it now sleeps between passes, drops the CPU to 80 MHz
  when nothing is happening, and lets the WiFi receiver park between beacons.
  Full speed returns instantly for the screen, music, a call, the emulator or an
  upload.
- **The phone records why it restarted** — crash, watchdog, brownout or a normal
  power-on — plus a health line every minute (memory, battery, CPU speed) to
  `/health.log` on the card. The log trims itself (newest ~4 hours kept), and is
  readable over WiFi at `http://wiphone.local/log` with an upload screen open
  (use `curl -L` — since 0.9.28 that address redirects to the legacy server on
  port 8080), so it survives being unplugged and rebooted.
- **File transfers work from a phone — and now reliably.** The upload page has a
  proper file button and a paste-a-link fetch, retries failed pieces, raises the
  screen timeouts while open (dim 5 min / sleep 10), and no longer knocks the
  phone off WiFi. Since **0.9.28** the page itself is served by the same lean
  single-connection engine that carries the file pieces (zero heap cost per
  request), so visiting it can no longer trip the low-memory breaker that used
  to answer "site cannot be loaded" — the old framework server still exists on
  port **8080** for `curl -F` and no-JavaScript browsers. From a computer,
  `python3 tools/wiphone_send.py --app books <files>` uploads a batch in one
  command, resuming partial files and byte-verifying each one.

---

## Firmware updates

**Updates are installed over USB.** See [Flashing](#flashing-beginner-friendly)
below — it is the same `pio run -t upload` you used the first time, and it keeps
your settings, books, ROMs and messages.

**Settings > Firmware update** on the phone shows those steps on the screen, so
you can read them with the phone in your hand.

### Why there is no over-the-air update

The phone cannot open an HTTPS connection, so it can never download an update by
itself. This is a memory limit, not a broken link or an expired certificate:

| | |
|---|---|
| mbedTLS needs (`CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` 16384, in **and** out buffers) | **~33 KB** |
| may any of it come from PSRAM? (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`) | **no, not set** |
| free internal heap on this phone (before 0.9.74; ~66 KB since) | **~19 KB** |
| largest contiguous internal block, fresh boot (before 0.9.74; ~66 KB since) | **~14.9 KB** |

The TLS handshake fails in `client->connect()` before a single byte of HTTP.
It is not close, and it is not fragmentation — there is not enough internal heap
even on a perfectly clean boot. The OTA controls have been removed rather than
left to report an error every time, and the boot-time update check is compiled
out entirely (`OTA_TRANSPORT_AVAILABLE` in `WiPhone/ota.h`) so the phone no
longer spends part of every startup on a connection that cannot open.

Changing this means changing the **transport** — a plain-HTTP mirror, or a TLS
stack that can allocate from PSRAM — not the URL. The code is still there and
still builds; flip `OTA_TRANSPORT_AVAILABLE` to `1` once the transport is real.
(The map downloader has since shown the way — mbedTLS pointed at PSRAM on a
worker task, 0.9.64 — and 0.9.74 gave the phone 40 KB more internal RAM, so the
numbers in the table are history; the OTA path itself has not been rewired.)

## Applying a channel setup link

Meshtastic shares channel setups as a link that looks like:

```
https://meshtastic.org/e/?add=true#CgcSAQE6AgggCjISIA-e2sF5bAtEulPtEtsB…
```

**Only the part after the `#` matters.** Everything before it
(`https://meshtastic.org/e/…`) is just a web redirect and contains no channel
data — the base64 text *after the `#`* is the entire channel configuration.

**To add channels to the WiPhone:**

1. On any Meshtastic device or the Meshtastic app, open your channel's **share /
   QR link** and copy it.
2. Send that link to the WiPhone **as a Meshtastic direct message**. You can send
   the whole URL, or — if it's too long to fit in one message — just the part
   **after the `#`** (that fragment is usually short enough on its own).
3. On the WiPhone: open **Meshtastic → Chats**, open the direct-message thread
   from that sender, **select the message** containing the link, and press
   **OK ("Apply link")**.
4. The custom channels are imported and appear as their own chats. They persist
   across reboot.

Applying a link **adds/merges** channels (it keeps your existing ones, including
LongFast). Decoding happens entirely on the device — **no internet needed.**

---

## Hardware

Made for **WiPhone** hardware:
- ESP32-WROVER (240 MHz, PSRAM, 16 MB flash)
- Semtech SX1276 LoRa radio (RFM95W), US 915 MHz / LongFast
- ST7789 display, CP2104 USB-to-serial

Optional add-on: the **[woods backplate](#woods-backplate)** — a custom back
cover with a bigger battery, a GPS receiver and a real 915 MHz whip antenna.

---

## Flashing (beginner-friendly)

You'll build and flash the firmware with **PlatformIO**. On a Mac / Linux /
Windows machine:

**1. Install Python 3** (if you don't have it): <https://www.python.org/downloads/>

**2. Install PlatformIO Core:**
```bash
pip3 install -U platformio
```

**3. Get this code** — clone or download this repository (use the green **Code**
button at the top of the page), then open a terminal in the project folder.

**4. Plug in the WiPhone** via USB. (On some systems you may need the Silicon Labs
CP210x USB-to-UART driver; macOS usually has it built in.)

**5. Build and flash the firmware:**
```bash
pio run -t upload
```
PlatformIO auto-detects the WiPhone's serial port. If it can't find it, list ports
with `pio device list` and add `--upload-port /dev/<your-port>`. If the upload
starts and then fails partway ("Invalid head of packet" is the classic), your
USB-serial doesn't like full speed — set `upload_speed = 230400` in
`platformio.ini` and try again.

**6. First time only — also flash the filesystem** (config files, the ringtone,
the wallpaper):
```bash
pio run -t uploadfs
```

That's it — the WiPhone reboots into the firmware. Open the main menu and look for
**Meshtastic**.

> Tip: to watch the phone's logs, run `pio device monitor` — the project pins the
> monitor to the WiPhone's **500000 baud** console, so it works as-is.

---

## Credits & license

Based on the official [WiPhone firmware](https://github.com/HackEDA/wiphone-firmware)
by HackEDA, Inc., whose code is under the **WiPhone Public License v1.0**.

**This repository as a whole is distributed under GPL v3** (`LICENSE`). It has to be:
RadioHead is compiled into every build and is offered as "GPL V3 or commercial", and
GPLv3 is the only licence that can cover the rest of the mix. HackEDA's original files
keep their own WPL headers, which is fine — the WPL is Apache-2.0-derived, and Apache 2.0
combines one-way into GPLv3.

Two vendored components do not sit cleanly under that, and rather than pretend otherwise
they are itemised in **[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)**: the Game Boy
emulator core (gnuboy, GPL v2 with no "or later" clause found) and the MP3 decoder (helix,
RealNetworks RPSL, which the FSF lists as GPL-incompatible). For a phone you build and
flash yourself this is moot — every obligation involved is about distribution, and the
source is public. It would need resolving before the project could be redistributed as one
clean work; that file says how.

The time zone grid (`WiPhone/tz_grid_data.h`) is **data, under the ODbL 1.0**. It is derived
from [timezone-boundary-builder](https://github.com/evansiroky/timezone-boundary-builder),
© [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors. It is a derivative
database, remade by `tools/gen_tz_grid.py`. Details are in THIRD-PARTY-NOTICES.md.

Meshtastic is a trademark of Meshtastic LLC; this is an independent, community project,
not affiliated with or endorsed by Meshtastic.

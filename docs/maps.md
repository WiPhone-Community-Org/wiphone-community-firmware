# Maps on the WiPhone

**Menu → Maps.** An offline map: tiles off the SD card, pins you drop yourself, and
everything the mesh already knows about where people are, on one screen.

The Meshtastic screens already carry every fact this draws — camp is a waypoint, the truck is
a waypoint, everyone's position arrives on port 3, and the Nodes list turns it into
"3.2km E of camp · 4 min ago". That sentence is correct and it is the wrong shape for the
question people actually ask, which is *is he on the other side of the creek or this side*.

It needs no radio traffic of its own and no GPS. With neither, it still shows the places the
mesh has shared and the last position of everyone who has spoken.

---

## Keys

| Key | What it does |
|---|---|
| **Arrows** | scroll. A tap is a **12 px nudge**, always — taps never speed up; **hold one down and it keeps scrolling and speeds up** (after 400 ms, 240 → 480 → 720 → 960 px/s over the next 0.6 s — by TIME, so a frame that comes late moves further, not later, at ~12 frames a second; since 0.9.73); let go and the next press is a nudge again. A hold also rides through the keypad chip's habit of reporting a release and a re-press 4 ms apart under a thumb that never moved (0.9.73 — it used to stop the sweep dead after one to four jumps). With **Snap to markers** on (the menu; it is on by default) the map snaps onto a pin, a place from the mesh or a node with a position **when the crosshair lands within 10 px of it** — after a tap, or where a hold stops. Nothing else: a marker in the path of a sweep is sailed past, and the tap after a snap steps clear (the nudge is bigger than the radius), so you can scroll around them. Since taps on the two axes can put the crosshair within 8.5 px of anything, "as near as you can get" is always near enough. The strip says what you landed on: `Pin 3 (3 of 4)`, `hunt camp (place from the mesh)`, `{user} H (node, 12m ago)`. `7` / `9` walk your pins in order |
| **2 4 6 8** | scroll too, for gloves |
| **Side button 1 / 2** (top two) | zoom in / out. Also `#`/`*` or `3`/`1` |
| **Side button 3** | centre on me: a live GPS fix (4+ satellites), else the pin you declared by hand. Also `0` |
| **Side button 4** (bottom) | the next map type — the same switch as **Menu → Map area**, one press at a time, round and round (`usgs-topo` → `usgs-img` → `otm` → …). The ground under the crosshair stays put; the strip says which map you are on. With one map on the card it says so |
| **OK** | drop a pin on the crosshair and name it — or, with a pin under the crosshair, open it (rename / move / share / delete). Also `5` |
| **Menu** (top-left soft key) | **What the buttons do...** is its first row — the phone's buttons drawn with what each does. Then download maps, go to coordinates, follow me, measure (an anchor at the crosshair; scroll away and the strip reads the distance and bearing), **Snap to markers: ON/off**, **Altitude: ON/off** and **Units: metric / US (ft, mi, mph, F)** (both below), pins, places, nodes — the three lists are **nearest-first** with distance and bearing on every row |
| **7 / 9** | previous / next pin, centring the map on it |
| **Back** | leave. Where you were looking is saved — also on power-off |

While the map is open the side buttons belong to it, not to the music player (the Game Boy
has the same rule), so zoom cannot silently turn into play/pause because a track is loaded.

The strip's message row grows to two rows when a message needs them — "No fix yet, and no pin
of your own to fall back on" used to end in `..` — and shrinks back at the next key (0.9.70).

The left soft key reads **Pin/Menu** because it does both: the pin under the crosshair if there
is one, the map menu if there is not. **Centre on me** means *this phone*; it falls through to
the reference place only as a last resort, and then the screen names it — a key labelled
"centre on me" that quietly lands on somebody else's camp has told you something false.

The same table is on the phone: **Menu → Keys and colours**.

## What the colours mean

| Mark | What it is |
|---|---|
| orange dot | one of your pins |
| yellow ring | one of your pins, shared with the mesh — hollow centre, so it is not a colour judgement |
| green gem | a place heard from the mesh (a Meshtastic waypoint — COVEY's map shares these) |
| cyan dot | somebody else's last position, with their name |
| grey dot | …the same, but over 30 minutes old, and it says how old |
| white ring | this phone, from a GPS fix under two minutes old |
| grey ring | …the same, but the fix is older than that, and it says how much older |
| white gem | this phone's manually declared pin |

A stale position is drawn grey **on purpose**. On foot, half an hour is far enough to be
somewhere else entirely, and a map that draws an old dot the same as a live one is lying by
omission. The same rule applies to **you**: `getGpsFix()` answers "there has ever been a fix",
so a phone that lost the sky under canopy would otherwise keep drawing a confident white ring
over where it was an hour ago. Past two minutes — the mesh service's own freshness bar, not a
second one invented for the map — your ring goes grey and wears its age.

**Names are dropped, never squashed.** Eight places plus twenty nodes on a 240×250 screen is
not hypothetical on a hunting channel, and unmanaged labels overprint into a grey smear. Names
are placed most-important-first — you, your pins, the mesh's places, then other people (starred
first) — and one that would land on another is not drawn. Its **marker** is still there, which
is the part that says where somebody is, and the name is on the Nodes list either way.

## Where it opens

1. Where you closed it last. That is the whole point of it, and it is the only one of these
   that works with no sky and no signal.
2. This phone's GPS fix, if the receiver is on and has one.
3. The mesh's reference place — a chosen waypoint, a fresh fix, or your declared pin,
   whichever the Meshtastic app's own precedence picks.
4. The middle of whatever tiles are actually on the card.
5. 0, 0 — with the map saying there is nothing to show.

## Pins, and what "share" means

A pin is a private mark on your own map: *park here*, *the gate*, *blood trail starts*. It
lives in a text file on the card (`/maps/pins.txt`), there can be 64 of them, and **nothing
about it reaches the air**.

**Share it on the mesh** turns that pin into a Meshtastic *waypoint*: it goes out on the
radio, appears on COVEY's map and on any Meshtastic device that shares the channel, and comes
back to this phone's own Places list. That is a deliberate act, every time, and:

- it prefers a **private** channel — a location on LongFast is readable by every Meshtastic
  radio in range;
- it is **locked to this node**, so nobody else's radio can move or delete it;
- the phone says whether it actually **reached the air**. "Saved, but NOT sent" means the
  radio was not ready and nobody else has it;
- renaming or moving a shared pin **re-sends it**, so the mesh never holds a stale copy;
- **Take it off the mesh** sends the deletion marker and removes it from other people's maps
  too. A camp that has moved is worse than no camp at all.

The mesh's own place table holds **eight** waypoints, shared with COVEY. Your 64 pins are
separate and unaffected — only shared ones take one of those eight slots.

---

## Stretching the most detailed tile there is

{user}: *"if the more detailed zoom levels don't exist, don't just let the higher zooms go
blank, just stretch the most detailed available tile and say what tile is being stretched and
by how much"* (0.9.78).

**Every tile position on the screen is drawn from the deepest level the card has there.** If
the position's own tile is missing, the map uses its parent's quarter, stretched 2x. If that
is missing too, it uses the grandparent's sixteenth, stretched 4x, and so on, down to the
area's shallowest level or 8 levels up, whichever comes first (past 8, a position would be
smaller than one pixel of the ancestor). The walk is per
position, so the edge of a download shows real tiles beside stretched ones instead of grey.
A real tile is never stretched. **A grey square means there is no tile at any level.**

Stretched is bigger, not sharper: the same ground detail in more pixels. The map says so
rather than let a fuzzy screen look like a fault:

- **the corner chip gets a star** (`z17*`) whenever any position on the screen is stretched;
- **the status chip names the stretched levels and their factors, deepest level first**, so
  each end of the range sits beside its own factor:
  - one level: `z16 tiles stretched 2x`, or `z13 tiles stretched 8x` at z16 where the card
    holds z13 and nothing deeper;
  - several: `z13-12 stretched 4-8x` at z15 means z13 at 4x and z12 at 8x;
  - `z16-14 x2-8` is the short form when the full one will not fit.

  Written the other way round, `z14-16 stretched 2-8x` would read as z14 at 2x. That is the
  one fact {user} asked to be told, told backwards;
- **`card read error`** on the status chip means a tile is on the card but the wrong length or
  unreadable. It is still drawn from its ancestor, but a failing card must not pass for
  ordinary stretching;
- the zoom step that goes **one level past an area's deepest tiles** says once *"Past the
  tiles: z16 stretched 2x. Same detail, bigger — and fuzzy."* The next press is refused with
  *"z17 is as close as this map goes, tiles and all"*.

**The scale bar stays honest.** It reads 50 m where z16 read 100 m, because the view really is
at z17; only the pixels are borrowed. Pins, the crosshair readout and "how far is that" are all
computed at the real zoom, so nothing on the screen is lying about distance.

The zoom STEP goes only one level past an area's deepest tiles (`MAP_OVERZOOM_MAX`): two levels
would be a screen of 64 fat squares, and nothing is learned from it. Filling a HOLE from
several levels up is different: that is not a zoom you chose, and the chip names the factor.
So an OpenTopoMap area holding z17 zooms to z18, and a USGS area stops at z17. The stretched
level is remembered across a power-off. It travels between map areas, but only ever one level
past *that* area's own deepest tiles.

While a tile is still being read off the card, the nearest shallower level already in memory
is drawn in its place and counted as stretched, so the chip is honest about that too. While a
download is writing into the area on the screen, the map checks it every 10 s, so a freshly
written tile replaces its stretched parent without a keypress. That includes a download
writing a level deeper than the area had when the map opened (the first z17 run over a z16
area): the map picks up the new level, draws it, and lets you zoom into it.

**COVEY does the same**, per source: OpenTopoMap views to z18 over its real z17, and USGS to
z17 over z16. Its info chip reads like `z18 · OpenTopo · z17 stretched 2x` or
`· z16-14 stretched 2-8x`. ⚠ The 0.9.77 version of this section said that online COVEY
"streams the real z17 tiles instead". That was only ever true for OpenTopoMap. Both USGS
services answer 404 at z17, so over USGS COVEY stretches z16 online and offline alike.

---

## Altitude

With an elevation layer on the card (0.9.80), the strip under the map gains a row, under the
coordinates, saying how high the ground under the crosshair is — and how that compares with
the ground under **you**:

| The row says | When |
|---|---|
| `Elev 1,352ft, 394ft above you` / `Elev 412m, 35m below you` | the crosshair's ground against yours |
| `Elev 412m, level with you` | the two are under 1 m apart |
| `Elev 412m, 35m above Camp` | you have no position of your own (no fix, no pin): the mesh's reference place, by name |
| `Elev 412m, 35m above poor fix` / `... below old fix` | "you" is a fix under 4 satellites or over HDOP 10 (one was measured 20 km off), or one over two minutes old - named for what it is, never "you" (the marker already says `me?` / `me, 12m ago`) |
| `Elev 412m` | nothing to compare with, or no tile under you |
| `Elev 412m (coarse)` / `Elev* 1800m, 1300m above you` | a height came from the coarse ~100 m layer (below). `(coarse)` when it fits the strip, the star when it does not — the map's own mark for borrowed, coarser data, as in `z17*` |
| `Elev: no tile here` / `Elev: no data here` | no elevation tile at the crosshair / a tile with a hole there |
| `Elev: card read error` | the card could not be read (not the same as no tile); the next move asks again |
| `Elev ...` | the map is moving: the height is read once the view has been still ~150 ms (under **Follow me** it is read in the same tick as each re-centre, so the frame that moves the map already shows it - no `Elev ...`, no second redraw. At most ONE card read a fix: while the fix stays within 5 m of the last sample - GPS noise moving the crosshair a pixel - that height stands and the card is not touched, and "you" is the crosshair's own sample, since the view is centred on you. Review 2026-09-27: every fix read the crosshair and you, up to four tile opens in the frame's pass) |

**"You"** is exactly who the white ring and side button 3 mean: a GPS fix under two minutes
old, else your declared pin, else a poor or an old fix (named on the row: `poor fix`, `old fix`)
— and only with none of those the reference place, named on the row. **Both heights come from the tiles, never from the GPS's
own altitude.** A phone GPS is ±10–20 m vertically and measures from a different surface than
the terrain model; "35 m above you" made from a GPS height and a tile height would be mostly the
GPS's error. Two readings of one surface are consistent with each other, and that is the
question the row answers: is that ridge above me, and by how much.

The row also shows up in three other places:
- **Measure** (the ruler): `Ruler: 1.2km SE, climb +85m` — from the anchor's ground to the
  crosshair's, when both have a height (`climb -40m` is downhill; `climb* +85m` when either end
  came from the coarse z10 layer, the strip's star); otherwise the old
  `Ruler: 1.2km SE of the anchor`.
- **A pin's options** end with its ground: `Ground 412m, 35m above you`.
- **Go to coordinates** repeats the crosshair's height under its `Crosshair:` line.

A two-row message on the strip takes the altitude row's place until the next key, as it takes
the coordinates'. **Menu → Altitude: ON/off** turns the whole thing off (nothing is read from
the layer at all); it is on by default, and does nothing without the layer.

Each height is read on the app's timer, one at a time, after the tiles on the screen — never
while drawing, never on a keypress, never while an arrow is held. A read is one small file open
and one or two 4-byte reads per layer; `elev 47.49643 -121.79` on the serial console times one.

### The tiles, and where they come from

```
/maps/elev/<z>/<x>/<y>.elv
```

256×256 **little-endian int16 metres**, row-major, 131072 bytes — the same z/x/y grid and the
same byte count as a map tile, and the same rule: a file of any other length is not a tile.
Two layers: **z13** (~13 m pixels at 47 N) under every area the maps hold at zoom 11 or finer,
and a coarse **z10** (~100 m) under everything down to zoom 8, so a zoomed-out view still reads
a height. A point tries z13 first, then z10. The heights are the AWS Open Data **Terrain
Tiles** (USGS 3DEP/NED in the US, SRTM, GMTED, ETOPO elsewhere; attribution in the README),
checked against USGS's own point service near North Bend: within ~1 m on flat ground.
The format and the sampling are in [almanac.md](almanac.md), "Elevation".

🛑 **`elev` is a reserved name under `/maps`.** The map never lists it as a map area (nor does
the `maps` console command, which reports it on a line of its own), whatever its case. Do not
name a map area `elev`. Firmware before 0.9.80 does not know this and would offer an empty
"elev" map — push the layer only to phones that run 0.9.80 or later.

Making and copying it, on the Mac:

```bash
python3 tools/make_elev_tiles.py all          # ~/elev-master/elev: z13 under the tile master's
                                              #   areas + the z10 layer (plan / fetch / build)
python3 tools/make_elev_tiles.py all --bbox 47.3,-122.0,47.6,-121.5   # an ad-hoc area

# onto a phone over WiFi (the maps uploader, `up on maps`): lands at /maps/elev/...
python3 tools/wiphone_send.py --app maps --tree ~/elev-master/elev

# or on card day: `tools/cardday.sh <phone> push` copies it as its step 4 (after the maps),
# and `tools/cardday.sh covey push` rsyncs it to COVEY's /root/covey-elev
```

Or **on the phone**: `Menu → Download maps...` with **Elevation too: ON** (the default) fetches
the layer for the area it downloads, from the same source, and the phone's decode gives the
same bytes as the Mac's (`tests/test_tilepng.cpp` checks a real tile, all 131072 bytes). A layer
a download writes is picked up by an open map as it lands — no Rescan needed.

Then **Menu → Rescan the card** (the layer is looked for when the map opens and on Rescan).
`tools/card_clone.sh --no-tiles` skips it like every numbered folder under `/maps`, so a card
swap needs the cardday step to bring it back.

## Units

**Menu → Units: metric / US (ft, mi, mph, F)** — one setting (the same row, word for word, as the
Almanac's Settings: `unitsSettingRow`) for the whole phone (NVS `wpmesh/units`;
serial `units metric|us`). Metric is exactly what the phone has always printed. US is feet and
miles: the scale bar (`500ft`, `2000ft`, `0.5mi`, `2mi`), every distance on the strip and in
the Pins / Places / Nodes lists (`850ft`, `1.4mi`), the Meshtastic app's node lines, the
altitude row (`1,352ft`, with a thousands comma), and the download form's radius
(`Radius: 5 km (3.1mi)` — the radii themselves stay COVEY's round kilometres). The Almanac
follows the same setting. The formats are in [almanac.md](almanac.md), "Units".

---

## The tiles

```
/maps/<area>/<z>/<x>/<y>.565
```

Standard slippy-map `z/x/y` numbering — the same numbering every tile downloader and every
`z/x/y` cache on a computer already uses, COVEY's included. `<area>` is whatever you want the
map called on the phone (letters, digits, `-`, `_`, `.`; at most 31 characters; not starting
with a dot; not `elev`, which is the altitude layer — see "Altitude"). You can have several;
the Maps menu lists them when there is more than one.

**A tile is 256×256 pixels, RGB565, little-endian, 131072 bytes exactly.** Raw. Not PNG, not
JPEG.

### Why raw, when raw is eight times bigger

The phone decodes nothing, and that is the design rather than a shortcut.

- There is **no PNG decoder** in this firmware. Adding one costs a vendored library plus
  contiguous internal RAM on a phone whose crashes are internal-heap fragmentation.
- The ESP32's ROM **JPEG decoder refuses greyscale outright** — that is why `jpeg_grey.cpp`
  exists at all (33 of 45 pictures in one test book were 1-component JPEGs).

So a map made of compressed tiles would work right up until the day it met a tile of the
wrong flavour, in the woods, with nothing on the screen to say what was wrong. Raw costs card
space and buys zero decode time, zero decoder memory, and exactly **one** failure mode:
*the file is not 131072 bytes*, which the phone checks in one comparison and reports.

It also means a PNG copied to the card without being converted is refused rather than drawn
as a screenful of confetti.

### What it costs on the card

A 20 × 20 km area at roughly 47° N:

| zoom | ground per pixel | tiles | size |
|---|---|---|---|
| z12 | ~26 m | ~9 | 1.2 MB |
| z13 | ~13 m | ~35 | 4.5 MB |
| z14 | ~6.5 m | ~130 | 17 MB |
| z15 | ~3.2 m | ~500 | 63 MB |
| z16 | ~1.6 m | ~2,400 | 300 MB |
| z17 (OpenTopoMap only) | ~0.8 m | ~9,600 | 1.2 GB |

About **85 MB for the whole stack to z15**, which is nothing on a 32 GB card. To z16 it is
~0.4 GB, and to z17 ~1.6 GB (the downloader's own count for 10 km around 47.5° N). z13–z15 is
the useful range on a 240×320 screen. USGS Topo's z16 is the z15 drawing scaled up, four
times the card space for ground you can already see. OpenTopoMap's z17 is a real drawing
(house numbers, footpaths). It is worth its 4x for a small area, not by default. **z17 exists
only for OpenTopoMap: USGS answers 404 past z16.**

---

## On the Mac: making the card

`tools/convert_tiles.py` converts a tile tree (or an `.mbtiles` file) into the format above.
It needs **nothing installed** — it uses Pillow when you have it and macOS's own `sips` when
you do not.

**The command**, with the card mounted and a tile tree in `~/covey-tiles`:

```bash
python3 tools/convert_tiles.py ~/covey-tiles /Volumes/WIPHONE/maps/home --zoom 12-15
```

Check the damage before committing to it:

```bash
python3 tools/convert_tiles.py ~/covey-tiles /Volumes/WIPHONE/maps/home --zoom 12-15 --dry-run
```

From an `.mbtiles` file instead (the script unpacks it; the phone never sees it):

```bash
python3 tools/convert_tiles.py camp.mbtiles /Volumes/WIPHONE/maps/camp --zoom 12-15
```

Re-running is cheap: tiles that are already there and the right length are left alone, so
adding a zoom level or a new corner of the map only converts what is new.

### Three things that bite

⚠ **Convert straight onto the card, or copy with `rsync`.** Finder writes a `._name` sidecar
next to every file it copies onto FAT32 — one per tile, thousands of them, which the phone
then has to skip through. The command above writes directly to the card and creates none. If
you must copy a converted tree:

```bash
rsync -a --exclude '._*' --exclude '.DS_Store' ~/tiles-565/ /Volumes/WIPHONE/maps/home/
dot_clean -m /Volumes/WIPHONE
```

⚠ **The card must be FAT32.** Same rule as everything else on this phone; see the README.
Eject it properly — `diskutil eject /Volumes/WIPHONE` — before pulling it, and swap cards
with the phone **off**.

⚠ **`.mbtiles` rows are upside down.** MBTiles counts rows from the south (TMS), slippy tiles
count from the north. The script flips them. If you unpack an `.mbtiles` with some other tool
and the map looks *almost* right but subtly wrong, that is what happened — and on forest it
is remarkably hard to see.

### Where tiles come from

Three ways, in the order you will probably use them:

1. **The phone downloads them itself** — `Menu → Download maps...` on the map, over WiFi.
   See the next section. This is the one that works with no computer in the loop.
2. **Push a converted tree from the Mac**, no card swapping: convert as above into any
   folder, then `python3 tools/wiphone_send.py --app maps --tree ~/tiles-565/home` sends it
   to `/maps/home/...` over the phone's own upload server (271 tiles, 34 MB: about ten
   minutes). The uploader's `maps` mode is the only one that accepts folder paths, and only
   `<area>/<z>/<x>/<y>.565` shaped ones. Two things to know: a tile that is already on the
   card at the right size is skipped (add `--replace` to push a re-converted tree, since every
   tile is the same size), and if Maps is OPEN while a tree arrives it may blacklist a tile it
   caught half-written — `Menu → Rescan the card` afterwards, or push with Maps closed. The
   same goes for `pins.txt`: push it with Maps closed, or the next pin edit writes over it.
3. **Straight onto the card**, as the commands above show.

A tile server's terms are between you and the server. USGS is public domain and asks for a
credit line. OpenTopoMap is CC-BY-SA, and its about page welcomes use as long as mass
downloads do not overburden its server. The phone waits 0.6 s after each OpenTopoMap tile and
says so on the help screen; both credits are printed there.

**z17 quadruples the requests**, so at z17 there is a **minimum of 2.0 s between the starts of
two OpenTopoMap requests**, on the phone and on COVEY. At z17 that interval IS the pace: a
real 2 km run on phone 2 measured 2,068 ms a z17 tile, while its z11-16 tiles took ~3.5-4 s. A
20 km area to z17 is ~51,000 requests (~38,000 of them at z17). A server that answers
429/502/503/504 gets the same tile retried after 60 s, doubling to 15 min, and the pace is
doubled for the rest of the run (capped at 10 s).

**z18 is never requested.** OpenTopoMap answers z18 with a 200 and a placeholder image reading
*"max zoom layer = 17"*: opaque text on a transparent background. ⚠ That is NOT caught by the
"fully transparent PNG = no tile" check (on the phone, on COVEY or in `convert_tiles.py`), so the
guard is that nothing ever asks for it. The phone's source ceiling for `otm` is z17, COVEY's
`TILE_ZMAX["otm"]` is 17, and `cardday.sh` never converts a z18 folder.

### Pooling the tiles of several devices

Two phones and a COVEY that each downloaded their own patches end up with three different
maps. The union of them is one tree of PNG originals on the Mac, `~/tiles-master`, and every
device gets the whole of it. The tree is the standard `<source>/<z>/<x>/<y>.png`, which is
COVEY's own layout. **Only COVEY's DOWNLOADED tiles go into the pool** (`/root/covey-tiles/<src>`).
Tiles it only STREAMED while panning live in a separate tree (`/root/covey-tiles-streamed`).
They stay on COVEY, marked as streamed and liable to expire when its card fills, and they
never reach the phones. A spot that exists only because it was streamed has to be downloaded
properly on COVEY before card day (below).

The phones need their **cards in the Mac's reader**: the union is 128 KB per tile, and a
hundred thousand tiles is ~13 GB — a week over the phone's WiFi uploader (measured 48–69 KB/s)
against under an hour onto a card. Nothing reads files off the phone over WiFi at all.

**Card day is `tools/cardday.sh`, in this order:**

```bash
# 0. THE EVENING BEFORE: the master's counts, and COVEY's streamed-only places.
#    Download those properly on COVEY (Map > DL > Streamed-only) or the phones will not get them.
tools/cardday.sh status

# 1. COVEY's DOWNLOADED tiles into the master (never its streamed ones; never overwrites)
tools/cardday.sh covey pull

# 2. each phone's OLD card, in the reader: a full backup, and its tiles into the master
#    (pull every phone before pushing any, so each card gets them all)
tools/cardday.sh phone1 pull /Volumes/WIPHONE1
tools/cardday.sh phone2 pull /Volumes/WIPHONE2

# 3. each NEW card: that phone's backup, then the WHOLE master
tools/cardday.sh phone1 push /Volumes/NEWCARD

# 4. and back to COVEY: whatever its downloaded tree lacks, over SSH
tools/cardday.sh covey push
```

🛑 **The old step 1 was `ssh covey 'sudo tar cf - -C /root/covey-tiles .' | tar xf - -C
~/tiles-master`. Do not use it.** `tar` overwrites by default, so it replaced master tiles
with whatever COVEY had. It also pulled the legacy root-level zoom folders and any half-written
`.part` files, and today it would pool streamed tiles as if they were downloaded.

**What `covey pull` does** (`tools/covey_pull.py`):
- It reads a manifest of COVEY's downloaded tree per source (`sudo find -printf`). It never
  names the streamed tree, the root-level digit folders or anything called `*-streamed`.
- Before anything moves, it prints COVEY's own streamed-only list (COVEY's `tilestore
  --places`) under *"Will NOT reach the phones unless downloaded on COVEY first:"*, one row per
  place with its tile count, zoom range, the date it was streamed, and how much of its deepest
  level the master already has. A place the master already covers completely (a phone's own
  download, a fetch on the Mac) is listed apart, because the phones get it anyway.
- **Without that report it refuses (exit 2).** A COVEY that cannot give it is most likely one
  whose streamed tiles still sit in the downloaded tree, and pulling them would put them on
  both phones as if chosen. `--without-report` overrides it, and is only for when COVEY's
  cut-over is known to be done.
- It stages the missing tiles in `~/tiles-master/.incoming/<src>`: a tar stream for a big zoom,
  and `rsync --files-from` passes to fill gaps. Each staged file must match COVEY's size
  exactly, be more than 0 bytes, AND end like a tile (a PNG's IEND chunk, a JPEG's FFD9). A
  file that fails is deleted from staging and fetched again. 🛑 The end check is not optional:
  when a tar stream stops mid-tile, bsdtar still pads that tile to the size in its header with
  zeros, so a cut tile is exactly COVEY's size. The tile bsdtar was writing when a stream
  stopped is dropped by name too.
- It links the verified tiles into the master with `os.link`, which fails when the name
  exists. **A master tile is never overwritten.**
- Its pass test is a set difference: every non-empty tile COVEY has must be in the master
  afterwards, non-empty (an empty master file where COVEY has a good tile fails it). It logs
  to `cardday.log` and writes the added paths to `covey_pulled_<date>.txt`. For a first run,
  try `python3 tools/covey_pull.py pull --dry-run` (manifests and counts only). The first real
  pull (2026-09-24, after COVEY's cut-over) added 250 tiles and ended with "0 on COVEY missing
  from the master"; openrsync's `--files-from` against COVEY worked.

**Zero-byte tiles are never pulled.** A 0-byte tile on COVEY is a failed fetch or a hard power
cut, not a map. It is listed in `~/tiles-master/covey_zero_bytes.txt`, and deleting it on COVEY
stays a deliberate act. On the way back, `tiles_565_to_png.py` treats a 0-byte master PNG as
absent and fills it, and `covey push` counts only non-empty tiles.

**What the other steps do:**
- **`cardday.sh status`** prints the master's per-source counts and, when COVEY answers, its
  streamed-only places (with how much of each the master already has) and its disk. Otherwise
  it prints "not reachable" and carries on.
- **`covey push`** writes only into COVEY's downloaded tree, with `rsync -rt` (so the files do
  not arrive owned by the Mac's uid). It skips a source that would leave COVEY under 4 GiB
  free.
- **The phone push** writes z0-16 for every source first, then z17, each after a free-space
  check against the card. It never writes z18. A card that fills up then loses only z17,
  never the whole of `usgs-img`/`usgs-topo`, which sort after `otm`.

The hand loops underneath, for reference:

```bash
tools/card_clone.sh pull /Volumes/WIPHONE ~/wiphone-cards/phone1          # the whole card
for a in ~/wiphone-cards/phone1/maps/*/; do                                # its tiles, as PNG
  python3 tools/tiles_565_to_png.py "$a" ~/tiles-master/$(basename "$a")   # --jpeg for usgs-img
done
tools/card_clone.sh push ~/wiphone-cards/phone1 /Volumes/NEWCARD           # onto a new card
for s in ~/tiles-master/*/; do
  python3 tools/convert_tiles.py "$s" /Volumes/NEWCARD/maps/$(basename "$s") --zoom 0-16
done                                                                       # then --zoom 17-17
```

The area name on the phone is the source's folder name (`usgs-topo`, `usgs-img`, `otm`), so
the same tile is the same file on all three devices and a re-run copies nothing twice:
`convert_tiles.py` keeps a tile that is already there at the right length, `tiles_565_to_png.py`
keeps a PNG that exists (unless it is 0 bytes), and `card_clone.sh` never deletes.
`convert_tiles.py` does ~300 tiles/s with numpy installed (`python3 -m pip install --user
numpy`); the card is the limit. It exits 1 when it stops early (a full or pulled card), so
`cardday.sh` stops too. It also refuses a fully transparent tile as unreadable rather than
flattening it into a solid square.

The pool on 2026-09-22 was **132,826 tiles, 16.2 GB on each phone**, with ~13 GB free on a
32 GB card. A 20 km OpenTopoMap area to z17 adds ~6.3 GB, so z17 fits, but it is a budget.

`tiles_565_to_png.py` is `convert_tiles.py` run backwards (5-6-5 bits replicated up to 8, so
white stays white); `tests/check_convert_tiles.py` holds the two to each other, because a tile
that goes phone → master → other phone passes through both. ⚠ COVEY's aerial tiles are JPEG
bytes under `.png` names (its downloader keeps whatever the server sent, and the map loads by
content) — `--jpeg` writes the same, at a third of the bytes of a true-colour PNG of a photo.

### Without opening a phone: the Mac's pool over WiFi (0.9.82)

The uploader is too slow for tiles (above), but the phone's own downloader can fetch from the Mac
instead of the internet. `tools/tile_relay.py` serves the pool read-only on the home network, and
the custom source (source 3) can now write into a **real map's folder**:

```bash
python3 tools/tile_relay.py                  # on the Mac: prints http://<mac>:8765/<src>/{z}/{x}/{y}.png
```
```
maps dlurl http://<mac>:8765/otm/{z}/{x}/{y}.png otm      # on the phone's console: into /maps/otm
maps dl 3 <lat> <lon> <km> 17 elev 0                      # the area; skips what the card has
```

- **The area word is new in 0.9.82.** Left out, it is `custom` (a map of its own, as before). Any
  map folder name works (`otm`, `usgs-topo`, `usgs-img`). Refused: `elev` in any case, and any name
  ending in `.`. The card is FAT, which ignores case and drops trailing dots, so `ELEV` and `elev.`
  are both the altitude layer's folder, and `otm.` is `otm` under a spelling the Files app's guard
  would not recognise. The form and the status call the source `Custom -> otm`.
- **Only the folder changes.** The source is still "custom" wherever it means the server: no
  OpenTopoMap z17 interval (that is for OpenTopoMap's servers, not a LAN), and a custom job is
  never saved for resuming. A restart or a stop ends it: send both lines again, and the re-run
  skips every tile already there.
- **The relay answers only `/<otm|usgs-img|usgs-topo>/<z>/<x>/<y>.png`**, from the pool as it is
  (USGS JPEG under the `.png` name; the phone reads the magic). It binds the Wi-Fi address only and
  has no TLS: home network only. A tile the pool lacks is a 404, which the phone counts as "no tile",
  so run the job again once the pool has the rest.
- **What it buys, measured on phone 1 (2026-10-05).** One z17 OpenTopoMap tile: 124 ms from the
  Mac against 1,220 ms from OpenTopoMap (`tlstest`, 20 GETs each). A whole job ran at **1.2 s a
  tile**, the rest being the phone's own decode and card write, against OpenTopoMap's 2.07 s at z17
  (the interval) and ~4 s below it. A 20 km z17 area (39,000-51,000 tiles, more the further north)
  is about 13-17 hours from the Mac, against 28-37 from OpenTopoMap. **USGS gains little**: 252 ms
  from the Mac against 382 ms over TLS, when a USGS tile costs the phone 1.2-2.1 s all in, so USGS
  is as quick from the internet.
- A card in the Mac's reader is still the fast way for a whole pool (minutes, not hours).

---

## Download maps on the phone

`Menu → Download maps...` fetches an area around the crosshair over WiFi and writes it to the
card in the raw format above, so the viewer never learns where a tile came from.

| Row | Choices |
|---|---|
| Source | **USGS Topo** (contours, roads, labels — the hunt map), **USGS Aerial**, **OpenTopoMap** (denser contours, CC-BY-SA, slow server). The form opens on the source of the map you are looking at |
| Radius | 2 / 5 / 10 / 20 km around the crosshair, at every depth |
| Detail | to z13, z14, z15 or z16, and **z17 for OpenTopoMap only** (`Detail: z17 (4x z16's cost)`). z15 is ~3 m per pixel at 47 N, and the row says it in the Units setting (`Detail: z15 (3m/pixel)`, US `(11ft/pixel)`, at the area's latitude); USGS z16 is the same drawing scaled up and costs four times as much. USGS has no z17, so on USGS the row stops at z16, and a press never lands on a step that shows the same depth twice. The depth shown is the depth used |
| Elevation too | **ON** (the default) / off (0.9.80): the same square's **altitude layer** as well — z13 and the coarse z10, from the AWS Terrain Tiles, into `/maps/elev` (see [Altitude](#altitude)). A 5 km area adds 18 tiles, 20 km adds 175 (22 MB). Kept with the other choices (NVS `maps/dlelev`) when you press Start |

The line under them is the honest estimate: tiles, size on the card, and time, e.g. `50451
tiles + 175 elevation, 6.2 GB, up to about 36 h` (OpenTopoMap, 20 km, z17; the size and the time
include the elevation tiles, at 131072 bytes and a guessed 2 s each - not measured yet). It uses the rates measured on the
phone: USGS about a second a tile, OpenTopoMap about four to z16, and OpenTopoMap's z17 at the
2.0 s interval plus 0.1 s (phone 2 measured ~2.07 s). It says **"up to"** because tiles
already on the card are skipped in milliseconds. Over 4 h it adds `Keep it on USB: about 36 h
of downloading`; off USB the job would stop at the battery floor and wait.
**Start download** needs WiFi and either USB power or a battery above 3.8 V, and says which is
missing. It is also refused during a game, a Files folder job, or within a minute of a call.

While it runs, the screen shows THAT job, read-only (`USGS Topo, 5 km to z15 + elevation, ...`):
- **the elevation layer first**, when the job has it: `Downloading elevation 12 of 18`, then
  `Elevation z13, ...` with its own failures (it is small, and it makes the altitude readout work
  minutes into a job that may run for days);
- `Downloading 24 of 271`, the zoom level it is on, the bytes, the seconds and the failures,
  and under them what the elevation layer came to (`Elevation: 16 new, 2 already had, 0 failed`);
- `About N left`, from the mean of the last 200 tiles fetched;
- `Server busy - trying again in N min` when the server has asked it to slow down;
- `Resumes by itself after a WiFi drop or a restart`.

Start is not offered until the job is stopped or finished: a new one would replace a job that
may be days old. **Stop takes two presses within 3 s.** The first says `Stopping forgets it: N
of M done, and it will not resume`. The second finishes the tile in hand and quits, and the
saved job is forgotten at once. The screen stays awake while you watch it, and lets go 2 min
after the last key. Leave it and the download carries on; the map's own tiles, the messages
and the clock all still work.

Re-running the same area only fetches what is missing. A tile is "there" only when it is
exactly 131072 bytes, so a power-off mid-tile leaves nothing the next run will not repair. When
a run is over the screen keeps its account (`Last run: 5517 new, 7332 already had, 4 failed,
7013 s`), and the elevation layer's under it (`Elevation: 173 new, 2 already had, 3 failed (last:
z13 1330/2862: HTTP 403)`). If anything failed, it adds `Last problem:` with the last map tile
that did and why.

**The elevation layer never costs you the map.** It has its own connection (to
`s3.amazonaws.com`, HTTPS only), its own counts, and its own failures: a tile that does not come
is counted as an elevation failure and never as a map tile's, never toward the twelve-in-a-row
stops, never toward the RAM strikes. Four failures in a row and the layer is left for the next
run (`- stopped trying this run`); the map tiles start either way. It has no cursor: every run,
a resume included, walks it first, and a tile already on the card at 131072 bytes costs one
look, so a resume or **Start download** again is its retry. The job's own rules do apply to it:
Stop, a call or a game pausing it, a WiFi drop, the battery floor. A job started before 0.9.80
resumes without the layer. While a job with the layer runs or waits, the Files app refuses to
delete or move `/maps/elev` too. Every
line on this screen **wraps** onto more rows rather than ending in `..` (0.9.68).

**A download is built to run for days (0.9.78).** A 20 km OpenTopoMap area to z17 is about 36
hours, and nothing that long happens without a WiFi drop, a call, a game or a restart:
- **The job is kept in NVS:** source, the WiFi network it started on, centre, radius, depth and
  a cursor, saved at most every 500 tiles, every 5 min, and at the end of each level. A job on
  the serial custom source is not kept.
- **A call or a game pauses it.** Past 10 min it stops instead.
- **A WiFi drop longer than 20 s stops it**, because the phone's WiFi rescue needs the RAM.
- **Off USB, below 3.75 V**, a running job stops.
- **A stopped job resumes by itself** when all of these hold:
  - the phone is back on **the same network** it started on, and has been for 60 s;
  - it is on USB, or the battery is at 3.8 V or more;
  - there has been no call for a minute;
  - no game is running and no Files folder job is running;
  - the card is in.
- **Cool-downs:** after a failure stop (the network gave up, nothing decoded, no RAM) it waits
  10, 20, then 40 min, then retries every 60 min (`Retrying in N min (the network gave up)`).
  **After 26 failure stops or refused starts in a row without one new tile (about a day of
  cool-downs) it gives up** and says so (`Stopped resuming: a day of retries without one new
  tile`). Time spent waiting at a gate, on another network or off USB, is not retrying and
  does not count. A card that keeps refusing writes never resumes by itself.
- **Crash strikes:** three restarts during boot resumes (a resume that neither ended in a
  recorded stop nor wrote 50 new tiles) stop the resuming: `Stopped resuming: the phone
  restarted 3 times during this download`. Switching the phone off (the held power button or
  the low-battery cut) and **Restart** from the menu are not crashes: each saves the live
  cursor and records a clean stop first. A reset from the cable, a reflash or a panic does
  count. Proven on phone 2: an esptool reset mid-run came back as `resumed the otm download at
  z16 #1 (pass 1, 217 written, strike 1)`, and the strike cleared after 50 new tiles.
- **On another network** it waits: `Paused: resumes on <network>, where it started`. On a trip
  the network is often a phone's hotspot, on somebody's data plan, so the phone never does
  that by itself. The form offers **Resume on this network**, which moves the job to the
  current one. A job waiting for any other reason (a give-up, a cool-down) gets **Resume it
  now**. A manual resume never counts a strike, and a refused one says why.
- **The order is centre-out.** Within each level the tiles go centre-out in 16×16 blocks,
  starting at z11, so a run cut short still holds the middle of the area. At the end of a run
  that was not stopped, **one automatic second pass** retries what did not land.
- **Card space.** A resume checks card space only for the tiles at or past the cursor that
  the job has not written, keeping max(256 MB, 2 % of the card) spare. If there is not enough
  room, the job is KEPT and stops resuming: `Stopped: no room on the card for the rest - free
  some, then Resume`.

The Files app refuses to delete or move a folder at or above the `/maps/<src>` of a download
that is running or waiting to resume.

**What counts as a tile:**
- **No tile:** HTTP 404 or 410, or a PNG whose every pixel is transparent. Either is counted
  and skipped, and nothing is written.
- **A failure:** a 200 that does not decode as a tile (a captive portal's login page, a
  truncated body). Twelve in a row stop the run, and it then resumes under the cool-downs
  above.

⚠ OpenTopoMap's z18 placeholder is not fully transparent (see above). The guard against it is
that the phone never asks for z18.

`card refused the write` is the card saying no to one tile's 128 KB: a short write from the
SD layer, usually one busy timeout on a card that is otherwise fine (phone 2 lost 4 tiles of
12,853 to it). A refused write is tried again after 200 ms, 1 s and 3 s before it counts
(0.9.68 tried once; 0.9.74 made it three). A tile that still fails is simply missing, and
**Start download** again fetches only the missing ones. A card that refuses twelve tiles in a
row stops the run, because that is a full or pulled card, and that stop never resumes by
itself.

Each source is its own area on the card (`/maps/usgs-topo`, `/maps/usgs-img`, `/maps/otm`);
`Map area:` in the menu switches between them, as does the bottom side button. An area the
downloader did not make — `home`, the tiles converted on the computer with
`tools/convert_tiles.py` — is a map too, just one the downloader cannot add to.

**Where a map's tiles are.** An area that is not where you are looking is indistinguishable
from an empty one — grey squares either way — so since 0.9.71 the phone reads each area's
extent once when Maps opens (the folder names at its coarsest zoom: three small directory
listings) and says so: switching onto a map whose tiles are elsewhere puts `Map: home (z11-15)
- its tiles are 35km NW of here` on the strip, and every row of **Map area** ends with `here`
or the distance and bearing to that map's tiles from the crosshair. Serial `maps` prints each
area's box in degrees. It is a bounding box, so an L-shaped download reads as its enclosing
rectangle. Deleting an area is a job for the computer: remove `/maps/<area>` from the card.

What it costs (measured on WiPhone 2, 2026-09-18):

| Area | Tiles | On the card | USGS Topo | OpenTopoMap |
|---|---|---|---|---|
| 2 km, z11–15 | ~55 | 7 MB | ~1 min | ~4 min |
| 5 km, z11–15 | ~270 | 34 MB | ~4.5 min | ~18 min |
| 10 km, z11–15 | ~865 | 113 MB | ~15 min | ~1 h |
| 20 km, z11–15 | ~3,260 | 427 MB | ~1 h | ~3.5 h |

OpenTopoMap to z17, computed with the phone's own count and rates (tileFetchEstimate's square
at 47.5° N; 4.0 s a tile to z16, and 2.1 s at z17, the 2.0 s interval plus 0.1 s). The count
moves a little with where you stand, and the Download form shows the exact figure for the
crosshair:

| Area | Tiles | On the card | OpenTopoMap |
|---|---|---|---|
| 2 km, z11–17 | ~570 | 70 MB | ~25 min |
| 5 km, z11–17 | ~3,400 | 424 MB | ~2.5 h |
| 10 km, z11–17 | ~12,900 | 1.6 GB | ~9 h |
| 20 km, z11–17 | ~51,000 | 6.2–6.3 GB | ~36–37 h |

Measured (phone 2, 2026-09-23): a real 2 km run was 567 tiles (555 new, 12 already there), 0
failed, in 1,243 s (21 min). z17 went at 2,068 ms a tile and z11–16 at ~3.5–4 s. The internal
largest block never fell below 49,616 B.

Each level is 4x the one above it, so ~three quarters of any z17 area is the z17 level itself.
z17 is for a hunt area and its approaches; 20 km of it is the overnight-and-then-some case
{user} asked for (*"overnight downloads or multi day downloads even pre-trips"*).

### How it works, and why it took a night to make it work

The phone had never opened an HTTPS connection: the framework builds mbedTLS to allocate from
**internal** RAM, its two 16 KB record buffers alone exceed the phone's largest free block, and
the handshake runs on the calling task's 8 KB stack. `tile_fetch.h` has the whole story. The
short version: the downloader runs on its own task with an 8 KB stack, redirects mbedTLS's
allocator into PSRAM once (`mbedtls_platform_set_calloc_free`), keeps one connection alive for
the whole area, decodes each tile with the decoders already in the ESP32's ROM (TJpgDec for
USGS's JPEG, the `tinfl` inflater for OpenTopoMap's PNG — `tile_png.cpp` is the 300 lines of
PNG around it, proven against 92 host checks - the elevation download's RGB rows among them), and writes the raw file to a temporary name
before renaming it into place.

Internal RAM is the thing to watch: the task's stack is 8 KB for the life of the firmware
from the first download (a real z17 run left 4,572 of its 8,192 bytes never touched: `task
stack floor 4572 of 8192`), and the first TLS handshake of a run dips the internal heap by
another ~12 KB for a second or two. The downloader refuses to start when the largest free
block is under 14 KB (24 KB before the stack exists) and tells you to reboot first. The
serial console has the numbers: `maps dl` prints the run's heap floor and the task's stack
high-water mark.

### From the console

```
maps dl                                  what the last / current run did, with the heap floor,
                                         the stack floor, ms a tile, and the saved job's resume
                                         state (network, cursor, strikes, last stop reason)
maps dl 0 47.42 -121.75 5 15 [elev 1|0]  start: source 0=USGS Topo 1=USGS Aerial 2=OpenTopoMap;
                                         the depth is capped at the source's (USGS 16, OTM 17)
                                         and the start line prints the depth actually used.
                                         The last number (the word `elev` before it optional):
                                         the elevation layer too (1) or not (0); left out, the
                                         form's "Elevation too" (ON); anything else is the
                                         usage line, never "left out". A resume counts the
                                         layer in its card-space check only until a run has
                                         had it whole (the job record's elev byte, 2). The job
                                         is kept and resumes by itself. The status has a line
                                         of its own for it: `  elevation: 18/18 seen - 16 new,
                                         2 had, 0 no-tile, 0 failed` (`RUNNING z13, ...` while
                                         it goes)
maps dl stop                             finish the tile in hand and quit; the job is
                                         forgotten and will not resume
maps dlurl http://192.168.1.17:8765/otm/{z}/{x}/{y}.png otm    a plain-HTTP relay as source 3,
                                         writing into /maps/<area> (default custom; otm = the
                                         OpenTopoMap map; never elev) - tools/tile_relay.py
tlstest <url> [n]                        the TLS bench: n kept-alive GETs, heap and timing
open maps                                jump into the app; `hold on` keeps the screen awake
maps hold right 1500 [300]               hold an arrow for 1500 ms from the cable; with the
                                         third number, fake the chip's release-blip at 300 ms
                                         (`MAPS: hold continues past a ...` proves the ride-through)
keys raw                                 the last 64 bytes the keypad chip sent, with gaps
```

---

## Checking it worked, without pressing anything

A map's failure mode is showing the **wrong ground**, and that looks exactly like showing the
right ground — no screenshot can tell a tile tree that is one column out from a correct one.
So the serial console can drive the whole thing:

```
maps                          what the card holds, the saved view, the pins file
maps goto 47.6062 -122.3321 15 home     set where Maps opens next
elev                          the altitude layer, the setting, and (Maps open) the last
                              samples of the crosshair and "you" as the strip words them
elev 47.49643 -121.79         sample the elevation tiles at any point, timed:
                              elev: 47.49643,-121.79000 -> 412.3 m (z13) [<t> ms]
units / units us              the unit setting / switch it
```

`maps goto` writes the same saved view the app itself writes, so you can put a known
coordinate in over the cable, open the app, and compare the screen against a map on the
computer. `tools/shot.py` will take the screenshot.

---

## What it costs the phone

- **Since 0.9.74 the whole phone has ~40 KB more internal RAM** (the SDK's unused Bluetooth
  reserve, released at boot — CHANGELOG 0.9.74), so the numbers below that say "~19 KB" or
  "~25 KB" of internal heap describe the phones as they were when measured; idle is ~66 KB now
  and a download bottoms out near 48 KB. The downloader's 14 KB handshake bar is unchanged;
  when it cannot be met the Download screen and `maps dl` now print how far short the phone is.
- **768 KB of PSRAM** while the app is open (six 128 KB tile slots), allocated once and freed
  on the way out. PSRAM runs ~3.6 MB free; internal RAM — the ~19 KB that SIP and WiFi fight
  over — is untouched by the viewer. The downloader is the exception: see above.
- **A tile arrives in 32 KB pieces**, one per 25 ms tick, because a 128 KB SD read is
  100–250 ms and everything in this firmware shares one task. 250 ms is exactly the threshold
  the superloop's stall detector was built to complain about; four tiles read the obvious way
  is a second of frozen phone, a dropped WiFi connection and some eaten keypresses, every
  time you pan. While a tile has not arrived, a shallower level already in memory is drawn
  stretched in its place (and the chip says so); with nothing to stretch, it is a
  plainly-marked grey square.
- **No timer at all** once the screen is painted. A map that is finished must not keep the
  CPU awake.

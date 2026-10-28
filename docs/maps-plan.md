# A lightweight maps app on the WiPhone — research (2026-08-19)

**Status: RESEARCH. Verdict up front: yes, it's doable, and comfortably** — the RAM fear
doesn't materialize because tiles live in PSRAM (measured ~3.6 MB free at runtime, the
HEALTH line) and internal RAM is barely touched. The trick that makes it cheap: don't
decode images on the phone at all — pre-convert tiles to raw RGB565 on the computer and
blit them straight from SD to screen.

Written while building the 0.9.7 position feature (the phone now learns node positions
and waypoints off the air). **A map is useful BEFORE any GPS exists**: centered on camp,
it shows waypoints and everyone's last position — the "where is he" picture. A future
GPS fix would just add an own-position marker to the same canvas.

## What the phone has (verified)

| Resource | Fact |
|---|---|
| Screen | ST7789, 240×320 portrait, TFT_eSPI, keypad-driven (no touch) |
| PSRAM | ~3.6 MB free at runtime (HEALTH log, live device) — tile cache lives here |
| Internal RAM | precious (the 0.9.3 lesson) — this design allocates ~0 from it |
| SD card | present (books/ROMs live there); tiles are just more files |
| Flash | 33% used of 6.5 MB app partition — room for ~20 KB of app code |
| Position data | v0.9.7 service: per-node lat/lon + waypoint table, already parsed |

## Tile pipeline — reuse COVEY's, add one converter

COVEY already has the whole acquisition story: `z/x/y` raster tiles under
`~/covey-tiles/`, fetched by `tools/fetch_tiles.py` (or on-device). The WiPhone reuses
that tree with ONE added step on the computer:

    fetch_tiles.py --> PNG tree --> convert_tiles.py --> raw RGB565 tree --> SD card

- A raw 565 tile is 256×256×2 = **128 KB, zero decode**: `fread` → PSRAM → `pushImage`.
  No PNG decoder ported, no decompress RAM, no surprise formats. (The phone's only
  image code today is a custom grey JPEG decoder for books — leave it alone.)
- Optional half-res variant (128×128, 32 KB) if SD throughput annoys; probably won't.

**Storage math** (20×20 km hunt area, z12–z15, ~47°N): z15 ≈ 484 tiles ≈ 62 MB,
z14 ≈ 15 MB, z13+z12 ≈ 6 MB → **~85 MB per area**. Trivial for the SD card. The PNG
source of the same area is ~10 MB, so keep PNGs on the computer and convert per trip.

## Rendering & RAM budget

- **Tile cache: 10 slots × 128 KB = 1.25 MB PSRAM**, allocated ONCE at app open
  (`ps_malloc` slab, freed on exit — no churn, the heap-fragility lesson respected).
  Screen shows at most 2×3 tiles worth of map; 10 slots make panning hitch-free.
- Draw: for each visible tile region, `pushImage` the crop straight from the PSRAM
  slot. Full-screen repaint = 240×320×2 = 150 KB over TFT SPI ≈ tens of ms — same
  class as the rest of the UI. Cache miss adds one SD read (~100–250 ms — visible as
  a briefly grey tile, acceptable and honest).
- Internal RAM: file handle + a line buffer. Nothing else.

## Input (keypad — no touch, and that's fine)

D-pad pans by half-screen steps; `*`/`#` zoom out/in ("zoom" = switch z-level tree,
no scaling math); OK cycles marker focus (camp → nodes → …) and centers on it; a
"follow <node>" mode re-centers when a new position arrives. Long-name marker labels
render in the existing UI font over the tiles.

## Markers (the point of the whole thing)

From the 0.9.7 service, already in RAM: waypoints (name + coords) and per-node last
position + age. Draw: waypoint flags, node dots with name + "12m" age, the reference
ring, and — when a pin/GPS exists — an "I am here" marker. A node whose position is
stale (>30 min) greys out rather than lying about freshness.

## The GPS question (hardware — {user}'s call, later)

The app needs nothing from it to be useful (markers above). When a GPS does arrive:
- Electrical: a UART NMEA module (like COVEY's HGLRC M100). ⚠ Which UART is actually
  free needs {user}'s schematic eye — UART0 is the serial console/panicwatch, and the
  radio/SD/TFT own the SPI buses. An I2C GPS (u-blox DDC) might be the friendlier bus
  if I2C has headroom.
- Software: a 50-line NMEA RMC/GGA reader feeding the same "own position" slot the
  pin uses today. The map, node list, and announce path don't change at all —
  the pin was designed as the GPS's stand-in.

## Risks, honestly

1. **SD throughput while music plays** — books already stream from SD concurrently;
   a map pan during MP3 playback may stutter one or the other. Measure; likely fine.
2. **pushImage from PSRAM** — TFT_eSPI reads PSRAM fine (slower than IRAM, ~2× —
   still tens of ms per frame). If it disappoints, stage through a 16 KB internal
   bounce buffer (still no heap churn).
3. **Tile provider licensing** — same story as COVEY's tiles; OSM raster for personal
   use, fetched politely by the existing script. Nothing new.
4. Battery: an SD-heavy, full-redraw app costs more than the clock screen; the
   existing adaptive-redraw idea (repaint only on pan/new-position) applies directly.

## Effort

- `tools/convert_tiles.py` (host, PNG→565 tree): half a session, host-testable.
- The app (cache, draw, pan/zoom, markers, follow mode): **1–2 sessions**.
- GPS integration when hardware exists: well under a session, by design.

**Recommendation:** worth building even GPS-less — it turns the position feature from
a line of text into a picture, for ~1.5 MB of PSRAM and zero internal RAM.

# Third-party components and their licences

This firmware is a **combined work** built from several separately-licensed pieces. The
repository as a whole is distributed under **GPL v3** (`LICENSE`), because RadioHead — which
is compiled into every build — is offered as "GPL V3 or commercial", and GPLv3 is the only
licence that can cover the rest of the mix.

That is the honest best answer, not a perfect one. Two items below do not sit cleanly under
it, and they are called out rather than papered over.

| Component | Path | Licence | Fits GPLv3? |
|---|---|---|---|
| WiPhone firmware (original) | `WiPhone/*.cpp`, `*.h` | WiPhone Public License v1.0, HackEDA Inc. | Yes — the WPL is Apache-2.0-derived, and Apache 2.0 is one-way compatible into GPLv3 |
| RadioHead | `lib/RadioHead/` | GPL v3, or commercial from AirSpayce | Yes — this project takes the GPLv3 option |
| TFT_eSPI | `WiPhone/src/TFT_eSPI/` | MIT-derived (from Adafruit_ILI9341) | Yes |
| curve25519-donna | `WiPhone/src/crypto/curve25519_donna.*` | BSD 3-clause, Google Inc. | Yes |
| tiny-AES | `WiPhone/src/crypto/tiny_aes.*` | see the file header | Yes |
| g711 / g722 / dr_wav | `WiPhone/src/audio/` | WiPhone Public License v1.0 | Yes |
| **gnuboy** (via the retro-go fork) | `WiPhone/gnuboy/` | **GPL v2** (`gnuboy/COPYING`) | ⚠️ **Unclear.** No "or any later version" clause appears in the sources here, and GPLv2-only does not combine with GPLv3-only code. If the upstream grant is "v2 or later" this resolves; nobody has established that. |
| **Helix MP3 decoder** | `WiPhone/src/audio/helix-mp3/` | **RPSL 1.0 / RCSL 1.0**, RealNetworks | ❌ **No.** The FSF lists the RPSL as GPL-incompatible: it requires derivative works to be licensed under the RPSL, and mandates litigation in Seattle. |
| Time zone grid (**data**, not code) | `WiPhone/tz_grid_data.h`, `tests/vectors_tzgrid.*` (made by `tools/gen_tz_grid.py`) | **ODbL 1.0**: derived from timezone-boundary-builder, © OpenStreetMap contributors | Not a code licence question. The ODbL governs the database and the GPL the code around it; see [below](#the-time-zone-grid-odbl-10). |

## The time zone grid (ODbL 1.0)

`WiPhone/tz_grid_data.h` holds the arrays the phone uses to find which time zone a measured
position is in. The test vectors in `tests/vectors_tzgrid.json` and `tests/vectors_tzgrid.h`
come from the same data. Both are a **Derivative Database** of
[timezone-boundary-builder](https://github.com/evansiroky/timezone-boundary-builder) (release
2026d, `combined.json`). That dataset is derived from **OpenStreetMap** data,
**© OpenStreetMap contributors**, and is made available under the
[Open Database License (ODbL) 1.0](https://opendatacommons.org/licenses/odbl/1-0/). These
files are offered under ODbL 1.0 as well.

What the licence asks of this repository:

- **Attribution.** The notice above. The same notice is compiled into the firmware as
  `TZGRID_ATTRIBUTION_TEXT` (`tzGridInfo()->attribution`), so every image that contains the
  grid carries it. That image is a *Produced Work* under the ODbL. A screen that shows a time
  zone found this way should credit "© OpenStreetMap contributors" or show the full string.
- **Share-alike for the database.** The grid itself is published here under ODbL 1.0. So is
  the means to remake it: `tools/gen_tz_grid.py` turns the public input release into these
  exact bytes (the output is deterministic, and its SHA-256 is in the header).
- **The code is not data.** `WiPhone/tz_grid.cpp`, `tz_grid.h` and `tools/gen_tz_grid.py` are
  GPL v3 like the rest of the repository. The ODbL reaches the database contents only.

COVEY ships the same grid as `covey_ui/data/tzgrid.bin` (same content hash) under the same
terms.

## What that means in practice

For a personal build flashed to your own handset, none of this bites — the obligations in
every licence above are about **distribution**, and the source is public, which is what the
copyleft terms are asking for.

It matters if you want the project to be cleanly redistributable as a single coherent work,
because today it is not. The two ways out, if that ever becomes the goal:

- **Drop MP3 playback**, removing `WiPhone/src/audio/helix-mp3/` and the music player's MP3
  path. That deletes the RPSL leg entirely. WAV playback is unaffected — it uses `dr_wav`.
- **Establish gnuboy's actual grant.** If upstream is "GPLv2 or later" the emulator is fine
  under GPLv3 and only the helix question remains.

## A note on the earlier reasoning

`platformio.ini` records that the GPLv3 Arduino wrapper around helix was avoided partly
because "GPLv3 does not sit well inside firmware under the WiPhone Public License". The
second reason given there — that helix's allocator has to be patched to reach PSRAM, and you
cannot patch a file a dependency manager re-downloads — is sound and is reason enough on its
own to vendor it.

The licensing half was mistaken, and is corrected here: this repository ships GPLv3, so a
GPLv3 wrapper would have been compatible, while the RPSL sources vendored in its place are
not. Nothing needs to change in the code because of that; it is recorded so the next person
does not repeat the reasoning.

*None of the above is legal advice.*

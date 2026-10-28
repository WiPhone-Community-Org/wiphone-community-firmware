# The music player

**Menu > Music.** MP3 and WAV, shuffle and repeat, an upload server of its own, and playback
that keeps going when you leave the screen. Since 0.9.79 it plays **mono, at half rate for
44.1/48 kHz files**, on an I2S ring of its own — see "The feed" below for why that trade was
made ({user}, 2026-09-25: *"Lower audio quality and having to take time to load in-between tracks
are things I can live with."*).

Built and flashed 2026-08-11. **Nobody has pressed play yet** — see the handoff.

## How it is put together

Four pieces, and the split is deliberate: everything that can be tested without the phone
is in a file that does not include Arduino.

| File | What it owns | Tested |
|---|---|---|
| `music_lib.{h,cpp}` | the track list and what plays next | 93 assertions, with `wav_reader` |
| `wav_reader.{h,cpp}` | WAV headers, downmix, resample | ″ |
| `mp3_stream.{h,cpp}` | ID3 skip, frame sync, feeding helix | 32 assertions, with the real decoder |
| `music_feed.{h,cpp}` | the per-pass feed: whole frames in, mono/half-rate out, the `drops` counter, the resume offset | `test_musicfeed`: the real decoder against a model of IDF 3.3's I2S DMA |
| `src/audio/helix-mp3/` | vendored decoder (RPSL) + our allocator | — |
| `Audio.cpp` | `playMusic()`, music's own I2S install, one `feed->pass()` per `loop()` | on hardware |
| `music_player.{h,cpp}` | the library, queue and current track | on hardware |
| `app_music.{h,cpp}` | the screen | on hardware |

## The three things worth knowing before changing any of it

### 1. The player is not the app, and that is the point
Apps here are constructed when opened and **deleted when you back out**. If the library and
queue lived in `MusicApp`, leaving the screen would stop the music and forget your place.
So the state lives in `music_player.cpp` with static lifetime, ticked from the main loop
beside `meshService.loop()`, and the app is a view that can come and go.

### 2. The decoder must never allocate from the internal heap
helix wants ~29 KB in seven blocks. The internal heap has ~16–26 KB free depending on what
is up, and the failure is not a failed malloc — it **succeeds**, eats the margin, and some
minutes later the WiFi PHY cannot get 2 KB for RF calibration and `phy_init` calls
`abort()`, with a backtrace pointing nowhere near audio. That is the same bug opening a book
used to cause.

⚠ **The automatic rule does not save you here.** arduino-esp32 diverts large mallocs to
PSRAM, but that threshold is 16 KB and every one of helix's blocks is smaller — the biggest,
`IMDCTInfo`, is about 9 KB. Seven allocations that are individually small and collectively
fatal is exactly what it is blind to.

The fix is `src/audio/helix-mp3/helix_memory.c`: helix routes every allocation through
`helix_malloc`/`helix_free` and upstream only *declares* them, so we implement them against
`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. Explicit rather than `ps_malloc`, because
`ps_malloc` falls back to internal RAM when PSRAM is exhausted and refusing to play is much
better than rebooting later.

**Measured on the phone 2026-08-11:**
```
internal  26388 -> 26340   cost 48 bytes  (the Mp3Stream object itself)
largest   21952 -> 21952   unchanged, nothing fragmented
psram   3619660 -> 3591604 cost 28056 (27.4 KB)
after_free 26388           fully returned
```
Repeat it with `-DMP3_HEAP_PROBE` and read the boot log at 500000 baud.

### 3. Configure I2S from TWO AGREEING DECODES, never from a header — or from one decode
A frame header can be read out of a false sync inside album art. So, it turns out, can a
successful decode: replaying 500 random resume offsets per file against the real decoder
(2026-09-25), 0.8-1.8% of resumes locked onto a false sync that decoded cleanly at 16, 22.05,
24, 32 or 48 kHz, and the rest of the track then played at 0.36x-1.09x speed. The feed now
believes a format only when two frames IN A ROW agree on it (`MusicFeed::openMp3`); the first
frame of a track is still kept (dropping it clips every track's start), and after a mid-file
start the first decoded frame is dropped (it is the IMDCT overlap transient, louder than the
music).

### 4. The feed, the ring, and the counter (0.9.79)
Measured on both phones 2026-09-25 and replayed on the Mac (details at the top of
`music_feed.h`):
- **`gaps` was arithmetic.** 512-byte reads against 627-byte 192 kbps frames forced 8.6 short
  passes a second, each one a "gap" — 8-9/s on every 192k file at 160 AND 240 MHz, 167 in 19 s
  on 0.9.78 — with zero dropouts at short passes, and 0 through real ones (a 4.2 s screenshot).
- **Real dropouts** came from passes longer than the ring: 4 x 1023 stereo at 44.1 kHz, of which
  IDF 3.3 lets a writer get only 3 buffers ahead (~70 ms). A map frame, a Files slice, a repaint
  plus anything else did it; `LOOP STALL` only logs above 250 ms.
- **The loudspeaker path played garbage** from the first MP3 commit: stereo PCM sent as 1152
  mono samples, half of each frame an octave down and the other half never played.
- **Resume re-fed the first frame** (`MAINDATA_UNDERFLOW` treated as "need bytes") and decoded
  against the old position's bit reservoir (helix was never reset): a loud burst on 2-33 of
  every 500 resumes.

What it is now: one pass decodes WHOLE frames (the compressed buffer is kept above 2 KB with
4 KB reads) into mono, halves 44.1/48 kHz through a 23-tap half-band filter, and writes music's
own ring — 24 x 512 samples, TX only, 24 KB (8 KB less than the install the phone boots with),
534 ms at 22.05 kHz — until it refuses, at most 4 frames a pass (8 when the ring is under 80%:
at 40% the ring SETTLED at 40% under map-pan-length passes, and a 300 ms stall on top dropped
out). Zero true dropouts in the model at passes of 5-80 ms, at a 100 ms stall every second, a
450 ms one every 5 s, and 77/100 ms passes with a 300 ms stall every 10 s. Any stall longer than
the ring (534 ms; 490 ms for a 48 kHz file) IS a dropout — the ring is the only slack. The Now
Playing line says `drops:N buf:X.Xs`; serial `music` says more:
- `drops` — passes that found the ring CERTAINLY dry: never invented, can miss one shorter than
  a buffer (23 ms);
- `minLead` — the lowest LOWER bound on what the ring held at any pass start: above 0 means
  there was certainly no dropout at all;
- `maxGap` — the longest the loop left music unfed (the stall `LOOP STALL` cannot see).
`music reset` zeroes them for a clean window. ⚠ **After a drop, `drops` is only a lower bound
until the ring is next FULL** (only a short write pins the upper bound again), and at long passes
that can be the rest of the track: judge a busy bench by `minLead > 0`, never by `drops=0` alone.
`reads=N (K KB, F failed)`: a read that fails before the end of the file is retried on a new handle
once a pass, three passes running, then the track stops as "Card read failed" and keeps its place
(F1 tries again) — it used to be taken for the end of the file.

⚠ **A ring in PSRAM would NOT help** (this doc and `mp3_stream.h` used to say it would): during a
stall nothing moves samples from it into the DMA, because the loop is the only thing that
writes I2S. Past ~0.5 s the only answer is a second writer (a pump task), which would sit beside
the Audio singleton that every pop, ring, call and game borrows. Not built; see "Not built".

## Why WAV is here too, and why MP3 took the work it did

The phone's audio path was already built for stereo music: `configureI2S()` selects
`I2S_CHANNEL_FMT_RIGHT_LEFT` when `monoOut` is false, `playChunk()` has a direct interleaved
L/R case, the clock runs off the **APLL** so 44100 is exact, and `playDec[2400]` is exactly
1152 stereo frames — one MPEG-1 Layer III frame. That is not a coincidence. The vestigial
`Playback::LocalMp3` enum, the commented-out `playFile("/ringtone.mp3")` at `WiPhone.ino:93`
and "TODO: migrate to dr_mp3" atop `Audio.h` are the rest of an MP3 player WiPhone intended
and never finished. It was only ever the allocator in the way.

WAV shipped first as the guaranteed path and stayed because it costs nothing: `dr_wav` was
already vendored, decoding is a memcpy, and it is a useful fallback if a file will not
decode.

## Traps

1. **`playFile()` loops forever** — it is the ringtone player and rewinds at EOF. Music uses
   `playMusic()`. `musicEnded()` waits until the last sample has CERTAINLY played (the ring is
   half a second deep), with ~116 ms of silence queued behind it — not a whole ring of it, or
   every track change is a 0.56 s gap, and not none, or IDF 3.3 replays the tail.
9. **Music has its own I2S install and nobody else may inherit it.** `configureMusicI2S()` is
   called only by `playMusic()`; every setter goes through `configureI2S()`, which puts the
   default (TX+RX, 4 x 1024) back. `turnMicOn()` calls it too — music's install has no RX, and the
   mic-level meters never call a setter. The loop's `i2s_read()` is gated on `i2sRx`.
10. **The mono pair swap.** In 16-bit ONLY_LEFT mode the ESP32 sends each pair of samples in the
   wrong order; `playChunk()` has always swapped them for the ring, the pop and calls, and the
   feed does too (`swapPairs`). `music swap off` A/Bs it by ear. ⚠ The swap is done PER STAGED
   UNIT, so every unit must reach the ring as an EVEN number of samples: MP3 frames always are; a
   WAV's are carried in whole groups of 4 (`produceWav()`), because its first unit used to come out
   odd at half rate and inverted the swap for the whole track.
11. **Nothing may take the device from under a playing track.** The pop, the ring, a call and the
   Game Boy finish a pop in flight FIRST, then pause music, then set their own rate and format
   (music's `startTrack()` finishes the pop too); the mic apps pause the player before
   `audio->start()`, and `turnMicOn()` stops a track itself as a backstop. `setSampleRate()`
   marks the ring's queue stale, so the configureI2S() after it reinstalls onto a clean ring
   (`tests/check_call_audio.py` pins all of it).
2. **Format comes from content, not extension.** The uploader has no extension filter — a
   `.wav` holding an MP3 is ordinary input.
3. **WAV `dataBytes` is a lie in streamed files** (`0xFFFFFFFF` or 0). Clamp to the real file
   size or playback runs off the end.
4. **ID3v2 sizes are syncsafe** — 7 bits per byte. Reading them big-endian is wrong for every
   tag over 128 bytes, i.e. all of them, and the result is seeking past the start of the song.
5. **End-of-file and the Next key are different operations.** Repeat-one replays at EOF but
   Next must still move, or you are trapped on one track.
6. **A rescan reorders the library**, so what is playing is remembered by path, not index.
7. **`ControlState::inputType` is a mode that persists across screens.** The now-playing
   screen forces Numeric, or 4 and 6 arrive as 'g' and 'm' depending on which app you opened
   earlier.
8. **helix is C.** It must not go through the C++ front end — C++11 narrowing rejects its
   constant tables. The host harness builds it with `$CC`, ASan on, UBSan off (it shifts
   negative values left all over the fixed-point DSP, which is what it means to do).

## Licensing

helix is **RealNetworks RPSL**, not MIT. Open source and satisfied by this repo being
public, but worth knowing before reusing any of it. The popular Arduino wrapper around it is
**GPLv3**, which is why only the raw sources are vendored and the glue is ours.

## Not built

- **Seeking.** No scrub bar. Pause/Resume (and a pop, a ring or a call cutting in) carries on
  from two frames before the one that was playing; seeking to an arbitrary time in a VBR MP3
  means either indexing the whole file or landing in the wrong place.
- **Tags.** The list shows filenames. Reading ID3 title/artist per track would mean opening
  every file to draw the library, which is the same reason Books shows no progress column.
- **Gapless playback.** There is a gap of ~0.1-0.2 s between tracks while the next file opens
  (up to ~0.6 s for the first track after a pop, a call or a game: a freshly installed ring
  plays its own half second of silence first).
- **Stereo, and 44.1 kHz.** Traded for a ring that survives the loop's long passes. The way back
  is a pump task writing I2S from a PSRAM ring (the other design the 2026-09-25 investigation
  modelled: zero drops through multi-second stalls) — at the price of a second I2S writer next
  to the singleton, with a stop-and-acknowledge handshake before every pop, ring, call and game.

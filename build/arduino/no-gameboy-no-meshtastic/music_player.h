/*
 * music_player.h — the part of the music player that outlives its screen.
 *
 * ── WHY THIS IS NOT INSIDE app_music ────────────────────────────────────────────────
 *
 * Apps here are created when you open them and DELETED when you leave. If the library,
 * the queue and the current track lived in MusicApp, backing out of the screen would
 * stop the music and forget where you were — which is not what a music player does. So
 * the state lives here, in one place with static lifetime, and MusicApp is a view onto
 * it that can come and go.
 *
 * musicPlayerLoop() is pumped from the main loop next to meshService.loop(), which is
 * what makes a track advance to the next one while you are reading a book or looking at
 * the map. Audio::loop() is already pumped from the same place and does the decoding;
 * this only decides what plays next.
 *
 * ⚠ Music and a phone call cannot both have the I2S peripheral. The rule is that a call
 * wins: musicPlayerPause() is called when one arrives, and nothing here resumes on its
 * own afterwards, because a phone that starts playing music at you when you hang up is
 * worse than one you have to press play on.
 */

#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include "music_lib.h"
#include "music_feed.h"

/* Where uploads land, and the first place the library is scanned. `/roms` and `/` are
 * also scanned, for the same reason Books does it: the Game Boy uploader has no
 * extension filter, so files genuinely do arrive in the wrong folder. */
#define MUSIC_DIR "/music"

void musicPlayerBegin();      // once, at boot: allocates the library in PSRAM
void musicPlayerLoop();       // from the main loop: advances the queue at end of track

/* Rebuild the library from the card. Returns the number of tracks found.
 * Safe to call while something is playing — the queue keeps its cursor on the track
 * that is playing if that file is still there. */
int  musicPlayerScan();

int  musicPlayerCount();
const MusicTrack* musicPlayerTrack(int i);

/* Start a library index. False if the file will not open or decode, in which case
 * musicPlayerError() says why in words fit for the screen. */
bool musicPlayerPlay(int libraryIndex);
void musicPlayerStop();               // stop and forget the position
void musicPlayerPause();              // stop the audio, remember the track
void musicPlayerResume();             // start the remembered track again
bool musicPlayerResumeAfterPop();     // the pop's teardown: carry on the track it cut (not a user's pause)
bool musicPlayerTogglePause();        // returns true if now playing

void musicPlayerNext();
void musicPlayerPrev();

bool musicPlayerIsPlaying();          // audio is actually running
bool musicPlayerIsPaused();           // a track is loaded but stopped
int  musicPlayerCurrent();            // library index, or -1
const char* musicPlayerError();       // last failure, or NULL

/* Seconds since the current track started. Wall-clock, not decoded-sample count: it is
 * for a progress line on screen, not for seeking. */
uint32_t musicPlayerElapsed();

/* The music feed's counters for the track playing (or last played): `drops` — the ring
 * CERTAINLY ran dry — and the buffered seconds, shown on the now-playing screen so "it
 * crackles a bit" can become a number. What each can and cannot claim is at the top of
 * music_feed.h. (This replaced 0.9.78's "gaps", which counted arithmetic: 8.6 a second on
 * every 192 kbps file with no dropout at all.) False before any track has been opened. */
bool musicPlayerFeedStats(MusicStats* out);

/* ── Volume ─────────────────────────────────────────────────────────────────────────
 * In dB, the units the WM875x codec actually takes: -69 is mute, +6 is maximum.
 *
 * Kept in RAM ONLY and deliberately not written to the configs file. It survives
 * stopping and starting playback, and resets to the default on a restart — which is what
 * you want on a phone whose volume you can nudge with a side button by accident.
 *
 * ⚠ It is a SEPARATE level from the call volume. The codec has one set of registers, so
 * playing music overwrites what calls use; the player captures the call levels the first
 * time it takes over and puts them back when music stops. Without that, one quiet album
 * leaves you unable to hear the next phone call. */
#define MUSIC_VOL_DEFAULT_DB  (-18)   // starts low on purpose: this drives headphones
#define MUSIC_VOL_STEP_DB     3
#define MUSIC_VOL_MIN_DB      (-45)
#define MUSIC_VOL_MAX_DB      6

int  musicPlayerVolume();             // current level in dB
void musicPlayerVolumeStep(int steps); // +1 louder, -1 quieter; clamped

/* ── A call takes the codec ─────────────────────────────────────────────────────────
 * Pause the track if one is SOUNDING (the place is kept, as a pause always keeps it), and
 * give the call levels and route back even if something ELSE already stopped the track.
 *
 * 🛑 THAT SECOND HALF IS THE FIX. The ringtone's playRingtone() -> ceasePlayback() takes
 * `playback` away from music before anything asks musicPlayerIsPlaying(), and so does a
 * notification pop — so musicPlayerPause() saw "not playing", skipped restoreCallVolume(), and
 * the phone rang, and the answered call ran, at the MUSIC level (default -18 dB, as low as
 * -45). Idempotent: with nothing playing and nothing stashed it does nothing, so the main loop
 * calls it on every pass of a live call. */
void musicPlayerYieldForCall();

/* ── The call levels while music holds the codec ────────────────────────────────────
 * While music plays, the codec's volume registers hold the MUSIC level and the call levels
 * live in the player's stash, put back when music lets go. So "what are the call volumes" is
 * answered by the stash while one is held, and a new call level must go INTO the stash:
 * writing it to the codec lands under the music (headphones jumped about 24 dB), and the
 * stale stash then overwrote it when the music stopped, so a Settings > Audio save did not
 * take until a reboot. Settings > Audio is the caller.
 *
 * musicPlayerCallVolumes: true + the stashed levels while music holds the codec; false means
 * the codec itself holds the call levels (ask audio->getVolumes()).
 * musicPlayerSetCallVolumes: true = stored in the stash, applied when music lets go; false =
 * no stash is held, the caller writes the codec itself. Not clamped here: restoring goes
 * through Audio::setVolumes(), which clamps. */
bool musicPlayerCallVolumes(int8_t& ear, int8_t& hp, int8_t& loud);
bool musicPlayerSetCallVolumes(int8_t ear, int8_t hp, int8_t loud);

void        musicPlayerSetShuffle(bool on);
bool        musicPlayerShuffle();
void        musicPlayerSetRepeat(MusicRepeat r);
MusicRepeat musicPlayerRepeat();

#endif // MUSIC_PLAYER_H

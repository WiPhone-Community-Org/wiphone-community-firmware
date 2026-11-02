/*
 * music_player.cpp — see music_player.h for why this is not inside the app.
 */

#include "music_player.h"
#include "Audio.h"
#include "Hardware.h"
#include "Arduino.h"
#include "SD.h"
#include <string.h>

extern Audio* audio;
/* WiPhone.ino: end a notification chirp still in flight — see startTrack(). */
extern void notifyPopFinishFor(const char* why);

// ─── state, with static lifetime so it survives the screen closing ───────────────────

static MusicTrack* s_tracks = NULL;      // PSRAM
static int         s_count = 0;
static MusicQueue  s_queue;
static bool        s_paused = false;     // a track is loaded but not running
static int         s_loaded = -1;        // library index of that track, or -1
static uint32_t    s_startedAt = 0;
static uint32_t    s_elapsedBase = 0;    // seconds already played before the last resume
/* Byte offset in the file where pause happened, so play-pause-play carries on instead of
 * starting the song again. 0 = from the beginning. */
static uint32_t    s_resumePos = 0;
static const char* s_error = NULL;
static bool        s_began = false;
/* The pause is a STOP the player adopted (a pop, the ring, a call - adoptStopPlace()), not F1.
 * Only such a pause may be undone by the pop's teardown (musicPlayerResumeAfterPop()): a track
 * the user paused stays paused through a chirp. */
static bool        s_pausedByStop = false;

/* Volume, in dB, RAM-only — see music_player.h. */
static int         s_vol = MUSIC_VOL_DEFAULT_DB;
static bool        s_volSaved = false;      // are the call levels stashed?
static int8_t      s_savedEar = 6, s_savedHp = 6, s_savedLoud = 0;
static bool        s_savedLoudspeaker = false;   // which speaker calls were using

/* Take the codec over, remembering what calls were using. */
static void applyMusicVolume() {
  if (!audio) {
    return;
  }
  if (!s_volSaved) {
    audio->getVolumes(s_savedEar, s_savedHp, s_savedLoud);
    s_savedLoudspeaker = audio->isLoudspeaker();
    s_volSaved = true;
  }
  /* ⚠ With nothing in the jack, music belongs on the LOUDSPEAKER.
   * The default is the earpiece — correct for a phone call, where you are holding it to
   * your head, and useless for music, which is why it sounded so quiet. chooseSpeaker()
   * switches the codec's output path to DAC_LOUDSPEAKER and turns on the separate
   * amplifier IC. Restored on stop, so the next call still goes to the earpiece.
   *
   * 🛑 UNCONDITIONALLY (0.9.79), the Game Boy's lesson (app_gbc.cpp, 2026-09-19): the route's
   * precedence is headphones > loudspeaker > earpiece and lives in Audio::start() and
   * codecReconfig(), so the flag is inert while the jack is occupied — and when the jack is
   * pulled mid-track, setHeadphones() reconfigures the live codec straight onto the
   * loudspeaker. Guarded on "no headphones now", a track STARTED with headphones in never set
   * it, and an unplug dropped the music into the earpiece. That is why the player used to
   * reopen the track on every jack change; the format no longer depends on the jack (music is
   * always mono now, music_feed.h), so the route is all that was left, and this covers it. */
  audio->chooseSpeaker(true);
  const int8_t v = (int8_t)s_vol;
  // The loudspeaker amp maxes out at 0 dB where the other two reach +6.
  const int8_t loud = v > 0 ? 0 : v;
  audio->setVolumes(v, v, loud);
}

/* Give the codec back at the levels the phone had before music started. Without this a
 * quiet album leaves the next call inaudible. */
static void restoreCallVolume() {
  if (audio && s_volSaved) {
    audio->setVolumes(s_savedEar, s_savedHp, s_savedLoud);
    audio->chooseSpeaker(s_savedLoudspeaker);
    s_volSaved = false;
  }
}

void musicPlayerYieldForCall() {
  musicPlayerPause();         // sounding: keep the place, stop, give the levels back
  restoreCallVolume();        // already stopped by the ring or a pop: the levels are still ours
}

bool musicPlayerCallVolumes(int8_t& ear, int8_t& hp, int8_t& loud) {
  if (!s_volSaved) {
    return false;
  }
  ear = s_savedEar;
  hp = s_savedHp;
  loud = s_savedLoud;
  return true;
}

bool musicPlayerSetCallVolumes(int8_t ear, int8_t hp, int8_t loud) {
  if (!s_volSaved) {
    return false;
  }
  s_savedEar = ear;
  s_savedHp = hp;
  s_savedLoud = loud;
  return true;
}

int musicPlayerVolume() {
  return s_vol;
}

void musicPlayerVolumeStep(int steps) {
  s_vol += steps * MUSIC_VOL_STEP_DB;
  if (s_vol > MUSIC_VOL_MAX_DB) {
    s_vol = MUSIC_VOL_MAX_DB;
  }
  if (s_vol < MUSIC_VOL_MIN_DB) {
    s_vol = MUSIC_VOL_MIN_DB;
  }
  // Only touch the codec while we own it; otherwise this is just a remembered number.
  if (s_volSaved) {
    const int8_t v = (int8_t)s_vol;
    audio->setVolumes(v, v, v > 0 ? 0 : v);
  }
}

void musicPlayerBegin() {
  if (s_began) {
    return;
  }
  s_began = true;
  /* PSRAM: 96 tracks x ~200 bytes is nearly 19 KB, and the internal heap has about 16 KB
   * free in total. This is the same rule that BooksApp learned the hard way. */
  s_tracks = (MusicTrack*)ps_malloc(sizeof(MusicTrack) * MUSIC_MAX_TRACKS);
  if (s_tracks) {
    memset(s_tracks, 0, sizeof(MusicTrack) * MUSIC_MAX_TRACKS);
  }
  s_queue.seed((uint32_t)esp_random());
  musicPlayerScan();
}

// ─── the library ────────────────────────────────────────────────────────────────────

static void addFrom(const char* dirPath) {
  File dir = SD.open(dirPath);
  if (!dir) {
    return;
  }
  if (!dir.isDirectory()) {
    dir.close();
    return;
  }
  File f;
  while (s_count < MUSIC_MAX_TRACKS && (f = dir.openNextFile())) {
    if (!f.isDirectory()) {
      const char* nm = f.name();
      if (musicIsPlayable(nm)) {
        char disp[MUSIC_NAME_MAX];
        musicDisplayName(nm, disp, sizeof(disp));

        bool dup = false;
        for (int i = 0; i < s_count; i++) {
          if (strcmp(s_tracks[i].name, disp) == 0) {
            dup = true;      // the same track in two folders is listed once
          }
        }
        if (!dup) {
          snprintf(s_tracks[s_count].name, MUSIC_NAME_MAX, "%s", disp);
          snprintf(s_tracks[s_count].path, MUSIC_PATH_MAX, "%s", nm);
          s_tracks[s_count].fmt = musicFormatOf(nm);
          s_count++;
        }
      }
    }
    f.close();
  }
  dir.close();
}

int musicPlayerScan() {
  if (!s_tracks) {
    return 0;
  }
  /* Remember what is playing by PATH, not by index: a scan can reorder everything, and
   * an index kept across it would silently point at a different song. */
  char playingPath[MUSIC_PATH_MAX];
  playingPath[0] = '\0';
  if (s_loaded >= 0 && s_loaded < s_count) {
    snprintf(playingPath, sizeof(playingPath), "%s", s_tracks[s_loaded].path);
  }

  /* Four folders, because the uploaders are interchangeable and files genuinely land in
   * the wrong one. `accept=` is a browser hint, not a filter — the Books uploader will
   * happily take an MP3 and put it in /books, which is exactly what happened the first
   * time this app shipped. musicIsPlayable() is what decides, so an .epub sitting in
   * /books never appears here. Books scans /roms for the same reason. */
  s_count = 0;
  addFrom(MUSIC_DIR);
  addFrom("/books");
  addFrom("/roms");
  addFrom("/");
  musicSortTracks(s_tracks, s_count);

  s_queue.reset(s_count);

  s_loaded = -1;
  if (playingPath[0]) {
    for (int i = 0; i < s_count; i++) {
      if (strcmp(s_tracks[i].path, playingPath) == 0) {
        s_loaded = i;
        s_queue.startAt(i);
        break;
      }
    }
    if (s_loaded < 0 && audio) {
      audio->stopMusic();      // the file that was playing is gone
      s_paused = false;
    }
  }
  return s_count;
}

int musicPlayerCount() {
  return s_count;
}

const MusicTrack* musicPlayerTrack(int i) {
  if (!s_tracks || i < 0 || i >= s_count) {
    return NULL;
  }
  return &s_tracks[i];
}

// ─── transport ──────────────────────────────────────────────────────────────────────

static bool startTrack(int idx, uint32_t startAt = 0) {
  s_error = NULL;
  if (!audio || !s_tracks || idx < 0 || idx >= s_count) {
    s_error = "No track";
    return false;
  }
  /* 🛑 REFUSED HERE, BEFORE ANYTHING IS TOUCHED, while an RTP session is armed. playMusic()
   * refuses too ("In a call"), but the failure branch below then unloads the track — s_loaded
   * -1, s_paused false — so a paused place was LOST: musicPlayerResume()'s fall-back retry
   * failed with "No track". That window is real: a session still armed while gui.inCall() is
   * already false (END's HangUp with the SIP block not running, an orphan before the 3 s
   * backstop, the `audio orphan` bench), and the transport keys are live again. A refusal is
   * not a bad file: the track, the place and the pause all stay as they were, and F1 once the
   * session is gone carries on. Pinned by tests/check_call_audio.py. */
  if (audio->rtpSessionArmed()) {
    s_error = "In a call";
    return false;
  }
  /* 🛑 THE POP FIRST, THEN THE MUSIC — the rule the ring, a call and the Game Boy already keep
   * (review, 2026-09-25). A pop that cuts a track leaves it PAUSED at its place (0.9.79), which
   * invites an F1 inside the chirp's ~300 ms; playMusic() took the device without finishing the
   * pop, and the pop's timed teardown then ceasePlayback()'d the NEW track and restore()d the
   * pre-pop state over it — the F1 undone a third of a second later. Finished here, the pop's
   * restore() lands first and the track sets everything it needs on top. */
  notifyPopFinishFor("music is starting");
  audio->stopMusic();
  if (!audio->playMusic(&SD, s_tracks[idx].path, startAt)) {
    s_error = audio->musicError() ? audio->musicError() : "Will not play";
    s_loaded = -1;
    s_paused = false;
    return false;
  }
  applyMusicVolume();
  s_loaded = idx;
  s_paused = false;
  s_startedAt = millis();
  return true;
}

bool musicPlayerPlay(int libraryIndex) {
  s_queue.startAt(libraryIndex);
  s_resumePos = 0;
  s_elapsedBase = 0;
  return startTrack(libraryIndex);
}

void musicPlayerStop() {
  if (audio) {
    audio->stopMusic();
  }
  restoreCallVolume();
  s_loaded = -1;
  s_paused = false;
  s_elapsedBase = 0;
  s_resumePos = 0;
}

void musicPlayerPause() {
  if (audio && audio->musicPlaying()) {
    s_elapsedBase = musicPlayerElapsed();
    /* Where to pick up from. Captured BEFORE stopMusic(), which closes the file. */
    s_resumePos = audio->musicFilePos();
    audio->stopMusic();
    restoreCallVolume();
    s_paused = s_loaded >= 0;
    s_pausedByStop = false;                  // the user's pause: nothing undoes it for them
  }
}

void musicPlayerResume() {
  if (s_paused && s_loaded >= 0) {
    /* Carry on from the byte where pause happened rather than restarting the song.
     *
     * This is a resume, not a seek: the offset is one we recorded ourselves while
     * playing, so there is no need to guess a position from a timestamp — which is the
     * hard part in a VBR MP3 and the reason a scrub bar still does not exist. It is two
     * frames before the one that was PLAYING (MusicFeed::playingPos()); the feed resets
     * helix, steps over the frame with no bit reservoir and drops the overlap transient,
     * so the first frame heard is the one that was cut off. (Until 0.9.79 a resume re-fed
     * that first frame and played a burst on ~1 in 30, and the offset was the READ
     * position, some way ahead of the ear.)
     *
     * s_elapsedBase is deliberately NOT reset, so the clock on screen keeps counting
     * from where it was instead of jumping back to 0:00. */
    const uint32_t at = s_resumePos;
    if (startTrack(s_loaded, at)) {
      s_resumePos = 0;
      s_pausedByStop = false;
    } else if (startTrack(s_loaded)) {
      // The offset was refused for some reason; falling back to the start still plays.
      s_resumePos = 0;
      s_elapsedBase = 0;
      s_pausedByStop = false;
    }
  }
}

/* Something other than the player stopped the track — a notification pop, the ring, a call, a
 * shutdown, a card that would not read: make it a pause at its place, so F1 carries on from
 * where it was cut instead of restarting the song at 0:00 (what every mesh message used to
 * cost). True if there was such a stop to adopt.
 * ⚠ The call levels are NOT handed back here: a pop still playing holds a snapshot of the MUSIC
 * levels and its teardown restore()s them after this runs, so giving them back now would be
 * undone (the "pop first, then the music" rule — see musicPlayerYieldForCall(), which still
 * gives them back when a call is what stopped the track). The stash stays held as before. */
static bool adoptStopPlace() {
  uint32_t pos = 0, stoppedMs = 0;
  if (!audio || s_paused || s_loaded < 0 || audio->musicPlaying() ||
      !audio->musicTakeStopPlace(&pos, &stoppedMs)) {
    return false;
  }
  s_elapsedBase += (stoppedMs - s_startedAt) / 1000;
  s_resumePos = pos;
  s_paused = true;
  s_pausedByStop = true;
  s_error = audio->musicError();   // "Card read failed"; NULL for a pop or a call, which are not faults
  return true;
}

/* The notification pop's teardown carries on the track it cut ({user}, 2026-09-26, the first
 * listening pass: "it stopped playing the song after the chirp" - the cut was kept as a pause at
 * its place, and then waited for F1). Only a pause the player ADOPTED from a stop, with no fault
 * ("Card read failed" stays stopped) - never one the user made - and only when the pop's own
 * branch asks, AFTER its restore() (WiPhone.ino: the "pop first, then the music" rule; a call
 * that cut the pop never asks). musicPlayerLoop() adopts the stop within a pass of the cut, so
 * the adoption here is for the pass the pop ends in. True when the track is running again. */
bool musicPlayerResumeAfterPop() {
  adoptStopPlace();
  if (!s_paused || !s_pausedByStop || s_error || s_loaded < 0) {
    return false;
  }
  musicPlayerResume();
  return musicPlayerIsPlaying();
}

bool musicPlayerTogglePause() {
  /* A stop not yet adopted (F1 in the same pass as the pop that cut the track): adopt it first,
   * or the key below would find neither playing nor paused and restart the track at 0:00. */
  adoptStopPlace();
  if (musicPlayerIsPlaying()) {
    musicPlayerPause();
    return false;
  }
  if (s_paused) {
    musicPlayerResume();
  } else if (s_count > 0) {
    int at = s_queue.current();
    musicPlayerPlay(at >= 0 ? at : 0);
  }
  return musicPlayerIsPlaying();
}

void musicPlayerNext() {
  int n = s_queue.skip(1);
  s_resumePos = 0;
  s_elapsedBase = 0;
  if (n >= 0) {
    startTrack(n);
  } else {
    musicPlayerStop();
  }
}

void musicPlayerPrev() {
  /* Under three seconds in, Previous means the previous track; after that it means the
   * start of this one. Every music player does this and it is missed when absent. */
  const bool restartThisOne = musicPlayerElapsed() >= 3 && s_loaded >= 0;
  s_resumePos = 0;
  s_elapsedBase = 0;
  if (restartThisOne) {
    startTrack(s_loaded);
    return;
  }
  int p = s_queue.skip(-1);
  if (p >= 0) {
    startTrack(p);
  }
}

bool musicPlayerIsPlaying() {
  return audio && audio->musicPlaying();
}

bool musicPlayerIsPaused() {
  return s_paused;
}

int musicPlayerCurrent() {
  return s_loaded;
}

const char* musicPlayerError() {
  return s_error;
}

uint32_t musicPlayerElapsed() {
  if (!musicPlayerIsPlaying()) {
    return s_elapsedBase;
  }
  return s_elapsedBase + (millis() - s_startedAt) / 1000;
}

bool musicPlayerFeedStats(MusicStats* out) {
  return audio && out && audio->musicStats(out);
}

void musicPlayerSetShuffle(bool on) {
  s_queue.setShuffle(on);
}
bool musicPlayerShuffle() {
  return s_queue.shuffle();
}
void musicPlayerSetRepeat(MusicRepeat r) {
  s_queue.setRepeat(r);
}
MusicRepeat musicPlayerRepeat() {
  return s_queue.repeat();
}

// ─── the tick ───────────────────────────────────────────────────────────────────────

void musicPlayerLoop() {
  if (!audio || s_paused || s_loaded < 0) {
    return;
  }
  if (!audio->musicPlaying()) {
    /* Stopped by something else. The device still yields exactly as it always has; what
     * changes (0.9.79) is that the PLACE is kept — see adoptStopPlace(). */
    adoptStopPlace();
    return;
  }

  /* The headphone jack needs nothing here any more (0.9.79). The track used to be reopened on
   * every jack change, because stereo-vs-mono was decided when a track opened; music is always
   * mono now, and applyMusicVolume() sets the loudspeaker flag unconditionally, so
   * setHeadphones() (the jack interrupt) moves the live codec between the headphones and the
   * loudspeaker by itself and the track plays on without a gap. */
  if (!audio->musicEnded()) {
    return;
  }

  /* End of track. advance() is the end-of-FILE step, so repeat-one replays here — which
   * is deliberately not what the Next key does. See music_lib.h. */
  s_resumePos = 0;
  s_elapsedBase = 0;
  int nxt = s_queue.advance();
  if (nxt < 0) {
    musicPlayerStop();           // end of the queue with repeat off
    return;
  }
  if (!startTrack(nxt)) {
    /* One bad file should not end the album. Try the one after it, and give up only if
     * the whole queue is unplayable, rather than looping over a broken library. */
    for (int guard = 0; guard < s_count; guard++) {
      int skip = s_queue.advance();
      if (skip < 0 || startTrack(skip)) {
        return;
      }
    }
    musicPlayerStop();
  }
}

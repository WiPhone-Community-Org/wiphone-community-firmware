/*
 * music_lib.h — the track list and what plays next.
 *
 * Deliberately free of Arduino and SD headers so the whole of it compiles on a Mac under
 * ASan and is exercised by tests/test_music.cpp. That is the same split that made
 * book_layout testable: the app owns the screen and the file handles, this owns the
 * decisions. Everything here is plain C++ over caller-supplied strings.
 *
 * ── WHAT "NEXT" MEANS, BECAUSE IT IS NOT OBVIOUS ────────────────────────────────────
 *
 * There are two orders in play and confusing them is the classic shuffle bug:
 *
 *   - the LIBRARY order, which is what the list on screen shows (sorted by name), and
 *   - the PLAY order, which is the library order or a shuffled permutation of it.
 *
 * `MusicQueue` stores a permutation `order[]` of track indices plus a cursor into it. Next
 * and previous move the CURSOR, never the library. So the on-screen list never reorders
 * under the user's finger when they toggle shuffle mid-song, and "previous" in shuffle
 * goes back to the track that actually played rather than to a fresh random one.
 *
 * ⚠ Toggling shuffle KEEPS THE CURRENT TRACK PLAYING and reshuffles the rest around it.
 * Rebuilding the permutation from scratch would jump to whatever landed in slot 0, which
 * reads as "shuffle skipped my song".
 */

#ifndef MUSIC_LIB_H
#define MUSIC_LIB_H

#include <stdint.h>
#include <stddef.h>

#define MUSIC_MAX_TRACKS   96     // tracks listed from the card
#define MUSIC_NAME_MAX     64     // display name, cut to fit
#define MUSIC_PATH_MAX    128     // full path on the card

enum MusicRepeat {
  MUSIC_REPEAT_OFF = 0,   // stop at the end of the last track
  MUSIC_REPEAT_ALL = 1,   // wrap to the start
  MUSIC_REPEAT_ONE = 2,   // the same track again
};

/* What a file's EXTENSION claims it is — used to decide what the library lists. What it
 * actually IS is decided later by Audio::playMusic(), from the content, because an
 * uploader with no extension filter means a renamed file is an ordinary thing to meet. */
/* ⚠ Headerless .pcm/.raw are deliberately NOT here. The card carries them — the recorder
 * writes /audio_*.pcm and the ringtones are .pcm — but they are 8/16 kHz mono telephony
 * captures, not music, and Audio::playMusic() cannot play one: it identifies a file by
 * CONTENT, and a headerless file looks like neither WAV nor MP3. Listing them put an
 * unplayable row at the top of the library (they sort before everything) and the first
 * on-device test tried to MP3-decode a phone recording. */
enum MusicFormat {
  MUSIC_FMT_UNKNOWN = 0,
  MUSIC_FMT_WAV,
  MUSIC_FMT_MP3,
};

struct MusicTrack {
  char name[MUSIC_NAME_MAX];   // what the list shows
  char path[MUSIC_PATH_MAX];   // what the player opens
  MusicFormat fmt;
};

/* Extension → format. Case-insensitive, and it looks at the LAST dot only, so
 * "Bad. Song.wav" works and "song.wav.txt" is correctly not audio. */
MusicFormat musicFormatOf(const char* filename);

/* True if this file belongs in the library at all. Keeps the scanner in the app tiny and
 * puts the rule somewhere a test can reach it. */
bool musicIsPlayable(const char* filename);

/* Strip the directory and the extension to make a display name, and cut it to `cap`
 * bytes without splitting a UTF-8 sequence. Always NUL-terminates. */
void musicDisplayName(const char* path, char* out, size_t cap);

/* Sort tracks by display name, case-insensitively, in place. A plain insertion sort:
 * MUSIC_MAX_TRACKS is 96 and this runs once per library scan. */
void musicSortTracks(MusicTrack* tracks, int n);

/* The play order and the cursor into it.
 *
 * Holds no strings and no files — just indices — so it is cheap to keep alive after the
 * app that made it has been destroyed, which is exactly what background playback needs. */
class MusicQueue {
public:
  MusicQueue();

  void reset(int trackCount);          // library changed: identity order, cursor at 0
  int  count() const { return n; }

  /* Index into the LIBRARY of the track currently playing, or -1 if the queue is empty
   * or has run off the end with repeat off. */
  int  current() const;

  /* Start playing a given library index. Used when the user taps a row: in shuffle the
   * permutation is re-drawn around that track so it plays now and the rest follow. */
  void startAt(int libraryIndex);

  /* Advance/retreat the cursor. Returns the new library index, or -1 when repeat is off
   * and there is nothing after the last track (the caller should stop). `advance` is what
   * end-of-file calls; `skip` is what the user's Next key calls, and they differ: repeat-one
   * replays on EOF but must NOT trap the Next key on one track forever. */
  int  advance();
  int  skip(int delta);

  void setShuffle(bool on);
  bool shuffle() const { return shuffled; }
  void setRepeat(MusicRepeat r) { rep = r; }
  MusicRepeat repeat() const { return rep; }

  /* Deterministic in tests, unpredictable enough on the phone. Seeded from millis() by
   * the app; tests seed a constant and assert on exact permutations. */
  void seed(uint32_t s) { rng = s ? s : 1; }

private:
  int  order[MUSIC_MAX_TRACKS];   // order[cursor] is a library index
  int  n;
  int  cursor;
  bool shuffled;
  MusicRepeat rep;
  uint32_t rng;

  uint32_t nextRandom();
  void buildIdentity();
  int  positionOf(int libraryIndex) const;
  /* Shuffle order[from..n) in place, leaving order[0..from) untouched. That "leave the
   * front alone" is what keeps the playing track playing when shuffle is switched on. */
  void shuffleFrom(int from);
};

#endif // MUSIC_LIB_H

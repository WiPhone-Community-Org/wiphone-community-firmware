/*
 * music_lib.cpp — see music_lib.h for why the play order and the library order are two
 * different things.
 */

#include "music_lib.h"
#include <string.h>

// ─── formats ────────────────────────────────────────────────────────────────────────

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive compare of a NUL-terminated extension. */
static bool extIs(const char* dot, const char* want) {
  size_t i = 0;
  for (; want[i]; i++) {
    if (!dot[i] || lower(dot[i]) != lower(want[i])) {
      return false;
    }
  }
  return dot[i] == '\0';
}

/* The last dot in the BASENAME. Looking in the whole path would find the dot in a
 * directory like "/my.music/song" and call the extension "music/song". */
static const char* lastDot(const char* filename) {
  const char* base = filename;
  for (const char* p = filename; *p; p++) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  const char* dot = NULL;
  for (const char* p = base; *p; p++) {
    if (*p == '.') {
      dot = p;
    }
  }
  // A leading dot is a hidden file, not an extension: ".wav" has no extension.
  if (dot == base) {
    return NULL;
  }
  return dot;
}

MusicFormat musicFormatOf(const char* filename) {
  if (!filename) {
    return MUSIC_FMT_UNKNOWN;
  }
  const char* dot = lastDot(filename);
  if (!dot) {
    return MUSIC_FMT_UNKNOWN;
  }
  if (extIs(dot, ".wav")) {
    return MUSIC_FMT_WAV;
  }
  if (extIs(dot, ".mp3")) {
    return MUSIC_FMT_MP3;
  }
  return MUSIC_FMT_UNKNOWN;
}

bool musicIsPlayable(const char* filename) {
  if (!filename || !filename[0]) {
    return false;
  }
  // Skip the dot-underscore files macOS sprinkles over a FAT card; they are resource
  // forks and they would otherwise appear as a phantom copy of every track.
  const char* base = filename;
  for (const char* p = filename; *p; p++) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  if (base[0] == '.') {
    return false;
  }
  return musicFormatOf(filename) != MUSIC_FMT_UNKNOWN;
}

void musicDisplayName(const char* path, char* out, size_t cap) {
  if (!out || cap == 0) {
    return;
  }
  out[0] = '\0';
  if (!path) {
    return;
  }

  const char* base = path;
  for (const char* p = path; *p; p++) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }

  const char* dot = lastDot(base);
  size_t len = dot ? (size_t)(dot - base) : strlen(base);
  if (len > cap - 1) {
    len = cap - 1;
    // Do not split a UTF-8 sequence: back off over continuation bytes (10xxxxxx).
    while (len > 0 && ((unsigned char)base[len] & 0xC0) == 0x80) {
      len--;
    }
  }
  memcpy(out, base, len);
  out[len] = '\0';
}

// ─── sorting ────────────────────────────────────────────────────────────────────────

static int nameCmp(const char* a, const char* b) {
  while (*a && *b) {
    char ca = lower(*a), cb = lower(*b);
    if (ca != cb) {
      return (unsigned char)ca < (unsigned char)cb ? -1 : 1;
    }
    a++;
    b++;
  }
  if (*a == *b) {
    return 0;
  }
  return *a ? 1 : -1;
}

void musicSortTracks(MusicTrack* tracks, int n) {
  if (!tracks || n < 2) {
    return;
  }
  for (int i = 1; i < n; i++) {
    MusicTrack key = tracks[i];
    int j = i - 1;
    while (j >= 0 && nameCmp(tracks[j].name, key.name) > 0) {
      tracks[j + 1] = tracks[j];
      j--;
    }
    tracks[j + 1] = key;
  }
}

// ─── the queue ──────────────────────────────────────────────────────────────────────

MusicQueue::MusicQueue()
  : n(0), cursor(0), shuffled(false), rep(MUSIC_REPEAT_OFF), rng(1) {
  memset(order, 0, sizeof(order));
}

/* xorshift32. Small, no libc dependency, and identical on the host and the phone — which
 * is what lets a test assert on an exact permutation. */
uint32_t MusicQueue::nextRandom() {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}

void MusicQueue::buildIdentity() {
  for (int i = 0; i < n; i++) {
    order[i] = i;
  }
}

void MusicQueue::reset(int trackCount) {
  if (trackCount < 0) {
    trackCount = 0;
  }
  if (trackCount > MUSIC_MAX_TRACKS) {
    trackCount = MUSIC_MAX_TRACKS;
  }
  n = trackCount;
  cursor = 0;
  buildIdentity();
  if (shuffled) {
    shuffleFrom(0);
  }
}

int MusicQueue::current() const {
  if (n <= 0 || cursor < 0 || cursor >= n) {
    return -1;
  }
  return order[cursor];
}

int MusicQueue::positionOf(int libraryIndex) const {
  for (int i = 0; i < n; i++) {
    if (order[i] == libraryIndex) {
      return i;
    }
  }
  return -1;
}

void MusicQueue::shuffleFrom(int from) {
  if (from < 0) {
    from = 0;
  }
  // Fisher-Yates over the tail only.
  for (int i = n - 1; i > from; i--) {
    int j = from + (int)(nextRandom() % (uint32_t)(i - from + 1));
    int t = order[i];
    order[i] = order[j];
    order[j] = t;
  }
}

void MusicQueue::startAt(int libraryIndex) {
  if (n <= 0 || libraryIndex < 0 || libraryIndex >= n) {
    return;
  }
  int pos = positionOf(libraryIndex);
  if (pos < 0) {
    return;
  }
  if (!shuffled) {
    /* In library order the play order IS the library, so tapping a row only moves the
     * cursor. ⚠ Do not "bring it to the front" here — that reorders the album, so the
     * track after the one you tapped is no longer the one below it in the list. */
    cursor = pos;
    return;
  }
  // Shuffled: play it now, and re-draw everything after it so what follows is not
  // simply the rest of the library in order.
  int t = order[0];
  order[0] = order[pos];
  order[pos] = t;
  cursor = 0;
  shuffleFrom(1);
}

int MusicQueue::advance() {
  if (n <= 0) {
    return -1;
  }
  if (rep == MUSIC_REPEAT_ONE) {
    return current();          // end of file replays the same track
  }
  if (cursor + 1 < n) {
    cursor++;
    return current();
  }
  if (rep == MUSIC_REPEAT_ALL) {
    // A fresh shuffle each time round, otherwise "shuffle + repeat all" plays the same
    // permutation forever, which is not what anyone means by shuffle.
    if (shuffled) {
      shuffleFrom(0);
    }
    cursor = 0;
    return current();
  }
  cursor = n;                  // off the end: current() now reports -1
  return -1;
}

int MusicQueue::skip(int delta) {
  if (n <= 0) {
    return -1;
  }
  // Deliberately ignores repeat-one: the user pressing Next means "a different track",
  // even though end-of-file on the same setting means "play it again".
  if (cursor >= n) {
    cursor = n - 1;            // we had run off the end; Next/Prev brings us back
  }
  int pos = cursor + delta;
  if (pos < 0) {
    pos = (rep == MUSIC_REPEAT_OFF) ? 0 : n - 1;
  } else if (pos >= n) {
    if (rep == MUSIC_REPEAT_OFF) {
      cursor = n;
      return -1;
    }
    if (shuffled) {
      shuffleFrom(0);
    }
    pos = 0;
  }
  cursor = pos;
  return current();
}

void MusicQueue::setShuffle(bool on) {
  if (on == shuffled) {
    return;
  }
  shuffled = on;
  if (n <= 0) {
    return;
  }

  int playing = current();
  if (on) {
    // Keep the current track where it is and shuffle everything after it.
    if (playing >= 0) {
      shuffleFrom(cursor + 1);
    } else {
      shuffleFrom(0);
    }
  } else {
    // Back to library order, with the cursor following the track that is playing so the
    // music does not jump when shuffle is switched off.
    buildIdentity();
    cursor = (playing >= 0) ? playing : 0;
  }
}

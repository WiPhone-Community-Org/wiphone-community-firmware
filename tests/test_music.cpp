/*
 * test_music.cpp — the music player's two testable halves: what plays next, and what a
 * .wav actually contains.
 *
 * Both modules are Arduino-free so they compile here with the Mac's compiler under
 * ASan+UBSan. Nothing in this file needs the phone.
 *
 * The two things worth testing hardest, because both fail SILENTLY on the device:
 *
 *   1. Shuffle. The bug is not "it isn't random" — it is shuffle reordering the list under
 *      the user or skipping the song that is playing. Those are invariants, so they are
 *      asserted directly rather than inferred from a distribution.
 *
 *   2. The resampler's priming. An off-by-one in which two samples the interpolator sits
 *      between does not crash and does not look wrong in a debugger; it plays the file at
 *      the wrong speed. The first version of this file caught exactly that.
 */
#include "../WiPhone/music_lib.h"
#include "../WiPhone/wav_reader.h"

#include <stdio.h>
#include <string.h>
#include <vector>

static int g_pass = 0, g_fail = 0;
static const char* g_group = "";

static void group(const char* name) {
  g_group = name;
  printf("\n\033[1m%s\033[0m\n", name);
}

static void ok(bool cond, const char* what) {
  if (cond) {
    g_pass++;
  } else {
    g_fail++;
    printf("  \033[31mFAIL\033[0m %s :: %s\n", g_group, what);
  }
}

// ───────────────────────────────────────────────────────── formats and names

static void testFormats() {
  group("what counts as a track");

  ok(musicFormatOf("song.wav") == MUSIC_FMT_WAV, ".wav");
  ok(musicFormatOf("SONG.WAV") == MUSIC_FMT_WAV, "extension is case-insensitive");
  ok(musicFormatOf("song.mp3") == MUSIC_FMT_MP3, ".mp3");
  ok(musicFormatOf("SONG.MP3") == MUSIC_FMT_MP3, "and case-insensitively");
  /* Headerless captures are NOT music. The recorder writes /audio_*.pcm and they sort
   * before everything, so listing them put an unplayable row at the top of the library —
   * found on the device, where the first timing run tried to MP3-decode one. */
  ok(musicFormatOf("beep.pcm") == MUSIC_FMT_UNKNOWN, ".pcm is a recording, not music");
  ok(musicFormatOf("beep.raw") == MUSIC_FMT_UNKNOWN, ".raw likewise");
  ok(!musicIsPlayable("/audio_230817_144844.pcm"), "a phone recording is not listed");
  ok(musicFormatOf("notes.txt") == MUSIC_FMT_UNKNOWN, "text is not audio");
  ok(musicFormatOf(NULL) == MUSIC_FMT_UNKNOWN, "null is safe");

  // The last dot wins, so a double extension is judged on the real one.
  ok(musicFormatOf("song.wav.txt") == MUSIC_FMT_UNKNOWN, "song.wav.txt is not audio");
  ok(musicFormatOf("song.mp3.bak") == MUSIC_FMT_UNKNOWN, "nor song.mp3.bak");
  ok(musicFormatOf("Bad. Song.wav") == MUSIC_FMT_WAV, "a dot in the name is fine");

  // A dot in a DIRECTORY must not be read as the file's extension.
  ok(musicFormatOf("/my.music/song") == MUSIC_FMT_UNKNOWN, "dot in a directory is not an extension");
  ok(musicFormatOf("/my.music/song.wav") == MUSIC_FMT_WAV, "extension found past a dotted directory");

  ok(!musicIsPlayable(".wav"), "a leading dot is a hidden file, not an extension");
  ok(!musicIsPlayable("/music/._Track.wav"), "macOS resource forks are skipped");
  ok(musicIsPlayable("/music/Track.wav"), "a real track is playable");
  ok(!musicIsPlayable(""), "empty name");
}

static void testDisplayName() {
  group("display names");

  char b[MUSIC_NAME_MAX];
  musicDisplayName("/music/Murder In My Mind.wav", b, sizeof(b));
  ok(strcmp(b, "Murder In My Mind") == 0, "directory and extension are stripped");

  musicDisplayName("bare", b, sizeof(b));
  ok(strcmp(b, "bare") == 0, "no directory, no extension");

  // Truncation must not split a UTF-8 sequence: the reader renders these as authored.
  char small[8];
  musicDisplayName("/m/caf\xc3\xa9 society.wav", small, sizeof(small));
  ok(strlen(small) <= 7, "cut to fit");
  // Every byte kept must leave a valid sequence: no trailing continuation byte.
  ok((small[strlen(small) - 1] & 0xC0) != 0x80, "never ends mid-UTF-8");

  char one[1];
  musicDisplayName("anything.wav", one, sizeof(one));
  ok(one[0] == '\0', "a 1-byte buffer yields an empty string, not a crash");

  musicDisplayName(NULL, b, sizeof(b));
  ok(b[0] == '\0', "null path is safe");
}

static void testSort() {
  group("library order");

  MusicTrack t[4];
  memset(t, 0, sizeof(t));
  snprintf(t[0].name, MUSIC_NAME_MAX, "%s", "zebra");
  snprintf(t[1].name, MUSIC_NAME_MAX, "%s", "Apple");
  snprintf(t[2].name, MUSIC_NAME_MAX, "%s", "banana");
  snprintf(t[3].name, MUSIC_NAME_MAX, "%s", "APRICOT");
  musicSortTracks(t, 4);

  ok(strcmp(t[0].name, "Apple") == 0, "case-insensitive: Apple first");
  ok(strcmp(t[1].name, "APRICOT") == 0, "APRICOT second");
  ok(strcmp(t[2].name, "banana") == 0, "banana third");
  ok(strcmp(t[3].name, "zebra") == 0, "zebra last");

  musicSortTracks(t, 0);
  musicSortTracks(NULL, 5);
  ok(true, "empty and null sorts do not crash");
}

// ───────────────────────────────────────────────────────── the queue

static void testQueueBasics() {
  group("next and previous");

  MusicQueue q;
  q.reset(4);
  ok(q.count() == 4, "four tracks");
  ok(q.current() == 0, "starts at the first");

  ok(q.advance() == 1, "advance to 1");
  ok(q.advance() == 2, "advance to 2");
  ok(q.advance() == 3, "advance to 3");
  ok(q.advance() == -1, "repeat off: runs off the end");
  ok(q.current() == -1, "and reports nothing playing");

  // Next/Prev after running off the end must come back, not stay stuck.
  ok(q.skip(-1) == 2, "previous from the end comes back");

  q.reset(4);
  ok(q.skip(-1) == 0, "repeat off: previous from the first stays put");
}

static void testQueueRepeat() {
  group("repeat");

  MusicQueue q;
  q.reset(3);
  q.setRepeat(MUSIC_REPEAT_ALL);
  q.advance();
  q.advance();
  ok(q.current() == 2, "at the last track");
  ok(q.advance() == 0, "repeat all wraps");
  ok(q.skip(-1) == 2, "and previous wraps backwards");

  q.reset(3);
  q.setRepeat(MUSIC_REPEAT_ONE);
  ok(q.advance() == 0, "repeat one replays on end-of-file");
  ok(q.advance() == 0, "again");
  /* The distinction that makes repeat-one usable: end-of-file replays, but pressing Next
   * must still move. Conflating them traps the user on one track with no way out. */
  ok(q.skip(1) == 1, "but the Next key still moves on");
}

static void testQueueShuffle() {
  group("shuffle keeps its promises");

  MusicQueue q;
  q.seed(12345);
  q.reset(8);
  q.advance();
  q.advance();
  int playing = q.current();
  ok(playing == 2, "playing track 2 in library order");

  q.setShuffle(true);
  ok(q.current() == playing, "turning shuffle ON does not change the current track");

  // Every track exactly once: a permutation, not a random walk with repeats.
  MusicQueue p;
  p.seed(999);
  p.reset(8);
  p.setShuffle(true);
  p.setRepeat(MUSIC_REPEAT_OFF);
  std::vector<int> seen;
  int cur = p.current();
  while (cur >= 0) {
    seen.push_back(cur);
    cur = p.advance();
  }
  ok(seen.size() == 8, "shuffle visits every track");
  bool all = true;
  for (int i = 0; i < 8; i++) {
    bool found = false;
    for (size_t j = 0; j < seen.size(); j++) {
      if (seen[j] == i) {
        found = true;
      }
    }
    if (!found) {
      all = false;
    }
  }
  ok(all, "and each exactly once");

  // Switching shuffle off returns to library order WITHOUT jumping tracks.
  MusicQueue r;
  r.seed(7);
  r.reset(6);
  r.setShuffle(true);
  r.advance();
  int nowPlaying = r.current();
  r.setShuffle(false);
  ok(r.current() == nowPlaying, "turning shuffle OFF does not change the current track");
  ok(r.advance() == nowPlaying + 1, "and the next track is the next in the library");

  // Deterministic for a given seed, so this suite can assert at all.
  MusicQueue a, b;
  a.seed(42);
  a.reset(10);
  a.setShuffle(true);
  b.seed(42);
  b.reset(10);
  b.setShuffle(true);
  bool same = true;
  for (int i = 0; i < 10; i++) {
    if (a.current() != b.current()) {
      same = false;
    }
    a.advance();
    b.advance();
  }
  ok(same, "same seed, same order");
}

static void testQueueStartAt() {
  group("tapping a row");

  MusicQueue q;
  q.reset(5);
  q.startAt(3);
  ok(q.current() == 3, "plays the row that was tapped");
  ok(q.advance() == 4, "and continues from there");

  q.reset(5);
  q.setShuffle(true);
  q.seed(3);
  q.startAt(2);
  ok(q.current() == 2, "in shuffle too, the tapped row plays first");

  /* A fresh queue for the degenerate cases: `q` above has shuffle on, and after a reset
   * the first track in a shuffled order is not library index 0 — asserting that it is
   * tests the seed, not the behaviour. */
  MusicQueue e;
  e.reset(0);
  ok(e.current() == -1, "an empty library plays nothing");
  ok(e.advance() == -1, "and advancing it is harmless");
  e.startAt(0);
  ok(e.current() == -1, "as is tapping it");

  e.reset(3);
  e.advance();
  int before = e.current();
  e.startAt(99);
  ok(e.current() == before, "an out-of-range row leaves the current track alone");
  e.startAt(-1);
  ok(e.current() == before, "so does a negative one");
}

// ───────────────────────────────────────────────────────── WAV headers

// Builds a WAV header so the tests exercise the real chunk walker.
struct WavBuilder {
  std::vector<uint8_t> b;

  void u32(uint32_t v) {
    b.push_back(v & 0xFF);
    b.push_back((v >> 8) & 0xFF);
    b.push_back((v >> 16) & 0xFF);
    b.push_back((v >> 24) & 0xFF);
  }
  void u16(uint16_t v) {
    b.push_back(v & 0xFF);
    b.push_back((v >> 8) & 0xFF);
  }
  void tag(const char* t) {
    for (int i = 0; i < 4; i++) {
      b.push_back((uint8_t)t[i]);
    }
  }

  void riff() {
    tag("RIFF");
    u32(0);
    tag("WAVE");
  }
  void fmt(uint16_t format, uint16_t ch, uint32_t rate, uint16_t bits) {
    tag("fmt ");
    u32(16);
    u16(format);
    u16(ch);
    u32(rate);
    u32(rate * ch * (bits / 8));
    u16((uint16_t)(ch * (bits / 8)));
    u16(bits);
  }
  // An arbitrary chunk, used to prove the walker steps over what it does not know.
  void junk(const char* t, uint32_t n) {
    tag(t);
    u32(n);
    for (uint32_t i = 0; i < n; i++) {
      b.push_back(0xAB);
    }
    if (n & 1u) {
      b.push_back(0);   // the pad byte that word-aligns the next chunk
    }
  }
  void data(uint32_t n) {
    tag("data");
    u32(n);
  }
};

static void testWavHeader() {
  group("reading a WAV header");

  WavInfo w;
  {
    WavBuilder v;
    v.riff();
    v.fmt(1, 1, 16000, 16);
    v.data(320);
    ok(wavParseHeader(v.b.data(), v.b.size(), &w), "mono 16-bit 16 kHz parses");
    ok(w.sampleRate == 16000 && w.channels == 1 && w.bits == 16, "fields are right");
    ok(w.dataOffset == v.b.size(), "data offset points past the header");
    ok(w.dataBytes == 320, "data length read");
  }
  {
    WavBuilder v;
    v.riff();
    v.fmt(1, 2, 44100, 16);
    v.data(1000);
    ok(wavParseHeader(v.b.data(), v.b.size(), &w) && w.channels == 2, "stereo 44.1 parses");
  }
  {
    /* The classic chunk-walker bug: an ODD-sized chunk is followed by a pad byte, and a
     * walker that forgets it reads the next tag one byte off and calls a good file
     * broken. LIST chunks in the wild are routinely odd. */
    WavBuilder v;
    v.riff();
    v.fmt(1, 1, 22050, 16);
    v.junk("LIST", 7);
    v.data(64);
    ok(wavParseHeader(v.b.data(), v.b.size(), &w), "an odd-sized chunk before data is stepped over");
    ok(w.sampleRate == 22050, "and the format still reads correctly");
  }
  {
    WavBuilder v;
    v.riff();
    v.fmt(1, 1, 8000, 8);
    v.data(10);
    ok(wavParseHeader(v.b.data(), v.b.size(), &w) && w.bits == 8, "8-bit parses");
  }
  {
    // WAVE_FORMAT_EXTENSIBLE hides the real tag in a GUID at the end of the chunk.
    WavBuilder v;
    v.riff();
    v.tag("fmt ");
    v.u32(40);
    v.u16(0xFFFE);
    v.u16(2);
    v.u32(48000);
    v.u32(48000 * 4);
    v.u16(4);
    v.u16(16);
    v.u16(22);
    v.u16(16);
    v.u32(3);
    v.u16(1);              // the real format tag, PCM
    v.u16(0);
    for (int i = 0; i < 12; i++) {
      v.b.push_back(0);
    }
    v.data(8);
    ok(wavParseHeader(v.b.data(), v.b.size(), &w), "EXTENSIBLE resolving to PCM is accepted");
  }
}

static void testWavRejects() {
  group("what it refuses, and says why");

  WavInfo w;
  {
    WavBuilder v;
    v.riff();
    v.fmt(3, 1, 44100, 32);        // IEEE float
    v.data(8);
    ok(!wavParseHeader(v.b.data(), v.b.size(), &w), "float WAV refused");
    ok(w.problem && strstr(w.problem, "16-bit"), "and says what to do about it");
  }
  {
    WavBuilder v;
    v.riff();
    v.fmt(2, 1, 44100, 4);         // MS ADPCM
    v.data(8);
    ok(!wavParseHeader(v.b.data(), v.b.size(), &w), "compressed WAV refused");
    ok(w.problem && strstr(w.problem, "PCM"), "and names PCM");
  }
  {
    WavBuilder v;
    v.riff();
    v.fmt(1, 6, 44100, 16);        // 5.1
    v.data(8);
    ok(!wavParseHeader(v.b.data(), v.b.size(), &w), "six channels refused");
  }
  {
    const uint8_t notWav[] = { 'O', 'g', 'g', 'S', 0, 0, 0, 0, 0, 0, 0, 0 };
    ok(!wavParseHeader(notWav, sizeof(notWav), &w), "a non-RIFF file is refused");
  }

  /* Truncation at EVERY length. The uploader has no extension filter, so a half-uploaded
   * or hostile file is an ordinary input; under ASan this is the check that it never
   * reads off the end. */
  WavBuilder full;
  full.riff();
  full.fmt(1, 2, 44100, 16);
  full.junk("LIST", 7);
  full.data(1024);
  bool safe = true;
  for (size_t n = 0; n <= full.b.size(); n++) {
    WavInfo t;
    bool parsed = wavParseHeader(full.b.data(), n, &t);
    if (parsed && n < full.b.size()) {
      safe = false;      // it must not claim success before the data chunk is present
    }
  }
  ok(safe, "no prefix of a valid file parses as complete");
  ok(wavParseHeader(full.b.data(), full.b.size(), &w), "and the whole file still parses");
  ok(!wavParseHeader(NULL, 100, &w), "null buffer is safe");
}

// ───────────────────────────────────────────────────────── conversion

// Little-endian 16-bit frames.
static void put16(std::vector<uint8_t>& v, int16_t s) {
  v.push_back((uint8_t)(s & 0xFF));
  v.push_back((uint8_t)((s >> 8) & 0xFF));
}

static void testConvert() {
  group("getting samples to the speaker");

  {
    WavInfo in;
    memset(&in, 0, sizeof(in));
    in.sampleRate = 44100;
    in.channels = 2;
    in.bits = 16;
    WavConverter c;

    c.begin(in, 44100, true);
    ok(c.passthrough(), "a 44.1 stereo file into stereo out is a straight copy");
    ok(c.outChannels() == 2, "two channels out");

    c.begin(in, 44100, false);
    ok(!c.passthrough(), "the same file into the earpiece needs the downmix");
    ok(c.outChannels() == 1, "one channel out");

    in.channels = 1;
    c.begin(in, 44100, true);
    ok(!c.passthrough(), "a mono file into stereo out has to be duplicated");

    c.begin(in, 22050, true);
    ok(!c.passthrough(), "a rate change always needs the resampler");
  }

  {
    // Stereo in, stereo out: the channels must stay separate and in the right order.
    WavInfo in;
    memset(&in, 0, sizeof(in));
    in.sampleRate = 44100;
    in.channels = 2;
    in.bits = 16;

    std::vector<uint8_t> src;
    for (int i = 0; i < 8; i++) {
      put16(src, (int16_t)1000);     // L
      put16(src, (int16_t)-2000);    // R
    }
    WavConverter c;
    c.begin(in, 44100, true);
    int16_t out[32];
    size_t used = 0;
    size_t n = c.feed(src.data(), 8, out, 16, &used);
    ok(n > 0, "produces frames");
    bool kept = true;
    for (size_t i = 0; i < n; i++) {
      if (out[i * 2] != 1000 || out[i * 2 + 1] != -2000) {
        kept = false;
      }
    }
    ok(kept, "left stays left and right stays right");
  }

  {
    // A MONO file through headphones must be heard in both ears, not just the left.
    WavInfo in;
    memset(&in, 0, sizeof(in));
    in.sampleRate = 44100;
    in.channels = 1;
    in.bits = 16;

    std::vector<uint8_t> src;
    for (int i = 0; i < 8; i++) {
      put16(src, (int16_t)1234);
    }
    WavConverter c;
    c.begin(in, 44100, true);
    int16_t out[32];
    size_t used = 0;
    size_t n = c.feed(src.data(), 8, out, 16, &used);
    bool both = n > 0;
    for (size_t i = 0; i < n; i++) {
      if (out[i * 2] != 1234 || out[i * 2 + 1] != 1234) {
        both = false;
      }
    }
    ok(both, "a mono source is duplicated to both ears");
  }

  {
    // Stereo source into the earpiece averages, rather than dropping the right channel —
    // otherwise anything mixed hard-right disappears.
    WavInfo in;
    memset(&in, 0, sizeof(in));
    in.sampleRate = 16000;
    in.channels = 2;
    in.bits = 16;

    std::vector<uint8_t> src;
    for (int i = 0; i < 8; i++) {
      put16(src, (int16_t)0);        // L silent
      put16(src, (int16_t)2000);     // R carries the part
    }
    WavConverter c;
    c.begin(in, 16000, false);
    int16_t out[16];
    size_t used = 0;
    size_t n = c.feed(src.data(), 8, out, 16, &used);
    bool heard = n > 0;
    for (size_t i = 0; i < n; i++) {
      if (out[i] != 1000) {
        heard = false;
      }
    }
    ok(heard, "a hard-right part is still audible in mono");
  }

  {
    /* 2:1 decimation. This is the assertion that caught the priming bug: with the
     * interpolator sitting between the wrong pair of samples the output is still smooth
     * and still the right LENGTH, but it is the wrong samples — the file plays at the
     * wrong speed and nothing else shows it. */
    WavInfo in;
    memset(&in, 0, sizeof(in));
    in.sampleRate = 32000;
    in.channels = 1;
    in.bits = 16;

    std::vector<uint8_t> src;
    for (int i = 0; i < 8; i++) {
      put16(src, (int16_t)(i * 100));    // 0,100,200,...,700
    }
    WavConverter c;
    c.begin(in, 16000, false);
    int16_t out[8];
    size_t used = 0;
    size_t n = c.feed(src.data(), 8, out, 8, &used);
    ok(n == 4, "eight frames at 2:1 give four frames");
    ok(out[0] == 0 && out[1] == 200 && out[2] == 400 && out[3] == 600,
       "and they are every OTHER sample, not the first four");
  }

  {
    // Resumability: the file is read in blocks that do not divide evenly into frames, so
    // feeding in two pieces must equal feeding in one.
    WavInfo in;
    memset(&in, 0, sizeof(in));
    in.sampleRate = 32000;
    in.channels = 1;
    in.bits = 16;

    std::vector<uint8_t> src;
    for (int i = 0; i < 16; i++) {
      put16(src, (int16_t)(i * 100));
    }

    int16_t whole[16];
    size_t usedW = 0;
    WavConverter a;
    a.begin(in, 16000, false);
    size_t nW = a.feed(src.data(), 16, whole, 16, &usedW);

    int16_t split[16];
    size_t used1 = 0, used2 = 0;
    WavConverter b;
    b.begin(in, 16000, false);
    size_t n1 = b.feed(src.data(), 5, split, 16, &used1);
    size_t n2 = b.feed(src.data() + used1 * 2, 16 - used1, split + n1, 16 - n1, &used2);

    ok(n1 + n2 == nW, "same number of samples either way");
    ok(memcmp(whole, split, nW * sizeof(int16_t)) == 0, "and the same samples");
  }

  {
    // 8-bit WAV is unsigned; 128 is silence, not full scale.
    WavInfo in;
    memset(&in, 0, sizeof(in));
    in.sampleRate = 16000;
    in.channels = 1;
    in.bits = 8;

    uint8_t src[4] = { 128, 128, 128, 128 };
    WavConverter c;
    c.begin(in, 16000, false);
    int16_t out[4];
    size_t used = 0;
    size_t n = c.feed(src, 4, out, 4, &used);
    bool silent = n > 0;
    for (size_t i = 0; i < n; i++) {
      if (out[i] != 0) {
        silent = false;
      }
    }
    ok(silent, "8-bit 128 is silence, not a DC offset");
  }

  {
    // Degenerate inputs must not read off the end.
    WavInfo in;
    memset(&in, 0, sizeof(in));
    in.sampleRate = 16000;
    in.channels = 1;
    in.bits = 16;
    WavConverter c;
    c.begin(in, 16000, false);

    int16_t out[4];
    size_t used = 99;
    uint8_t one[2] = { 0, 0 };
    ok(c.feed(one, 0, out, 4, &used) == 0 && used == 0, "no input, no output");
    ok(c.feed(one, 1, out, 4, &used) == 0, "one frame cannot prime a pair yet");
    ok(c.feed(NULL, 4, out, 4, &used) == 0, "null source is safe");
    ok(c.feed(one, 1, NULL, 4, &used) == 0, "null output is safe");
  }
}

int main() {
  testFormats();
  testDisplayName();
  testSort();
  testQueueBasics();
  testQueueRepeat();
  testQueueShuffle();
  testQueueStartAt();
  testWavHeader();
  testWavRejects();
  testConvert();

  printf("\n%s%d passed, %d failed\033[0m\n",
         g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}

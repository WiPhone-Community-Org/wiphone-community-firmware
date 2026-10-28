/*
 * test_chunk.cpp — the chunked-upload protocol core (chunk_proto.h).
 *
 * The server half of the 2026-08-20 upload redesign is thin on purpose: every
 * decision that can be wrong lives in chunk_proto.h where this suite can reach
 * it. The properties under test are the ones the page's JS RELIES on:
 *
 *  - the held size only moves by whole committed pieces, so a piece is either
 *    exactly at the boundary (append), wholly behind it (duplicate resend —
 *    ack, never rewrite), or the client needs the truth (resync);
 *  - a filename off the network can only ever land IN the transfer directory;
 *  - the CRC here and the table CRC in the page JS are the same CRC32 (IEEE),
 *    pinned by the standard vectors, because the server refuses pieces on it.
 */
#include "../WiPhone/chunk_proto.h"

#include <stdio.h>
#include <string.h>

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
    printf("  \033[31mFAIL\033[0m [%s] %s\n", g_group, what);
  }
}

static void testDecide() {
  group("chunkDecide: the boundary rule");
  ok(chunkDecide(0, 0, 4096) == CHUNK_APPEND, "first piece of a fresh file appends");
  ok(chunkDecide(8192, 8192, 4096) == CHUNK_APPEND, "piece at the held boundary appends");
  ok(chunkDecide(8192, 4096, 4096) == CHUNK_DUPLICATE, "piece wholly behind the boundary is a duplicate");
  ok(chunkDecide(8192, 0, 4096) == CHUNK_DUPLICATE, "ancient resend is still just a duplicate");
  ok(chunkDecide(8192, 0, 8192) == CHUNK_DUPLICATE, "resend ending exactly at the boundary is a duplicate");
  ok(chunkDecide(8192, 12288, 4096) == CHUNK_RESYNC, "gap ahead of the boundary resyncs");
  ok(chunkDecide(8192, 6144, 4096) == CHUNK_RESYNC, "piece straddling the boundary resyncs (no partial trust)");
  ok(chunkDecide(8192, 8192, 0) == CHUNK_BAD, "empty piece is malformed");
  ok(chunkDecide(0, 0, 0) == CHUNK_BAD, "empty piece on an empty file is still malformed");
  group("chunkDecide: hostile arithmetic");
  const size_t MAX = (size_t)-1;
  ok(chunkDecide(8192, MAX, 4096) == CHUNK_RESYNC, "off+len overflow cannot fake a duplicate");
  ok(chunkDecide(8192, MAX - 4095, 4096) == CHUNK_RESYNC, "wrap to exactly 0 is caught by the overflow guard");
  ok(chunkDecide(MAX, MAX - 4096, 4096) == CHUNK_DUPLICATE, "a piece ending exactly at a SIZE_MAX file is honest arithmetic, not a wrap");
}

static void testSafeTreeName() {
  group("chunkSafeTreeName: what a map-tile upload may name a file");
  char out[64];
  ok(chunkSafeTreeName("home/15/5249/11443.565", out, sizeof(out)) && !strcmp(out, "home/15/5249/11443.565"),
     "a tile path passes through whole");
  ok(chunkSafeTreeName("pins.txt", out, sizeof(out)) && !strcmp(out, "pins.txt"),
     "a plain name passes through");
  ok(chunkSafeTreeName("hunt-2026_a/11/1/2.565", out, sizeof(out)), "dash and underscore are fine in a folder");
  ok(!chunkSafeTreeName("../x.565", out, sizeof(out)), "dot-dot is refused");
  ok(!chunkSafeTreeName("home/15/../x.565", out, sizeof(out)), "dot-dot in the middle is refused");
  ok(!chunkSafeTreeName("home//x.565", out, sizeof(out)), "an empty segment is refused");
  ok(!chunkSafeTreeName("/home/x.565", out, sizeof(out)), "a leading slash is refused");
  ok(!chunkSafeTreeName("home/x.565/", out, sizeof(out)), "a trailing slash is refused");
  ok(!chunkSafeTreeName("a/b/c/d/e.565", out, sizeof(out)), "five segments is too deep");
  ok(chunkSafeTreeName("a/b/c/d.565", out, sizeof(out)), "four segments is the limit");
  ok(!chunkSafeTreeName("home/.hidden", out, sizeof(out)), "a leading-dot segment is refused");
  ok(!chunkSafeTreeName("._x.565", out, sizeof(out)), "an AppleDouble sidecar is refused");
  ok(!chunkSafeTreeName("home\\15\\x.565", out, sizeof(out)), "backslashes are not separators, they are refused");
  ok(!chunkSafeTreeName("ho me/x.565", out, sizeof(out)), "a space is refused");
  ok(!chunkSafeTreeName("home.old/x.565", out, sizeof(out)), "a dot in a folder segment is refused");
  ok(!chunkSafeTreeName("x.tar.565", out, sizeof(out)), "two dots in a name is refused");
  ok(!chunkSafeTreeName("", out, sizeof(out)), "empty is refused");
  char tiny[4];
  ok(!chunkSafeTreeName("abcd", tiny, sizeof(tiny)), "too long for the buffer is refused");
  ok(chunkSafeTreeName("a234567890123456789012345678901/1/2/3.565", out, sizeof(out)),
     "a 31-character area name is the viewer's limit and passes");
  ok(!chunkSafeTreeName("a2345678901234567890123456789012/1/2/3.565", out, sizeof(out)),
     "a 32-character area name would never be listed and is refused");
}

static void testSafeName() {
  group("chunkSafeName: what the network may name a file");
  char out[64];
  ok(chunkSafeName("book.epub", out, sizeof(out)) && !strcmp(out, "book.epub"),
     "a plain name passes through");
  ok(chunkSafeName("a/b/book.epub", out, sizeof(out)) && !strcmp(out, "book.epub"),
     "a path keeps only its basename");
  ok(chunkSafeName("../../etc/passwd", out, sizeof(out)) && !strcmp(out, "passwd"),
     "traversal collapses to the basename");
  ok(chunkSafeName("..\\..\\boot.ini", out, sizeof(out)) && !strcmp(out, "boot.ini"),
     "backslash paths are paths too");
  ok(!chunkSafeName("", out, sizeof(out)), "empty is refused");
  ok(!chunkSafeName("books/", out, sizeof(out)), "a trailing slash leaves nothing");
  ok(!chunkSafeName(".", out, sizeof(out)), "dot is refused");
  ok(!chunkSafeName("..", out, sizeof(out)), "dot-dot is refused");
  /* 🛑 REVERSED 2026-09-04, deliberately. This used to assert that ".hidden" was "a
   * legitimate name". Nothing this protocol carries -- ROMs, books, photos -- ever begins
   * with a dot, and the one thing that does is the class that already cost a session: macOS
   * writes an AppleDouble sidecar `._Name.gbc` beside every real file when a folder is
   * copied, and twelve of them reached a card and showed up as duplicate ROMs (0.9.57 taught
   * the picker to hide them). Those sidecars are ~4 KB, which is also exactly the truncated
   * ROM that made gnuboy run uninitialised PSRAM. Refusing the whole leading-dot class here
   * stops them at the door instead of hiding them afterwards. */
  ok(!chunkSafeName(".hidden", out, sizeof(out)), "a leading dot is refused");
  ok(!chunkSafeName("._Pokemon.gbc", out, sizeof(out)),
     "the macOS AppleDouble sidecar is refused by name");
  ok(!chunkSafeName("roms/._Pokemon.gbc", out, sizeof(out)),
     "...and still refused when it arrives with a path in front of it");
  ok(chunkSafeName("v1.2.rom", out, sizeof(out)) && !strcmp(out, "v1.2.rom"),
     "dots elsewhere in the name are untouched");
  ok(!chunkSafeName("a\tb.epub", out, sizeof(out)), "control characters are refused");
  ok(!chunkSafeName(NULL, out, sizeof(out)), "NULL is refused");
  char longname[80];
  memset(longname, 'x', sizeof(longname));
  longname[79] = '\0';
  ok(!chunkSafeName(longname, out, sizeof(out)), "a name that cannot fit is refused, not truncated");
  char exact[63 + 1];
  memset(exact, 'y', 63);
  exact[63] = '\0';
  ok(chunkSafeName(exact, out, 64) && strlen(out) == 63, "cap-1 characters just fit");
  ok(chunkSafeName("тайга.epub", out, sizeof(out)) && !strcmp(out, "тайга.epub"),
     "UTF-8 bytes are not control characters");
}

static void testHex() {
  group("chunkParseHex32: exactly eight hex digits");
  uint32_t v = 0;
  ok(chunkParseHex32("cbf43926", &v) && v == 0xCBF43926u, "lowercase parses");
  ok(chunkParseHex32("CBF43926", &v) && v == 0xCBF43926u, "uppercase parses");
  ok(chunkParseHex32("0000002a", &v) && v == 0x2Au, "leading zeros parse");
  ok(!chunkParseHex32("2a", &v), "short input is a client bug, refused");
  ok(!chunkParseHex32("cbf439261", &v), "nine digits refused");
  ok(!chunkParseHex32("", &v), "empty refused");
  ok(!chunkParseHex32("cbf4392g", &v), "non-hex refused");
  ok(!chunkParseHex32(NULL, &v), "NULL refused");
}

static void testCrc() {
  group("CRC32: pinned to the standard, split-invariant");
  const uint8_t* nine = (const uint8_t*)"123456789";
  uint32_t c = chunkCrc32Update(chunkCrc32Init(), nine, 9);
  ok(chunkCrc32Final(c) == 0xCBF43926u, "'123456789' -> cbf43926 (the check-value every CRC32 table cites)");
  const char* fox = "The quick brown fox jumps over the lazy dog";
  c = chunkCrc32Update(chunkCrc32Init(), (const uint8_t*)fox, strlen(fox));
  ok(chunkCrc32Final(c) == 0x414FA339u, "the fox sentence -> 414fa339");
  ok(chunkCrc32Final(chunkCrc32Init()) == 0x00000000u, "empty input -> 0");
  // The server CRCs a piece across several 1436-byte UPLOAD_FILE_WRITE
  // callbacks; the split points must not matter.
  c = chunkCrc32Init();
  c = chunkCrc32Update(c, nine, 3);
  c = chunkCrc32Update(c, nine + 3, 1);
  c = chunkCrc32Update(c, nine + 4, 5);
  ok(chunkCrc32Final(c) == 0xCBF43926u, "incremental update equals one-shot");
}

int main() {
  testDecide();
  testSafeName();
  testSafeTreeName();
  testHex();
  testCrc();

  printf("\n%s%d passed, %d failed\033[0m\n", g_fail ? "\033[31m" : "\033[32m", g_pass, g_fail);
  return g_fail ? 1 : 0;
}

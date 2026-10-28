/*
 * epub_parse.h — EPUB and plain-text parsing, matching COVEY's covey_ui/epub.py.
 *
 * An EPUB is a zip of XHTML. This reads it straight off the SD card: central directory,
 * inflate (ESP32 ROM miniz on the phone, zlib in the host tests), then a deliberately dumb
 * XHTML-to-text pass. No layout, no CSS — a 240x320 panel re-wraps everything anyway.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * POSITION IS (spine, offset), NEVER A PAGE NUMBER
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * A page number is a property of THIS screen at THIS font size. COVEY's 480x800 panel and
 * the WiPhone's are not the same, so a page number means nothing across devices — which is
 * the entire problem booksync exists to solve. The portable unit is the spine index plus a
 * character offset into that spine item's extracted text, both of which come out of the
 * file's own structure.
 *
 * ⚠ The offset is only exactly comparable between readers that flatten XHTML identically.
 * Across implementations it drifts by the length of whatever markup one keeps and the other
 * drops — a few characters per paragraph, and more here than on COVEY because this parser
 * decodes a smaller set of HTML entities (see EPUB_ENTITY_NOTE in the .cpp). That drift is
 * expected and handled: booksync also carries the whole-book FRACTION, and epubLocate()
 * turns a fraction back into a position, which is what makes a jump from COVEY land
 * somewhere sane. It is also why a jump is confirmed by the reader rather than taken
 * silently.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * IDENTITY — this is what makes sync match, so it must agree with COVEY exactly
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * Three ids, best first, and ANY overlap means "same book":
 *   "id:<slug of dc:identifier>"   absent from plenty of files
 *   "ta:<slug of title|author>"    matches the same book from two different sources
 *   "fp:<content fingerprint>"     matches only a byte-identical copy
 *
 * The filename is NOT an id for an EPUB. It is for a .txt, where the title comes from the
 * filename stem and there is no dc:identifier.
 *
 * Only the FIRST id travels over LoRa, so byte-identical copies on both devices is the
 * happy path — it makes all three agree at once.
 */
#ifndef EPUB_PARSE_H
#define EPUB_PARSE_H

#include <stddef.h>
#include <stdint.h>

#define EPUB_NAME_MAX      192      // a zip entry path
#define EPUB_META_MAX      512      // 120 characters of UTF-8 metadata, with room
#define EPUB_ID_MAX        72       // "ta:" + a 64-character slug
#define EPUB_CH_TITLE_MAX  84       // COVEY caps chapter titles at 80 characters
#define EPUB_MAX_SPINE     512
#define EPUB_MAX_DOC       (512 * 1024)   // per spine document, decompressed (COVEY: 4 MB)

/* Random-access bytes. Backed by an SD File on the phone and by a FILE* in the tests, which
 * is what lets the same parser be proven on a Mac. */
struct EpubSource {
  void*    ctx;
  uint64_t size;
  size_t (*read)(void* ctx, uint64_t off, void* buf, size_t len);
};

struct EpubSpineItem {
  char name[EPUB_NAME_MAX];              // zip entry path, already resolved against the OPF
  char title[EPUB_CH_TITLE_MAX];         // "" when the book has no usable table of contents
};

/* ══════════════════════════════════════════════════════════════════════════════════
 * KOSync — the SECOND spine, which exists only to talk percentages with other readers
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * KOSync (KOReader's sync protocol, which the Xteink X4's CrossPoint firmware and COVEY also
 * speak) moves a position as ONE number: a whole-book percentage. The number the X4 sends is
 * CrossPoint's, BYTE-weighted over CrossPoint's idea of the spine — and that is not this
 * reader's spine. CrossPoint counts every <itemref> naming a manifest item, linear="no"
 * covers and images included, looks each one up by its %XX-DECODED path, and weighs it by
 * its uncompressed size in the zip. The reading spine above skips all three kinds, so the
 * two lists differ in length and in order of magnitude (a 200 KB map image in the spine is
 * two thirds of one fixture's "book" to CrossPoint and nothing at all to us).
 *
 * 🛑 THIS MAP IS ADDITIVE. It never feeds the reading spine, epubFraction/epubLocate, the
 * position store or the LoRa record — those stay spine-EQUAL and byte-for-byte what they
 * were, which is what keeps every saved place where it was across the update (proven by
 * tests/golden_positions.h). A KOSync percentage is made from (spine, offset, chapter
 * length) on the way OUT and turned back into (spine, within) on the way IN, and nowhere
 * else. The rules are pinned by tools/gen_kosync_vectors.py; every number in
 * tests/vectors_kosync.h is reproduced by tests/test_kosync.cpp.
 *
 * Sizes come from the central-directory walk epubOpen already makes to learn which chapters
 * exist, so the map costs ZERO extra card reads. When that walk does not finish, sizes are
 * unknown (sizesKnown=false) and the book is simply not syncable over KOSync — never a
 * guessed total. */
#define EPUB_KOSYNC_ID_CHARS 33          // 32 lower-case hex + NUL

/* Why a book is NOT syncable even though it reads (EpubKosyncMap::refused). Each is a case
 * where every number this phone sent would differ from the X4's, silently — so the book says
 * so instead (epubKosyncWhyNot) and sends nothing. */
enum EpubKosyncRefusal {
  EPUB_KOSYNC_SYNCABLE = 0,
  EPUB_KOSYNC_TOO_MANY_ITEMS,            // the OPF ran past EPUB_MAX_SPINE items or itemrefs
  EPUB_KOSYNC_TEXT_TOO_BIG,              // a .txt bigger than the reader shows (EPUB_MAX_DOC)
};

struct EpubKosyncMap {
  int      nCp;                          // CrossPoint spine items
  int      nRead;                        // the reading spine (EpubBook.nSpine)
  bool     sizesKnown;                   // false: the zip walk failed, do not sync
  uint8_t  refused;                      // EpubKosyncRefusal: not syncable, and why
  uint32_t cum[EPUB_MAX_SPINE];          // cum[i] = size[0] + ... + size[i], CrossPoint order
  int16_t  cpToRead[EPUB_MAX_SPINE];     // reading index with the same path (first), or -1
  int16_t  readToCp[EPUB_MAX_SPINE];     // FIRST CrossPoint item with that path, or -1
};

struct EpubBook {
  EpubSource*     src;
  bool            isText;                // a plain .txt "book": one spine item, no zip
  char            title[EPUB_META_MAX];
  char            author[EPUB_META_MAX];
  char            identifier[EPUB_META_MAX];
  EpubSpineItem*  spine;
  int             nSpine;
  char            fingerprint[17];       // 16 hex characters, or "" if unreadable
  EpubKosyncMap*  kosync;                // PSRAM; NULL if it could not be allocated
};

enum EpubStatus {
  EPUB_OK = 0,
  EPUB_ERR_NOT_ZIP,
  EPUB_ERR_NO_CONTAINER,
  EPUB_ERR_NO_ROOTFILE,
  EPUB_ERR_NO_OPF,
  EPUB_ERR_NO_CHAPTERS,
  EPUB_ERR_MEMORY,
};

/* Open `src` as a book. `displayName` is the file's basename, used for the title of a .txt
 * and as the fallback title of an EPUB with no dc:title — pass it, not a path.
 * `isTextFile` selects the plain-text path (caller checks the extension). */
EpubStatus epubOpen(EpubBook* b, EpubSource* src, const char* displayName, bool isTextFile);
void       epubClose(EpubBook* b);
const char* epubStatusText(EpubStatus s);

/* The ids another device might match this book by, best first. Writes up to 3 into `out`,
 * returns how many. Each buffer must be EPUB_ID_MAX. */
int epubIds(const EpubBook* b, char out[3][EPUB_ID_MAX]);

// Extracted text of spine item `i`. Writes into `buf`; returns the length, or 0.
// Never fails loudly: an unreadable chapter yields a short placeholder, as on COVEY.
size_t epubChapterText(EpubBook* b, int i, char* buf, size_t cap);

#define EPUB_MAX_IMAGES  12       // pictures reported per chapter

/* A picture in a chapter, and where it belongs in the reading flow.
 *
 * ⚠ `off` is a byte offset into the EXTRACTED TEXT, and finding it changes that text by
 * exactly nothing. `img` is neither a block tag nor a dropped one in COVEY's extractor or in
 * this one, so it emits no characters and forces no paragraph break; the offset is simply the
 * output length already committed when the tag was passed. That matters more than it looks:
 * a reading position IS a byte offset into this text, so an extractor that moved by even one
 * byte would silently shift every position after the first picture — and COVEY, which has no
 * pictures at all, would still have to agree with us byte for byte. */
struct EpubImage {
  uint32_t off;                   // byte offset into the chapter's extracted text
  char     name[EPUB_NAME_MAX];   // zip entry, resolved against the chapter's own directory
  uint16_t w, h;                  // pixels, filled in by the caller via epubImageSize()
};

/* epubChapterText, plus the chapter's pictures in reading order. Pass imgs=NULL for exactly
 * the old behaviour. `nImgs` is capped at `maxImgs`; the text is never affected by either. */
size_t epubChapterTextImages(EpubBook* b, int i, char* buf, size_t cap,
                             EpubImage* imgs, int maxImgs, int* nImgs);

// Read a whole zip member (a picture) into `buf`. Returns the byte count, or 0.
size_t epubReadEntry(EpubBook* b, const char* name, void* buf, size_t cap);

/* Just the first `cap` bytes of a member — enough for epubImageSize(). Only a prefix of the
 * compressed data is touched, so sizing a 1 MB cover costs kilobytes, not megabytes. */
size_t epubReadEntryPrefix(EpubBook* b, const char* name, void* buf, size_t cap);

// Uncompressed size of a zip member without reading it — for sizing a buffer.
size_t epubEntrySize(EpubBook* b, const char* name);

// Pixel size from a JPEG or PNG header. False if it is neither, or is truncated.
bool epubImageSize(const void* data, size_t len, uint16_t* w, uint16_t* h);

/* The same, plus how many colour components the JPEG has (0 for a PNG).
 *
 * ⚠ Worth knowing because **1 means greyscale, and the ESP32's ROM TJpgDec refuses those** —
 * it decodes 3-component YCbCr only. In the book this was written against, 33 of 45 pictures
 * are greyscale, so "cannot show this picture" is the common case and not the odd one. A
 * caller that knows the component count can say WHY instead of showing a blank box. */
bool epubImageInfo(const void* data, size_t len, uint16_t* w, uint16_t* h, uint8_t* comps);

// Length of a chapter's extracted text without keeping it — for fraction()/locate().
size_t epubChapterLen(EpubBook* b, int i);

// 0.0-1.0 through the whole book. Costs ONE chapter extraction, not the whole book.
double epubFraction(EpubBook* b, int spine, uint32_t offset);

// Inverse of epubFraction: the (spine, offset) nearest a whole-book fraction. This is what
// makes a position from a foreign reader land somewhere sane.
void epubLocate(EpubBook* b, double fraction, int* spineOut, uint32_t* offsetOut);

// The chapter's title, or "Chapter N".
const char* epubChapterTitle(const EpubBook* b, int i, char* buf, size_t cap);

// ---------------------------------------------------------------- KOSync (see the map above)
/* KOReader's partial MD5, the KOSync document id every "binary" matcher uses: 1024-byte
 * chunks at offset 0 and then at 1024 << (2*i), i = 0..10, a chunk starting at or past EOF
 * skipped, a short last one hashed as it is. At most 12 small card reads. Lower-case hex. */
bool epubKosyncPartialMd5(EpubSource* src, char out[EPUB_KOSYNC_ID_CHARS]);
/* CrossPoint's DEFAULT matcher ("filename"): MD5 of the file's base name, extension
 * included. Pass the basename, not a path. */
void epubKosyncFilenameMd5(const char* basename, char out[EPUB_KOSYNC_ID_CHARS]);
/* Both at once. False when the partial MD5 could not be read (`partial` is then "", and the
 * file-name id alone still matches a CrossPoint left on its default). */
bool epubKosyncIds(EpubSource* src, const char* basename,
                   char partial[EPUB_KOSYNC_ID_CHARS], char byName[EPUB_KOSYNC_ID_CHARS]);

// Sum of every CrossPoint item's size. 0 = "this book cannot sync over KOSync".
uint32_t epubKosyncTotal(const EpubKosyncMap* m);
/* Why epubKosyncTotal() is 0, in words for a screen note ("" when the book is syncable). */
const char* epubKosyncWhyNot(const EpubKosyncMap* m);

/* The KOSync percentage for reading-spine item `readIdx`, `within` (0..1) of the way
 * through it. False when the book is not syncable (no map, sizes unknown, total 0) or this
 * chapter has no CrossPoint counterpart — a caller says so rather than sending a guess. */
bool epubKosyncPercent(const EpubKosyncMap* m, int readIdx, double within, double* pct);

/* The inverse: which reading chapter, how far through it. A percentage that lands in an item
 * we do not read (a linear="no" cover, a spine image) resolves to the NEXT reading chapter at
 * its start, or to the end of the last one. `cpIdx`/`cpWithin` (either may be NULL) report
 * the CrossPoint item it fell in, for the tests. */
bool epubKosyncLocate(const EpubKosyncMap* m, double pct, int* readIdx, double* readWithin,
                      int* cpIdx, double* cpWithin);

/* From a reader position: within = offset / chapLen (clamped). A chapter bigger than
 * EPUB_MAX_DOC shows only a placeholder here, so its within is 0 — the offset means
 * nothing. Pure arithmetic: pass the chapter length already in hand, no extraction. */
bool epubKosyncPercentAt(const EpubBook* b, int spine, uint32_t offset, size_t chapLen,
                         double* pct);

/* To a reader position: one chapter extraction (epubChapterLen), exactly what epubLocate
 * already pays. */
bool epubKosyncLocateOffset(EpubBook* b, double pct, int* spine, uint32_t* offset);

#if !defined(ARDUINO)
/* HOST TESTS ONLY: non-zero makes every KOSync allocation fail, to prove a book still opens
 * — identically — when the optional KOSync blocks cannot be had. Not in the firmware. */
extern int epubTestFailKosyncAllocs;
#endif

// CrossPoint's path rules for ITS spine only (FsHelpers): %XX decoded (either case) ...
size_t epubKosyncDecodePath(const char* in, char* out, size_t cap);
// ... then normalised: empty components dropped, '..' pops, '.' KEPT, no leading '/'.
size_t epubKosyncNormPath(const char* in, char* out, size_t cap);

// ---------------------------------------------------------------- exposed for tests
// COVEY's _slug: lowercase, runs of non-[a-z0-9] become '-', trimmed, cut to 64 characters.
size_t epubSlug(const char* in, char* out, size_t cap);

// COVEY's _fingerprint: sha1(str(size) + first 64 KB + last 64 KB), first 16 hex characters.
bool epubFingerprint(EpubSource* src, char out[17]);

// COVEY's _TextExtractor: XHTML in, paragraphs joined by blank lines out.
size_t epubExtractText(const char* html, size_t htmlLen, char* out, size_t cap);

// COVEY's _norm: resolve `href` against the OPF's directory (the trap that yields a book
// with zero chapters and no error when it is skipped).
size_t epubNormPath(const char* base, const char* href, char* out, size_t cap);

#endif // EPUB_PARSE_H

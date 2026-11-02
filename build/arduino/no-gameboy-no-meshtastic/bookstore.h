/*
 * bookstore.h — "where am I in every book", persisted.
 *
 * The reader's memory. Mirrors the semantics of COVEY's booksync.PositionStore; the on-disk
 * format is local and need not match COVEY's (only the WIRE has to agree — see booksync.h).
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * WHY MATCHING IS BY ID-OVERLAP AND NOT BY KEY
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * A book has up to three ids (dc:identifier, title|author, content fingerprint). Which ones
 * exist depends on the file, and a position can arrive from a peer carrying only ONE of them
 * — a mesh packet only has room for the first. So a lookup has to succeed on ANY overlap,
 * not just on the primary key, or a position synced from COVEY would land in a second entry
 * and the same book would quietly hold two different places.
 *
 * put() therefore also DROPS any other entry sharing an id. Without that, opening the same
 * book through two different id paths forks it permanently.
 *
 * ══════════════════════════════════════════════════════════════════════════════════
 * DURABILITY
 * ══════════════════════════════════════════════════════════════════════════════════
 *
 * The WiPhone can lose power at any instant: the rail is held by a software latch, and a flat
 * battery or a powerOff() drops it with no unmount. So the store is written whole, to a
 * temporary file, and renamed over the real one — a torn write then costs the last session's
 * position rather than every book's. Saving is explicit (saveIfDirty) so a page turn does not
 * mean an SD write; call it on leaving the reader and on a low-battery shutdown.
 */
#ifndef BOOKSTORE_H
#define BOOKSTORE_H

#include <stddef.h>
#include <stdint.h>
#include "booksync.h"

#define BOOKSTORE_MAX_BOOKS  48
#define BOOKSTORE_BLOB_MAX   16384      // whole-file buffer; ~260 bytes per book
#define BOOKSTORE_MAGIC      "CBSTORE1"

struct BookPos {
  char     ids[BOOKSYNC_MAX_IDS][BOOKSYNC_ID_MAX];
  uint8_t  nIds;
  uint32_t spine;
  uint32_t offset;
  double   fraction;
  uint32_t turnedAt;                    // unix time of the last page turn
};

/* Whole-file load and atomic whole-file store. Backed by SD on the phone and by stdio in the
 * tests, which is what lets the matching and persistence logic be proven on a Mac. */
struct BookStoreIo {
  void* ctx;
  // Read the whole file into buf. Returns false if it does not exist or does not fit.
  bool (*load)(void* ctx, char* buf, size_t cap, size_t* outLen);
  // Write the whole file, atomically (temp + rename). Returns false on any failure.
  bool (*store)(void* ctx, const char* buf, size_t len);
};

struct BookStore {
  BookPos books[BOOKSTORE_MAX_BOOKS];
  int     count;
  bool    dirty;

  void init();
  bool load(BookStoreIo* io);           // false = nothing loaded (fresh store, not an error)
  bool saveIfDirty(BookStoreIo* io);
  bool save(BookStoreIo* io);

  // The stored position for ANY of `ids`, or false.
  bool get(const char* const* ids, int nIds, BookPos* out) const;

  /* Record a position. Any other entry sharing an id is dropped first, so one book cannot
   * end up with two diverging places. Evicts the oldest entry when full. */
  void put(const char* const* ids, int nIds, uint32_t spine, uint32_t offset,
           double fraction, uint32_t turnedAt);

  bool remove(const char* const* ids, int nIds);

  // Serialise / parse, exposed so the format itself is testable without an SD card.
  size_t serialise(char* buf, size_t cap) const;
  bool   parse(const char* buf, size_t len);
};

#endif // BOOKSTORE_H

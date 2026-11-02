/*
 * bookstore.cpp — see bookstore.h.
 *
 * Format, one record per line after a magic line:
 *
 *   CBSTORE1
 *   <spine>\t<offset>\t<fraction 6dp>\t<turnedAt>\t<id>[,<id>[,<id>]]
 *
 * Text rather than a packed struct on purpose: this file lives on a removable SD card that
 * a human may well look at, a truncated line can be skipped instead of poisoning everything
 * after it, and adding a field later does not silently reinterpret old bytes. It is also
 * what makes the round-trip testable without an SD card at all.
 */
#include "bookstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ⚠ The whole-file buffer comes from PSRAM on the phone.
 *
 * It is BOOKSTORE_BLOB_MAX — 16 KB — and the WiPhone's INTERNAL heap runs at about 15 KB free
 * with a book open, its largest single block around 12 KB. A plain malloc of 16 KB therefore
 * FAILS, and both load() and save() then return false without a word: the reading position
 * could never be written to the card and could never be read back.
 *
 * The symptom was a place that survived closing and reopening a book — the store was still in
 * RAM — and was gone after a power cycle, with Book info reporting "none saved". Nothing in
 * the store's own logic was wrong, which is why the host tests were all green throughout.
 *
 * The host build keeps plain malloc; that is also what lets this file be tested off-device. */
#if defined(ARDUINO)
#include "Arduino.h"
static void* bsAlloc(size_t n) {
  void* p = ps_malloc(n);
  return p ? p : malloc(n);
}
#else
static void* bsAlloc(size_t n) {
  return malloc(n);
}
#endif

void BookStore::init() {
  count = 0;
  dirty = false;
}

// ---------------------------------------------------------------- matching
static bool idsOverlap(const BookPos* p, const char* const* ids, int nIds) {
  for (int i = 0; i < p->nIds; i++) {
    if (!p->ids[i][0]) {
      continue;
    }
    for (int j = 0; j < nIds; j++) {
      if (ids[j] && ids[j][0] && strcmp(p->ids[i], ids[j]) == 0) {
        return true;
      }
    }
  }
  return false;
}

bool BookStore::get(const char* const* ids, int nIds, BookPos* out) const {
  for (int i = 0; i < count; i++) {
    if (idsOverlap(&books[i], ids, nIds)) {
      if (out) {
        *out = books[i];
      }
      return true;
    }
  }
  return false;
}

void BookStore::put(const char* const* ids, int nIds, uint32_t spine, uint32_t offset,
                    double fraction, uint32_t turnedAt) {
  // Drop EVERY entry sharing an id, not just the first. A book reachable by two id paths
  // must not be able to hold two positions — that is the bug this loop exists to prevent.
  int w = 0;
  for (int i = 0; i < count; i++) {
    if (!idsOverlap(&books[i], ids, nIds)) {
      if (w != i) {
        books[w] = books[i];
      }
      w++;
    }
  }
  count = w;

  if (count >= BOOKSTORE_MAX_BOOKS) {
    /* Full: evict the book SAVED longest ago — books[0], because every put() takes its book
     * out and appends it last, and the file keeps that order. Losing the place in the book you
     * have not opened in longest is the least bad option available.
     * 🛑 NOT the smallest turnedAt. That stamp is 0 for every save made with no trusted clock
     * (the woods; phone 1 has no GPS), so a book read last night lost its place to one stamped
     * a year ago the moment a 49th book was opened (review, 2026-09-25). Order needs no clock. */
    for (int i = 0; i + 1 < count; i++) {
      books[i] = books[i + 1];
    }
    count--;
  }

  BookPos* p = &books[count++];
  memset(p, 0, sizeof(*p));
  int n = nIds > BOOKSYNC_MAX_IDS ? BOOKSYNC_MAX_IDS : nIds;
  int kept = 0;
  for (int i = 0; i < n; i++) {
    if (!ids[i] || !ids[i][0]) {
      continue;
    }
    snprintf(p->ids[kept], BOOKSYNC_ID_MAX, "%s", ids[i]);
    kept++;
  }
  p->nIds = (uint8_t)kept;
  p->spine = spine;
  p->offset = offset;
  p->fraction = fraction;
  p->turnedAt = turnedAt;
  dirty = true;
}

bool BookStore::remove(const char* const* ids, int nIds) {
  int w = 0;
  bool hit = false;
  for (int i = 0; i < count; i++) {
    if (idsOverlap(&books[i], ids, nIds)) {
      hit = true;
      continue;
    }
    if (w != i) {
      books[w] = books[i];
    }
    w++;
  }
  count = w;
  if (hit) {
    dirty = true;
  }
  return hit;
}

// ---------------------------------------------------------------- format
size_t BookStore::serialise(char* buf, size_t cap) const {
  size_t n = 0;
  int w = snprintf(buf, cap, "%s\n", BOOKSTORE_MAGIC);
  if (w < 0 || (size_t)w >= cap) {
    return 0;
  }
  n = (size_t)w;
  for (int i = 0; i < count; i++) {
    const BookPos* p = &books[i];
    char ids[BOOKSYNC_MAX_IDS * BOOKSYNC_ID_MAX];
    size_t il = 0;
    ids[0] = '\0';
    for (int k = 0; k < p->nIds; k++) {
      int iw = snprintf(ids + il, sizeof(ids) - il, "%s%s", k ? "," : "", p->ids[k]);
      if (iw < 0 || (size_t)iw >= sizeof(ids) - il) {
        break;
      }
      il += (size_t)iw;
    }
    w = snprintf(buf + n, cap - n, "%lu\t%lu\t%.6f\t%lu\t%s\n",
                 (unsigned long)p->spine, (unsigned long)p->offset, p->fraction,
                 (unsigned long)p->turnedAt, ids);
    if (w < 0 || (size_t)w >= cap - n) {
      return n;                      // out of room: keep what fits rather than write nothing
    }
    n += (size_t)w;
  }
  return n;
}

bool BookStore::parse(const char* buf, size_t len) {
  count = 0;
  dirty = false;
  if (!buf || len < strlen(BOOKSTORE_MAGIC)) {
    return false;
  }
  if (strncmp(buf, BOOKSTORE_MAGIC, strlen(BOOKSTORE_MAGIC)) != 0) {
    return false;                    // a foreign or corrupt file: start clean, do not guess
  }
  size_t i = 0;
  while (i < len && buf[i] != '\n') {
    i++;
  }
  i++;

  while (i < len && count < BOOKSTORE_MAX_BOOKS) {
    size_t lineEnd = i;
    while (lineEnd < len && buf[lineEnd] != '\n') {
      lineEnd++;
    }
    size_t lineLen = lineEnd - i;
    if (lineLen == 0) {
      i = lineEnd + 1;
      continue;
    }
    // Copy the line out so the fields can be NUL-terminated in place.
    char line[BOOKSYNC_MAX_IDS * BOOKSYNC_ID_MAX + 128];
    if (lineLen >= sizeof(line)) {
      i = lineEnd + 1;               // absurd line: skip it, keep the rest of the file
      continue;
    }
    memcpy(line, buf + i, lineLen);
    line[lineLen] = '\0';
    i = lineEnd + 1;

    char* f[5];
    int nf = 0;
    char* p = line;
    f[nf++] = p;
    while (*p && nf < 5) {
      if (*p == '\t') {
        *p = '\0';
        f[nf++] = p + 1;
      }
      p++;
    }
    if (nf < 5) {
      continue;                      // truncated line: skip THIS record, not the whole file
    }

    BookPos rec;
    memset(&rec, 0, sizeof(rec));
    rec.spine = (uint32_t)strtoul(f[0], NULL, 10);
    rec.offset = (uint32_t)strtoul(f[1], NULL, 10);
    rec.fraction = strtod(f[2], NULL);
    rec.turnedAt = (uint32_t)strtoul(f[3], NULL, 10);

    char* idp = f[4];
    while (*idp && rec.nIds < BOOKSYNC_MAX_IDS) {
      char* comma = strchr(idp, ',');
      if (comma) {
        *comma = '\0';
      }
      if (*idp) {
        snprintf(rec.ids[rec.nIds], BOOKSYNC_ID_MAX, "%s", idp);
        rec.nIds++;
      }
      if (!comma) {
        break;
      }
      idp = comma + 1;
    }
    if (rec.nIds == 0) {
      continue;                      // a position with no id can never be looked up again
    }
    books[count++] = rec;
  }
  return true;
}

// ---------------------------------------------------------------- persistence
bool BookStore::load(BookStoreIo* io) {
  init();
  if (!io || !io->load) {
    return false;
  }
  char* blob = (char*)bsAlloc(BOOKSTORE_BLOB_MAX);
  if (!blob) {
    return false;
  }
  size_t len = 0;
  bool ok = io->load(io->ctx, blob, BOOKSTORE_BLOB_MAX, &len) && parse(blob, len);
  free(blob);
  return ok;
}

bool BookStore::save(BookStoreIo* io) {
  if (!io || !io->store) {
    return false;
  }
  char* blob = (char*)bsAlloc(BOOKSTORE_BLOB_MAX);
  if (!blob) {
    return false;
  }
  size_t len = serialise(blob, BOOKSTORE_BLOB_MAX);
  bool ok = len > 0 && io->store(io->ctx, blob, len);
  free(blob);
  if (ok) {
    dirty = false;
  }
  return ok;
}

bool BookStore::saveIfDirty(BookStoreIo* io) {
  return dirty ? save(io) : true;
}

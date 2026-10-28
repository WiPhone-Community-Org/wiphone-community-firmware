/*
 * mesh_dbfile.cpp — see mesh_dbfile.h. Pure, no Arduino, host-tested by tests/test_dbfile.cpp.
 */

#include "mesh_dbfile.h"

static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

/* Read the i32 count at `off` and step over it and its records. False when the count is not
 * there, is negative, or its records run past the end. 64-bit so a garbage count cannot wrap. */
static bool stepSection(const uint8_t* img, uint64_t len, uint64_t* off, uint16_t recSize) {
  if (*off + 4 > len) {
    return false;
  }
  const int32_t n = (int32_t)rd32(img + *off);
  if (n < 0) {
    return false;
  }
  *off += 4 + (uint64_t)n * recSize;
  return *off <= len;
}

bool meshDbImageComplete(const uint8_t* img, size_t len, uint32_t magic, uint16_t version,
                         uint16_t nodeSize, uint16_t msgSize, uint16_t wpSize) {
  if (!img || len < 12) {
    return false;
  }
  if (rd32(img) != magic || rd16(img + 4) != version || rd16(img + 6) != nodeSize ||
      rd16(img + 8) != msgSize || rd16(img + 10) != wpSize) {
    return false;
  }
  uint64_t off = 12;
  return stepSection(img, len, &off, nodeSize) && stepSection(img, len, &off, msgSize) &&
         stepSection(img, len, &off, wpSize) && off == (uint64_t)len;
}

bool meshFavImageComplete(const uint8_t* img, size_t len, uint32_t magic, int maxCount) {
  if (!img || len < 5 || rd32(img) != magic) {
    return false;
  }
  const int n = img[4];
  return n <= maxCount && len == (size_t)(5 + 4 * n);
}

uint32_t meshSaveChunk(uint32_t len, uint32_t off, bool onCard) {
  if (off >= len) {
    return 0;
  }
  const uint32_t left = len - off;
  const uint32_t chunk = onCard ? MESH_SAVE_CHUNK_SD : MESH_SAVE_CHUNK_FLASH;
  return left < chunk ? left : chunk;
}

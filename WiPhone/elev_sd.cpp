/* elev_sd.cpp - see elev_sd.h. */

#include "elev_sd.h"
#include <Arduino.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* The card's POSIX mount (SD.begin's default mountpoint) + MAPS_ROOT. tile_fetch's tileOnCard
 * and the Game Boy's ROM stream reach the card the same way. */
#define ELEV_SD_MAPS_ROOT  "/sd/maps"

/* One sample's reader state. ⚠ LIVES ONLY FOR ONE elevSampleCard() CALL: the descriptor of the
 * tile being read is kept between the (at most two) reads of one layer, and closed before the
 * call returns - never across calls, so no descriptor can outlive a card pull or the app. */
struct ElevSdCtx {
  int  fd;                 // the open tile, -1 = none
  int  z, x, y;            // which tile fd is
  bool bad;                // that tile is there but not ELEV_TILE_BYTES long: refuse its reads
  bool ioErr;              // something other than "no such file" went wrong
};

static void elevSdClose(ElevSdCtx* c) {
  if (c->fd >= 0) {
    ::close(c->fd);
    c->fd = -1;
  }
}

/* ElevReadFn. open() + read() on the descriptor, not fopen(): no FILE object, and no stdio
 * buffer malloc'd on the internal heap for a 4-byte read (the FATFS file objects themselves are
 * allocated once, at mount). */
static int elevSdRead(void* ctx, int z, int x, int y, uint32_t offset, uint8_t* buf, int n) {
  ElevSdCtx* c = (ElevSdCtx*)ctx;
  if (!c || !buf || n <= 0) {
    return -1;
  }
  if (c->fd < 0 || c->z != z || c->x != x || c->y != y) {
    elevSdClose(c);
    c->bad = false;
    char path[64];
    if (!elevTilePath(path, sizeof(path), ELEV_SD_MAPS_ROOT, z, x, y)) {
      return -1;                           // z/x/y out of range: not a tile anybody could have
    }
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
      /* ENOENT is the ordinary "no tile here" (FR_NO_FILE and FR_NO_PATH both map to it).
       * Anything else - EIO for a card that has gone, ENFILE with every handle in use - is
       * the card, not the map, and must not be remembered as a hole. */
      if (errno != ENOENT && errno != ENOTDIR) {
        c->ioErr = true;
      }
      return -1;
    }
    c->fd = fd;
    c->z = z;
    c->x = x;
    c->y = y;
    struct stat sb;
    if (::fstat(fd, &sb) != 0) {
      c->ioErr = true;
      c->bad = true;
    } else if ((long)sb.st_size != (long)ELEV_TILE_BYTES) {
      /* 🛑 THE LENGTH IS THE WHOLE FORMAT CHECK. A PNG copied unconverted, or a half-written
       * file, is simply the wrong length; read anyway, its bytes would be heights. */
      log_e("ELEV: %s is %ld bytes, expected %u - not an elevation tile", path, (long)sb.st_size,
            (unsigned)ELEV_TILE_BYTES);
      c->bad = true;
    }
  }
  if (c->bad) {
    return -1;
  }
  if (::lseek(c->fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
    c->ioErr = true;
    return -1;
  }
  const int got = (int)::read(c->fd, buf, (size_t)n);
  if (got != n) {
    c->ioErr = true;                       // the file is the right length: a short read is the card
  }
  return got;
}

int elevSampleCard(double lat, double lon, double* metres, int* zUsed, bool* ioErr) {
  ElevSdCtx c;
  c.fd = -1;
  c.z = c.x = c.y = -1;
  c.bad = false;
  c.ioErr = false;
  const int rc = elevSample(elevSdRead, &c, lat, lon, metres, zUsed);
  elevSdClose(&c);
  if (ioErr) {
    *ioErr = c.ioErr;
  }
  return rc;
}

bool elevCardPresent() {
  struct stat sb;
  return ::stat(ELEV_SD_MAPS_ROOT "/" ELEV_DIR, &sb) == 0 && S_ISDIR(sb.st_mode);
}

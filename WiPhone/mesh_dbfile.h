/*
 * mesh_dbfile.h — the mesh database file's arithmetic: is a saved image (or the star list)
 * WHOLE, and how much of one a save pass writes. Pure, no Arduino, host-tested by
 * tests/test_dbfile.cpp.
 *
 * ── WHY ────────────────────────────────────────────────────────────────────────────────────
 * 🛑 THE SAVE'S LAST TWO STEPS LEFT A WINDOW WITH NO DATABASE AT ALL. saveDbStep() writes the
 * image to /meshdb.tmp, closes it, then remove("/meshdb.bin") and rename(tmp -> bin) — the
 * filesystem cannot rename over an existing file. A power cut between those two calls (the pack
 * pulled, the battery at its 3.30 V cutoff mid-save) leaves the only good copy in the TEMP file,
 * and loadDb() never looked at it: on the card it fell through to the stale pre-0.9.19 SPIFFS
 * copy (or to nothing), and the next save opened the temp file with "w" and truncated the one
 * good copy. The star list (saveFavourites) has the same two steps and the same window.
 *
 * Now boot looks for exactly that state — the real file missing, the temp file present — and
 * takes the temp file ONLY IF IT IS WHOLE: the header this build writes, and a length that is
 * exactly what the header's own counts say it must be. A temp file cut off mid-write (the save
 * was interrupted earlier, while the real file still existed) cannot pass, and is left alone.
 * The rename happens at boot, before any save can truncate it.
 */

#ifndef MESH_DBFILE_H
#define MESH_DBFILE_H

#include <stdint.h>
#include <stddef.h>

/* The database image, as saveDb() writes it (all little-endian, the ESP32's order):
 *   magic u32 | version u16 | nodeSize u16 | msgSize u16 | wpSize u16      (12 bytes)
 *   nodeCount i32 | nodeCount x nodeSize
 *   msgCount  i32 | msgCount  x msgSize
 *   wpCount   i32 | wpCount   x wpSize
 * Whole = the header matches THIS build's magic/version/sizes, no count is negative, and the
 * file ends exactly where the last record does — not a byte short, not a byte over. */
bool meshDbImageComplete(const uint8_t* img, size_t len, uint32_t magic, uint16_t version,
                         uint16_t nodeSize, uint16_t msgSize, uint16_t wpSize);

/* The star list, as saveFavourites() writes it: magic u32 | count u8 | count x u32 node ids.
 * Whole = the magic matches, count <= maxCount, and len == 5 + 4 x count exactly. */
bool meshFavImageComplete(const uint8_t* img, size_t len, uint32_t magic, int maxCount);

/* ── HOW MUCH ONE SAVE PASS WRITES (0.9.79) ────────────────────────────────────────────────
 * ON SPIFFS THE WHOLE IMAGE GOES IN ONE PASS: an erase is atomic and blocking however small the
 * write that triggers it, and 128-byte chunks MEASURED 31 stalls where one write made one (the
 * note above saveDb() in meshtastic_service.cpp). A SPIFFS image is at most ~31 KB (200 nodes +
 * the 60-message flash budget), so 64 KB is "all of it".
 * ON THE CARD THERE IS NO SUCH CLIFF — 8 KB writes in 11 ms (`bench`) — so a full ~66 KB image
 * (200 nodes + 200 messages) goes in 16 KB pieces, one a pass, instead of ~0.1 s in one.
 * On both, the close/remove/rename (on the card ~30 ms: rm 15-24, rename 12; on SPIFFS up to
 * ~1 s) gets a pass of its own instead of riding on the last write. */
#define MESH_SAVE_CHUNK_SD      16384u
#define MESH_SAVE_CHUNK_FLASH   65536u

/* Bytes the next save pass writes: min(what is left, the filesystem's chunk). 0 = everything is
 * written, and this pass is the finishing one (close, remove, rename). */
uint32_t meshSaveChunk(uint32_t len, uint32_t off, bool onCard);

#endif // MESH_DBFILE_H

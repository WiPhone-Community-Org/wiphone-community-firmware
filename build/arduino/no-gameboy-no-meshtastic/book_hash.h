/*
 * book_hash.h — SHA-256, SHA-1, HMAC-SHA256 and base32, for booksync and the EPUB reader.
 *
 * Why not mbedtls, which is already linked in for mesh_crypto's AES?
 *
 * Because these bytes have to match COVEY's Python EXACTLY or a sync packet is dropped in
 * silence — a wrong implementation and a wrong passcode are indistinguishable on the wire,
 * by design (see booksync.h). The only way to prove a match is to run the shipping code
 * against vectors generated from COVEY's own booksync.py, and that test has to build on a
 * Mac. Self-contained code means the native test exercises the identical bytes that run on
 * the phone; an mbedtls backend would mean testing one implementation and shipping another.
 *
 * Cost is about 2 KB of flash. There is no Arduino dependency here on purpose.
 */
#ifndef BOOK_HASH_H
#define BOOK_HASH_H

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------- SHA-256
typedef struct {
  uint32_t state[8];
  uint64_t bitCount;
  uint8_t  buf[64];
  size_t   bufLen;
} BsSha256;

void bsSha256Init(BsSha256* c);
void bsSha256Update(BsSha256* c, const void* data, size_t len);
void bsSha256Final(BsSha256* c, uint8_t out[32]);
void bsSha256(const void* data, size_t len, uint8_t out[32]);

// ---------------------------------------------------------------- SHA-1
// Only used for epub's content fingerprint, which COVEY also computes with SHA-1. Not a
// security choice there and not treated as one: it identifies a file, it does not guard it.
typedef struct {
  uint32_t state[5];
  uint64_t bitCount;
  uint8_t  buf[64];
  size_t   bufLen;
} BsSha1;

void bsSha1Init(BsSha1* c);
void bsSha1Update(BsSha1* c, const void* data, size_t len);
void bsSha1Final(BsSha1* c, uint8_t out[20]);

// ---------------------------------------------------------------- MD5 (RFC 1321)
/* KOSync's document id (KOReader's partial MD5) and its x-auth-key (MD5 of the password).
 * Not a security choice in either place — KOReader chose it and every server and client
 * speaks it — so it is here for interoperability, proven against RFC 1321 and hashlib. */
typedef struct {
  uint32_t state[4];
  uint64_t bitCount;
  uint8_t  buf[64];
  size_t   bufLen;
} BsMd5;

void bsMd5Init(BsMd5* c);
void bsMd5Update(BsMd5* c, const void* data, size_t len);
void bsMd5Final(BsMd5* c, uint8_t out[16]);
void bsMd5Hex(const void* data, size_t len, char out[33]);   // lower-case, NUL-terminated

// Lower-case hex of `len` bytes into `out` (2*len + 1 bytes).
void bsHex(const uint8_t* d, size_t len, char* out);

// ---------------------------------------------------------------- HMAC-SHA256
void bsHmacSha256(const uint8_t* key, size_t keyLen,
                  const void* msg, size_t msgLen, uint8_t out[32]);

// ---------------------------------------------------------------- base32 (RFC 4648)
// Upper-case alphabet, NO padding written — matching Python's
// `base64.b32encode(x).decode().rstrip("=")`.
// Returns the number of characters written (excluding the NUL), or 0 if `cap` is too small.
size_t bsBase32Encode(const uint8_t* src, size_t len, char* out, size_t cap);

/* Decode `len` base32 characters into `out`.
 *
 * Rejects anything Python's b32decode would reject, because the two must agree on which
 * packets are malformed: characters outside [A-Z2-7], and lengths that cannot be a valid
 * base32 body. len % 8 of 1, 3 or 6 is impossible for real base32 output and is refused
 * rather than silently truncated.
 *
 * Returns bytes written, or -1 on any rejection.
 */
int bsBase32Decode(const char* src, size_t len, uint8_t* out, size_t cap);

// Constant-time equality for MACs. Length mismatch returns false immediately (a length is
// not a secret); equal lengths compare every byte regardless of where they first differ.
bool bsConstTimeEqual(const char* a, const char* b);

// ---------------------------------------------------------------- UTF-8 truncation
/* Python slices strings by CHARACTER and bytes by BYTE, and booksync/epub both rely on the
 * difference (device names cut at 16 characters then at 12 bytes; metadata at 120
 * characters). Getting the unit wrong changes the signed string, so both live here rather
 * than being re-derived in each module.
 */

// Cut to at most `maxBytes` bytes, then drop a trailing partial character — the equivalent
// of Python's s.encode()[:n].decode("utf-8", "ignore"). Returns bytes written.
size_t bsUtf8TruncBytes(char* dst, size_t cap, const char* src, size_t maxBytes);

// Cut to at most `maxChars` characters — the equivalent of Python's s[:n].
size_t bsUtf8TruncChars(char* dst, size_t cap, const char* src, size_t maxChars);

#endif // BOOK_HASH_H

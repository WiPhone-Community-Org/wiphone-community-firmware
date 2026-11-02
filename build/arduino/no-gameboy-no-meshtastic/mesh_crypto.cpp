/*
 * mesh_crypto.cpp — Meshtastic payload crypto + channel hashing
 */

#include "mesh_crypto.h"
#include <string.h>
#include "mbedtls/aes.h"

/* The channel hash and the default key moved to mesh_hash.cpp on 2026-09-02 so the host
 * suite can pin them against upstream — this file needs mbedtls and cannot build there.
 * Nothing else here changed. */

// AES-CTR transform (encrypt == decrypt). Supports 16-byte (AES-128) and
// 32-byte (AES-256) keys. Meshtastic CTR nonce (16 bytes): packet id as
// little-endian u64 in bytes 0..7, sender node as little-endian u32 in 8..11.
bool meshCryptCtr(const uint8_t* key, size_t keyLen,
                  uint32_t fromNode, uint32_t packetId,
                  const uint8_t* in, size_t len, uint8_t* out) {
  if (!key || !in || !out || len == 0) {
    return false;
  }
  if (keyLen != 16 && keyLen != 32) {
    return false;
  }

  uint8_t nonce[16];
  memset(nonce, 0, sizeof(nonce));
  nonce[0] = (uint8_t)(packetId >> 0);
  nonce[1] = (uint8_t)(packetId >> 8);
  nonce[2] = (uint8_t)(packetId >> 16);
  nonce[3] = (uint8_t)(packetId >> 24);
  nonce[8]  = (uint8_t)(fromNode >> 0);
  nonce[9]  = (uint8_t)(fromNode >> 8);
  nonce[10] = (uint8_t)(fromNode >> 16);
  nonce[11] = (uint8_t)(fromNode >> 24);

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_enc(&ctx, key, (unsigned)keyLen * 8) != 0) {
    mbedtls_aes_free(&ctx);
    return false;
  }

  size_t  ncOff = 0;
  uint8_t streamBlock[16];
  memset(streamBlock, 0, sizeof(streamBlock));
  int rc = mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, nonce, streamBlock, in, out);
  mbedtls_aes_free(&ctx);
  return rc == 0;
}

/*
 * mesh_pki.cpp — Meshtastic PKC primitives. See mesh_pki.h for the scheme and
 * the source-verification notes; tests/test_pki.cpp proves these exact bytes
 * against Python `cryptography` playing the stock-firmware side.
 */

#include "mesh_pki.h"
#include "book_hash.h"
#include "src/crypto/curve25519_donna.h"
#include "src/crypto/tiny_aes.h"
#include <string.h>

// ---------------------------------------------------------------- X25519

static const uint8_t X25519_BASEPOINT[32] = {9};

void meshPkiClampPrivate(uint8_t priv[MESH_PKI_KEY_LEN]) {
  priv[0]  &= 248;
  priv[31] &= 127;
  priv[31] |= 64;
}

void meshPkiPublicKey(uint8_t pub[MESH_PKI_KEY_LEN], const uint8_t priv[MESH_PKI_KEY_LEN]) {
  curve25519_donna(pub, priv, X25519_BASEPOINT);
}

bool meshPkiDeriveKey(const uint8_t myPriv[MESH_PKI_KEY_LEN],
                      const uint8_t peerPub[MESH_PKI_KEY_LEN],
                      uint8_t keyOut[MESH_PKI_KEY_LEN]) {
  if (!myPriv || !peerPub || !keyOut) {
    return false;
  }
  uint8_t shared[32];
  curve25519_donna(shared, myPriv, peerPub);

  // Contributory-behaviour check: a small-order (or all-zero) peer key yields
  // an all-zero shared secret. Stock firmware rejects it in Curve25519::dh2;
  // accepting it would mean "encrypting" with a key an attacker also knows.
  uint8_t acc = 0;
  for (int i = 0; i < 32; i++) {
    acc |= shared[i];
  }
  if (acc == 0) {
    return false;
  }

  bsSha256(shared, sizeof(shared), keyOut);
  memset(shared, 0, sizeof(shared));
  return true;
}

// ---------------------------------------------------------------- CCM

/* The 13-byte CCM nonce, byte-equal to stock's CryptoEngine::initNonce():
 * the packet id is laid down as a little-endian u64 (its high half is zero —
 * on-air ids are 32-bit) and extraNonce then overwrites bytes 4..7. */
static void pkiNonce(uint8_t n[13], uint32_t fromNode, uint32_t packetId, uint32_t extraNonce) {
  n[0]  = (uint8_t)(packetId >> 0);
  n[1]  = (uint8_t)(packetId >> 8);
  n[2]  = (uint8_t)(packetId >> 16);
  n[3]  = (uint8_t)(packetId >> 24);
  n[4]  = (uint8_t)(extraNonce >> 0);
  n[5]  = (uint8_t)(extraNonce >> 8);
  n[6]  = (uint8_t)(extraNonce >> 16);
  n[7]  = (uint8_t)(extraNonce >> 24);
  n[8]  = (uint8_t)(fromNode >> 0);
  n[9]  = (uint8_t)(fromNode >> 8);
  n[10] = (uint8_t)(fromNode >> 16);
  n[11] = (uint8_t)(fromNode >> 24);
  n[12] = 0;
}

/* One pass of CCM (RFC 3610) with L=2, M=8, no AAD — the parameters hostap's
 * aes_ccm_ae() hardcodes and stock firmware uses. Encrypt and decrypt differ
 * only in which side of the XOR the CBC-MAC runs over (always the PLAINTEXT):
 * macOverOut=false MACs `in` (encrypting), true MACs `out` (decrypting). */
static void ccmRun(const struct AES_ctx* ctx, const uint8_t nonce[13],
                   const uint8_t* in, size_t len, uint8_t* out,
                   bool macOverOut, uint8_t tag[8]) {
  uint8_t X[16], A[16], S0[16], S[16];

  // B0 flags: Adata=0, M'=(8-2)/2=3, L'=2-1=1 -> 0x19. Length is big-endian 16-bit.
  X[0] = 0x19;
  memcpy(X + 1, nonce, 13);
  X[14] = (uint8_t)(len >> 8);
  X[15] = (uint8_t)(len & 0xFF);
  AES_ECB_encrypt(ctx, X);

  // Counter blocks: flags L'=1, same nonce, big-endian counter (0 keys the tag).
  A[0] = 0x01;
  memcpy(A + 1, nonce, 13);
  A[14] = 0;
  A[15] = 0;
  memcpy(S0, A, 16);
  AES_ECB_encrypt(ctx, S0);

  size_t   off = 0;
  uint16_t ctr = 1;
  while (off < len) {
    size_t nblk = (len - off < 16) ? (len - off) : 16;
    memcpy(S, A, 16);
    S[14] = (uint8_t)(ctr >> 8);
    S[15] = (uint8_t)(ctr & 0xFF);
    AES_ECB_encrypt(ctx, S);
    for (size_t i = 0; i < nblk; i++) {
      out[off + i] = in[off + i] ^ S[i];
    }
    const uint8_t* mac = macOverOut ? (out + off) : (in + off);
    for (size_t i = 0; i < nblk; i++) {
      X[i] ^= mac[i];                        // zero-padded short final block
    }
    AES_ECB_encrypt(ctx, X);
    off += nblk;
    ctr++;
  }

  for (int i = 0; i < 8; i++) {
    tag[i] = X[i] ^ S0[i];
  }
}

// ---------------------------------------------------------------- frame

size_t meshPkiEncrypt(const uint8_t key[MESH_PKI_KEY_LEN],
                      uint32_t fromNode, uint32_t packetId, uint32_t extraNonce,
                      const uint8_t* plain, size_t len, uint8_t* out) {
  if (!key || !plain || !out || len == 0 || out == plain) {
    return 0;
  }
  uint8_t nonce[13];
  pkiNonce(nonce, fromNode, packetId, extraNonce);

  struct AES_ctx ctx;
  AES_init_ctx(&ctx, key);

  uint8_t tag[8];
  ccmRun(&ctx, nonce, plain, len, out, false, tag);
  memcpy(out + len, tag, 8);
  out[len + 8]  = (uint8_t)(extraNonce >> 0);
  out[len + 9]  = (uint8_t)(extraNonce >> 8);
  out[len + 10] = (uint8_t)(extraNonce >> 16);
  out[len + 11] = (uint8_t)(extraNonce >> 24);
  memset(&ctx, 0, sizeof(ctx));
  return len + MESH_PKI_OVERHEAD;
}

bool meshPkiDecrypt(const uint8_t key[MESH_PKI_KEY_LEN],
                    uint32_t fromNode, uint32_t packetId,
                    const uint8_t* in, size_t inLen,
                    uint8_t* plainOut, size_t* plainLen) {
  if (!key || !in || !plainOut || !plainLen ||
      inLen <= MESH_PKI_OVERHEAD || in == plainOut) {
    return false;                            // stock requires rawSize > overhead too
  }
  size_t len = inLen - MESH_PKI_OVERHEAD;

  uint32_t extraNonce = (uint32_t)in[len + 8] |
                        ((uint32_t)in[len + 9]  << 8) |
                        ((uint32_t)in[len + 10] << 16) |
                        ((uint32_t)in[len + 11] << 24);
  uint8_t nonce[13];
  pkiNonce(nonce, fromNode, packetId, extraNonce);

  struct AES_ctx ctx;
  AES_init_ctx(&ctx, key);

  uint8_t tag[8];
  ccmRun(&ctx, nonce, in, len, plainOut, true, tag);
  memset(&ctx, 0, sizeof(ctx));

  uint8_t diff = 0;                          // constant-time tag compare
  for (int i = 0; i < 8; i++) {
    diff |= (uint8_t)(tag[i] ^ in[len + i]);
  }
  if (diff != 0) {
    memset(plainOut, 0, len);                // don't leave unauthenticated bytes around
    return false;
  }
  *plainLen = len;
  return true;
}

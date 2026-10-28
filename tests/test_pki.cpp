/*
 * test_pki.cpp — mesh_pki.cpp against an independent implementation.
 *
 * The vectors (vectors_pki.h, from tools/gen_pki_vectors.py) were produced by
 * Python `cryptography` doing X25519 + AES-256-CCM(tag=8) — the same scheme
 * stock Meshtastic 2.5+ uses for PKC DMs (verified against the 2.7 firmware
 * source). Frame vectors are asserted BYTE-FOR-BYTE in both directions, so a
 * pass means the phone's ciphertext is what a RAK will accept, and a RAK's is
 * what the phone will read. This compiles the shipping sources: mesh_pki.cpp,
 * book_hash.cpp, and the vendored donna + tiny-AES underneath them.
 */

#include "../WiPhone/mesh_pki.h"
#include "vectors_pki.h"
#include <cstdio>
#include <cstring>

static int failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  ok  %s\n", name); } \
    else { printf("  FAIL %s (line %d)\n", name, __LINE__); failures++; } \
  } while (0)

// The vendored donna's raw entry point, for the RFC 7748 vector (an arbitrary
// point, not the basepoint — meshPkiPublicKey can't reach it).
extern "C" int curve25519_donna(uint8_t*, const uint8_t*, const uint8_t*);

int main() {
  printf("test_pki\n");

  // ---- X25519 --------------------------------------------------------------
  {
    uint8_t out[32];
    curve25519_donna(out, PKI_RFC7748_SCALAR, PKI_RFC7748_U);
    CHECK(memcmp(out, PKI_RFC7748_EXPECT, 32) == 0, "RFC 7748 vector 1");

    uint8_t pub[32];
    meshPkiPublicKey(pub, PKI_PHONE_PRIV);
    CHECK(memcmp(pub, PKI_PHONE_PUB, 32) == 0, "phone public key matches python");
    meshPkiPublicKey(pub, PKI_RAK_PRIV);
    CHECK(memcmp(pub, PKI_RAK_PUB, 32) == 0, "rak public key matches python");
  }

  // ---- key derivation ------------------------------------------------------
  {
    uint8_t kA[32], kB[32];
    CHECK(meshPkiDeriveKey(PKI_PHONE_PRIV, PKI_RAK_PUB, kA), "derive phone->rak");
    CHECK(meshPkiDeriveKey(PKI_RAK_PRIV, PKI_PHONE_PUB, kB), "derive rak->phone");
    CHECK(memcmp(kA, kB, 32) == 0, "both sides agree");
    CHECK(memcmp(kA, PKI_SESSION_KEY, 32) == 0, "session key = SHA-256(shared), matches python");

    uint8_t zeroPub[32] = {0};
    uint8_t k[32];
    CHECK(!meshPkiDeriveKey(PKI_PHONE_PRIV, zeroPub, k), "all-zero peer key rejected");
  }

  // ---- clamping ------------------------------------------------------------
  {
    uint8_t priv[32];
    memset(priv, 0xFF, sizeof(priv));
    meshPkiClampPrivate(priv);
    CHECK((priv[0] & 7) == 0 && (priv[31] & 0x80) == 0 && (priv[31] & 0x40) != 0,
          "clamp clears/sets the RFC 7748 bits");
  }

  // ---- frames, byte-for-byte against python --------------------------------
  struct FrameCase {
    const char*    name;
    const uint8_t* plain;  size_t plainLen;
    const uint8_t* frame;  size_t frameLen;
    uint32_t from, id, extra;
  };
  const FrameCase cases[] = {
    { "rak->phone 'hi'",  PKI_RAK2PHONE_HI_PLAIN,  sizeof(PKI_RAK2PHONE_HI_PLAIN),
      PKI_RAK2PHONE_HI_FRAME,  sizeof(PKI_RAK2PHONE_HI_FRAME),
      PKI_RAK2PHONE_HI_FROM,  PKI_RAK2PHONE_HI_ID,  PKI_RAK2PHONE_HI_EXTRA },
    { "rak->phone 32B, extraNonce 0", PKI_RAK2PHONE_BLK_PLAIN, sizeof(PKI_RAK2PHONE_BLK_PLAIN),
      PKI_RAK2PHONE_BLK_FRAME, sizeof(PKI_RAK2PHONE_BLK_FRAME),
      PKI_RAK2PHONE_BLK_FROM, PKI_RAK2PHONE_BLK_ID, PKI_RAK2PHONE_BLK_EXTRA },
    { "phone->rak text",  PKI_PHONE2RAK_TXT_PLAIN, sizeof(PKI_PHONE2RAK_TXT_PLAIN),
      PKI_PHONE2RAK_TXT_FRAME, sizeof(PKI_PHONE2RAK_TXT_FRAME),
      PKI_PHONE2RAK_TXT_FROM, PKI_PHONE2RAK_TXT_ID, PKI_PHONE2RAK_TXT_EXTRA },
    { "phone->rak max-length", PKI_PHONE2RAK_MAX_PLAIN, sizeof(PKI_PHONE2RAK_MAX_PLAIN),
      PKI_PHONE2RAK_MAX_FRAME, sizeof(PKI_PHONE2RAK_MAX_FRAME),
      PKI_PHONE2RAK_MAX_FROM, PKI_PHONE2RAK_MAX_ID, PKI_PHONE2RAK_MAX_EXTRA },
  };

  for (const FrameCase& c : cases) {
    uint8_t buf[300];
    size_t n = meshPkiEncrypt(PKI_SESSION_KEY, c.from, c.id, c.extra,
                              c.plain, c.plainLen, buf);
    char name[128];
    snprintf(name, sizeof(name), "encrypt %s == python frame", c.name);
    CHECK(n == c.frameLen && memcmp(buf, c.frame, c.frameLen) == 0, name);

    uint8_t plain[300];
    size_t plainLen = 0;
    bool ok = meshPkiDecrypt(PKI_SESSION_KEY, c.from, c.id,
                             c.frame, c.frameLen, plain, &plainLen);
    snprintf(name, sizeof(name), "decrypt %s", c.name);
    CHECK(ok && plainLen == c.plainLen && memcmp(plain, c.plain, c.plainLen) == 0, name);
  }

  // ---- tamper and misuse ---------------------------------------------------
  {
    uint8_t frame[300], plain[300];
    size_t plainLen;
    memcpy(frame, PKI_RAK2PHONE_HI_FRAME, sizeof(PKI_RAK2PHONE_HI_FRAME));
    const size_t flen = sizeof(PKI_RAK2PHONE_HI_FRAME);

    frame[0] ^= 0x01;    // ciphertext bit flip
    CHECK(!meshPkiDecrypt(PKI_SESSION_KEY, PKI_RAK2PHONE_HI_FROM, PKI_RAK2PHONE_HI_ID,
                          frame, flen, plain, &plainLen), "flipped ciphertext rejected");
    frame[0] ^= 0x01;

    frame[flen - 12] ^= 0x80;   // tag bit flip
    CHECK(!meshPkiDecrypt(PKI_SESSION_KEY, PKI_RAK2PHONE_HI_FROM, PKI_RAK2PHONE_HI_ID,
                          frame, flen, plain, &plainLen), "flipped tag rejected");
    frame[flen - 12] ^= 0x80;

    frame[flen - 1] ^= 0x01;    // extraNonce flip changes the nonce -> tag fails
    CHECK(!meshPkiDecrypt(PKI_SESSION_KEY, PKI_RAK2PHONE_HI_FROM, PKI_RAK2PHONE_HI_ID,
                          frame, flen, plain, &plainLen), "flipped extraNonce rejected");
    frame[flen - 1] ^= 0x01;

    CHECK(!meshPkiDecrypt(PKI_SESSION_KEY, PKI_RAK2PHONE_HI_FROM + 1, PKI_RAK2PHONE_HI_ID,
                          frame, flen, plain, &plainLen), "wrong sender rejected");
    CHECK(!meshPkiDecrypt(PKI_SESSION_KEY, PKI_RAK2PHONE_HI_FROM, PKI_RAK2PHONE_HI_ID + 1,
                          frame, flen, plain, &plainLen), "wrong packet id rejected");

    uint8_t wrongKey[32];
    memcpy(wrongKey, PKI_SESSION_KEY, 32);
    wrongKey[5] ^= 0x10;
    CHECK(!meshPkiDecrypt(wrongKey, PKI_RAK2PHONE_HI_FROM, PKI_RAK2PHONE_HI_ID,
                          frame, flen, plain, &plainLen), "wrong key rejected");

    CHECK(!meshPkiDecrypt(PKI_SESSION_KEY, PKI_RAK2PHONE_HI_FROM, PKI_RAK2PHONE_HI_ID,
                          frame, 12, plain, &plainLen), "overhead-only input rejected");
    CHECK(meshPkiEncrypt(PKI_SESSION_KEY, 1, 2, 3, plain, 0, frame) == 0,
          "zero-length encrypt refused");
  }

  // ---- round-trip with a fresh keypair (no vectors involved) ---------------
  {
    uint8_t privA[32], privB[32];
    for (int i = 0; i < 32; i++) { privA[i] = (uint8_t)(i * 13 + 1); privB[i] = (uint8_t)(200 - i * 3); }
    meshPkiClampPrivate(privA);
    meshPkiClampPrivate(privB);
    uint8_t pubA[32], pubB[32], kA[32], kB[32];
    meshPkiPublicKey(pubA, privA);
    meshPkiPublicKey(pubB, privB);
    CHECK(meshPkiDeriveKey(privA, pubB, kA) && meshPkiDeriveKey(privB, pubA, kB) &&
          memcmp(kA, kB, 32) == 0, "fresh keypair agreement");

    const char* msg = "the covey got EVERYTHING";
    uint8_t frame[300], plain[300];
    size_t plainLen = 0;
    size_t n = meshPkiEncrypt(kA, 0xAABBCCDD, 0x11223344, 0x99887766,
                              (const uint8_t*)msg, strlen(msg), frame);
    CHECK(n == strlen(msg) + MESH_PKI_OVERHEAD &&
          meshPkiDecrypt(kB, 0xAABBCCDD, 0x11223344, frame, n, plain, &plainLen) &&
          plainLen == strlen(msg) && memcmp(plain, msg, plainLen) == 0,
          "fresh round trip");
  }

  if (failures) {
    printf("test_pki: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("test_pki: all passed\n");
  return 0;
}

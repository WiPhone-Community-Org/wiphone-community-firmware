/*
 * mesh_pki.h — Meshtastic PKC (public-key crypto) for direct messages.
 *
 * Meshtastic 2.5+ refuses DMs both ways without this: a stock node will not
 * SEND a DM to a peer with no known public key (PKI_SEND_FAIL_PUBLIC_KEY), and
 * it silently DROPS channel-encrypted text DMs it receives ("Rejecting legacy
 * DM", Router.cpp). Channels are unaffected. So DMs between this phone and any
 * 2.5+ node work only with the scheme below, verified against the 2.7 firmware
 * source (src/mesh/CryptoEngine.cpp, Router.cpp) on 2026-08-19:
 *
 *   key       = SHA-256( X25519(my private, peer public) )      — 32 bytes
 *   nonce(13) = packetId LE(4) | extraNonce LE(4) | fromNode LE(4) | 0x00
 *               (stock writes the id as a u64 and overwrites its high half
 *                with a random per-packet extraNonce)
 *   payload   = AES-256-CCM ciphertext | tag(8) | extraNonce LE(4)
 *               (CCM per RFC 3610: L=2, M=8, no AAD — hostap aes_ccm_ae)
 *   on air    = normal 16-byte MeshPacketHeader with channelHash = 0x00
 *
 * Public keys travel in NodeInfo broadcasts (User protobuf field 8) — that is
 * the entire key exchange, trust-on-first-use.
 *
 * Self-contained on purpose (vendored curve25519-donna + tiny-AES under
 * src/crypto/, SHA-256 from book_hash): the host tests run the identical bytes
 * that ship, against vectors from Python's `cryptography` playing the RAK.
 * book_hash.h explains why that beats an mbedtls backend. No Arduino headers.
 */
#ifndef MESH_PKI_H
#define MESH_PKI_H

#include <stddef.h>
#include <stdint.h>

#define MESH_PKI_KEY_LEN   32     // X25519 keys and the derived AES-256 key
#define MESH_PKI_OVERHEAD  12     // 8-byte CCM tag + 4-byte extraNonce

// Clamp a candidate private key per RFC 7748 (clear low 3 bits, set bit 254,
// clear bit 255). curve25519_donna also clamps on use; storing the key clamped
// keeps what's in NVS equal to what's used, like stock firmware's dh1.
void meshPkiClampPrivate(uint8_t priv[MESH_PKI_KEY_LEN]);

// pub = X25519(priv, basepoint 9).
void meshPkiPublicKey(uint8_t pub[MESH_PKI_KEY_LEN], const uint8_t priv[MESH_PKI_KEY_LEN]);

// keyOut = SHA-256(X25519(myPriv, peerPub)). Returns false when the shared
// secret is all zero (peer key of small order / all-zero key) — the same
// rejection stock firmware gets from Curve25519::dh2.
bool meshPkiDeriveKey(const uint8_t myPriv[MESH_PKI_KEY_LEN],
                      const uint8_t peerPub[MESH_PKI_KEY_LEN],
                      uint8_t keyOut[MESH_PKI_KEY_LEN]);

// out = ciphertext(len) | tag(8) | extraNonce(4). Returns len + MESH_PKI_OVERHEAD,
// or 0 on bad args. `out` needs room for that much; in-place (out == plain) is
// NOT supported. extraNonce should be random per packet (caller supplies it so
// this file stays free of any RNG and the tests stay deterministic).
size_t meshPkiEncrypt(const uint8_t key[MESH_PKI_KEY_LEN],
                      uint32_t fromNode, uint32_t packetId, uint32_t extraNonce,
                      const uint8_t* plain, size_t len, uint8_t* out);

// Inverse of meshPkiEncrypt: verifies the CCM tag (constant-time) and writes
// inLen - MESH_PKI_OVERHEAD plaintext bytes. False on bad args, short input, or
// authentication failure — a failed tag is indistinguishable from "not for us",
// which is what makes trying PKI first on hash-0 packets safe.
bool meshPkiDecrypt(const uint8_t key[MESH_PKI_KEY_LEN],
                    uint32_t fromNode, uint32_t packetId,
                    const uint8_t* in, size_t inLen,
                    uint8_t* plainOut, size_t* plainLen);

#endif // MESH_PKI_H

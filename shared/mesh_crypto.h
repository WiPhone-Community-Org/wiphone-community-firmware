/*
 * mesh_crypto.h — Meshtastic payload crypto + channel hashing
 *
 * Meshtastic encrypts the packet payload (everything after the 16-byte
 * PacketHeader) with AES-CTR using the channel key (128- or 256-bit). The CTR
 * nonce is built from the packet id and sender node number.
 */

#ifndef MESH_CRYPTO_H
#define MESH_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include "mesh_hash.h"   // channel hash + default key: split out so the host suite can pin them

// AES-CTR transform (encrypt == decrypt). keyLen must be 16 or 32 bytes.
bool meshCryptCtr(const uint8_t* key, size_t keyLen,
                  uint32_t fromNode, uint32_t packetId,
                  const uint8_t* in, size_t len, uint8_t* out);

#endif // MESH_CRYPTO_H

/*
 * mesh_hash.h — the Meshtastic channel hash, on its own so it can be tested.
 *
 * channelHash = xorHash(name) ^ xorHash(psk). One byte; it rides in the PacketHeader and is
 * how every radio decides which channel a frame belongs to before it can decrypt a thing.
 * Get it wrong and the phone is not "on the wrong channel", it is silently deaf.
 *
 * 🔑 SPLIT OUT OF mesh_crypto.cpp ON 2026-09-02 FOR ONE REASON: that file needs mbedtls for
 * AES and will not build on a Mac, so the hash could not join the host suite. Upstream exposes
 * the same function (meshtastic.util.generate_channel_hash) and tests/test_wire.cpp now pins
 * ours against it. Before the split the comparison had been done by hand once and then
 * written down as a comment, which is the kind of proof that quietly stops being true.
 *
 * Arduino-free. mesh_crypto.h includes this, so nothing that used to reach these through
 * mesh_crypto.h has to change.
 */

#ifndef MESH_HASH_H
#define MESH_HASH_H

#include <stdint.h>
#include <stddef.h>

// XOR-hash of a byte range (used to build the channel hash).
uint8_t meshXorHash(const uint8_t* data, size_t len);

// Channel hash byte for a (name, key): xorHash(name) ^ xorHash(key).
uint8_t meshChannelHash(const char* name, const uint8_t* key, size_t keyLen);

// The well-known default channel key (16 bytes) and the LongFast channel hash.
const uint8_t* meshDefaultKey();
uint8_t        meshDefaultChannelHash();

#endif // MESH_HASH_H

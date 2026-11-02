/*
 * mesh_hash.cpp — see mesh_hash.h. Moved verbatim out of mesh_crypto.cpp on 2026-09-02.
 */

#include "mesh_hash.h"
#include <string.h>

// The well-known Meshtastic default channel key (AES-128). A channel whose PSK
// is the single byte {0x01} uses exactly this key ("default" / LongFast).
static const uint8_t MESH_DEFAULT_KEY[16] = {
  0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
  0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

static const char MESH_DEFAULT_CHANNEL_NAME[] = "LongFast";

uint8_t meshXorHash(const uint8_t* data, size_t len) {
  uint8_t h = 0;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
  }
  return h;
}

// Channel hash byte = xorHash(name) ^ xorHash(key). Meshtastic uses this both
// for channel matching and to select which channel a packet belongs to.
uint8_t meshChannelHash(const char* name, const uint8_t* key, size_t keyLen) {
  uint8_t nameHash = meshXorHash((const uint8_t*)name, name ? strlen(name) : 0);
  uint8_t keyHash = meshXorHash(key, keyLen);
  return nameHash ^ keyHash;
}

const uint8_t* meshDefaultKey() {
  return MESH_DEFAULT_KEY;
}

uint8_t meshDefaultChannelHash() {
  return meshChannelHash(MESH_DEFAULT_CHANNEL_NAME, MESH_DEFAULT_KEY, sizeof(MESH_DEFAULT_KEY));
}

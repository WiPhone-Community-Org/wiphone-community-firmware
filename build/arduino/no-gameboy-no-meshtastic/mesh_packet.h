/*
 * mesh_packet.h — Meshtastic on-air protocol definitions (Phase 2, step 2)
 *
 * The unencrypted LoRa PacketHeader that precedes every Meshtastic frame, plus
 * the flag bit layout. The payload that follows the header is the AES-CTR
 * encrypted, protobuf-encoded Data message (decoded in later steps).
 *
 * All multi-byte fields are little-endian, matching the ESP32, so the struct
 * can be memcpy'd directly from the received buffer.
 */

#ifndef MESH_PACKET_H
#define MESH_PACKET_H

#include <stdint.h>

// Meshtastic uses 0xFFFFFFFF as the broadcast destination on the air.
#define MESH_ADDR_BROADCAST_ONAIR  0xFFFFFFFFu

// Flags byte layout (matches Meshtastic PacketHeader.flags).
#define MESH_FLAGS_HOP_LIMIT_MASK   0x07
#define MESH_FLAGS_WANT_ACK         0x08
#define MESH_FLAGS_VIA_MQTT         0x10
#define MESH_FLAGS_HOP_START_MASK   0xE0
#define MESH_FLAGS_HOP_START_SHIFT  5

// 16-byte header at the start of every Meshtastic LoRa packet.
// (next_hop / relay_node were added in firmware 2.3; older senders leave them 0.)
typedef struct __attribute__((packed)) {
  uint32_t dest;         // destination node number (0xFFFFFFFF = broadcast)
  uint32_t sender;       // source node number
  uint32_t packetId;     // packet id
  uint8_t  flags;        // hop_limit | want_ack | via_mqtt | hop_start
  uint8_t  channelHash;  // channel hash: matches channel + seeds the AES nonce
  uint8_t  nextHop;      // next hop (0 if unused)
  uint8_t  relayNode;    // relay node (0 if unused)
} MeshPacketHeader;

#define MESH_HEADER_LEN  ((int)sizeof(MeshPacketHeader))   // 16 bytes

#endif // MESH_PACKET_H

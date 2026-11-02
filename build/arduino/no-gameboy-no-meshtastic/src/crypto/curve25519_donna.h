/* Declaration for the vendored curve25519_donna.c (which ships no header).
 * out = X25519(secret, basepoint): the RFC 7748 scalar-mult, with the secret
 * clamped internally — a stored private key can be passed as-is.
 * Always returns 0; an all-zero result is the caller's weak-key check
 * (mesh_pki.cpp does it, matching stock Meshtastic's dh2 rejection). */
#ifndef CURVE25519_DONNA_H
#define CURVE25519_DONNA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int curve25519_donna(uint8_t* mypublic, const uint8_t* secret, const uint8_t* basepoint);

#ifdef __cplusplus
}
#endif

#endif // CURVE25519_DONNA_H

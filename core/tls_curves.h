/*
 * tls_curves.h - the order this phone OFFERS its elliptic curves in a TLS ClientHello (0.9.81,
 * for the Almanac's weather). The decision is here, pure and host-tested (tests/test_weather.cpp);
 * the link-time wrap that applies it is tls_curves.cpp.
 *
 * WHY. mbedTLS 2.16.7 (arduino-esp32 1.0.6) offers the curves in its ecp.o table's order,
 * LARGEST FIRST - read off this build's own libmbedtls.a (.rodata.ecp_supported_curves, 11
 * entries): secp521r1, brainpoolP512r1, secp384r1, brainpoolP384r1, secp256r1, secp256k1,
 * brainpoolP256r1, secp224r1, secp224k1, secp192r1, secp192k1 - and WiFiClientSecure never
 * calls mbedtls_ssl_conf_curves. api.open-meteo.com FOLLOWS THE CLIENT'S ORDER (checked
 * 2026-10-03 with openssl: offered 521 first it picks P-521, 256 first P-256, 384 first P-384),
 * so this phone's handshake with it would be a P-521 ECDHE - all software here (no MPI hardware),
 * roughly 6-9x a P-256 one, against a 10 s connect+handshake cap on a core shared with the loop.
 * Google and api.weather.gov pick P-256 themselves; tile servers that follow the client move to
 * P-256 too (expected faster - re-time a tile run).
 *
 * THE RULE: exactly the curves the linked table has - nothing added, nothing dropped - with
 * secp256r1, secp384r1, secp521r1 first (in that order, those of them present), then the rest in
 * the table's own order. ⚠ NEVER an id outside the table (x25519 included: mbedTLS 2.16 has no
 * TLS x25519, though CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED is set). This build's
 * ssl_write_supported_elliptic_curves_ext looks every id up (mbedtls_ecp_curve_info_from_grp_id)
 * and on an unknown one RETURNS MBEDTLS_ERR_SSL_BAD_CONFIG (-0x5E80, its literal pool) - the
 * ClientHello is never written and EVERY TLS connection on the phone fails (read from the
 * disassembly of ssl_cli.o, 2026-10-03; the map's guess was "drops the extension").
 *
 * Pure: no Arduino or IDF headers. The ids are mbedtls_ecp_group_id values (ecp.h: NONE 0,
 * SECP192R1 1, SECP224R1 2, SECP256R1 3, SECP384R1 4, SECP521R1 5, ...).
 */
#ifndef TLS_CURVES_H
#define TLS_CURVES_H

#define TLS_CURVE_NONE       0
#define TLS_CURVE_SECP256R1  3
#define TLS_CURVE_SECP384R1  4
#define TLS_CURVE_SECP521R1  5

/* `in`: a NONE-terminated id list (the table's); `out`: room for `cap` ids including the NONE.
 * Writes the reordered list, NONE-terminated, and returns its length (NONE not counted) - the
 * same as `in`'s. A list that does not fit `cap` is copied as it is (cut to fit, terminated):
 * the order is an optimisation, never a reason to change the set. */
static inline int tlsCurveOrder(const int* in, int* out, int cap) {
  if (!in || !out || cap <= 0) {
    return 0;
  }
  int n = 0;
  while (in[n] != TLS_CURVE_NONE) {
    n++;
  }
  if (n + 1 > cap) {
    int k = 0;
    for (; k < cap - 1; k++) {
      out[k] = in[k];
    }
    out[k] = TLS_CURVE_NONE;
    return k;
  }
  static const int FIRST[3] = { TLS_CURVE_SECP256R1, TLS_CURVE_SECP384R1, TLS_CURVE_SECP521R1 };
  int k = 0;
  for (int f = 0; f < 3; f++) {
    for (int i = 0; i < n; i++) {
      if (in[i] == FIRST[f]) {
        out[k++] = in[i];
        break;
      }
    }
  }
  for (int i = 0; i < n; i++) {
    if (in[i] != TLS_CURVE_SECP256R1 && in[i] != TLS_CURVE_SECP384R1 && in[i] != TLS_CURVE_SECP521R1) {
      out[k++] = in[i];
    }
  }
  out[k] = TLS_CURVE_NONE;
  return k;
}

#endif // TLS_CURVES_H

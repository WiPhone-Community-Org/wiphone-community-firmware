/*
 * tls_curves.cpp - the ClientHello's curve order, P-256 first (tls_curves.h has the why and the
 * rule; the rule is host-tested there).
 *
 * 🛑 -Wl,--wrap=mbedtls_ecp_grp_id_list (platformio.ini, beside the esp_wifi_start/stop wraps).
 * mbedtls_ssl_config_defaults() sets conf->curve_list = mbedtls_ecp_grp_id_list(), and ssl_tls.o
 * and pkparse.o hold that symbol UNDEFINED (nm on libmbedtls.a), so the linker sends both here.
 * ecp.o's own calls are not wrapped (they are inside the object that defines it), and nothing in
 * it calls the list anyway. pkparse only walks the list to match named parameters, and the
 * ServerKeyExchange check (mbedtls_ssl_check_curve) only asks membership - the same SET, so both
 * are unchanged.
 *
 * ⚠ IT IS GLOBAL: every TLS client on the phone - the weather, the AI (Google picks P-256 itself:
 * no change), the map downloader (servers that follow the client's order move to P-256, expected
 * faster) and the OTA check. `cpu`-style proof on the bench: `wx` prints each host's handshake ms.
 */
#include "tls_curves.h"

#include <mbedtls/ecp.h>

extern "C" {
const mbedtls_ecp_group_id* __real_mbedtls_ecp_grp_id_list(void);
const mbedtls_ecp_group_id* __wrap_mbedtls_ecp_grp_id_list(void);
}

/* Built once from the REAL list (so it is exactly the linked table's set), on whichever task first
 * makes a TLS config. Two tasks building it at once write the same values in the same order, and
 * neither reads it before its own build is done - so no lock. */
static mbedtls_ecp_group_id s_order[MBEDTLS_ECP_DP_MAX + 1];
static volatile bool s_built = false;

const mbedtls_ecp_group_id* __wrap_mbedtls_ecp_grp_id_list(void) {
  if (!s_built) {
    const mbedtls_ecp_group_id* real = __real_mbedtls_ecp_grp_id_list();
    int in[MBEDTLS_ECP_DP_MAX + 1];
    int out[MBEDTLS_ECP_DP_MAX + 1];
    int n = 0;
    while (real && real[n] != MBEDTLS_ECP_DP_NONE && n < MBEDTLS_ECP_DP_MAX) {
      in[n] = (int)real[n];
      n++;
    }
    in[n] = TLS_CURVE_NONE;
    const int k = tlsCurveOrder(in, out, MBEDTLS_ECP_DP_MAX + 1);
    for (int i = 0; i <= k; i++) {
      s_order[i] = (mbedtls_ecp_group_id)out[i];
    }
    s_built = true;
  }
  return s_order;
}

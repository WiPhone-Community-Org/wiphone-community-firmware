/*
 * isrg_roots.h - the Let's Encrypt roots this firmware trusts, as string literals that
 * concatenate: ISRG_ROOT_X1_PEM alone (the OTA check, ota.cpp), ISRG_ROOT_X1_PEM ISRG_ROOT_YR_PEM
 * (the Almanac's weather, weather_net.cpp) or X1 + X2 + YE (Wi-Fi location, geoloc.h's
 * GEO_ROOTS_PEM) - mbedTLS takes several PEMs in one buffer: ssl_client.cpp's
 * mbedtls_x509_crt_parse of the whole string.
 *
 * WHY BOTH, FOR THE WEATHER (checked 2026-10-03). api.open-meteo.com and api.weather.gov both
 * serve leaf <- Let's Encrypt YR1 / YR2 (RSA-2048) <- "Root YR" CROSS-SIGNED by ISRG Root X1
 * (RSA-4096, notAfter 2032-09-02). So X1 alone verifies today - but only while the servers keep
 * sending that cross certificate; Let's Encrypt gives X1 "trusted until 2030-06-04" in the root
 * programs, and a server that sends the short chain (leaf <- YR1 <- Root YR) fails an X1-only
 * pin (measured from the Mac with a one-root store). The self-signed Root YR covers that day,
 * and with it pinned mbedTLS stops at Root YR and skips X1's RSA-4096 signature check - one less
 * software RSA verify in a handshake that already runs ~4-8 s on this core.
 *
 *   ISRG Root X1  SHA-256 96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6
 *                 RSA-4096, 2015-06-04 .. 2035-06-04. The bytes ota.cpp has carried since 0.9.x,
 *                 byte-identical to the macOS system keychain's (SystemRootCertificates) and
 *                 certifi's copy - re-hashed 2026-10-03.
 *   ISRG Root YR  SHA-256 E5:7B:7E:6F:15:0C:41:91:02:E8:D5:C0:55:72:9F:F9:67:B9:D1:A8:29:BF:00:CE:C8:9C:A6:04:EB:F4:A8:6F
 *                 RSA-4096, 2025-09-03 .. 2045-09-02, from letsencrypt.org/certs/gen-y/root-yr.pem.
 *                 It is in NO trust store yet (not macOS, not certifi), so it was SECOND-SOURCED
 *                 by its key instead (2026-10-03, python `cryptography`): its public key
 *                 (SubjectPublicKeyInfo SHA-256 7e4e8838...d8618bcf) and subject equal those
 *                 of the "Root YR" cross certificate that BOTH APIs serve today (SHA-256
 *                 07:26:39:D0...07:CF:76, byte-identical from the two hosts), whose signature
 *                 verifies under the X1 above; and its own self-signature verifies. A forged
 *                 Root YR would need X1's key to have a matching cross certificate.
 * tests/test_weather.cpp decodes both PEMs and checks these two SHA-256 fingerprints.
 *
 * This build never checks certificate DATES (CONFIG_MBEDTLS_HAVE_TIME_DATE is unset - ai_net.cpp),
 * so neither root's expiry, nor the cross certificate's 2032 one, is a clock-time failure; only a
 * change in what the servers send can break X1-only trust.
 * ⚠ NO OVERRIDE FILE for the weather (ai_net.cpp's reason: the uploader has no password and runs
 * on the phone's own open hotspot). New roots are a firmware update.
 *
 * WHY X1 + X2 + YE, FOR WI-FI LOCATION (beaconDB, geoloc_net.cpp; checked 2026-10-10, the review's
 * item 6). api.beacondb.net serves an ECDSA chain: leaf (P-256) <- Let's Encrypt YE1 (P-384) <-
 * "Root YE" CROSS-SIGNED by ISRG Root X2 <- X2 CROSS-SIGNED by ISRG Root X1. An X1-only pin works
 * only while the server keeps sending both cross certificates; X1 + X2 survives the X1 cross
 * going; and a server that sends the SHORT chain (leaf <- YE1 <- Root YE, the same day the weather
 * hosts may drop theirs) fails both - so the self-signed Root YE is pinned too, the way Root YR
 * is. With YE pinned mbedTLS stops at it: no P-384 verify of the YE cross and no RSA-4096 one of
 * the X2 cross, in a handshake that is all software ECC on this core.
 *
 *   ISRG Root X2  SHA-256 69:72:9B:8E:15:A8:6E:FC:17:7A:57:AF:B7:17:1D:FC:64:AD:D2:8C:2F:CA:8C:F1:50:7E:34:45:3C:CB:14:70
 *                 ECDSA P-384, 2020-09-04 .. 2040-09-17. Byte-identical in the macOS system
 *                 keychain (SystemRootCertificates), certifi and letsencrypt.org/certs/
 *                 isrg-root-x2.pem (2026-10-10). Its key (SubjectPublicKeyInfo SHA-256
 *                 762195c2...cce68332) is the one in the X2 cross certificate beaconDB serves,
 *                 and that cross verifies under the X1 above.
 *   ISRG Root YE  SHA-256 E1:4F:FC:AD:5B:00:25:73:10:06:CA:A4:3A:12:1A:22:D8:E9:70:0F:4F:B9:CF:85:2F:02:A7:08:AA:5D:56:66
 *                 ECDSA P-384, 2025-09-03 .. 2045-09-02, from letsencrypt.org/certs/gen-y/root-ye.pem.
 *                 In NO trust store yet (not macOS, not certifi), so SECOND-SOURCED by its key
 *                 like Root YR (2026-10-10, python `cryptography` 48): its public key
 *                 (SubjectPublicKeyInfo SHA-256 b0292ae5...c71d47bc) and subject equal those of
 *                 the "Root YE" cross certificate api.beacondb.net serves (SHA-256
 *                 0F:C0:90:1C...C4:85:F0), whose signature verifies under the X2 above; its own
 *                 self-signature verifies; and the served YE1 verifies under it, the leaf under
 *                 YE1. A forged Root YE would need X2's key to have a matching cross certificate.
 * tests/test_geoloc.cpp decodes GEO_ROOTS_PEM (geoloc.h: X1 + X2 + YE) and checks all three.
 *
 * Pure: no Arduino or IDF headers (the host suite includes it).
 */
#ifndef ISRG_ROOTS_H
#define ISRG_ROOTS_H

// ISRG Root X1 - SHA-256 96:BC:EC:...:DF:08:C6
#define ISRG_ROOT_X1_PEM \
  "-----BEGIN CERTIFICATE-----\n" \
  "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\n" \
  "-----END CERTIFICATE-----\n"

// ISRG Root YR (self-signed) - SHA-256 E5:7B:7E:...:A8:6F
#define ISRG_ROOT_YR_PEM \
  "-----BEGIN CERTIFICATE-----\n" \
  "MIIFKTCCAxGgAwIBAgIRAOxGNJNgz0sP+KmC2Tqpyj0wDQYJKoZIhvcNAQELBQAw\n" \
  "LjELMAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWVIw\n" \
  "HhcNMjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzEN\n" \
  "MAsGA1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZUjCCAiIwDQYJKoZIhvcNAQEB\n" \
  "BQADggIPADCCAgoCggIBANvGJnN78CTJdWL3+eGfsLN5TrNBJs+VH9hRXqRbwxu9\n" \
  "sGNiB0BD1fcOxbSUQCJIM1xE13Db+5Cw1w0s0EBYsvuIP/6joF0w8cuImbgR1OGg\n" \
  "YbSQ4OpzI+DG8SGuTlcE873OCS+kh3srlo6vl43M5OJg4Aeo1sfHp6kTJDoIiFBN\n" \
  "JAY+OKfX/FUvYKuhjT+no49lmqmupSBI5PkBQiqrEGtWU5uxU/cQWHGu8jSjFBzn\n" \
  "ZqvbNPLMXMLFxCb3WTfrJBXXjqvWG+v4bjzxjjeAtOlU7qarRDvNOyAuQYLln904\n" \
  "M+faKx8hnLCpJ15ZqaEgcNlY+9MMWcC5yvL2A2j3l9+2buggZX+dOE91zYmIdawT\n" \
  "vSZuVvlbRrAlLxIB6pwMBjneXCjYQ8+3BCCjssbSNpZU3hTcBDdhfAlEDlYr6pEa\n" \
  "tnMdmDT5BqnKC92bd0EhM1fbLHioLccLCuievT8ZkPhZrq7Mii7gNXAcUEAR8+lz\n" \
  "Yal+9zTg7C5DALyVOeG/CqfRAMn1KSHCR0NSA6P8tn/mGRlnCct5rtVCLnVySVpU\n" \
  "6H1qGg3DgTOuskf8eahTMiYbI5ezPJmO5ertalskQ1utp74+eDy92PI4ftHKTbq9\n" \
  "IWhH4YZKh3WnJEIt+oQvlYZbY8tpEroKrFB6PFGzrJIDRyts4HqvuH52RFj2zv/B\n" \
  "AgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud\n" \
  "DgQWBBTe51tg0CJtQCh9Pw0B/qS1UrRRlDANBgkqhkiG9w0BAQsFAAOCAgEAWHnf\n" \
  "713Bdkq7t5yN2dNIgQakUb94X9WuyhMEHHkgx4oDpSUlnG0w4g94MoqaEUE31ZjR\n" \
  "LU7L5LD1g9ujFHTQu8AD215AHMVQFbm6j8hQxdXHAzDajFNQnOlDJrLjzIx176oy\n" \
  "AjvUtejZx2NNmdb5fd0WGVGsCdoAJ3N8ozo7ajE8t6vfxStZb4BQ9WYJGHUDrv2N\n" \
  "i5tJF6CNiPnlzs3BUfECRbE4JSk+jvy8+VoGiFE8qsH/j78x2fjgQhAQFV7P7Zxy\n" \
  "dBTZ1wEkNpZNW2qnaK1SKBLa+xf6E06YRIq5uaI+HWH8SY1y5VbRgzq40EKg3yxP\n" \
  "06fz+uYAUIFJoLNfhwRCc3Q6pQVuMX3yAjHAes4gk4moGcLQ5p7HAh39yeylZc1J\n" \
  "41sx/jKwLIkPE6Rr1Nf4pxdsxf9SA4yOEiAkDgq04DVxn8hgYFdUtBCuiuVC2heA\n" \
  "EiqVEa+8QZjuw8Gj0EbHXcRd1nInvGqRS1o9Is7YBdQN57X1AYveGBNNqjICSb7c\n" \
  "awuw1EawTDrs13VUlJVEsbQ0/O/1aaV73mCdOQ8azqL2KTv1Ewu1xbquE2S+kdQU\n" \
  "To9TUwat3wUA6cwXh1EfpS/3fJ0aGah5hdpRyoCLDlsSn8tkrjMfFFX0viC+GxHc\n" \
  "sI1ANRYvqSFC2X1VRZfDg+wD6E21BccmifG4yWc=\n" \
  "-----END CERTIFICATE-----\n"

// ISRG Root X2 (self-signed, ECDSA P-384) - SHA-256 69:72:9B:...:CB:14:70
#define ISRG_ROOT_X2_PEM \
  "-----BEGIN CERTIFICATE-----\n" \
  "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n" \
  "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n" \
  "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n" \
  "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n" \
  "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n" \
  "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n" \
  "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n" \
  "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n" \
  "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n" \
  "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n" \
  "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n" \
  "/q4AaOeMSQ+2b1tbFfLn\n" \
  "-----END CERTIFICATE-----\n"

// ISRG Root YE (self-signed, ECDSA P-384) - SHA-256 E1:4F:FC:...:5D:56:66
#define ISRG_ROOT_YE_PEM \
  "-----BEGIN CERTIFICATE-----\n" \
  "MIIB2TCCAWCgAwIBAgIRAKQCa6LvbHwg1AR+XmWmk4AwCgYIKoZIzj0EAwMwLjEL\n" \
  "MAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWUUwHhcN\n" \
  "MjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsG\n" \
  "A1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZRTB2MBAGByqGSM49AgEGBSuBBAAi\n" \
  "A2IABDwS/6vhrcVqcbBo+wgdI3fwn9x7DNJJOY/lTOti0vkwuRN87RhEhTH17E7X\n" \
  "yFjWsPYhIPt/wzOqxTd2b+4ZJNy9ID04YywF9U5zasDVyGSNErVNtz8uSGh5izW8\n" \
  "7j77GaNCMEAwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0O\n" \
  "BBYEFKPIJlqOoUzQNWP8myPIOq5W809WMAoGCCqGSM49BAMDA2cAMGQCMHhMr8N9\n" \
  "LdL1VQKs9BdV81r76eXRB6mtjuNjzk6/lBsPNToWLTDzGYgtQKO1jl63uAIwGV7m\n" \
  "onyF377c+MM1oqVNs17sgu7F9YKZwgLmVbeOMDbKAXHtKMDLbiGllCcs8f47\n" \
  "-----END CERTIFICATE-----\n"

#endif // ISRG_ROOTS_H

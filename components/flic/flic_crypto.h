// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#pragma once
// Cryptography for Flic Twist FULL-VERIFY pairing (the "bond a Twist to this ESP" path).
// Reconnect/runtime needs only Chaskey (chaskey.h); pairing additionally needs:
//   - X25519 ECDH               -> vendored TweetNaCl (crypto_scalarmult)
//   - Ed25519 signature verify  -> vendored TweetNaCl (crypto_sign_open), 4 Flic variants
//   - SHA-256 + HMAC-SHA256 KDF -> mbedTLS (built into ESP-IDF)
// Ported to match pyflic-ble security.py (derive_full_verify_keys, verify_ed25519_*).

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <cstdint>
#include <cstddef>
#include <cstring>

extern "C" {
#include "tweetnacl.h"
}
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>

namespace esphome {
namespace flic {

// Flic Twist Ed25519 public key (pyflic-ble const.py TWIST_ED25519_PUBLIC_KEY).
static const uint8_t TWIST_ED25519_PUBLIC_KEY[32] = {
    0xa8, 0xb7, 0xdf, 0x10, 0x43, 0x4f, 0x56, 0x50, 0x69, 0xe4, 0x13, 0x1f, 0x5b, 0x13, 0xf1, 0xd9,
    0x05, 0x6f, 0xaf, 0x2b, 0x61, 0xcf, 0x92, 0x9b, 0x05, 0xd0, 0x2d, 0x63, 0x0b, 0xda, 0xf4, 0x8b};

// Flic 2 (button/Duo) Ed25519 public key (pyflic-ble const.py FLIC2_ED25519_PUBLIC_KEY).
// Distinct from the Twist key — the only crypto-constant difference in the full-verify identity
// check between the two device families.
static const uint8_t FLIC2_ED25519_PUBLIC_KEY[32] = {
    0xd3, 0x3f, 0x24, 0x40, 0xdd, 0x54, 0xb3, 0x1b, 0x2e, 0x1d, 0xcf, 0x40, 0x13, 0x2e, 0xfa, 0x41,
    0xd8, 0xf8, 0xa7, 0x47, 0x41, 0x68, 0xdf, 0x40, 0x08, 0xf5, 0xa9, 0x5f, 0xb3, 0xb0, 0xd0, 0x22};

// Generate an X25519 keypair. The raw random private scalar is stored as-is; TweetNaCl's
// scalar-mult clamps a copy internally (same as python-cryptography), so the same private
// bytes drive both the public key and the shared secret consistently.
inline void flic_x25519_keypair(uint8_t priv[32], uint8_t pub[32]) {
  esp_fill_random(priv, 32);
  crypto_scalarmult_base(pub, priv);
}

inline void flic_x25519_shared(uint8_t out[32], const uint8_t priv[32], const uint8_t peer_pub[32]) {
  crypto_scalarmult(out, priv, peer_pub);
}

// Verify the button's Ed25519 signature over `msg`, trying the 4 Flic signature variants
// (low 2 bits of signature[32]). Returns the variant that verifies (0-3) or -1 if none do.
// Detached verify via crypto_sign_open on [signature(64) || msg].
inline int flic_ed25519_verify_variant(const uint8_t pub[32], const uint8_t *msg, size_t msglen,
                                       const uint8_t sig[64]) {
  if (msglen > 64)
    return -1;  // Flic signed_data is 39 bytes; guard the fixed buffers below
  for (int v = 0; v < 4; v++) {
    uint8_t sm[64 + 64];
    uint8_t recovered[64 + 64];
    unsigned long long rlen = 0;
    memcpy(sm, sig, 64);
    sm[32] = (uint8_t) ((sm[32] & 0xFC) | v);
    memcpy(sm + 64, msg, msglen);
    if (crypto_sign_open(recovered, &rlen, sm, (unsigned long long) (64 + msglen), pub) == 0)
      return v;
  }
  return -1;
}

inline void flic_sha256(uint8_t out[32], const uint8_t *data, size_t len) {
  mbedtls_sha256(data, len, out, 0);  // is224 = 0 -> SHA-256
}

inline void flic_hmac_sha256(uint8_t out[32], const uint8_t *key, size_t keylen, const uint8_t *data,
                             size_t datalen) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, key, keylen, data, datalen, out);
}

// FullVerify KDF, matching pyflic-ble derive_full_verify_keys:
//   secret          = SHA256(shared || variant || device_random || client_random || client_flags)
//   verifier        = HMAC-SHA256(secret, "AT")[:16]
//   pairing_material = HMAC-SHA256(secret, "PK")
//   pairing_id      = LE u32 of pairing_material[0:4]; pairing_key = pairing_material[4:20]
// (the "SK" session key is not derived here — after pairing we reconnect + quick-verify).
// client_flags is the only per-family difference: 0x00 for Twist, 0x80 (supportsDuo) for Flic 2.
inline void flic_derive_full_verify_keys(const uint8_t shared[32], uint8_t variant,
                                         const uint8_t device_random[8], const uint8_t client_random[8],
                                         uint8_t client_flags, uint8_t out_verifier[16],
                                         uint8_t out_pairing_key[16], uint32_t *out_pairing_id) {
  uint8_t concat[32 + 1 + 8 + 8 + 1];
  size_t o = 0;
  memcpy(concat + o, shared, 32);
  o += 32;
  concat[o++] = variant;
  memcpy(concat + o, device_random, 8);
  o += 8;
  memcpy(concat + o, client_random, 8);
  o += 8;
  concat[o++] = client_flags;  // Twist 0x00 / Flic 2 0x80 (supportsDuo)

  uint8_t secret[32];
  flic_sha256(secret, concat, o);

  uint8_t at[32], pk[32];
  flic_hmac_sha256(at, secret, 32, (const uint8_t *) "AT", 2);
  memcpy(out_verifier, at, 16);

  flic_hmac_sha256(pk, secret, 32, (const uint8_t *) "PK", 2);
  *out_pairing_id = (uint32_t) pk[0] | ((uint32_t) pk[1] << 8) | ((uint32_t) pk[2] << 16) |
                    ((uint32_t) pk[3] << 24);
  memcpy(out_pairing_key, pk + 4, 16);
}

}  // namespace flic
}  // namespace esphome

#endif  // USE_ESP32

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#pragma once
// Chaskey-LTS (16-round) MAC, ported bit-for-bit from pyflic-ble's security.py and verified
// bit-exact against it (8,400 test vectors). This is the ONLY cryptographic primitive the Flic Twist reconnect ("quick
// verify") + authenticated-write path needs — no X25519/Ed25519/SHA on this path.
//
// keys[] is a 12-word subkey table: [0..3] = k0 (the raw 16-byte key as 4 LE u32),
// [4..7] = k1 = times2(k0), [8..11] = k2 = times2(k1).
//
// CAUTION (see the verified pitfall list): little-endian throughout; the seed block
// (counter/direction) is permuted BEFORE any data is mixed; a full final 16-byte block
// finalizes with k1, a partial block with k2; the 5-byte MAC is LE32(v0) + low byte of v1.

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace esphome {
namespace flic {

inline uint32_t chaskey_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t chaskey_load_le32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

inline void chaskey_store_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) (v & 0xFF);
  p[1] = (uint8_t) ((v >> 8) & 0xFF);
  p[2] = (uint8_t) ((v >> 16) & 0xFF);
  p[3] = (uint8_t) ((v >> 24) & 0xFF);
}

// The 16-round ARX permutation core (rotations 5,16,8,16,7,13 as in security.py).
inline void chaskey_permute(uint32_t &v0, uint32_t &v1, uint32_t &v2, uint32_t &v3) {
  for (int i = 0; i < 16; i++) {
    v0 += v1;
    v1 = v0 ^ chaskey_rotl(v1, 5);
    v2 = v3 + chaskey_rotl(v2, 16);
    v3 = v2 ^ chaskey_rotl(v3, 8);
    v2 += v1;
    v0 = v3 + chaskey_rotl(v0, 16);
    v1 = v2 ^ chaskey_rotl(v1, 7);
    v3 = v0 ^ chaskey_rotl(v3, 13);
  }
}

// times2 in GF(2^128) with reduction constant 0x87. Each word is computed from the
// not-yet-updated lower word, matching security.py exactly.
inline void chaskey_times2_(uint32_t &v0, uint32_t &v1, uint32_t &v2, uint32_t &v3) {
  uint32_t c = ((v3 >> 31) & 1u) * 0x87u;
  v3 = (v3 << 1) | (v2 >> 31);
  v2 = (v2 << 1) | (v1 >> 31);
  v1 = (v1 << 1) | (v0 >> 31);
  v0 = (v0 << 1) ^ c;
}

// Build the 12-word subkey table (k0, k1, k2) from a 16-byte key.
inline void chaskey_subkeys(const uint8_t key[16], uint32_t out[12]) {
  uint32_t v0 = chaskey_load_le32(key + 0);
  uint32_t v1 = chaskey_load_le32(key + 4);
  uint32_t v2 = chaskey_load_le32(key + 8);
  uint32_t v3 = chaskey_load_le32(key + 12);
  out[0] = v0;
  out[1] = v1;
  out[2] = v2;
  out[3] = v3;
  chaskey_times2_(v0, v1, v2, v3);  // k1
  out[4] = v0;
  out[5] = v1;
  out[6] = v2;
  out[7] = v3;
  chaskey_times2_(v0, v1, v2, v3);  // k2
  out[8] = v0;
  out[9] = v1;
  out[10] = v2;
  out[11] = v3;
}

// 5-byte MAC with a direction (always 1 = client->button here) and a 64-bit packet
// counter seeded into the state before the message. `data` must be non-empty.
inline void chaskey_mac_dir_counter(const uint32_t keys[12], uint32_t direction, uint64_t counter,
                                    const uint8_t *data, size_t len, uint8_t out_mac[5]) {
  uint32_t v0 = keys[0] ^ (uint32_t) (counter & 0xFFFFFFFFu);
  uint32_t v1 = keys[1] ^ (uint32_t) ((counter >> 32) & 0xFFFFFFFFu);
  uint32_t v2 = keys[2] ^ direction;
  uint32_t v3 = keys[3];

  size_t offset = 0;
  size_t length = len;
  bool first = true;

  while (true) {
    int keys_offset = 0;

    if (!first) {
      if (length >= 16) {
        v0 ^= chaskey_load_le32(data + offset);
        v1 ^= chaskey_load_le32(data + offset + 4);
        v2 ^= chaskey_load_le32(data + offset + 8);
        v3 ^= chaskey_load_le32(data + offset + 12);
        offset += 16;
        length -= 16;
        if (length == 0)
          keys_offset = 4;  // exact final block -> k1
      } else {
        uint8_t tmp[16] = {0};
        memcpy(tmp, data + offset, length);
        tmp[length] = 0x01;  // pad marker, remainder already zeroed
        v0 ^= chaskey_load_le32(tmp + 0);
        v1 ^= chaskey_load_le32(tmp + 4);
        v2 ^= chaskey_load_le32(tmp + 8);
        v3 ^= chaskey_load_le32(tmp + 12);
        keys_offset = 8;  // partial final block -> k2
      }
      if (keys_offset != 0) {
        v0 ^= keys[keys_offset];
        v1 ^= keys[keys_offset + 1];
        v2 ^= keys[keys_offset + 2];
        v3 ^= keys[keys_offset + 3];
      }
    } else {
      first = false;  // first pass permutes the seed only (no data)
    }

    v2 = chaskey_rotl(v2, 16);
    chaskey_permute(v0, v1, v2, v3);
    v2 = chaskey_rotl(v2, 16);

    if (keys_offset != 0) {
      v0 ^= keys[keys_offset];
      v1 ^= keys[keys_offset + 1];
      chaskey_store_le32(out_mac, v0);
      out_mac[4] = (uint8_t) (v1 & 0xFF);
      return;
    }
  }
}

// Single-block MAC over exactly 16 bytes, used for the quick-verify session-key KDF.
// Init is k0 ^ k1 ^ data (NOT just k1 ^ data).
inline void chaskey_16(const uint32_t keys[12], const uint8_t data[16], uint8_t out[16]) {
  uint32_t v0 = keys[0] ^ keys[4] ^ chaskey_load_le32(data + 0);
  uint32_t v1 = keys[1] ^ keys[5] ^ chaskey_load_le32(data + 4);
  uint32_t v2 = keys[2] ^ keys[6] ^ chaskey_load_le32(data + 8);
  uint32_t v3 = keys[3] ^ keys[7] ^ chaskey_load_le32(data + 12);

  v2 = chaskey_rotl(v2, 16);
  chaskey_permute(v0, v1, v2, v3);
  v2 = chaskey_rotl(v2, 16);

  v0 ^= keys[4];
  v1 ^= keys[5];
  v2 ^= keys[6];
  v3 ^= keys[7];

  chaskey_store_le32(out + 0, v0);
  chaskey_store_le32(out + 4, v1);
  chaskey_store_le32(out + 8, v2);
  chaskey_store_le32(out + 12, v3);
}

}  // namespace flic
}  // namespace esphome

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#pragma once
// Decoder for the Flic Duo's ButtonEventDuoNotification payload (Flic Duo protocol specification,
// "Event encoding"). Header-only and free of ESPHome dependencies so it can be tested on a host
// against the spec. FlicDuo (flic_duo.cpp) feeds it every notification and acts on the updates.
//
// The payload is a little-endian bit stream of variable-length updates. Per update: button (1 bit);
// for the first update of each button in a packet an event-counter diff (0 -> 0, 10 -> 1,
// 11 + idx:2 -> extract_bits({2,4,8,32}[idx])), plus one, added to that button's count (later
// updates add one); a timestamp delta of extract_bits({8,10,13,16,24,32,40,48}[extract_bits(3)])
// ms; until the end-of-queue marker has been seen, the queue markers; the event type (3 bits) with
// one extra bit for types 4 and 7; gesture bits for "up" types (0-4) and the single-click timeout
// (6); and the accelerometer x/y/z as signed bytes. Decoding stops when less than a byte remains.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace flic_twist {

// Per-session decoder state. Before the first notification of a session: ts_ms = 0, count = the
// init response's event counts, end_of_queue = !has_queued_events.
struct DuoDecoderState {
  uint64_t ts_ms{0};
  uint32_t count[2] = {0, 0};
  bool end_of_queue{false};
};

struct DuoUpdate {
  uint8_t button;    // 0 = big, 1 = small
  uint8_t type;      // 0-2 up (<0.5 s / 0.5-1 s / >=1 s), 3-4 up ending a double click, 5 down,
                     // 6 single-click timeout, 7 hold
  bool flag;         // type 4: the double click was also a hold; type 7: the next up is a double click
  int8_t gesture;    // -1 none, -2 performed but not recognised, 0-3 left / right / up / down
  bool queued;       // happened before this session (replayed from the device's queue)
  int8_t accel[3];   // x, y, z; 64.036875 per g
  uint32_t count;    // event_count[button] for this update
  uint64_t ts_ms;    // device time, ms since its boot
};

// Little-endian bit reader: bits(n) returns the next n bits, first bit least significant. Reading
// past the end sets ok() false and returns 0.
class DuoBitReader {
 public:
  DuoBitReader(const uint8_t *data, size_t len) : data_(data), nbits_(len * 8) {}
  size_t remaining() const { return this->nbits_ - this->pos_; }
  bool ok() const { return this->ok_; }
  uint64_t bits(unsigned width) {
    if (width > this->remaining()) {
      this->ok_ = false;
      this->pos_ = this->nbits_;
      return 0;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < width; i++, this->pos_++) {
      if ((this->data_[this->pos_ >> 3] >> (this->pos_ & 7)) & 1)
        v |= (uint64_t) 1 << i;
    }
    return v;
  }

 protected:
  const uint8_t *data_;
  size_t nbits_;
  size_t pos_{0};
  bool ok_{true};
};

// Decode every complete update in `data` (the payload after the opcode), advancing `st` and calling
// on_update(const DuoUpdate &) for each. A truncated trailing update is dropped and leaves `st` as
// it was before it; the return value is false in that case.
template<typename F> bool duo_decode_events(DuoDecoderState &st, const uint8_t *data, size_t len, F &&on_update) {
  static const uint8_t COUNT_BITS[4] = {2, 4, 8, 32};
  static const uint8_t TS_BITS[8] = {8, 10, 13, 16, 24, 32, 40, 48};
  DuoBitReader r(data, len);
  bool first_for_button[2] = {true, true};
  while (r.remaining() >= 8) {
    const DuoDecoderState snapshot = st;
    const bool first0 = first_for_button[0], first1 = first_for_button[1];

    DuoUpdate u{};
    u.button = (uint8_t) r.bits(1);
    if (first_for_button[u.button]) {
      uint32_t diff = (uint32_t) r.bits(1);
      if (diff != 0 && r.bits(1) != 0)
        diff = (uint32_t) r.bits(COUNT_BITS[r.bits(2)]);
      st.count[u.button] += diff + 1;
      first_for_button[u.button] = false;
    } else {
      st.count[u.button] += 1;
    }
    st.ts_ms += r.bits(TS_BITS[r.bits(3)]);

    u.queued = false;
    if (!st.end_of_queue) {
      if (r.bits(1) == 0) {
        u.queued = true;  // still inside the queue
      } else {
        st.end_of_queue = true;
        u.queued = r.bits(1) == 0;  // 0: the last queued event; 1: the first live one
      }
    }

    u.type = (uint8_t) r.bits(3);
    u.flag = (u.type == 4 || u.type == 7) ? r.bits(1) != 0 : false;
    u.gesture = -1;
    if (u.type <= 4 || u.type == 6) {
      if (r.bits(1) != 0)
        u.gesture = r.bits(1) != 0 ? (int8_t) r.bits(2) : (int8_t) -2;
    }
    for (int i = 0; i < 3; i++)
      u.accel[i] = (int8_t) (uint8_t) r.bits(8);

    if (!r.ok()) {
      st = snapshot;
      first_for_button[0] = first0;
      first_for_button[1] = first1;
      return false;
    }
    // An up or down not preceded by a hold / single-click timeout skips a count.
    if (u.type <= 5 && (st.count[u.button] % 2) == 0)
      st.count[u.button]++;
    u.count = st.count[u.button];
    u.ts_ms = st.ts_ms;
    on_update(u);
  }
  return true;
}

}  // namespace flic_twist
}  // namespace esphome

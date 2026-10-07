// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov

#pragma once
// Flic 2 button-event codes (Flic 2 protocol specification, "Processing Button Events"), mapped for
// the spec's "single click / double click / hold" use case. Header-only, host-tested
// (tests/flic).
//
// event_encoded (4 bits): bits 0-1 = up / down / single-click timeout / hold. With bit 3 set it is
// an up carrying wasHold (bit 2), singleClick (bit 1 && !bit 0) and doubleClick (bit 1 && bit 0).
// 7 = a hold during the second press of a double click (that press ends as the double click).
// pyflic-ble maps only 2 / 11 / 3, which loses a click released after 0.5-1 s (10) and a double
// click whose second press was held (15).

#include <cstdint>

namespace esphome {
namespace flic {

// "click" / "double_click" / "hold", or nullptr for codes that are no such event.
inline const char *flic2_event_type(uint8_t e) {
  if (e & 0x08) {
    const bool was_hold = (e & 0x04) != 0;
    const bool click_info = (e & 0x02) != 0;
    const bool second = (e & 0x01) != 0;
    if (click_info && second)
      return "double_click";  // 11, 15
    if (click_info && !was_hold)
      return "click";  // 10: released after 0.5-1 s, no double click possible
    return nullptr;    // 8 / 9 / 12 / 13 / 14: an up that is no (new) click
  }
  switch (e & 0x03) {
    case 2:
      return "click";  // single-click timeout after a short press
    case 3:
      return e == 7 ? nullptr : "hold";
    default:
      return nullptr;  // up / down
  }
}

// The spec's acknowledgement rule: (up && (singleClick || doubleClick)) || single-click timeout.
inline bool flic2_event_needs_ack(uint8_t e) { return (e & 0x08) ? (e & 0x02) != 0 : (e & 0x03) == 2; }

}  // namespace flic
}  // namespace esphome

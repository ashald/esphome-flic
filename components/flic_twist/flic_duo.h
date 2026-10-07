// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#pragma once
// flic_duo: the Flic Duo member of the Flic client family. Two buttons (big = 0, small = 1), an
// accelerometer with swipe-gesture recognition, and push-twist: rotating the Duo while a button is
// held. It speaks the Flic 2 protocol (FlicButton: 0042 GATT, framing, verify with supports_duo,
// battery, firmware, ping, SetAdvParameters) plus the extensions in Flic's Duo protocol
// specification (github.com/50ButtonsEach/flic2-documentation/wiki/Flic-Duo-Protocol-Specification):
//   - InitButtonEventsDuoLightRequest (35) / responses 30-31, with a per-button event counter,
//   - bit-packed ButtonEventDuoNotification (32), decoded with per-session state, ACKed with 36,
//   - EnablePushTwistInd (37) and PushTwistDataNotification (33): angle deltas while a button is
//     held, 65536 units per turn, positive = clockwise.
//
// Surface per Duo: one event entity per button (click / double_click / hold, swipe_left / right /
// up / down, rotate_clockwise / rotate_counter_clockwise) and one 0-100 % dial number per button.
//
// The event / dial behaviour (swipes replace clicks, twists drop their click and hold, rotation
// gating and backlash filtering) lives in duo_input.h, the bit-stream decoder in duo_codec.h; both
// are header-only and host-tested (tests/flic_twist).
//
// The dial: `dial_range` of rotation (default 90 degrees) spans 0-100 %, clamped. A rotate event
// fires per notification in which the rotation crossed at least one 1 %-of-range step, measured on
// the unclamped travel so relative control keeps working at either end. The dial is virtual (the
// Duo has no position indicator): writing the number re-bases it, e.g. to a light's current
// brightness so the next twist continues from there. Persisted in NVS like the Twist's rings.

#include "duo_codec.h"
#include "duo_input.h"
#include "flic_button.h"

#ifdef USE_ESP32

namespace esphome {
namespace flic_twist {

class FlicDuo : public FlicButton, public DuoInputListener {
 public:
  void dump_config() override;

  // `button`: 0 = big, 1 = small.
  void set_duo_button_event(int button, event::Event *e) {
    if (button == 0 || button == 1)
      this->duo_events_[button] = e;
  }
  // Rotation (65536 units per turn) that spans the dial's 0-100 %.
  void set_dial_range_units(int32_t units) { this->input_.set_dial_range(units); }

  // Generic position interface: selector = button (0 big, 1 small, -1 = big). Re-bases that
  // button's dial; always in effect at once (the dial lives on the hub).
  bool write_position(float percentage, int selector) override;

 protected:
  const char *device_kind_() const override { return "Flic Duo"; }
  const char *device_type_id_() const override { return "duo"; }
  bool expects_duo_() const override { return true; }
  void on_setup_() override;
  void on_session_established_() override;  // Duo init (35) + enable push-twist (37)
  void on_reset_session_() override;
  bool handle_session_frame_(const uint8_t *data, uint16_t len) override;

  void send_init_button_events_duo_();
  void send_enable_push_twist_();
  void parse_init_response_duo_(const uint8_t *data, uint16_t len);
  void parse_button_events_duo_(const uint8_t *data, uint16_t len);  // opcode stripped
  void parse_push_twist_(const uint8_t *data, uint16_t len);         // includes opcode
  void send_ack_duo_();
  void publish_dial_(uint8_t button);
  void save_dials_();
  // DuoInputListener
  void on_duo_event(uint8_t button, const char *type) override;
  void on_duo_dial(uint8_t button, int32_t units) override;

  event::Event *duo_events_[2] = {nullptr, nullptr};
  DuoInput input_;

  // Event decoding state (spec "Event encoding", duo_codec.h): reset per session, counts seeded
  // from the init response.
  DuoDecoderState decoder_;
  // Catch-up state sent in the next init (RAM; survives reconnects, not hub reboots).
  uint32_t catchup_count_[2] = {0, 0};

  struct StoredDials {
    uint32_t magic;
    int32_t units[2];
  } __attribute__((packed));
  ESPPreferenceObject dial_pref_;
};

}  // namespace flic_twist
}  // namespace esphome

#endif  // USE_ESP32

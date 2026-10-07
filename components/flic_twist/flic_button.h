// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#pragma once
// flic_button: the Flic 2 (button) member of the Flic client family, and the base of the Flic Duo
// (flic_duo.h), which speaks the same protocol plus its own event and push-twist extensions. Owns
// the BLE GATT link to one Flic 2 (via FlicClientBase) and surfaces its click / double-click / hold
// events, battery and firmware version.
//
// The shared machinery (BLE lifecycle, NVS creds, the verify handshake, the authenticated write
// path, housekeeping, common entities) lives in FlicClientBase. Flic 2 differs from the Twist in
// exactly the ways this class supplies: the 0042 GATT family, the Flic 2 verify constants (qv flag
// 0x40, fv flag 0x80 = supports_duo, the Flic 2 Ed25519 key), a 1-byte connId/fragment FRAME
// HEADER (with fragmentation) that the Twist lacks, its own opcode numbering, 7-byte button-event
// slots (no rotation/selector), the event-catch-up + ACK model, and a 10-bit ADC battery reading of
// its CR2032 cell. Mirrors pyflic-ble handlers/flic2.py and Flic's protocol specification
// (github.com/50ButtonsEach/flic2-documentation/wiki), including the session duties pyflic-ble
// skips: answering the button's ping requests, honouring a session the button terminates, and
// SetAdvParameters (keep advertising after a lost link so the hub can reconnect without a press).

#include "flic_client_base.h"

#ifdef USE_ESP32

namespace esphome {
namespace flic_twist {

class FlicButton : public FlicClientBase {
 public:
  void dump_config() override;

  // How the button advertises after it loses the link (SetAdvParametersRequest; firmware >= 7):
  // 100 ms for the first 5 s, then every `interval_units` (x 0.625 ms) for `timeout_s` seconds, so
  // the hub can reconnect without a press. timeout_s == 0 restores the button's own default (a
  // short burst, after which it stays silent until pressed) by removing ours.
  void set_reconnect_advertising(uint16_t interval_units, uint32_t timeout_s) {
    this->adv_interval_units_ = interval_units;
    this->adv_timeout_s_ = timeout_s;
  }

 protected:
  // ---- FlicClientBase protocol hooks ----
  const char *service_uuid_() const override;
  const char *tx_char_uuid_() const override;
  const char *rx_char_uuid_() const override;
  const char *device_kind_() const override { return "Flic 2"; }
  const char *device_type_id_() const override { return "flic2"; }
  bool is_flic2_family_() const override { return true; }
  uint8_t verify_flag_qv_() const override { return 0x40; }  // supportsDuo (bit6)
  uint8_t verify_flag_fv_() const override { return 0x80; }  // supportsDuo (bit7)
  const uint8_t *verify_ed25519_key_() const override;
  uint8_t op_quick_verify_response_() const override { return 8; }
  uint8_t op_quick_verify_negative_() const override { return 6; }
  // Flic 2 prepends a 1-byte frame header (connId | fragment-more) and fragments large packets;
  // the MAC (already appended by write_authenticated_) covers opcode+payload, NOT the header.
  void emit_frame_(const uint8_t *body, size_t len) override;
  // Reassemble fragments + strip the frame header, then route the assembled frame.
  void on_ble_notify_(const uint8_t *data, uint16_t len) override;
  void on_session_established_() override;  // send init-button-events (opcode 23, autoDisconnect=511)
  void on_reset_session_() override;
  void request_battery_() override;           // opcode 20 -> response 20 [raw:u16], V = raw*3.6/1024
  void request_firmware_version_() override;  // opcode 8 -> response 5 [version:u32]
  float battery_level_from_voltage_(float volts) const override;  // CR2032 manufacturer curve
  // Refuse a bond with the wrong member of the family (the pairing response says is_duo).
  bool accept_paired_device_(const uint8_t *data, uint16_t len) override;
  virtual bool expects_duo_() const { return false; }

  // ---- Flic 2 protocol ----
  void dispatch_frame_(const uint8_t *data, uint16_t len);  // de-framed [opcode][payload][MAC]
  // An established-session frame (MAC stripped). Returns false if unhandled. The Duo overrides it
  // for its own event packets and falls back to handle_common_frame_ for the shared ones.
  virtual bool handle_session_frame_(const uint8_t *data, uint16_t len);
  // Opcodes shared by Flic 2 and Duo: ping, session terminated, firmware, battery, adv parameters.
  bool handle_common_frame_(const uint8_t *data, uint16_t len);
  // Button events are initialised (init response in): request the firmware version, which gates
  // SetAdvParameters (sent when the version arrives).
  void on_events_initialized_();
  void send_adv_parameters_();
  void send_init_button_events_();
  void parse_button_events_(const uint8_t *data, size_t len);  // opcode already stripped
  void parse_init_response_(const uint8_t *data, size_t len, bool with_boot_id);
  void send_ack_(uint32_t event_counter);
  const char *map_event_(uint8_t encoded) const;

  // logical connection id assigned by the button (frame header bits 0-4); 0 until assigned
  uint8_t conn_id_{0};
  // fragment reassembly buffer (max reassembled Flic 2 frame ~128 B; FullVerifyResponse1 = 117 B)
  uint8_t reasm_[160] = {0};
  uint16_t reasm_len_{0};
  // event catch-up state (persisted in RAM across reconnects; reset only on device reboot)
  uint32_t last_event_count_{0};
  uint32_t last_boot_id_{0};
  // firmware version from this session's response (0 = not known yet); gates SetAdvParameters
  uint32_t fw_version_{0};
  bool adv_params_sent_{false};  // per session
  uint16_t adv_interval_units_{1600};  // 1 s
  uint32_t adv_timeout_s_{86400};      // 24 h
};

}  // namespace flic_twist
}  // namespace esphome

#endif  // USE_ESP32

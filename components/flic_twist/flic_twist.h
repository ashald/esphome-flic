// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#pragma once
// flic_twist: the Flic Twist member of the Flic client family. Owns the BLE GATT link to one
// Twist (via FlicClientBase), decodes its button + rotation events, mirrors the knob into writable
// position numbers, tracks the selector mode, and writes the LED ring back.
//
// Everything protocol-generic (BLE lifecycle, NVS creds, the verify handshake, the authenticated
// write path, firmware/battery/RSSI housekeeping, the common entities) lives in FlicClientBase.
// This class supplies only the Twist specifics: the 00c9 GATT family, the Twist verify constants
// (both flag bytes 0x00, the Twist Ed25519 key), the headerless framing, and the Twist
// event/rotation/position protocol.
//
// Ring positions: the Twist has 13 "modes" (12 selector slots, chosen by push-twisting, + mode 12
// for push-twist/continuous use). Each mode has its own 0-100% ring position. The hub remembers a
// DESIRED position per mode (persisted in NVS): it is applied whenever a session comes up (seeded
// into the init packet, which is how the Twist restores ring positions across connections), so a
// write that lands while the Twist is disconnected is not lost and a hub reboot does not bring the
// ring up dark. Writes come from the Position number entities (FlicPositionNumber, bound to the
// active mode or to a fixed mode) or from the flic_twist.set_position action.

#include "flic_client_base.h"

#ifdef USE_ESP32

#include "chaskey.h"
#include "rotate_tracker.h"

namespace esphome {
namespace flic_twist {

enum PushTwistMode : uint8_t {
  PTM_DEFAULT = 0,
  PTM_SELECTOR = 1,
  PTM_CONTINUOUS = 2,
};

static const int TWIST_MODE_ACTIVE = -1;  // "whatever mode the Twist is currently in"
static const int TWIST_MODE_COUNT = 13;

class FlicTwist : public FlicClientBase {
 public:
  void dump_config() override;

  void set_push_twist_mode(PushTwistMode m) { this->push_twist_mode_ = m; }
  void set_mode_sensor(sensor::Sensor *s) { this->mode_sensor_ = s; }

  // Set the LED-ring position (0-100%) of `mode` (0-12, or TWIST_MODE_ACTIVE for the current mode).
  // Remembered as the desired position (NVS) even without a session and applied on the next session
  // establishment. Returns true if the write was issued now, false if deferred.
  bool write_twist_position(float percentage, int mode = TWIST_MODE_ACTIVE);
  // Generic position interface: selector = Twist mode 0-12, or -1 for the active mode.
  bool write_position(float percentage, int selector) override {
    return this->write_twist_position(percentage, selector);
  }
  uint8_t active_mode() const { return this->active_mode_; }

 protected:
  // ---- FlicClientBase protocol hooks ----
  const char *service_uuid_() const override;
  const char *tx_char_uuid_() const override;
  const char *rx_char_uuid_() const override;
  const char *device_kind_() const override { return "Twist"; }
  const char *device_type_id_() const override { return "twist"; }
  uint8_t verify_flag_qv_() const override { return 0x00; }
  uint8_t verify_flag_fv_() const override { return 0x00; }
  const uint8_t *verify_ed25519_key_() const override;
  uint8_t op_quick_verify_response_() const override { return 0x06; }
  uint8_t op_quick_verify_negative_() const override { return 0x05; }
  void emit_frame_(const uint8_t *body, size_t len) override;  // Twist is headerless: write as-is
  void on_ble_notify_(const uint8_t *data, uint16_t len) override;
  void on_session_established_() override;  // seed ring positions + send init-button-events
  void request_battery_() override;
  void request_firmware_version_() override;
  float battery_level_from_voltage_(float volts) const override;  // 2xAAA alkaline, rough
  void on_setup_() override;
  void on_reset_session_() override;

  // ---- Twist protocol ----
  void send_init_button_events_();
  void send_update_twist_position_(float percentage, uint8_t mode);  // raw UpdateTwistPosition
  void parse_button_events_(const uint8_t *data, size_t len);  // opcode already stripped
  void parse_twist_event_(const uint8_t *data, size_t len);    // includes opcode
  void parse_battery_response_(const uint8_t *data, size_t len);
  void parse_name_response_(const uint8_t *data, size_t len);
  const char *map_button_event_(uint8_t event_encoded) const;

  // ---- desired ring positions ----
  bool desired_valid_(uint8_t mode) const { return (this->desired_.valid_mask >> mode) & 1; }
  void seed_tracker_from_desired_(uint8_t mode);  // make get_mode_percentage(mode) == desired
  void publish_numbers_for_mode_(uint8_t mode, float pct);
  void set_active_mode_(uint8_t mode);  // + Mode sensor

  PushTwistMode push_twist_mode_{PTM_DEFAULT};

  // runtime state
  MultiModeRotateTracker tracker_;
  uint8_t twist_mode_index_{0};  // last mode/slot seen from events
  // Mode the "active" position numbers read/write. Learned from the first rotation/button event
  // after boot (the protocol has no query); 0 until then.
  uint8_t active_mode_{0};
  // Per-mode "seen a rotation packet this session" flag — cleared on reset so the first
  // packet of each mode only seeds a baseline (no replayed increment/decrement events).
  bool rotate_baseline_valid_[TWIST_MODE_COUNT] = {false};
  uint32_t twist_packet_counter_{0};
  uint32_t last_event_count_{0};
  uint32_t last_boot_id_{0};

  // Desired ring position per mode (integer percent), persisted in NVS (coalesced by the
  // preferences flash_write_interval). See write_twist_position().
  struct DesiredPositions {
    uint32_t magic;
    uint16_t valid_mask;  // bit m set => pct[m] is meaningful
    uint8_t pct[TWIST_MODE_COUNT];
  } __attribute__((packed));
  DesiredPositions desired_{};
  ESPPreferenceObject desired_pref_;

  sensor::Sensor *mode_sensor_{nullptr};
};

}  // namespace flic_twist
}  // namespace esphome

#endif  // USE_ESP32

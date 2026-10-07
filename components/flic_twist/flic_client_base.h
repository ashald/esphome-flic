// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#pragma once
// FlicClientBase: the shared ESP-side BLE client for the Flic protocol family. A dedicated
// ESP node owns the GATT link to one Flic device, holding (and re-establishing) the connection
// independently of Home Assistant. This base owns everything that is identical across the
// Flic Twist, the Flic 2 button and the Flic Duo:
//   - the BLE link lifecycle (service/char discovery, notify subscribe, conn-param update),
//   - NVS credential storage (MAC-keyed, legacy migration) + on-device pairing arming,
//   - the verify handshake (quick-verify reconnect + full-verify pairing) — the request
//     opcodes (5 / 0 / 2) and response field offsets are common to both families; only two
//     flag bytes and the Ed25519 identity key differ, exposed as virtuals below,
//   - the authenticated write path (Chaskey MAC over opcode+payload),
//   - the per-session housekeeping: GAP name read, firmware-version request, battery poll, RSSI,
//   - the common entity surface: type / status / firmware / name / MAC text sensors, connected
//     binary sensor, battery voltage + level and RSSI sensors, the events entity.
//
// Protocol-specific behaviour lives in the derived classes (FlicTwist / FlicButton / FlicDuo, the
// Duo extending FlicButton) behind the
// small set of virtual hooks: the GATT UUIDs, the two verify flag bytes + Ed25519 key, how a frame
// is put on the wire (Twist is headerless; Flic 2 prepends a connId/fragment header and fragments),
// how an inbound notification is de-framed, what to send once the session is up, how the
// established-session opcodes / events are parsed, and the battery/firmware request opcodes and
// battery chemistry curve. This mirrors pyflic-ble's handlers/{base,twist,flic2,duo}.py split.

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#ifdef USE_ESP32

#include "esphome/core/preferences.h"
#ifdef USE_OTA_STATE_LISTENER
#include "esphome/components/ota/ota_backend.h"
#endif
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/button/button.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/event/event.h"
#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>
#include <string>
#include <vector>

namespace esphome {
namespace flic_twist {

namespace espbt = esphome::esp32_ble_tracker;

// Application-layer session state, distinct from the BLE-link node_state.
enum class FlicSession : uint8_t {
  IDLE = 0,             // not connected / no session
  WAIT_QUICK_VERIFY,    // reconnect: quick-verify request sent, awaiting response
  WAIT_FULL_VERIFY_R1,  // pairing: request-1 sent, awaiting device identity + signature
  WAIT_FULL_VERIFY_R2,  // pairing: request-2 sent, awaiting pairing confirmation
  ESTABLISHED,          // session key derived; MACs active; events flow
};

class FlicPositionNumber;

// FlicClientBase is also a tracker ESPBTDeviceListener so it can discover a public-mode Flic
// during mac-optional pairing. It MUST be registered at codegen time (esp32_ble_tracker
// register_ble_device) — that reserves the listener slot, without which ESPHome compiles the
// tracker's listener-dispatch loop out entirely and parse_device is never called.
class FlicClientBase : public ble_client::BLEClientNode,
                       public espbt::ESPBTDeviceListener,
                       public Component {
 public:
  // Registers this slot with the hub-wide OTA handoff and the hub-wide FlicFleet summary. Done at
  // construction, NOT setup(): OTA listeners fire in registration order and ours must run before
  // esp32_ble_tracker's, which registers in its setup().
  FlicClientBase();
  void setup() override;
  // Tracker listener hook: forwarded to on_advert_ (only acts during a mac-optional pair scan).
  bool parse_device(const ble_device_base::ESPBTDevice &device) override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  // Receives GAP events forwarded by ble_client (e.g. ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT).
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;

  // --- config (from Python codegen) ---
  void set_pairing_id(uint32_t id) { this->pairing_id_ = id; }
  void set_pairing_key_hex(const std::string &hex) { this->pairing_key_hex_ = hex; }
  void set_conn_params(uint16_t min_iv, uint16_t max_iv, uint16_t latency, uint16_t timeout) {
    this->conn_min_iv_ = min_iv;
    this->conn_max_iv_ = max_iv;
    this->conn_latency_ = latency;
    this->conn_timeout_ = timeout;
  }
  // Stable per-instance key for NVS credential storage (from codegen).
  void set_storage_hash(uint32_t h) { this->storage_hash_ = h; }

  // True while the authenticated session is up (what the Connected binary sensor shows).
  bool session_up() const { return this->session_ == FlicSession::ESTABLISHED; }
  // True once usable pairing credentials exist (NVS or config).
  bool paired() const { return this->creds_valid_; }

  // Arm a single full-verify pairing attempt (called by the Pair button). Put the device in
  // pairing mode first (hold its button ~7s until the LED ring does the pairing flash).
  void request_pairing();

  // OTA handoff: if a session is up, ask the device to drop the link itself and keep advertising
  // (ForceBtDisconnectInd, restart_adv=1). Returns how long (ms) to hold before the link may be
  // torn down for the message to reach the device, or 0 if nothing was sent.
  uint32_t send_ota_handoff();
  // True while the host still has a BLE link to the device (safe to poll while the loop is held).
  bool link_up();

  // One-shot: log this device's pairing_id + pairing_key (hex) to the ESP log so they can be
  // copied into a new board's config (secrets.yaml) to migrate it WITHOUT re-pairing. Called by
  // the (diagnostic) "dump creds" button. Deliberately NOT a sensor — a bond secret must not
  // live in HA states/history.
  void dump_creds();

  // Wipe this slot's stored credentials so it is free to (re)pair — e.g. to move a device to a
  // different hub. LOCAL only: the device keeps its bond until it is physically put back into
  // public mode and re-paired (the Flic protocol has no over-the-air "forget"). Called by the
  // (diagnostic, disabled-by-default) "unpair" button.
  void unpair();

  // Generic 0-100 % position, used by FlicPositionNumber and the flic_twist.set_position action.
  // `selector` is device-specific: Twist = selector mode 0-12 (-1 = the mode it is in now, the
  // LED ring follows); Duo = dial of button 0 (big) / 1 (small) (-1 = big). Returns true when the
  // value is in effect now (a Twist write is deferred while disconnected). Devices without a
  // position (Flic 2) ignore it.
  virtual bool write_position(float percentage, int selector) { return false; }
  void add_position_number(FlicPositionNumber *n) { this->position_numbers_.push_back(n); }

  // --- child entities (from Python codegen) ---
  void set_button_event(event::Event *e) { this->button_event_ = e; }
  void set_battery_voltage_sensor(sensor::Sensor *s) { this->battery_voltage_sensor_ = s; }
  void set_battery_level_sensor(sensor::Sensor *s) { this->battery_level_sensor_ = s; }
  void set_connected_binary_sensor(binary_sensor::BinarySensor *b) { this->connected_bsensor_ = b; }
  void set_name_text_sensor(text_sensor::TextSensor *t) { this->name_tsensor_ = t; }
  void set_mac_text_sensor(text_sensor::TextSensor *t) { this->mac_tsensor_ = t; }
  void set_status_text_sensor(text_sensor::TextSensor *t) { this->status_tsensor_ = t; }
  void set_type_text_sensor(text_sensor::TextSensor *t) { this->type_tsensor_ = t; }
  void set_firmware_text_sensor(text_sensor::TextSensor *t) { this->fw_tsensor_ = t; }
  void set_rssi_sensor(sensor::Sensor *s) { this->rssi_sensor_ = s; }

 protected:
  // ---- protocol hooks (implemented by FlicTwist / FlicButton) ----
  virtual const char *service_uuid_() const = 0;
  virtual const char *tx_char_uuid_() const = 0;  // host writes here
  virtual const char *rx_char_uuid_() const = 0;   // host subscribes here (notify)
  virtual const char *device_kind_() const = 0;     // for log lines ("Twist" / "Flic 2")
  // Stable machine-readable family id for the Type text sensor: "twist" / "flic2" (/ "duo").
  virtual const char *device_type_id_() const = 0;
  // Quick-verify request+KDF flag byte: Twist 0x00, Flic 2 0x40 (supportsDuo).
  virtual uint8_t verify_flag_qv_() const = 0;
  // Full-verify client flags/variant byte (in the KDF concat AND the request-2 flags): Twist
  // 0x00, Flic 2 0x80 (supportsDuo).
  virtual uint8_t verify_flag_fv_() const = 0;
  virtual const uint8_t *verify_ed25519_key_() const = 0;
  // Response opcodes that differ by family (full-verify resp opcodes 0x00/0x01 are common).
  virtual uint8_t op_quick_verify_response_() const = 0;
  virtual uint8_t op_quick_verify_negative_() const = 0;
  // Put `body` (opcode+payload, MAC already appended by write_authenticated_) on the wire.
  // Twist writes it as-is; Flic 2 prepends its connId/fragment header and fragments if needed.
  virtual void emit_frame_(const uint8_t *body, size_t len) = 0;
  // Deliver an inbound notification. Twist forwards raw; Flic 2 reassembles fragments and strips
  // its frame header first, then routes the assembled frame through handle_verify_notify_ / its
  // own established-session dispatch.
  virtual void on_ble_notify_(const uint8_t *data, uint16_t len) = 0;
  // Session just came up (quick-verify OK): send the protocol's init/subscribe packet.
  virtual void on_session_established_() = 0;
  // Request a battery reading over the authenticated session (protocol-specific opcode). The
  // response handler calls publish_battery_voltage_().
  virtual void request_battery_() {}
  // Request the firmware version over the authenticated session (protocol-specific opcode). The
  // response handler calls publish_firmware_version_().
  virtual void request_firmware_version_() {}
  // Battery chemistry curve: remaining capacity (0-100) for a cell voltage, or a negative value
  // when unknown. Twist = 2xAAA, Flic 2 = CR2032.
  virtual float battery_level_from_voltage_(float volts) const { return -1.0f; }
  // Per-protocol setup / session-reset extras (tracker config, packet counters, reassembly).
  virtual void on_setup_() {}
  virtual void on_reset_session_() {}
  virtual void on_disconnected_() {}
  // True for the Flic 2 / Duo family (used during the mac-optional pair scan to read the 0x030f
  // "connected elsewhere" advert bit). Twist has no such bit.
  virtual bool is_flic2_family_() const { return false; }
  // Pairing just succeeded: `data` is the device's FullVerifyResponse2 (de-framed, MAC included).
  // Return false to refuse the bond — e.g. a Flic Duo answering a Flic 2 slot, which would only
  // half work — and the credentials are not saved.
  virtual bool accept_paired_device_(const uint8_t *data, uint16_t len) { return true; }

  // ---- mac-optional pairing (config MAC omitted -> ble_client address 0; learn it on pair) ----
  void on_advert_(const ble_device_base::ESPBTDevice &device);  // called by parse_device (tracker listener)
  void start_pair_scan_();
  void commit_pair_candidate_();

  // ---- shared verify handshake ----
  void start_session_();  // after link ready: pairing? full-verify : quick-verify
  void start_quick_verify_();
  void start_full_verify_();
  void handle_quick_verify_response_(const uint8_t *data, uint16_t len);
  void handle_full_verify_response1_(const uint8_t *data, uint16_t len);
  void handle_full_verify_result_(const uint8_t *data, uint16_t len, bool success);
  // Route a de-framed inbound frame through the verify state machine. Returns true if it was a
  // verify-phase packet (handled here); false means the session is ESTABLISHED and the caller
  // should dispatch it as a protocol opcode.
  bool handle_verify_notify_(const uint8_t *data, uint16_t len);
  void end_pairing_(bool success);

  // ---- shared write path ----
  void write_authenticated_(const uint8_t *opcode_payload, size_t len);  // append MAC, emit_frame_
  void write_raw_(const uint8_t *data, size_t len);                       // emit_frame_ (no MAC)
  void gattc_write_(const uint8_t *data, size_t len);                     // raw GATT write

  // ---- shared creds / session / entities ----
  bool load_creds_();
  void save_creds_(uint32_t pairing_id, const uint8_t key[16]);
  void publish_link_status_();  // Status text sensor + FlicFleet refresh
  void publish_battery_voltage_(float volts);  // voltage sensor + level via the family curve
  void publish_firmware_version_(uint32_t version);
  void reset_session_();  // resets base session state, then on_reset_session_()
  void update_conn_params_();
  bool get_link_params_(esp_gap_conn_params_t *p);  // negotiated params; false once the link is gone
  void trigger_event_(const char *type);

  // config
  uint32_t pairing_id_{0};
  std::string pairing_key_hex_;
  uint8_t pairing_key_[16] = {0};
  bool creds_valid_{false};   // usable quick-verify creds present (from NVS or config)
  uint32_t storage_hash_{0};  // PRIMARY NVS creds key = md5(config id) — the stable slot identity
  uint32_t mac_hash_{0};      // legacy NVS key = hash(config MAC); read once to migrate old bonds
  ESPPreferenceObject pref_;
  uint16_t conn_min_iv_{80}, conn_max_iv_{90}, conn_latency_{17}, conn_timeout_{800};

  // The config MAC is only a discovery filter. mac_optional_ (no config MAC) => Pair scans for a
  // public-mode device of this type. learned_mac_ caches the last-bonded device MAC in its own NVS
  // slot for ALL slots, so a slot reconnects even after its config MAC is removed from config.
  bool mac_optional_{false};
  uint64_t learned_mac_{0};
  ESPPreferenceObject learned_mac_pref_;
  bool pair_scan_active_{false};
  uint64_t pair_candidate_{0};
  int pair_best_rssi_{-127};
  uint16_t pair_adverts_seen_{0};  // diagnostic: total adverts on_advert_ saw this scan window

  // BLE handles
  uint16_t notify_handle_{0};
  uint16_t write_handle_{0};
  uint16_t gap_name_handle_{0};  // GAP Device Name (0x2A00) characteristic handle, if present
  esp_err_t last_write_err_{ESP_OK};  // sticky: set by a failed gattc_write_, reset by the caller

  // session state
  FlicSession session_{FlicSession::IDLE};
  uint32_t chaskey_keys_[12] = {0};
  uint64_t counter_to_button_{0};
  uint8_t client_random_[7] = {0};
  uint32_t tmp_id_{0};
  uint32_t established_ms_{0};
  uint32_t last_battery_ms_{0};
  uint32_t last_rssi_ms_{0};

  // pairing (full verify) — transient per attempt
  bool pairing_mode_{false};
  uint8_t pairing_priv_[32] = {0};
  uint8_t pairing_client_random_[8] = {0};
  uint32_t full_verify_tmp_id_{0};
  uint32_t pending_pairing_id_{0};       // derived in R1, committed on R2 success
  uint8_t pending_pairing_key_[16] = {0};

  // entities (shared across families)
  event::Event *button_event_{nullptr};
  sensor::Sensor *battery_voltage_sensor_{nullptr};
  sensor::Sensor *battery_level_sensor_{nullptr};
  binary_sensor::BinarySensor *connected_bsensor_{nullptr};
  text_sensor::TextSensor *name_tsensor_{nullptr};
  text_sensor::TextSensor *mac_tsensor_{nullptr};
  text_sensor::TextSensor *status_tsensor_{nullptr};
  text_sensor::TextSensor *type_tsensor_{nullptr};
  text_sensor::TextSensor *fw_tsensor_{nullptr};
  sensor::Sensor *rssi_sensor_{nullptr};
  std::vector<FlicPositionNumber *> position_numbers_;  // Twist ring / Duo dial numbers
  bool name_requested_{false};  // read the device name once per session
  bool fw_requested_{false};    // request the firmware version once per session
  bool mac_published_{false};
};

// Hub-wide summary of every Flic slot on this node (one instance per ESP). Slots register from
// their constructor; the optional node-level sensors (packages/flic-hub.yaml) are published,
// deduplicated, on every slot's link-status change:
//   slots      — slots configured on this node,
//   paired     — slots holding pairing credentials,
//   connected  — slots with an authenticated session up.
class FlicFleet {
 public:
  static FlicFleet *get();
  void add(FlicClientBase *c) { this->clients_.push_back(c); }
  void set_slots_sensor(sensor::Sensor *s) { this->slots_ = s; }
  void set_paired_sensor(sensor::Sensor *s) { this->paired_ = s; }
  void set_connected_sensor(sensor::Sensor *s) { this->connected_ = s; }
  // Recompute and publish whatever changed. Cheap; called from FlicClientBase::publish_link_status_.
  void refresh();

 protected:
  std::vector<FlicClientBase *> clients_;
  sensor::Sensor *slots_{nullptr};
  sensor::Sensor *paired_{nullptr};
  sensor::Sensor *connected_{nullptr};
  int last_slots_{-1};
  int last_paired_{-1};
  int last_connected_{-1};
};

// Momentary "Pair" button: arms one full-verify pairing attempt on the next connect. Put the
// device in pairing mode (hold its button ~7s) before pressing.
class FlicPairButton : public button::Button, public Parented<FlicClientBase> {
 protected:
  void press_action() override { this->parent_->request_pairing(); }
};

// Diagnostic "dump creds" button: logs pairing_id + pairing_key once so they can be copied to a
// replacement board's config (migrate without re-pairing). Disabled-by-default in HA.
class FlicDumpCredsButton : public button::Button, public Parented<FlicClientBase> {
 protected:
  void press_action() override { this->parent_->dump_creds(); }
};

// Diagnostic "unpair" button: wipes this slot's stored creds so it can be re-paired (e.g. moved to
// another hub). Destructive — disabled-by-default in HA; enable it only when intentionally freeing
// a slot.
class FlicUnpairButton : public button::Button, public Parented<FlicClientBase> {
 protected:
  void press_action() override { this->parent_->unpair(); }
};

// Writable 0-100 % position: a Twist's LED-ring position (bound to the active selector mode, or to
// a fixed mode with `twist_mode:`) or a Duo button's dial (`duo_button:`). Physical rotation
// mirrors into it; writing it moves the ring / re-bases the dial. The two-way source of truth a
// light or a media volume tracks.
class FlicPositionNumber : public number::Number, public Parented<FlicClientBase> {
 public:
  void set_selector(int selector) { this->selector_ = selector; }
  int selector() const { return this->selector_; }

 protected:
  void control(float value) override {
    // Only reflect the value in HA once it is in effect; a Twist write deferred while disconnected
    // is published when it is applied on reconnect, so the entity shows what the ring really shows.
    if (this->parent_->write_position(value, this->selector_))
      this->publish_state(value);
  }
  int selector_{-1};
};

// Automation action flic_twist.set_position { id, position: 0-100, twist_mode: 0-12 | duo_button }.
template<typename... Ts> class SetPositionAction : public Action<Ts...>, public Parented<FlicClientBase> {
 public:
  TEMPLATABLE_VALUE(float, position)
  TEMPLATABLE_VALUE(int, selector)

  void play(Ts... x) override {
    const int selector = this->selector_.has_value() ? this->selector_.value(x...) : -1;
    this->parent_->write_position(this->position_.value(x...), selector);
  }
};

}  // namespace flic_twist
}  // namespace esphome

#endif  // USE_ESP32

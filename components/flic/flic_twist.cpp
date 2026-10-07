// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#include "flic_twist.h"

#ifdef USE_ESP32

#include "chaskey.h"
#include "flic_crypto.h"  // TWIST_ED25519_PUBLIC_KEY
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <cmath>

namespace esphome {
namespace flic {

static const char *const TAG = "flic_twist";

// Flic Twist GATT (from pyflic-ble const.py — the Twist uses the 00c9 family for BOTH
// discovery AND comms; host WRITES ...0001 and SUBSCRIBES ...0002).
static const char *const TWIST_SERVICE_UUID = "00c90000-2cbd-4f2a-a725-5ccd960ffb7d";
static const char *const TWIST_TX_CHAR_UUID = "00c90001-2cbd-4f2a-a725-5ccd960ffb7d";  // write
static const char *const TWIST_RX_CHAR_UUID = "00c90002-2cbd-4f2a-a725-5ccd960ffb7d";  // notify

// Twist established-session opcodes (verify opcodes live in FlicClientBase). See pyflic-ble.
static const uint8_t OP_GET_FIRMWARE_VERSION_REQUEST = 0x07;
static const uint8_t OP_GET_FIRMWARE_VERSION_RESPONSE = 0x04;
static const uint8_t OP_DISCONNECTED_VERIFIED_LINK = 0x07;
static const uint8_t OP_INIT_BUTTON_EVENTS = 0x0C;
static const uint8_t OP_INIT_BUTTON_EVENTS_RESPONSE = 0x08;
static const uint8_t OP_BUTTON_EVENT = 0x09;
static const uint8_t OP_TWIST_EVENT = 0x0A;
static const uint8_t OP_UPDATE_TWIST_POS = 0x0E;
static const uint8_t OP_GET_BATTERY_LEVEL_REQUEST = 0x11;
static const uint8_t OP_GET_BATTERY_LEVEL_RESPONSE = 0x10;
static const uint8_t OP_GET_NAME_RESPONSE = 0x0C;  // device->host

static const uint8_t MAC_LEN = 5;
static const uint32_t DESIRED_MAGIC = 0x54445031;  // "TDP1"

// HA event-type strings (must match pyflic-ble EVENT_TYPE_* and event.py's registered set).
static const char *const EV_CLICK = "click";
static const char *const EV_DOUBLE_CLICK = "double_click";
static const char *const EV_HOLD = "hold";
static const char *const EV_TWIST_INCREMENT = "twist_increment";
static const char *const EV_TWIST_DECREMENT = "twist_decrement";
static const char *const EV_PUSH_TWIST_INCREMENT = "push_twist_increment";
static const char *const EV_PUSH_TWIST_DECREMENT = "push_twist_decrement";
static const char *const EV_ROTATE_CW = "rotate_clockwise";
static const char *const EV_ROTATE_CCW = "rotate_counter_clockwise";
static const char *const EV_SELECTOR_CHANGED = "selector_changed";

static uint16_t load_le16(const uint8_t *p) { return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8)); }
static uint32_t load_le32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
static int32_t load_signed24(const uint8_t *p) {
  int32_t v = (int32_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16));
  if (v & 0x800000)
    v |= (int32_t) 0xFF000000;  // sign-extend
  return v;
}
static float clamp_pct(float pct) {
  if (pct < 0.0f)
    return 0.0f;
  if (pct > 100.0f)
    return 100.0f;
  return pct;
}

const char *FlicTwist::service_uuid_() const { return TWIST_SERVICE_UUID; }
const char *FlicTwist::tx_char_uuid_() const { return TWIST_TX_CHAR_UUID; }
const char *FlicTwist::rx_char_uuid_() const { return TWIST_RX_CHAR_UUID; }
const uint8_t *FlicTwist::verify_ed25519_key_() const { return TWIST_ED25519_PUBLIC_KEY; }

void FlicTwist::emit_frame_(const uint8_t *body, size_t len) { this->gattc_write_(body, len); }

void FlicTwist::on_setup_() {
  const bool bound_12 = this->push_twist_mode_ != PTM_SELECTOR;   // DEFAULT/CONTINUOUS bind mode 12
  const bool wrap = this->push_twist_mode_ == PTM_CONTINUOUS;
  this->tracker_.configure(bound_12, wrap);
  this->active_mode_ = 0;  // learned from the first rotation/button event

  // Restore the desired ring positions so a reboot does not bring the ring up dark: the init
  // packet positions come from the tracker, which we seed from these values.
  this->desired_pref_ = global_preferences->make_preference<DesiredPositions>(this->storage_hash_ ^ 0x64657369UL);
  DesiredPositions d{};
  if (this->desired_pref_.load(&d) && d.magic == DESIRED_MAGIC) {
    this->desired_ = d;
    for (uint8_t m = 0; m < TWIST_MODE_COUNT; m++) {
      if (this->desired_valid_(m))
        this->seed_tracker_from_desired_(m);
    }
    ESP_LOGI(TAG, "Restored desired ring positions from NVS (modes mask 0x%04x)", (unsigned) d.valid_mask);
  } else {
    this->desired_.magic = DESIRED_MAGIC;
    this->desired_.valid_mask = 0;
  }
  if (this->mode_sensor_ != nullptr)
    this->mode_sensor_->publish_state(this->active_mode_);
}

void FlicTwist::seed_tracker_from_desired_(uint8_t mode) {
  const int32_t desired_units = (int32_t) (this->desired_.pct[mode] / 100.0f * D360);
  const int64_t absolute = this->tracker_.get_absolute_position(mode);
  this->tracker_.set_mode_min(mode, absolute - desired_units);
}

void FlicTwist::publish_numbers_for_mode_(uint8_t mode, float pct) {
  const float rounded = roundf(pct);
  for (auto *n : this->position_numbers_) {
    const int bound = n->selector();  // Twist numbers: selector = bound mode, -1 = active
    if (bound != TWIST_MODE_ACTIVE && bound != (int) mode)
      continue;
    if (bound == TWIST_MODE_ACTIVE && mode != this->active_mode_)
      continue;
    if (!n->has_state() || n->state != rounded)
      n->publish_state(rounded);
  }
}

void FlicTwist::set_active_mode_(uint8_t mode) {
  if (mode == this->active_mode_)
    return;
  this->active_mode_ = mode;
  if (this->mode_sensor_ != nullptr)
    this->mode_sensor_->publish_state(mode);
  // The "active" numbers now follow another mode: show that mode's position.
  this->publish_numbers_for_mode_(mode, this->tracker_.get_mode_percentage(mode));
}

void FlicTwist::on_reset_session_() {
  // Per-connection state the device also resets on (re)connect (matches
  // TwistProtocolHandler.reset_state): the received-packet counter that seeds
  // UpdateTwistPosition, and the first-packet rotation baselines. The rotation POSITIONS
  // deliberately persist so the dial resumes where it was (restored via init).
  this->twist_packet_counter_ = 0;
  for (int i = 0; i < TWIST_MODE_COUNT; i++)
    this->rotate_baseline_valid_[i] = false;
}

void FlicTwist::dump_config() {
  FlicClientBase::dump_config();
  const char *mode = this->push_twist_mode_ == PTM_SELECTOR
                         ? "selector"
                         : (this->push_twist_mode_ == PTM_CONTINUOUS ? "continuous" : "default");
  ESP_LOGCONFIG(TAG, "  push_twist_mode: %s", mode);
  ESP_LOGCONFIG(TAG, "  position numbers: %u, desired modes mask: 0x%04x", (unsigned) this->position_numbers_.size(),
                (unsigned) this->desired_.valid_mask);
}

void FlicTwist::on_session_established_() {
  // The Twist does not keep ring positions across connections; the init packet below restores
  // them from the tracker. Seed every mode we have a desired value for first, so a ring written
  // while we were disconnected (or zeroed by a reboot) comes up right away.
  for (uint8_t m = 0; m < TWIST_MODE_COUNT; m++) {
    if (this->desired_valid_(m))
      this->seed_tracker_from_desired_(m);
  }
  this->send_init_button_events_();
}

void FlicTwist::send_init_button_events_() {
  // InitButtonEventsTwistRequest: [0x0C][event_count:u32][13 x TwistModeConfig(5)][boot_id:u32][api:1]
  uint8_t p[75];
  size_t o = 0;
  p[o++] = OP_INIT_BUTTON_EVENTS;
  chaskey_store_le32(p + o, this->last_event_count_);
  o += 4;

  for (int i = 0; i < TWIST_MODE_COUNT; i++) {
    uint8_t led_mode;
    bool has_click, has_double_click;
    uint8_t timeout_s;
    if (this->push_twist_mode_ == PTM_SELECTOR) {
      if (i < 12) {
        led_mode = 1;
        has_click = true;
        has_double_click = true;
        timeout_s = 60;
      } else {
        led_mode = 3;
        has_click = false;
        has_double_click = false;
        timeout_s = 0;
      }
    } else if (this->push_twist_mode_ == PTM_CONTINUOUS) {
      led_mode = 2;
      has_click = false;
      has_double_click = false;
      timeout_s = 0;
    } else {  // DEFAULT
      led_mode = 1;
      has_click = false;
      has_double_click = false;
      timeout_s = 0;
    }
    uint16_t position = (uint16_t) (this->tracker_.get_mode_percentage(i) / 100.0f * D360);
    // The Twist starts the session with this mode at absolute `position`, min 0. Re-base our tracker
    // to the same frame: keeping the previous session's absolute position (as pyflic-ble does)
    // offsets every later UpdateTwistPosition by the old min, so a ring at 60 % came back full.
    this->tracker_.rebase(i, position);
    uint32_t packed = (uint32_t) (led_mode & 0x3F) | ((uint32_t) (has_click ? 1 : 0) << 6) |
                      ((uint32_t) (has_double_click ? 1 : 0) << 7) | ((uint32_t) (position & 0xFFFF) << 16);
    chaskey_store_le32(p + o, packed);
    o += 4;
    p[o++] = timeout_s;
  }

  chaskey_store_le32(p + o, this->last_boot_id_);
  o += 4;
  p[o++] = 2;  // api_version

  ESP_LOGD(TAG, "Init button events (%u bytes)", (unsigned) o);
  this->write_authenticated_(p, o);
}

void FlicTwist::request_battery_() {
  uint8_t p[1] = {OP_GET_BATTERY_LEVEL_REQUEST};
  this->write_authenticated_(p, 1);
}

void FlicTwist::request_firmware_version_() {
  uint8_t p[1] = {OP_GET_FIRMWARE_VERSION_REQUEST};
  this->write_authenticated_(p, 1);
}

float FlicTwist::battery_level_from_voltage_(float volts) const {
  // Rough 2xAAA alkaline curve: ~3.0 V full, ~2.0 V empty. The Twist reports its voltage under
  // BLE-transmit load (~2.4 V even on fresh cells), so treat this as a trend, not a gauge.
  return clamp_pct((volts - 2.0f) / (3.0f - 2.0f) * 100.0f);
}

void FlicTwist::on_ble_notify_(const uint8_t *data, uint16_t len) {
  if (len < 1)
    return;
  ESP_LOGD(TAG, "RX notify: opcode=0x%02x len=%u state=%d", data[0], (unsigned) len, (int) this->session_);

  // Verify-phase packets (quick/full verify) are handled by the base state machine.
  if (this->handle_verify_notify_(data, len))
    return;
  if (this->session_ != FlicSession::ESTABLISHED)
    return;

  // Session active: strip the trailing 5-byte MAC (Twist SDK does not verify inbound MACs).
  uint16_t plen = len;
  if (plen > MAC_LEN + 1)
    plen -= MAC_LEN;

  const uint8_t opcode = data[0];
  switch (opcode) {
    case OP_GET_FIRMWARE_VERSION_RESPONSE:
      // [0x04][version:u32]
      if (plen >= 5)
        this->publish_firmware_version_(load_le32(data + 1));
      break;
    case OP_DISCONNECTED_VERIFIED_LINK:
      ESP_LOGW(TAG, "Twist rejected link (reason %d); resetting session", plen > 1 ? data[1] : -1);
      this->reset_session_();
      break;
    case OP_INIT_BUTTON_EVENTS_RESPONSE:
      if (plen >= 15) {
        this->last_event_count_ = load_le32(data + 7);
        this->last_boot_id_ = load_le32(data + 11);
        ESP_LOGD(TAG, "Init response: event_count=%u boot_id=%u", (unsigned) this->last_event_count_,
                 (unsigned) this->last_boot_id_);
      }
      // Session fully up: apply the active mode's desired ring position explicitly (belt and braces
      // over the seeded init positions) and let HA see the ring values now in effect.
      if (this->desired_valid_(this->active_mode_))
        this->send_update_twist_position_(this->desired_.pct[this->active_mode_], this->active_mode_);
      for (uint8_t m = 0; m < TWIST_MODE_COUNT; m++) {
        if (this->desired_valid_(m))
          this->publish_numbers_for_mode_(m, this->desired_.pct[m]);
      }
      break;
    case OP_BUTTON_EVENT:
      this->parse_button_events_(data + 1, plen - 1);  // opcode stripped
      break;
    case OP_TWIST_EVENT:
      this->parse_twist_event_(data, plen);  // includes opcode
      break;
    case OP_GET_BATTERY_LEVEL_RESPONSE:
      this->parse_battery_response_(data, plen);
      break;
    case OP_GET_NAME_RESPONSE:
      this->parse_name_response_(data, plen);
      break;
    default:
      ESP_LOGD(TAG, "Unhandled opcode 0x%02x (%u bytes)", opcode, plen);
      break;
  }
}

const char *FlicTwist::map_button_event_(uint8_t event_encoded) const {
  // bits[1:0] base (0 up / 1 down / 2 click / 3 hold); bit3 = double-click concluding.
  if (event_encoded & 0x08) {
    if ((event_encoded & 0x03) == 0)
      return EV_DOUBLE_CLICK;  // UP + dbl
    // fall through to base type for non-UP + dbl
  }
  switch (event_encoded & 0x03) {
    case 2:
      return EV_CLICK;
    case 3:
      return EV_HOLD;
    default:
      return nullptr;  // up/down are not surfaced as HA events
  }
}

void FlicTwist::parse_button_events_(const uint8_t *data, size_t len) {
  if (len < 4)
    return;
  const size_t n = (len - 4) / 8;
  for (size_t k = 0; k < n; k++) {
    const uint8_t *s = data + 4 + k * 8;
    const uint8_t flags = s[6];
    const uint8_t etype = flags & 0x0F;
    const bool was_queued = (flags & 0x10) != 0;
    uint8_t mode = (uint8_t) (((flags >> 6) & 0x03) | ((s[7] & 0x03) << 2));
    if (mode > 12)
      mode = 12;

    // Always track the current mode/slot (even from queued events) so state stays correct.
    const bool mode_changed = (mode != this->twist_mode_index_);
    if (mode_changed) {
      this->twist_mode_index_ = mode;
      this->set_active_mode_(mode);
    }

    if (was_queued)
      continue;  // don't replay historical presses/selector-changes on (re)connect

    if (mode_changed && this->push_twist_mode_ == PTM_SELECTOR && mode < 12)
      this->trigger_event_(EV_SELECTOR_CHANGED);

    const char *ev = this->map_button_event_(etype);
    if (ev != nullptr) {
      ESP_LOGD(TAG, "Button event: %s (mode=%u)", ev, mode);
      this->trigger_event_(ev);
    }
  }
}

void FlicTwist::parse_twist_event_(const uint8_t *data, size_t len) {
  if (len < 13)
    return;
  const uint8_t flags = data[1];
  uint8_t mode = flags & 0x0F;
  if (mode > 12)
    mode = 12;  // valid Twist modes are 0-12; guard array/index use
  const bool last_min_update_was_top = (flags & 0x10) != 0;
  const int32_t total_delta = load_signed24(data + 2);
  const int32_t min_delta = load_signed24(data + 5);
  const int32_t max_delta = load_signed24(data + 8);

  this->twist_packet_counter_++;
  this->twist_mode_index_ = mode;
  this->set_active_mode_(mode);

  RotateOutcome out = this->tracker_.apply(mode, total_delta, min_delta, max_delta, last_min_update_was_top);
  ESP_LOGD(TAG, "Twist event: mode=%u total=%d pct=%.1f detents=%d", (unsigned) mode, (int) total_delta,
           out.mode_percentage, out.detent_crossings);

  this->publish_numbers_for_mode_(mode, out.mode_percentage);
  // Physical rotation redefines what the ring should show for this mode.
  this->desired_.pct[mode] = (uint8_t) roundf(clamp_pct(out.mode_percentage));
  this->desired_.valid_mask |= (uint16_t) (1u << mode);
  this->desired_pref_.save(&this->desired_);

  // First rotation packet of this mode since (re)connect: seed the baseline only, don't
  // replay the delta accumulated while disconnected as live increment/decrement events
  // (mirrors pyflic clearing _prev_int_pct on reset_state). The numbers were still updated.
  if (!this->rotate_baseline_valid_[mode]) {
    this->rotate_baseline_valid_[mode] = true;
    return;
  }

  if (out.detent_crossings == 0 || this->button_event_ == nullptr)
    return;

  // detent_crossings sign is correct in all modes (unbounded for continuous), so it gives both
  // direction and magnitude directly.
  const bool cw = out.detent_crossings > 0;
  if (this->push_twist_mode_ == PTM_SELECTOR) {
    this->trigger_event_(cw ? EV_ROTATE_CW : EV_ROTATE_CCW);
    return;
  }

  int steps = cw ? out.detent_crossings : -out.detent_crossings;
  if (steps > 100)
    steps = 100;  // safety cap against a pathological single-packet delta
  const bool is_push = (mode == 12);
  const char *ev = cw ? (is_push ? EV_PUSH_TWIST_INCREMENT : EV_TWIST_INCREMENT)
                      : (is_push ? EV_PUSH_TWIST_DECREMENT : EV_TWIST_DECREMENT);
  for (int i = 0; i < steps; i++)
    this->trigger_event_(ev);
}

void FlicTwist::parse_battery_response_(const uint8_t *data, size_t len) {
  // [0x10][millivolts:u16] — the Twist (2xAAA) reports millivolts (pyflic client.py).
  if (len < 3)
    return;
  this->publish_battery_voltage_(load_le16(data + 1) / 1000.0f);
}

void FlicTwist::parse_name_response_(const uint8_t *data, size_t len) {
  // [0x0C][timestamp:6][name:var]
  if (len < 7)
    return;
  std::string name(reinterpret_cast<const char *>(data + 7), len - 7);
  ESP_LOGD(TAG, "Twist name: '%s'", name.c_str());
  if (this->name_tsensor_ != nullptr)
    this->name_tsensor_->publish_state(name);
}

bool FlicTwist::write_twist_position(float percentage, int mode) {
  percentage = clamp_pct(percentage);
  if (mode < 0 || mode >= TWIST_MODE_COUNT)
    mode = this->active_mode_;
  const uint8_t m = (uint8_t) mode;

  // Remember it regardless of link state (the NVS write is coalesced by the preferences syncer).
  this->desired_.pct[m] = (uint8_t) roundf(percentage);
  this->desired_.valid_mask |= (uint16_t) (1u << m);
  this->desired_pref_.save(&this->desired_);

  if (this->session_ != FlicSession::ESTABLISHED) {
    ESP_LOGD(TAG, "write_twist_position %.0f%% (mode %u) deferred: no session (applied on reconnect)", percentage, m);
    return false;
  }
  this->send_update_twist_position_(percentage, m);
  // Keep every number bound to this mode in step (the caller publishes its own entity).
  this->publish_numbers_for_mode_(m, percentage);
  return true;
}

void FlicTwist::send_update_twist_position_(float percentage, uint8_t mode) {
  const int32_t desired_units = (int32_t) (percentage / 100.0f * D360);
  const int64_t absolute = this->tracker_.get_absolute_position(mode);
  const int64_t new_min = absolute - desired_units;
  this->tracker_.set_mode_min(mode, new_min);

  // UpdateTwistPositionRequest: [0x0E][mode:1][new_min:48-bit LE][num_received:u32]
  uint8_t p[12];
  p[0] = OP_UPDATE_TWIST_POS;
  p[1] = mode;
  const uint64_t nm = (uint64_t) new_min & 0xFFFFFFFFFFFFULL;
  for (int i = 0; i < 6; i++)
    p[2 + i] = (uint8_t) ((nm >> (8 * i)) & 0xFF);
  chaskey_store_le32(p + 8, this->twist_packet_counter_);

  ESP_LOGD(TAG, "UpdateTwistPosition: mode=%u pct=%.1f new_min=%lld", mode, percentage, (long long) new_min);
  this->write_authenticated_(p, sizeof(p));
}

}  // namespace flic
}  // namespace esphome

#endif  // USE_ESP32

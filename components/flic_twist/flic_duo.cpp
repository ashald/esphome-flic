// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#include "flic_duo.h"

#ifdef USE_ESP32

#include "chaskey.h"
#include "duo_codec.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstdlib>

namespace esphome {
namespace flic_twist {

static const char *const TAG = "flic_duo";

// Duo extension opcodes (Flic Duo protocol specification). Shared Flic 2 opcodes (firmware,
// battery, ping, adv parameters) are handled by FlicButton::handle_common_frame_.
static const uint8_t OP_INIT_BUTTON_EVENTS_DUO = 35;    // to the Duo
static const uint8_t OP_INIT_DUO_RESPONSE_A = 30;       // from the Duo (with / without boot id:
static const uint8_t OP_INIT_DUO_RESPONSE_B = 31;       //  the spec's names and layouts disagree,
                                                        //  so the boot id is taken by length)
static const uint8_t OP_BUTTON_EVENT_DUO = 32;          // from the Duo, bit-packed
static const uint8_t OP_PUSH_TWIST_DATA = 33;           // from the Duo
static const uint8_t OP_ACK_BUTTON_EVENTS_DUO = 36;     // to the Duo
static const uint8_t OP_ENABLE_PUSH_TWIST = 37;         // to the Duo

static const uint32_t DIALS_MAGIC = 0x44554F31;  // "DUO1"

static const char *const BUTTON_NAME[2] = {"big", "small"};

static uint32_t load_le32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// ---------------------------------------------------------------------------------------------
// setup / session

void FlicDuo::on_setup_() {
  FlicButton::on_setup_();
  this->input_.set_listener(this);
  this->dial_pref_ = global_preferences->make_preference<StoredDials>(this->storage_hash_ ^ 0x6469616CUL);  // "dial"
  StoredDials d{};
  if (this->dial_pref_.load(&d) && d.magic == DIALS_MAGIC) {
    for (uint8_t b = 0; b < 2; b++)
      this->input_.set_dial(b, d.units[b]);
    ESP_LOGI(TAG, "Restored dials from NVS (big %d, small %d units)", (int) this->input_.dial(0),
             (int) this->input_.dial(1));
  }
  for (uint8_t b = 0; b < 2; b++)
    this->publish_dial_(b);
}

void FlicDuo::dump_config() {
  FlicButton::dump_config();
  ESP_LOGCONFIG(TAG, "  dial range: %.0f degrees, dial numbers: %u, event entities: big %s, small %s",
                this->input_.dial_range() * 360.0f / 65536.0f, (unsigned) this->position_numbers_.size(),
                this->duo_events_[0] != nullptr ? "yes" : "no", this->duo_events_[1] != nullptr ? "yes" : "no");
}

void FlicDuo::on_reset_session_() {
  FlicButton::on_reset_session_();
  this->decoder_ = DuoDecoderState{};
  this->input_.reset_session();  // press state is per session; the dials persist
}

void FlicDuo::on_session_established_() {
  this->decoder_.count[0] = this->catchup_count_[0];
  this->decoder_.count[1] = this->catchup_count_[1];
  this->send_init_button_events_duo_();
  this->send_enable_push_twist_();
}

void FlicDuo::send_init_button_events_duo_() {
  // InitButtonEventsDuoLightRequest (35), signed:
  //   [35][event_count[2]: 2 x u32][boot_id:u32][auto_disconnect_time:9 | max_queued_packets:5 |
  //    max_queued_packets_age:20 | rfu:6 = 5 bytes]
  // Same choices as the Flic 2 init: never idle-disconnect (511), no queue limits (queued events
  // are ACKed but not replayed as live events).
  uint8_t p[18];
  size_t o = 0;
  p[o++] = OP_INIT_BUTTON_EVENTS_DUO;
  chaskey_store_le32(p + o, this->catchup_count_[0]);
  o += 4;
  chaskey_store_le32(p + o, this->catchup_count_[1]);
  o += 4;
  chaskey_store_le32(p + o, this->last_boot_id_);
  o += 4;
  uint64_t bf = 0;
  bf |= (uint64_t) 511u << 0;        // auto_disconnect_time: disabled
  bf |= (uint64_t) 31u << 9;         // max_queued_packets: no limit
  bf |= (uint64_t) 0xFFFFFu << 14;   // max_queued_packets_age: no limit
  for (int i = 0; i < 5; i++)
    p[o++] = (uint8_t) ((bf >> (8 * i)) & 0xFF);
  ESP_LOGD(TAG, "Init button events (Duo): counts %u/%u, boot id %u", (unsigned) this->catchup_count_[0],
           (unsigned) this->catchup_count_[1], (unsigned) this->last_boot_id_);
  this->write_authenticated_(p, o);
}

void FlicDuo::send_enable_push_twist_() {
  // EnablePushTwistInd (37), signed: [37][buttons bitmask: bit0 big, bit1 small].
  const uint8_t p[2] = {OP_ENABLE_PUSH_TWIST, 0x03};
  this->write_authenticated_(p, sizeof(p));
}

bool FlicDuo::handle_session_frame_(const uint8_t *data, uint16_t len) {
  switch (data[0]) {
    case OP_BUTTON_EVENT_DUO:
      this->parse_button_events_duo_(data + 1, len - 1);
      return true;
    case OP_PUSH_TWIST_DATA:
      this->parse_push_twist_(data, len);
      return true;
    case OP_INIT_DUO_RESPONSE_A:
    case OP_INIT_DUO_RESPONSE_B:
      this->parse_init_response_duo_(data, len);
      return true;
    default:
      return this->handle_common_frame_(data, len);
  }
}

void FlicDuo::parse_init_response_duo_(const uint8_t *data, uint16_t len) {
  // [30|31][has_queued_events:1 | timestamp_ms:47 = 6 bytes][event_count[2]: 2 x u32][boot_id:u32]?
  if (len < 15) {
    ESP_LOGW(TAG, "Init response too short (%u)", (unsigned) len);
    return;
  }
  const bool has_queued = (data[1] & 0x01) != 0;
  this->decoder_.count[0] = load_le32(data + 7);
  this->decoder_.count[1] = load_le32(data + 11);
  this->catchup_count_[0] = this->decoder_.count[0];
  this->catchup_count_[1] = this->decoder_.count[1];
  if (len >= 19) {
    const uint32_t bid = load_le32(data + 15);
    if (bid != this->last_boot_id_)
      ESP_LOGD(TAG, "Boot id %u -> %u (Duo rebooted or first session)", (unsigned) this->last_boot_id_, (unsigned) bid);
    this->last_boot_id_ = bid;
  }
  this->decoder_.ts_ms = 0;
  this->decoder_.end_of_queue = !has_queued;
  ESP_LOGD(TAG, "Init response: counts %u/%u, queued events %s", (unsigned) this->decoder_.count[0],
           (unsigned) this->decoder_.count[1], has_queued ? "follow" : "none");
  this->on_events_initialized_();
}

// ---------------------------------------------------------------------------------------------
// button events

void FlicDuo::parse_button_events_duo_(const uint8_t *data, uint16_t len) {
  bool need_ack = false;
  unsigned decoded = 0;
  const bool complete = duo_decode_events(this->decoder_, data, len, [&](const DuoUpdate &u) {
    // Single click, double click and single-click timeout updates are acknowledged.
    if (u.type == 1 || u.type == 2 || u.type == 3 || u.type == 4 || u.type == 6)
      need_ack = true;
    decoded++;
    ESP_LOGD(TAG, "Duo event: %s type %u%s gesture %d%s count %u t=%llu ms accel %d,%d,%d", BUTTON_NAME[u.button],
             (unsigned) u.type, u.flag ? " (flag)" : "", u.gesture, u.queued ? " QUEUED" : "", (unsigned) u.count,
             (unsigned long long) u.ts_ms, u.accel[0], u.accel[1], u.accel[2]);
    this->input_.on_update(u, millis());
  });
  if (!complete)
    ESP_LOGW(TAG, "Duo event packet (%u bytes) ended mid-update after %u updates", (unsigned) len, decoded);

  this->catchup_count_[0] = this->decoder_.count[0];
  this->catchup_count_[1] = this->decoder_.count[1];
  if (need_ack)
    this->send_ack_duo_();
}

void FlicDuo::send_ack_duo_() {
  // AckButtonEventsDuoInd (36), signed: [36][event_count[2]: 2 x u32] — both counts, always.
  uint8_t p[9];
  p[0] = OP_ACK_BUTTON_EVENTS_DUO;
  chaskey_store_le32(p + 1, this->decoder_.count[0]);
  chaskey_store_le32(p + 5, this->decoder_.count[1]);
  this->write_authenticated_(p, sizeof(p));
}

void FlicDuo::on_duo_event(uint8_t b, const char *type) {
  ESP_LOGD(TAG, "Duo %s button: %s", BUTTON_NAME[b & 1], type);
  if (this->duo_events_[b & 1] != nullptr)
    this->duo_events_[b & 1]->trigger(type);
}

void FlicDuo::on_duo_dial(uint8_t b, int32_t units) {
  this->save_dials_();
  this->publish_dial_(b);
}

// ---------------------------------------------------------------------------------------------
// push-twist / dials

void FlicDuo::parse_push_twist_(const uint8_t *data, uint16_t len) {
  // [33][buttons_pressed:2 | is_first_event:2 | held_half_second:2 | rfu:2][angle_diff:i32]
  if (len < 6)
    return;
  const int32_t diff = (int32_t) load_le32(data + 2);
  ESP_LOGV(TAG, "Push-twist: flags 0x%02x diff %d", data[1], (int) diff);
  this->input_.on_push_twist(data[1], diff, millis());
}

void FlicDuo::publish_dial_(uint8_t b) {
  const float pct = roundf(this->input_.dial(b) * 100.0f / this->input_.dial_range());
  for (auto *n : this->position_numbers_) {
    const int sel = n->selector() < 0 ? 0 : n->selector();
    if (sel != b)
      continue;
    if (!n->has_state() || n->state != pct)
      n->publish_state(pct);
  }
}

void FlicDuo::save_dials_() {
  StoredDials d{};
  d.magic = DIALS_MAGIC;
  d.units[0] = this->input_.dial(0);
  d.units[1] = this->input_.dial(1);
  this->dial_pref_.save(&d);  // coalesced into flash by the preferences flash_write_interval
}

bool FlicDuo::write_position(float percentage, int selector) {
  const uint8_t b = selector == 1 ? 1 : 0;
  if (percentage < 0.0f)
    percentage = 0.0f;
  if (percentage > 100.0f)
    percentage = 100.0f;
  this->input_.set_dial(b, (int32_t) lroundf(percentage / 100.0f * this->input_.dial_range()));
  this->save_dials_();
  this->publish_dial_(b);
  ESP_LOGD(TAG, "Duo %s dial set to %.0f%%", BUTTON_NAME[b], percentage);
  return true;
}

}  // namespace flic_twist
}  // namespace esphome

#endif  // USE_ESP32

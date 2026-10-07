// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#include "flic_button.h"

#ifdef USE_ESP32

#include "chaskey.h"
#include "flic2_events.h"
#include "flic_crypto.h"  // FLIC2_ED25519_PUBLIC_KEY
#include "esphome/core/log.h"

namespace esphome {
namespace flic {

static const char *const TAG = "flic_button";

// Flic 2 GATT (pyflic-ble const.py — the 0042 family, shared with Duo). Host WRITES ...0001 and
// SUBSCRIBES ...0002.
static const char *const FLIC2_SERVICE_UUID = "00420000-8f59-4420-870d-84f3b617e493";
static const char *const FLIC2_TX_CHAR_UUID = "00420001-8f59-4420-870d-84f3b617e493";  // write
static const char *const FLIC2_RX_CHAR_UUID = "00420002-8f59-4420-870d-84f3b617e493";  // notify

// Flic 2 established-session opcodes (verify opcodes live in FlicClientBase). See the Flic 2
// protocol specification and pyflic-ble const.py. Request and response opcode spaces are separate.
static const uint8_t OP_GET_FIRMWARE_VERSION_REQUEST = 8;
static const uint8_t OP_GET_FIRMWARE_VERSION_RESPONSE = 5;
static const uint8_t OP_GET_BATTERY_LEVEL_REQUEST = 20;
static const uint8_t OP_GET_BATTERY_LEVEL_RESPONSE = 20;
static const uint8_t OP_BUTTON_EVENT = 12;
static const uint8_t OP_INIT_BUTTON_EVENTS = 23;         // INIT_BUTTON_EVENTS_LIGHT_REQUEST
static const uint8_t OP_INIT_RESP_WITH_BOOT_ID = 10;
static const uint8_t OP_INIT_RESP_WITHOUT_BOOT_ID = 11;
static const uint8_t OP_ACK_BUTTON_EVENTS = 16;
// Session duties from the protocol spec (not in pyflic-ble): the button may ping at any time and
// a response is mandatory, else it ends the session (DisconnectedVerifiedLinkInd, reason 0 = ping
// timeout); SetAdvParameters keeps it advertising after a lost link (firmware >= 7).
static const uint8_t OP_PING_REQUEST = 15;                 // from the button
static const uint8_t OP_PING_RESPONSE = 14;                // to the button
static const uint8_t OP_DISCONNECTED_VERIFIED_LINK = 9;    // from the button: session terminated
static const uint8_t OP_SET_ADV_PARAMETERS_REQUEST = 27;   // to the button
// The wiki lists the response as 27; the button answers with 25 (seen on firmware 11).
static const uint8_t OP_SET_ADV_PARAMETERS_RESPONSE = 25;  // from the button
static const uint16_t ADV_FIRST_INTERVAL_UNITS = 160;      // 100 ms for the first 5 s after a drop
static const uint32_t ADV_PARAMS_MIN_FIRMWARE = 7;

static const uint8_t MAC_LEN = 5;

// Frame header bit fields (Flic 2 protocol specification; pyflic-ble const.py).
static const uint8_t HDR_CONN_ID_MASK = 0x1F;
static const uint8_t HDR_NEWLY_ASSIGNED = 0x20;  // button-set: adopt the assigned connId
static const uint8_t HDR_FRAGMENT_MORE = 0x80;   // set on every fragment except the last

// Body bytes per fragment: ATT_MTU 23 -> mtu-4 = 19 (write = 1 header + 19 = 20). Safe for any
// negotiated MTU; our runtime packets (init 19 B, ack 5 B) are single-fragment anyway.
static const size_t FRAG_BODY = 19;


static uint16_t load_le16(const uint8_t *p) { return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8)); }
static uint32_t load_le32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

const char *FlicButton::service_uuid_() const { return FLIC2_SERVICE_UUID; }
const char *FlicButton::tx_char_uuid_() const { return FLIC2_TX_CHAR_UUID; }
const char *FlicButton::rx_char_uuid_() const { return FLIC2_RX_CHAR_UUID; }
const uint8_t *FlicButton::verify_ed25519_key_() const { return FLIC2_ED25519_PUBLIC_KEY; }

void FlicButton::on_reset_session_() {
  this->conn_id_ = 0;      // reassigned by the button (newlyAssigned) on each new connection
  this->reasm_len_ = 0;    // drop any partial reassembly
  this->fw_version_ = 0;   // re-read each session (gates SetAdvParameters)
  this->adv_params_sent_ = false;
  // last_event_count_/last_boot_id_ deliberately persist across reconnects (catch-up state).
}

void FlicButton::dump_config() {
  FlicClientBase::dump_config();
  if (this->adv_timeout_s_ == 0) {
    ESP_LOGCONFIG(TAG, "  reconnect advertising: device default (ours removed)");
  } else {
    ESP_LOGCONFIG(TAG, "  reconnect advertising: every %u ms for %u s after a lost link",
                  (unsigned) (this->adv_interval_units_ * 5 / 8), (unsigned) this->adv_timeout_s_);
  }
}

void FlicButton::emit_frame_(const uint8_t *body, size_t len) {
  // Prepend the 1-byte frame header (connId | fragment-more) and fragment if the body exceeds one
  // fragment. The header is NOT covered by the MAC (already appended over opcode+payload by
  // write_authenticated_) — per the spec and pyflic-ble.
  size_t off = 0;
  uint8_t pkt[1 + FRAG_BODY];
  do {
    const size_t chunk = (len - off) > FRAG_BODY ? FRAG_BODY : (len - off);
    const bool more = (off + chunk) < len;
    pkt[0] = (uint8_t) ((this->conn_id_ & HDR_CONN_ID_MASK) | (more ? HDR_FRAGMENT_MORE : 0));
    memcpy(pkt + 1, body + off, chunk);
    this->gattc_write_(pkt, 1 + chunk);
    off += chunk;
  } while (off < len);
}

void FlicButton::on_ble_notify_(const uint8_t *data, uint16_t len) {
  if (len < 1)
    return;
  const uint8_t hdr = data[0];
  // The button assigns us a logical connId in its first response (newlyAssigned bit).
  if (hdr & HDR_NEWLY_ASSIGNED)
    this->conn_id_ = hdr & HDR_CONN_ID_MASK;

  const uint8_t *frag = data + 1;
  const uint16_t flen = len - 1;
  if ((uint32_t) this->reasm_len_ + flen > sizeof(this->reasm_)) {
    ESP_LOGW(TAG, "reassembly overflow (%u+%u), dropping frame", this->reasm_len_, flen);
    this->reasm_len_ = 0;
    return;
  }
  memcpy(this->reasm_ + this->reasm_len_, frag, flen);
  this->reasm_len_ += flen;

  if (hdr & HDR_FRAGMENT_MORE)
    return;  // more fragments to come — wait for the last

  const uint16_t frame_len = this->reasm_len_;
  this->reasm_len_ = 0;  // consume; reset for the next frame
  this->dispatch_frame_(this->reasm_, frame_len);
}

void FlicButton::dispatch_frame_(const uint8_t *data, uint16_t len) {
  if (len < 1)
    return;
  ESP_LOGD(TAG, "RX frame: opcode=0x%02x len=%u state=%d", data[0], (unsigned) len, (int) this->session_);

  // Verify-phase packets (quick/full verify) are handled by the base state machine.
  if (this->handle_verify_notify_(data, len))
    return;
  if (this->session_ != FlicSession::ESTABLISHED)
    return;

  // Session active: strip the trailing 5-byte MAC (not verified, as in pyflic-ble).
  uint16_t plen = len;
  if (plen > MAC_LEN + 1)
    plen -= MAC_LEN;

  if (!this->handle_session_frame_(data, plen))
    ESP_LOGD(TAG, "Unhandled opcode 0x%02x (%u bytes)", data[0], (unsigned) plen);
}

bool FlicButton::handle_session_frame_(const uint8_t *data, uint16_t plen) {
  switch (data[0]) {
    case OP_BUTTON_EVENT:
      this->parse_button_events_(data + 1, plen - 1);  // opcode stripped
      return true;
    case OP_INIT_RESP_WITH_BOOT_ID:
      this->parse_init_response_(data, plen, /*with_boot_id=*/true);
      return true;
    case OP_INIT_RESP_WITHOUT_BOOT_ID:
      this->parse_init_response_(data, plen, /*with_boot_id=*/false);
      return true;
    default:
      return this->handle_common_frame_(data, plen);
  }
}

bool FlicButton::handle_common_frame_(const uint8_t *data, uint16_t plen) {
  switch (data[0]) {
    case OP_PING_REQUEST: {
      // Mandatory reply, signed; an unanswered ping makes the button end the session.
      ESP_LOGD(TAG, "Ping from the %s, answering", this->device_kind_());
      const uint8_t rsp[1] = {OP_PING_RESPONSE};
      this->write_authenticated_(rsp, sizeof(rsp));
      return true;
    }
    case OP_DISCONNECTED_VERIFIED_LINK: {
      // The button ended the session (0 ping timeout, 1 invalid signature, 2 new session with the
      // same pairing, 3 by user). Re-verify on the same link rather than disconnecting: a
      // host-initiated disconnect would leave the button silent until pressed.
      const int reason = plen > 1 ? data[1] : -1;
      ESP_LOGW(TAG, "%s ended the session (reason %d); re-verifying", this->device_kind_(), reason);
      this->reset_session_();
      if (this->connected_bsensor_ != nullptr)
        this->connected_bsensor_->publish_state(false);
      this->publish_link_status_();
      this->set_timeout("start_session", 1000, [this]() { this->start_session_(); });
      return true;
    }
    case OP_GET_FIRMWARE_VERSION_RESPONSE:
      // [5][version:u32]
      if (plen >= 5) {
        this->fw_version_ = load_le32(data + 1);
        this->publish_firmware_version_(this->fw_version_);
        this->send_adv_parameters_();
      }
      return true;
    case OP_GET_BATTERY_LEVEL_RESPONSE:
      // [20][level:u16] — 10-bit ADC reading of the CR2032 against a 3.6 V reference.
      if (plen >= 3)
        this->publish_battery_voltage_(load_le16(data + 1) * 3.6f / 1024.0f);
      return true;
    case OP_SET_ADV_PARAMETERS_RESPONSE:
      ESP_LOGD(TAG, "%s accepted the reconnect advertising parameters", this->device_kind_());
      return true;
    default:
      return false;
  }
}

void FlicButton::on_events_initialized_() {
  // Queued events (if any) follow the init response; the firmware request rides along. Its
  // response gates and sends SetAdvParameters, and feeds the Firmware text sensor (the base's
  // own 5 s request is skipped via fw_requested_).
  if (!this->fw_requested_) {
    this->request_firmware_version_();
    this->fw_requested_ = true;
  } else if (this->fw_version_ != 0) {
    this->send_adv_parameters_();
  }
}

void FlicButton::send_adv_parameters_() {
  if (this->adv_params_sent_ || this->session_ != FlicSession::ESTABLISHED)
    return;
  this->adv_params_sent_ = true;
  if (this->fw_version_ < ADV_PARAMS_MIN_FIRMWARE) {
    ESP_LOGD(TAG, "Firmware %u < %u: keeping the device's default reconnect advertising",
             (unsigned) this->fw_version_, (unsigned) ADV_PARAMS_MIN_FIRMWARE);
    return;
  }
  if (this->adv_timeout_s_ == 0) {
    // Device default: remove this pairing's parameters (they would otherwise live on in the
    // button's RAM until it reboots). is_active = 0, the rest ignored.
    uint8_t p[13] = {OP_SET_ADV_PARAMETERS_REQUEST, 0};
    ESP_LOGD(TAG, "SetAdvParameters: removing ours (device default)");
    this->write_authenticated_(p, sizeof(p));
    return;
  }
  // SetAdvParametersRequest (27), signed: [27][is_active][remove_other_pairings][short_range]
  // [long_range][adv_intervals: 2 x u16, 0.625 ms][timeout_seconds:u32]. Held in the button's RAM
  // per pairing (lost on a button reboot), so it is re-sent every session. Short range (LE 1M)
  // only: ESPHome scans LE 1M, Coded PHY adverts would only cost battery.
  uint8_t p[13];
  p[0] = OP_SET_ADV_PARAMETERS_REQUEST;
  p[1] = 1;  // is_active
  p[2] = 0;  // leave other pairings' (e.g. a phone's) settings alone
  p[3] = 1;  // with_short_range
  p[4] = 0;  // with_long_range
  p[5] = (uint8_t) (ADV_FIRST_INTERVAL_UNITS & 0xFF);
  p[6] = (uint8_t) (ADV_FIRST_INTERVAL_UNITS >> 8);
  p[7] = (uint8_t) (this->adv_interval_units_ & 0xFF);
  p[8] = (uint8_t) (this->adv_interval_units_ >> 8);
  chaskey_store_le32(p + 9, this->adv_timeout_s_);
  ESP_LOGD(TAG, "SetAdvParameters: 100 ms for 5 s, then %u ms for %u s", (unsigned) (this->adv_interval_units_ * 5 / 8),
           (unsigned) this->adv_timeout_s_);
  this->write_authenticated_(p, sizeof(p));
}

bool FlicButton::accept_paired_device_(const uint8_t *data, uint16_t len) {
  // FullVerifyResponse2: [1][flags][uuid:16][name_len][name:23][firmware:u32][battery:u16]
  // [serial:11][color:16 (Duo)] + MAC. flags bit 2 = is_duo (reported because we set supports_duo).
  if (len < 2)
    return true;
  const bool is_duo = (data[1] & 0x04) != 0;
  char serial[12] = {0};
  if (len >= 59)
    memcpy(serial, data + 48, 11);
  const unsigned fw = len >= 46 ? (unsigned) load_le32(data + 42) : 0;
  ESP_LOGI(TAG, "Paired a %s (serial '%s', firmware %u)", is_duo ? "Flic Duo" : "Flic 2", serial, fw);
  if (is_duo == this->expects_duo_())
    return true;
  ESP_LOGE(TAG, "Pairing refused: this slot is device_type: %s but the device is a %s. Use a slot with "
                "device_type: %s for it.",
           this->expects_duo_() ? "duo" : "button", is_duo ? "Flic Duo" : "Flic 2", is_duo ? "duo" : "button");
  return false;
}

void FlicButton::on_session_established_() { this->send_init_button_events_(); }

void FlicButton::request_battery_() {
  uint8_t p[1] = {OP_GET_BATTERY_LEVEL_REQUEST};
  this->write_authenticated_(p, 1);
}

void FlicButton::request_firmware_version_() {
  uint8_t p[1] = {OP_GET_FIRMWARE_VERSION_REQUEST};
  this->write_authenticated_(p, 1);
}

float FlicButton::battery_level_from_voltage_(float volts) const {
  // Manufacturer's CR2032 millivolt curve (as used by the Flic app; flat between 50 and 100 %):
  // 2100 -> 0, 2440 -> 6, 2740 -> 18, 2900 -> 42, 3000 -> 100.
  static const struct {
    float mv, pct;
  } CURVE[] = {{2100, 0}, {2440, 6}, {2740, 18}, {2900, 42}, {3000, 100}};
  const float mv = volts * 1000.0f;
  if (mv <= CURVE[0].mv)
    return 0.0f;
  for (size_t i = 1; i < sizeof(CURVE) / sizeof(CURVE[0]); i++) {
    if (mv < CURVE[i].mv) {
      const float f = (mv - CURVE[i - 1].mv) / (CURVE[i].mv - CURVE[i - 1].mv);
      return CURVE[i - 1].pct + f * (CURVE[i].pct - CURVE[i - 1].pct);
    }
  }
  return 100.0f;
}

void FlicButton::send_init_button_events_() {
  // INIT_BUTTON_EVENTS_LIGHT_REQUEST (opcode 23), signed:
  //   [23][eventCount:u32][bootId:u32][packed 40-bit bitfield:5]
  // bitfield (LE bit order): autoDisconnectTime(0-8) | maxQueuedPackets(9-13) |
  //                          maxQueuedPacketsAge(14-33) | padding(34-39)
  // autoDisconnectTime = 511 (0x1FF) disables the button's idle disconnect, the point of an
  // always-connected hub (pyflic-ble 0.2.5 sends 0, which lets the button drop the link).
  // maxQueuedPackets = 31 and maxQueuedPacketsAge = 0xFFFFF (seconds) mean no limit per the spec;
  // queued events are acknowledged but not replayed as live events.
  uint8_t p[14];
  size_t o = 0;
  p[o++] = OP_INIT_BUTTON_EVENTS;
  chaskey_store_le32(p + o, this->last_event_count_);
  o += 4;
  chaskey_store_le32(p + o, this->last_boot_id_);
  o += 4;

  uint64_t bf = 0;
  bf |= (uint64_t) (511u & 0x1FFu) << 0;         // autoDisconnectTime = infinite
  bf |= (uint64_t) (31u & 0x1Fu) << 9;           // maxQueuedPackets = 31 (5-bit max)
  bf |= (uint64_t) (0xFFFFFu & 0xFFFFFu) << 14;  // maxQueuedPacketsAge = 20-bit max
  for (int i = 0; i < 5; i++)
    p[o++] = (uint8_t) ((bf >> (8 * i)) & 0xFF);

  ESP_LOGD(TAG, "Init button events (%u bytes, eventCount=%u bootId=%u)", (unsigned) o,
           (unsigned) this->last_event_count_, (unsigned) this->last_boot_id_);
  this->write_authenticated_(p, o);
}

void FlicButton::parse_init_response_(const uint8_t *data, size_t len, bool with_boot_id) {
  // [op][hasQueuedEvents:1b|timestamp:47b = 6 bytes][eventCount:u32][bootId:u32 if WITH].
  if (len < 1 + 6 + 4)
    return;
  const uint32_t ec = load_le32(data + 1 + 6);
  uint32_t bid = this->last_boot_id_;
  if (with_boot_id && len >= 1 + 6 + 4 + 4)
    bid = load_le32(data + 1 + 6 + 4);
  if (bid != this->last_boot_id_) {
    // The button rebooted — its event counters are no longer comparable; treat as fresh.
    ESP_LOGD(TAG, "Init resp: boot id changed (%u->%u), resetting catch-up", (unsigned) this->last_boot_id_,
             (unsigned) bid);
    this->last_boot_id_ = bid;
  }
  this->last_event_count_ = ec;
  ESP_LOGD(TAG, "Init resp: event_count=%u boot_id=%u", (unsigned) ec, (unsigned) bid);
  this->on_events_initialized_();
}

const char *FlicButton::map_event_(uint8_t encoded) const { return flic2_event_type(encoded); }

void FlicButton::parse_button_events_(const uint8_t *data, size_t len) {
  // [eventCounter:u32][7-byte slots]* — the eventCounter is the value at the LAST slot.
  if (len < 4)
    return;
  const uint32_t event_counter = load_le32(data);
  const size_t n = (len - 4) / 7;
  bool should_ack = false;
  for (size_t k = 0; k < n; k++) {
    const uint8_t *s = data + 4 + k * 7;
    const uint8_t b6 = s[6];
    const uint8_t encoded = b6 & 0x0F;
    const bool was_queued = (b6 & 0x10) != 0;

    // ACK clicks / double clicks / single-click timeouts so the button can drop them from its
    // queue — for live AND replayed events (the ACK is what clears the queue).
    if (flic2_event_needs_ack(encoded))
      should_ack = true;

    if (was_queued)
      continue;  // don't replay historical presses on (re)connect

    const char *ev = this->map_event_(encoded);
    if (ev != nullptr) {
      ESP_LOGD(TAG, "Button event: %s (encoded=%u)", ev, encoded);
      this->trigger_event_(ev);
    }
  }
  // Track the newest counter so a reconnect's init requests only newer events.
  this->last_event_count_ = event_counter;
  if (should_ack)
    this->send_ack_(event_counter);
}

void FlicButton::send_ack_(uint32_t event_counter) {
  // AckButtonEventsInd (opcode 16): [16][eventCounter:u32], signed.
  uint8_t p[5];
  p[0] = OP_ACK_BUTTON_EVENTS;
  chaskey_store_le32(p + 1, event_counter);
  this->write_authenticated_(p, sizeof(p));
}

}  // namespace flic
}  // namespace esphome

#endif  // USE_ESP32

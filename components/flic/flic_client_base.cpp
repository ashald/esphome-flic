// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Borys Pierov
// Portions derived from pyflic-ble (https://github.com/50ButtonsEach/pyflic-ble),
// Copyright Shortcut Labs, Apache-2.0: ported to C++ and modified. See NOTICE.

#include "flic_client_base.h"

#ifdef USE_ESP32

#include "chaskey.h"
#include "flic_crypto.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"

#include <esp_gap_ble_api.h>
#include <esp_attr.h>
#include <esp_random.h>

#include <algorithm>
#include <vector>

// TweetNaCl references randombytes() (from keypair/sign, which we don't call — but the linker
// still needs the symbol). Back it with the ESP32 hardware RNG. Defined once here, for the whole
// flic component (FlicTwist, FlicButton and FlicDuo link against it).
extern "C" void randombytes(unsigned char *buf, unsigned long long n) {
  esp_fill_random(buf, (size_t) n);
}

namespace esphome {
namespace flic {

static const char *const TAG = "flic";

// Verify request opcodes — common to the Twist and Flic 2 families (per pyflic-ble and the spec).
static const uint8_t OP_FULL_VERIFY_REQUEST_1 = 0x00;
static const uint8_t OP_FULL_VERIFY_RESPONSE_1 = 0x00;
static const uint8_t OP_FULL_VERIFY_REQUEST_2 = 0x02;
static const uint8_t OP_FULL_VERIFY_RESPONSE_2 = 0x01;  // success (both families)
static const uint8_t OP_QUICK_VERIFY_REQUEST = 0x05;
// ForceBtDisconnectInd — host->device, same opcode in both families: [0x06][restart_adv], signed.
// pyflic-ble sends it (restart_adv=1) to end a firmware update.
static const uint8_t OP_FORCE_BT_DISCONNECT_IND = 0x06;

static const uint8_t MAC_LEN = 5;

#ifdef USE_OTA_STATE_LISTENER
// Outcome of the last OTA handoff, kept in RTC memory across the OTA's software reboot: log lines
// emitted during an OTA never reach a log client (the main loop is blocked, then the chip resets),
// so the next boot reports it instead (FlicClientBase::dump_config).
struct HandoffRecord {
  uint32_t magic;
  uint8_t sent;         // devices asked to disconnect + keep advertising
  uint8_t dropped;      // of those, links already gone when the hold ended
  uint16_t elapsed_ms;  // how long the hold lasted
};
static const uint32_t HANDOFF_MAGIC = 0x46485230;  // "FHR0"
RTC_NOINIT_ATTR static HandoffRecord rtc_last_handoff;

// Hub-wide OTA handoff. When an OTA starts, esp32_ble_tracker gracefully disconnects every BLE
// client, and a Flic its host cleanly disconnects goes SILENT (stops advertising) until it is
// physically pressed/twisted — so every flash cost a walk-around, while an abrupt drop (power loss,
// Node Restart) recovers on its own. Instead, ask each device to drop the link itself and keep
// advertising, then hold the main loop (already blocked by the OTA transfer) until the links are
// gone or the delivery budget runs out. One shared listener so all slots are asked first and
// waited on ONCE.
class FlicOtaHandoff : public ota::OTAGlobalStateListener {
 public:
  static FlicOtaHandoff *get() {
    static FlicOtaHandoff *instance = nullptr;
    if (instance == nullptr) {
      instance = new FlicOtaHandoff();  // NOLINT(cppcoreguidelines-owning-memory)
      // Take (and clear) the record so it is reported for the boot right after an OTA only.
      if (rtc_last_handoff.magic == HANDOFF_MAGIC)
        instance->boot_record_ = rtc_last_handoff;
      rtc_last_handoff.magic = 0;
      ota::get_global_ota_callback()->add_global_state_listener(instance);
    }
    return instance;
  }
  void add(FlicClientBase *client) { this->clients_.push_back(client); }

  // Logged from the first slot's dump_config only.
  void dump_boot_record(const FlicClientBase *client) const {
    if (this->clients_.empty() || this->clients_.front() != client || this->boot_record_.magic != HANDOFF_MAGIC)
      return;
    ESP_LOGCONFIG(TAG, "Last OTA handoff: %u of %u Flic(s) dropped their link within %u ms%s",
                  this->boot_record_.dropped, this->boot_record_.sent, this->boot_record_.elapsed_ms,
                  this->boot_record_.dropped < this->boot_record_.sent ? " (rest were disconnected by the tracker)"
                                                                       : "");
  }

  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error,
                           ota::OTAComponent *comp) override {
    if (state == ota::OTA_ERROR || state == ota::OTA_ABORT) {
      rtc_last_handoff.magic = 0;  // no reboot follows; don't report this handoff on a later boot
      return;
    }
    if (state != ota::OTA_STARTED)
      return;
    rtc_last_handoff.magic = 0;  // only this OTA's handoff (if any) may survive into its reboot
    static const uint32_t MAX_HOLD_MS = 6000;  // espota waits far longer for the prepare ack
    std::vector<FlicClientBase *> linked;
    uint32_t hold_ms = 0;
    for (auto *client : this->clients_) {
      const uint32_t ms = client->send_ota_handoff();
      if (ms != 0) {
        linked.push_back(client);
        hold_ms = std::max(hold_ms, ms);
      }
    }
    const size_t sent = linked.size();
    if (sent == 0)
      return;
    // Stop scanning for the hold: nothing drains the BLE event queue while the main loop is held,
    // so adverts would overflow it — and a dropped STOP_COMPLETE leaves the tracker stuck in
    // STOPPING if the OTA then fails. Keep scan_continuous_ set so the tracker's own OTA listener
    // (runs after us; its stop_scan() is then a no-op) still restarts scanning on OTA_ERROR/ABORT.
    // Also frees airtime for the handoff writes.
    auto *tracker = espbt::global_esp32_ble_tracker;
    if (tracker != nullptr && tracker->get_scanner_state() == espbt::ScannerState::RUNNING) {
      tracker->stop_scan();
      tracker->set_scan_continuous(true);
    }
    hold_ms = std::min(hold_ms, MAX_HOLD_MS);
    ESP_LOGI(TAG, "OTA starting: asked %u Flic(s) to disconnect + keep advertising; holding up to %u ms",
             (unsigned) sent, (unsigned) hold_ms);
    // Done once every device has dropped its link (it obeyed); whatever is still linked when the
    // budget runs out gets the tracker's clean disconnect, as before.
    const uint32_t start = millis();
    while (!linked.empty() && millis() - start < hold_ms) {
      App.feed_wdt();
      delay(10);  // yields to the BT host/controller tasks that put the writes on air
      linked.erase(std::remove_if(linked.begin(), linked.end(), [](FlicClientBase *c) { return !c->link_up(); }),
                   linked.end());
    }
    const uint32_t elapsed = millis() - start;
    rtc_last_handoff = {HANDOFF_MAGIC, (uint8_t) sent, (uint8_t) (sent - linked.size()),
                        (uint16_t) std::min<uint32_t>(elapsed, 0xFFFF)};
    ESP_LOGI(TAG, "OTA handoff: %u of %u Flic link(s) dropped after %u ms", (unsigned) (sent - linked.size()),
             (unsigned) sent, (unsigned) elapsed);
  }

 protected:
  std::vector<FlicClientBase *> clients_;
  HandoffRecord boot_record_{};
};
#endif

FlicFleet *FlicFleet::get() {
  static FlicFleet *instance = nullptr;
  if (instance == nullptr)
    instance = new FlicFleet();  // NOLINT(cppcoreguidelines-owning-memory)
  return instance;
}

void FlicFleet::refresh() {
  int up = 0, paired = 0;
  for (auto *c : this->clients_) {
    if (c->paired())
      paired++;
    if (c->session_up())
      up++;
  }
  const int total = (int) this->clients_.size();
  if (this->slots_ != nullptr && total != this->last_slots_) {
    this->slots_->publish_state(total);
    this->last_slots_ = total;
  }
  if (this->paired_ != nullptr && paired != this->last_paired_) {
    this->paired_->publish_state(paired);
    this->last_paired_ = paired;
  }
  if (this->connected_ != nullptr && up != this->last_connected_) {
    this->connected_->publish_state(up);
    this->last_connected_ = up;
  }
}

FlicClientBase::FlicClientBase() {
#ifdef USE_OTA_STATE_LISTENER
  FlicOtaHandoff::get()->add(this);
#endif
  FlicFleet::get()->add(this);
}

// Credentials persisted to NVS after an on-device pairing (override config creds).
struct StoredCreds {
  uint32_t magic;
  uint32_t pairing_id;
  uint8_t pairing_key[16];
} __attribute__((packed));
static const uint32_t CREDS_MAGIC = 0x466C6331;  // "Flc1"

static int hex_nibble_(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool FlicClientBase::parse_device(const ble_device_base::ESPBTDevice &device) {
  this->on_advert_(device);
  return false;  // observe only; never claim the device
}

void FlicClientBase::setup() {
  // Credentials are keyed by the STABLE slot id (md5 of the config id, storage_hash_). The config
  // MAC is only a discovery filter — present: connect to exactly that MAC; absent: scan for a
  // public-mode device of this type on Pair. A learned-MAC cache (own NVS slot) lets a slot
  // reconnect after its config MAC is removed, so once a slot has booted with its MAC (migrated +
  // cached), removing the MAC is a no-op. CAVEAT for the upgrade from the older MAC-keyed scheme:
  // a slot's creds live in the hash(MAC) slot until its FIRST id-keyed boot migrates them — that
  // boot MUST still carry the config MAC (we can't recompute the old slot key without it). Removing
  // the MAC in the same flash that first introduces id-keying orphans the bond (see load_creds_).
  // Credentials (NVS) take priority over config.
  uint64_t cfg_mac = (this->parent() != nullptr) ? this->parent()->get_address() : 0;
  if (cfg_mac != 0) {
    // Precompute the old MAC-slot key so load_creds_ can migrate a pre-id-keying bond (no re-pair).
    char b[24];
    snprintf(b, sizeof(b), "flictw:%012llx", (unsigned long long) cfg_mac);
    this->mac_hash_ = fnv1_hash(std::string(b));
  }
  this->pref_ = global_preferences->make_preference<StoredCreds>(this->storage_hash_);
  this->learned_mac_pref_ =
      global_preferences->make_preference<uint64_t>(this->storage_hash_ ^ 0x6D61635FUL);  // "mac_"
  uint64_t cached = 0;
  if (this->learned_mac_pref_.load(&cached) && cached != 0)
    this->learned_mac_ = cached;

  if (this->load_creds_()) {
    this->creds_valid_ = true;
    ESP_LOGI(TAG, "Loaded stored pairing (id=%u) from NVS", (unsigned) this->pairing_id_);
  } else if (this->pairing_key_hex_.size() == 32) {
    bool ok = true;
    for (int i = 0; i < 16; i++) {
      int hi = hex_nibble_(this->pairing_key_hex_[2 * i]);
      int lo = hex_nibble_(this->pairing_key_hex_[2 * i + 1]);
      if (hi < 0 || lo < 0) {
        ok = false;
        break;
      }
      this->pairing_key_[i] = (uint8_t) ((hi << 4) | lo);
    }
    this->creds_valid_ = ok;
    if (!ok)
      ESP_LOGE(TAG, "Invalid pairing_key hex");
  }
  if (!this->creds_valid_)
    ESP_LOGW(TAG, "No pairing credentials yet — press Pair (device in pairing mode) to bond it");

  if (this->type_tsensor_ != nullptr)
    this->type_tsensor_->publish_state(this->device_type_id_());
  if (this->connected_bsensor_ != nullptr)
    this->connected_bsensor_->publish_state(false);
  this->publish_link_status_();

  // Resolve which MAC to target and whether Pair must discover.
  this->mac_optional_ = (cfg_mac == 0);  // no config MAC -> Pair scans for a public-mode device
  if (cfg_mac != 0) {
    // Refresh the cache from the config MAC so that if the MAC is later removed from config the
    // slot still reconnects via the cache (making MAC removal a true no-op).
    if (this->learned_mac_ != cfg_mac) {
      this->learned_mac_ = cfg_mac;
      this->learned_mac_pref_.save(&cfg_mac);
      global_preferences->sync();
    }
  } else if (this->learned_mac_ != 0) {
    // No config MAC but we bonded before: reconnect via the cached MAC (normal quick-verify).
    this->parent()->set_address(this->learned_mac_);
    this->parent()->set_auto_connect(true);
    ESP_LOGI(TAG, "reconnect: targeting cached MAC %012llx (config MAC omitted)",
             (unsigned long long) this->learned_mac_);
  } else if (this->creds_valid_) {
    // Creds but no MAC to reach the device: a bond whose config MAC was removed before its first
    // id-keyed boot could migrate/cache it (or stale creds from an even-older scheme). Warn loudly
    // rather than silently idle — restore the config MAC + reflash once to migrate, then remove it.
    ESP_LOGW(TAG, "have creds but no MAC to reach the %s — if this slot had a config MAC, restore "
                  "it + reflash once to migrate/cache, then it is safe to remove", this->device_kind_());
  } else {
    ESP_LOGI(TAG, "no MAC (config or cached) yet — press Pair (device in public mode) to bond "
                  "(if this slot was bonded under a config MAC, restore it + reflash once instead)");
  }
  // NB: this instance is registered as a tracker listener at codegen time (register_ble_device);
  // its parse_device -> on_advert_ only acts while a mac-optional pair scan is armed.

  this->on_setup_();
}

bool FlicClientBase::load_creds_() {
  // Migration from the older MAC-keyed scheme takes priority: when a config MAC is present the
  // most-recent creds live in the hash(MAC) slot. Adopt them into the id slot and ERASE the MAC
  // slot (one-time) so it can never shadow a later re-pair, and so stale creds from an even-older
  // id-slot pairing can't win. After this runs, the id slot is authoritative.
  if (this->mac_hash_ != 0 && this->mac_hash_ != this->storage_hash_) {
    auto macslot = global_preferences->make_preference<StoredCreds>(this->mac_hash_);
    StoredCreds mc{};
    if (macslot.load(&mc) && mc.magic == CREDS_MAGIC) {
      ESP_LOGI(TAG, "Migrating pairing creds from the legacy MAC-slot to the id-slot");
      this->save_creds_(mc.pairing_id, mc.pairing_key);  // writes id slot + retires the MAC slot
      return true;
    }
  }
  StoredCreds c{};
  if (this->pref_.load(&c) && c.magic == CREDS_MAGIC) {  // id slot: authoritative post-migration
    this->pairing_id_ = c.pairing_id;
    memcpy(this->pairing_key_, c.pairing_key, 16);
    return true;
  }
  return false;
}

void FlicClientBase::save_creds_(uint32_t pairing_id, const uint8_t key[16]) {
  StoredCreds c{};
  c.magic = CREDS_MAGIC;
  c.pairing_id = pairing_id;
  memcpy(c.pairing_key, key, 16);
  this->pref_.save(&c);
  global_preferences->sync();
  this->pairing_id_ = pairing_id;
  memcpy(this->pairing_key_, key, 16);
  this->creds_valid_ = true;
  // Retire the legacy hash(MAC) slot whenever we persist creds to the id slot, so a stale MAC-slot
  // payload (e.g. a failed one-time migration erase) can never resurrect over these creds on a
  // later reboot and silently undo a re-pair.
  if (this->mac_hash_ != 0 && this->mac_hash_ != this->storage_hash_) {
    auto macslot = global_preferences->make_preference<StoredCreds>(this->mac_hash_);
    StoredCreds empty{};
    macslot.save(&empty);
    global_preferences->sync();
  }
  this->publish_link_status_();
}

// Publish the richer link status: no creds -> "not paired"; creds but no session -> "disconnected";
// authenticated session up -> "connected". Mirrors the connectivity binary sensor but distinguishes
// "never bonded / unpaired" from "paired but not currently linked".
void FlicClientBase::publish_link_status_() {
  if (this->status_tsensor_ != nullptr) {
    const char *s = !this->creds_valid_                              ? "not paired"
                    : (this->session_ == FlicSession::ESTABLISHED)   ? "connected"
                                                                     : "disconnected";
    this->status_tsensor_->publish_state(s);
  }
  FlicFleet::get()->refresh();  // hub-wide summary (slots / paired / connected)
}

void FlicClientBase::publish_battery_voltage_(float volts) {
  if (this->battery_voltage_sensor_ != nullptr)
    this->battery_voltage_sensor_->publish_state(volts);
  if (this->battery_level_sensor_ != nullptr) {
    const float level = this->battery_level_from_voltage_(volts);
    if (level >= 0.0f)
      this->battery_level_sensor_->publish_state(level);
  }
  ESP_LOGD(TAG, "%s battery: %.3f V", this->device_kind_(), volts);
}

void FlicClientBase::publish_firmware_version_(uint32_t version) {
  ESP_LOGD(TAG, "%s firmware version: %u", this->device_kind_(), (unsigned) version);
  if (this->fw_tsensor_ != nullptr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned) version);
    this->fw_tsensor_->publish_state(std::string(buf));
  }
}

void FlicClientBase::dump_creds() {
  if (!this->creds_valid_) {
    ESP_LOGW(TAG, "dump_creds: no credentials stored yet");
    return;
  }
  char hex[33];
  for (int i = 0; i < 16; i++)
    snprintf(hex + 2 * i, 3, "%02x", this->pairing_key_[i]);
  ESP_LOGW(TAG, "==== CREDS DUMP — copy to the new board to migrate WITHOUT re-pairing ====");
  ESP_LOGW(TAG, "  pairing_id: %u", (unsigned) this->pairing_id_);
  ESP_LOGW(TAG, "  pairing_key: %s", hex);
  ESP_LOGW(TAG, "=========================================================================");
}

void FlicClientBase::unpair() {
  // Wipe creds for this slot. Erase the id slot (primary) and the legacy hash(MAC) slot so the
  // boot-time migration can't resurrect it. Local only — the device keeps its side of the bond
  // until it is physically put back into public mode and re-paired (no over-the-air forget).
  StoredCreds empty{};  // magic 0 => treated as absent by load_creds_
  this->pref_.save(&empty);  // primary (id) slot
  if (this->mac_hash_ != 0 && this->mac_hash_ != this->storage_hash_) {
    auto legacy = global_preferences->make_preference<StoredCreds>(this->mac_hash_);  // pre-migration MAC slot
    legacy.save(&empty);
  }
  global_preferences->sync();
  this->creds_valid_ = false;
  this->pairing_id_ = 0;
  memset(this->pairing_key_, 0, sizeof(this->pairing_key_));
  this->pairing_key_hex_.clear();
  // Always drop the learned-MAC cache: if the config MAC is later removed, this slot must be free
  // to bond a replacement device (a stale cache would keep targeting the retired one and suppress
  // rediscovery). For a mac-specified slot the cache is harmlessly re-seeded from config next boot.
  uint64_t z = 0;
  this->learned_mac_pref_.save(&z);
  global_preferences->sync();
  this->learned_mac_ = 0;
  // For a mac-optional slot (no config MAC), also stop targeting the old device so Pair rescans.
  if (this->mac_optional_)
    this->parent()->set_address(0);  // Pair then rescans to learn a new device
  ESP_LOGW(TAG, "UNPAIRED (%s) — creds wiped. Put the device in public mode + press Pair to re-bond; "
                "its bond persists device-side until then.", this->device_kind_());
  this->reset_session_();
  if (this->node_state == espbt::ClientState::ESTABLISHED)
    this->parent()->disconnect();
  this->publish_link_status_();
}

void FlicClientBase::dump_config() {
  ESP_LOGCONFIG(TAG, "Flic %s:", this->device_kind_());
  ESP_LOGCONFIG(TAG, "  pairing_id: %u", (unsigned) this->pairing_id_);
  ESP_LOGCONFIG(TAG, "  credentials: %s", this->creds_valid_ ? "present" : "NONE (press Pair to bond)");
  ESP_LOGCONFIG(TAG, "  conn params: interval %u-%u (x1.25ms), latency %u, timeout %u (x10ms)",
                this->conn_min_iv_, this->conn_max_iv_, this->conn_latency_, this->conn_timeout_);
#ifdef USE_OTA_STATE_LISTENER
  FlicOtaHandoff::get()->dump_boot_record(this);
#endif
}

void FlicClientBase::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  // ble_client forwards every GAP event to every node; act only on OUR connection's RSSI read.
  if (event != ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT || this->rssi_sensor_ == nullptr ||
      this->parent() == nullptr)
    return;
  const auto &r = param->read_rssi_cmpl;
  if (r.status == ESP_BT_STATUS_SUCCESS &&
      memcmp(r.remote_addr, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t)) == 0)
    this->rssi_sensor_->publish_state(r.rssi);
}

void FlicClientBase::loop() {
  // Publish the configured MAC once (no BLE op — zero stability cost).
  if (this->mac_tsensor_ != nullptr && !this->mac_published_ && this->parent() != nullptr) {
    uint64_t a = this->parent()->get_address();
    if (a != 0) {
      char buf[18];
      snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned) ((a >> 40) & 0xFF),
               (unsigned) ((a >> 32) & 0xFF), (unsigned) ((a >> 24) & 0xFF), (unsigned) ((a >> 16) & 0xFF),
               (unsigned) ((a >> 8) & 0xFF), (unsigned) (a & 0xFF));
      this->mac_tsensor_->publish_state(std::string(buf));
      this->mac_published_ = true;
    }
  }

  if (this->session_ != FlicSession::ESTABLISHED || this->established_ms_ == 0)
    return;
  const uint32_t now = millis();

  // Read the GAP Device Name once over the OPEN connection — no scanning, so no airtime
  // contention. Best-effort: only if the device exposes 0x1800/0x2A00.
  if (this->name_tsensor_ != nullptr && this->gap_name_handle_ != 0 && !this->name_requested_ &&
      (now - this->established_ms_) > 3000) {
    esp_ble_gattc_read_char((esp_gatt_if_t) this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                            this->gap_name_handle_, ESP_GATT_AUTH_REQ_NONE);
    this->name_requested_ = true;
  }

  // Firmware version once per session, ~5 s after it comes up (a signed request on the open link).
  if (this->fw_tsensor_ != nullptr && !this->fw_requested_ && (now - this->established_ms_) > 5000) {
    this->request_firmware_version_();
    this->fw_requested_ = true;
  }

  // Battery poll (hourly), first read ~15s after the session comes up.
  static const uint32_t BATTERY_POLL_MS = 3600000UL;
  static const uint32_t BATTERY_FIRST_DELAY_MS = 15000UL;
  if ((this->battery_voltage_sensor_ != nullptr || this->battery_level_sensor_ != nullptr) &&
      (now - this->established_ms_) >= BATTERY_FIRST_DELAY_MS &&
      (this->last_battery_ms_ == 0 || (now - this->last_battery_ms_) >= BATTERY_POLL_MS)) {
    this->request_battery_();
    this->last_battery_ms_ = now;
  }

  // Connection RSSI — a GAP read on the OPEN link (no scanning, negligible airtime). The async
  // result arrives in gap_event_handler(). Poll ~every 60s while the session is up.
  if (this->rssi_sensor_ != nullptr &&
      (this->last_rssi_ms_ == 0 || (now - this->last_rssi_ms_) >= 60000)) {
    esp_bd_addr_t bda;
    memcpy(bda, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t));
    esp_ble_gap_read_rssi(bda);
    this->last_rssi_ms_ = now;
  }
}

void FlicClientBase::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status == ESP_GATT_OK)
        ESP_LOGD(TAG, "BLE link open");
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      using esphome::esp32_ble::ESPBTUUID;
      auto *notify_chr = this->parent()->get_characteristic(ESPBTUUID::from_raw(this->service_uuid_()),
                                                            ESPBTUUID::from_raw(this->rx_char_uuid_()));
      auto *write_chr = this->parent()->get_characteristic(ESPBTUUID::from_raw(this->service_uuid_()),
                                                           ESPBTUUID::from_raw(this->tx_char_uuid_()));
      if (notify_chr == nullptr || write_chr == nullptr) {
        ESP_LOGW(TAG, "GATT characteristics not found; not a Flic %s?", this->device_kind_());
        break;
      }
      this->notify_handle_ = notify_chr->handle;
      this->write_handle_ = write_chr->handle;
      ESP_LOGD(TAG, "chars: notify=0x%04x write=0x%04x", this->notify_handle_, this->write_handle_);
      // Best-effort GAP Device Name handle (for the name text sensor; read later over the link).
      auto *gap_name = this->parent()->get_characteristic(ESPBTUUID::from_uint16(0x1800),
                                                          ESPBTUUID::from_uint16(0x2A00));
      this->gap_name_handle_ = (gap_name != nullptr) ? gap_name->handle : 0;
      esp_err_t st = this->parent()->register_for_notify(this->notify_handle_);
      if (st != ESP_OK)
        ESP_LOGW(TAG, "register_for_notify failed: %d", st);
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.handle != this->notify_handle_)
        break;
      // Signal the ble_client parent that this node is ready (releases the GATT cache).
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->update_conn_params_();
      this->reset_session_();
      // The ble_client base writes the notify-enable descriptor (CCCD) AFTER this event, so
      // notifications aren't live yet. Speaking now would let the device's quick-verify response
      // be dropped. Wait briefly for the CCCD write + conn-param update to settle (mirrors
      // pyflic awaiting start_notify before the handshake).
      this->set_timeout("start_session", 500, [this]() { this->start_session_(); });
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->notify_handle_)
        break;
      this->on_ble_notify_(param->notify.value, param->notify.value_len);
      break;
    }

    case ESP_GATTC_READ_CHAR_EVT:
      if (param->read.status == ESP_GATT_OK && this->gap_name_handle_ != 0 &&
          param->read.handle == this->gap_name_handle_ && this->name_tsensor_ != nullptr) {
        std::string name(reinterpret_cast<const char *>(param->read.value), param->read.value_len);
        ESP_LOGD(TAG, "GAP device name: '%s'", name.c_str());
        this->name_tsensor_->publish_state(name);
      }
      break;

    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGD(TAG, "BLE disconnect (reason 0x%02x)", param->disconnect.reason);
      this->notify_handle_ = 0;
      this->write_handle_ = 0;
      this->gap_name_handle_ = 0;
      this->reset_session_();
      if (this->connected_bsensor_ != nullptr)
        this->connected_bsensor_->publish_state(false);
      this->publish_link_status_();
      this->on_disconnected_();
      break;

    default:
      break;
  }
}

void FlicClientBase::update_conn_params_() {
  esp_ble_conn_update_params_t params{};
  memcpy(params.bda, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t));
  params.min_int = this->conn_min_iv_;
  params.max_int = this->conn_max_iv_;
  params.latency = this->conn_latency_;
  params.timeout = this->conn_timeout_;
  esp_err_t st = esp_ble_gap_update_conn_params(&params);
  if (st != ESP_OK)
    ESP_LOGW(TAG, "update_conn_params failed: %d", st);
}

void FlicClientBase::reset_session_() {
  this->session_ = FlicSession::IDLE;
  this->counter_to_button_ = 0;
  this->established_ms_ = 0;
  this->last_battery_ms_ = 0;
  memset(this->chaskey_keys_, 0, sizeof(this->chaskey_keys_));
  this->on_reset_session_();
}

void FlicClientBase::trigger_event_(const char *type) {
  if (this->button_event_ != nullptr && type != nullptr)
    this->button_event_->trigger(type);
}

void FlicClientBase::start_session_() {
  if (this->write_handle_ == 0)
    return;
  if (this->pairing_mode_) {
    this->start_full_verify_();
    return;
  }
  if (!this->creds_valid_) {
    ESP_LOGW(TAG, "Connected but no credentials — press Pair (device in pairing mode) to bond it");
    return;
  }
  this->start_quick_verify_();
}

void FlicClientBase::start_quick_verify_() {
  this->session_ = FlicSession::WAIT_QUICK_VERIFY;
  this->counter_to_button_ = 0;
  this->tmp_id_ = esp_random();
  esp_fill_random(this->client_random_, sizeof(this->client_random_));

  // QuickVerifyRequest: [0x05][client_random:7][flags:1][tmp_id:u32][pairing_id:u32].
  // flags = 0x00 (Twist) / 0x40 (Flic 2 supportsDuo). Sent unsigned; framing per family.
  uint8_t req[17];
  req[0] = OP_QUICK_VERIFY_REQUEST;
  memcpy(req + 1, this->client_random_, 7);
  req[8] = this->verify_flag_qv_();
  chaskey_store_le32(req + 9, this->tmp_id_);
  chaskey_store_le32(req + 13, this->pairing_id_);
  ESP_LOGD(TAG, "Quick-verify request (tmp_id=0x%08x pairing_id=%u)", (unsigned) this->tmp_id_,
           (unsigned) this->pairing_id_);
  this->write_raw_(req, sizeof(req));
}

void FlicClientBase::handle_quick_verify_response_(const uint8_t *data, uint16_t len) {
  if (len < 1 + 8) {
    ESP_LOGW(TAG, "Quick-verify response too short (%u)", len);
    return;
  }
  const uint8_t *button_random = data + 1;  // 8 bytes
  // [op][button_random:8][tmp_id:4][flags:1] — Flic 2 family flags carry is_duo (spec: bit 2).
  if (len >= 14)
    ESP_LOGD(TAG, "Quick-verify response flags 0x%02x", data[13]);

  // Session-key KDF: chaskey_16(subkeys(pairing_key), client_random[7] || flag || button_random[8]).
  // flag = 0x00 (Twist) / 0x40 (Flic 2). Then chaskey_keys_ = subkeys(session_key).
  uint8_t kdf[16];
  memcpy(kdf, this->client_random_, 7);
  kdf[7] = this->verify_flag_qv_();
  memcpy(kdf + 8, button_random, 8);

  uint32_t pairing_subkeys[12];
  chaskey_subkeys(this->pairing_key_, pairing_subkeys);
  uint8_t session_key[16];
  chaskey_16(pairing_subkeys, kdf, session_key);
  chaskey_subkeys(session_key, this->chaskey_keys_);

  this->counter_to_button_ = 0;
  this->session_ = FlicSession::ESTABLISHED;
  this->established_ms_ = millis();
  this->last_battery_ms_ = 0;
  this->name_requested_ = false;
  this->fw_requested_ = false;
  ESP_LOGI(TAG, "Session established (quick verify OK)");
  if (this->connected_bsensor_ != nullptr)
    this->connected_bsensor_->publish_state(true);
  this->publish_link_status_();

  this->on_session_established_();
}

void FlicClientBase::start_full_verify_() {
  this->session_ = FlicSession::WAIT_FULL_VERIFY_R1;
  this->full_verify_tmp_id_ = esp_random();
  uint8_t req[5];
  req[0] = OP_FULL_VERIFY_REQUEST_1;
  chaskey_store_le32(req + 1, this->full_verify_tmp_id_);
  ESP_LOGI(TAG, "Pairing: full-verify request 1 (tmp_id=0x%08x)", (unsigned) this->full_verify_tmp_id_);
  this->write_raw_(req, sizeof(req));
}

void FlicClientBase::handle_full_verify_response1_(const uint8_t *data, uint16_t len) {
  // [0x00][tmp_id:4][sig:64][addr:6][addr_type:1][button_pubkey:32][device_random:8] (+flags for
  // Flic 2, ignored) = 116/117, no MAC. Field offsets are identical across families.
  if (len < 116) {
    ESP_LOGW(TAG, "FullVerifyResponse1 too short (%u)", (unsigned) len);
    this->end_pairing_(false);
    return;
  }
  if (chaskey_load_le32(data + 1) != this->full_verify_tmp_id_) {
    ESP_LOGW(TAG, "FullVerifyResponse1 tmp_id mismatch");
    this->end_pairing_(false);
    return;
  }
  const uint8_t *sig = data + 5;
  const uint8_t *button_addr = data + 69;
  const uint8_t addr_type = data[75];
  const uint8_t *button_pubkey = data + 76;
  const uint8_t *device_random = data + 108;

  // Verify the device's Ed25519 identity signature (over addr || addr_type || pubkey), 4 variants.
  uint8_t signed_data[6 + 1 + 32];
  memcpy(signed_data, button_addr, 6);
  signed_data[6] = addr_type;
  memcpy(signed_data + 7, button_pubkey, 32);
  int variant =
      flic_ed25519_verify_variant(this->verify_ed25519_key_(), signed_data, sizeof(signed_data), sig);
  if (variant < 0) {
    ESP_LOGE(TAG, "Pairing: device signature failed to verify — not a genuine Flic?");
    this->end_pairing_(false);
    return;
  }
  ESP_LOGD(TAG, "Pairing: device signature verified (variant=%d)", variant);

  // ECDH + derive pairing credentials (held pending until the device confirms).
  uint8_t app_pub[32];
  flic_x25519_keypair(this->pairing_priv_, app_pub);
  uint8_t shared[32];
  flic_x25519_shared(shared, this->pairing_priv_, button_pubkey);
  esp_fill_random(this->pairing_client_random_, 8);
  uint8_t verifier[16];
  flic_derive_full_verify_keys(shared, (uint8_t) variant, device_random, this->pairing_client_random_,
                               this->verify_flag_fv_(), verifier, this->pending_pairing_key_,
                               &this->pending_pairing_id_);

  // FullVerifyRequest2: [0x02][ecdh_pub:32][client_random:8][flags:1][verifier:16] = 58, no MAC.
  // flags = 0x00 (Twist) / 0x80 (Flic 2 supportsDuo).
  uint8_t req[58];
  size_t o = 0;
  req[o++] = OP_FULL_VERIFY_REQUEST_2;
  memcpy(req + o, app_pub, 32);
  o += 32;
  memcpy(req + o, this->pairing_client_random_, 8);
  o += 8;
  req[o++] = this->verify_flag_fv_();
  memcpy(req + o, verifier, 16);
  o += 16;
  this->session_ = FlicSession::WAIT_FULL_VERIFY_R2;
  ESP_LOGD(TAG, "Pairing: full-verify request 2 (%u bytes)", (unsigned) o);
  this->write_raw_(req, o);
}

void FlicClientBase::handle_full_verify_result_(const uint8_t *data, uint16_t len, bool success) {
  if (success && !this->accept_paired_device_(data, len)) {
    // Wrong kind of device for this slot (logged by the hook): keep no creds, and point a mac-optional
    // slot back at its previous bond (or nothing) so it does not keep reconnecting to this device.
    this->end_pairing_(false);
    if (this->mac_optional_)
      this->parent()->set_address(this->learned_mac_);
    return;
  }
  if (success) {
    this->save_creds_(this->pending_pairing_id_, this->pending_pairing_key_);
    // Cache the connected device's MAC so this slot reconnects even if its config MAC is later
    // removed (the id-keyed creds + cached MAC are the bond; the config MAC is only a filter).
    uint64_t m = this->parent()->get_address();
    if (m != 0 && m != this->learned_mac_) {
      this->learned_mac_ = m;
      this->learned_mac_pref_.save(&m);
      global_preferences->sync();
      ESP_LOGI(TAG, "cached device MAC %012llx", (unsigned long long) m);
    }
    ESP_LOGI(TAG, "Pairing SUCCESS — new pairing_id=%u saved to flash", (unsigned) this->pending_pairing_id_);
    this->end_pairing_(true);
    return;
  }
  const uint8_t reason = len > 1 ? data[1] : 0xFF;
  if (reason == 1 && this->pairing_mode_) {
    // The device isn't in public/pairing mode yet. Stay armed and retry on the next connect
    // (within the pairing window) — keep working the device into its pairing animation.
    ESP_LOGW(TAG, "Device not in pairing mode yet — keep holding it; retrying (window still open)");
    this->session_ = FlicSession::IDLE;
    this->parent()->disconnect();
    return;
  }
  const char *rs = reason == 0 ? "INVALID_VERIFIER" : reason == 1 ? "NOT_IN_PUBLIC_MODE" : "unknown";
  ESP_LOGE(TAG, "Pairing FAILED: %s (reason %u)", rs, (unsigned) reason);
  this->end_pairing_(false);
}

bool FlicClientBase::handle_verify_notify_(const uint8_t *data, uint16_t len) {
  if (len < 1)
    return this->session_ != FlicSession::ESTABLISHED;  // nothing to dispatch either way

  if (this->session_ == FlicSession::WAIT_QUICK_VERIFY) {
    if (data[0] == this->op_quick_verify_response_()) {
      this->handle_quick_verify_response_(data, len);
    } else if (data[0] == this->op_quick_verify_negative_()) {
      ESP_LOGE(TAG, "Quick-verify REJECTED (stale/unknown pairing). Put the device in pairing mode "
                    "and press Pair to re-bond it.");
      this->session_ = FlicSession::IDLE;
      this->parent()->disconnect();
    } else {
      ESP_LOGW(TAG, "Unexpected opcode 0x%02x during quick verify", data[0]);
    }
    return true;
  }

  if (this->session_ == FlicSession::WAIT_FULL_VERIFY_R1) {
    if (data[0] == OP_FULL_VERIFY_RESPONSE_1)
      this->handle_full_verify_response1_(data, len);
    else
      ESP_LOGW(TAG, "Unexpected opcode 0x%02x during pairing (r1)", data[0]);
    return true;
  }

  if (this->session_ == FlicSession::WAIT_FULL_VERIFY_R2) {
    this->handle_full_verify_result_(data, len, data[0] == OP_FULL_VERIFY_RESPONSE_2);
    return true;
  }

  return false;  // ESTABLISHED (or IDLE): caller dispatches protocol opcodes
}

void FlicClientBase::request_pairing() {
  ESP_LOGI(TAG, "PAIRING MODE ACTIVE (~60s) — long-hold the device button now to bond it");
  this->pairing_mode_ = true;
  this->set_timeout("pairing", 60000, [this]() {
    if (this->pairing_mode_) {
      ESP_LOGW(TAG, "Pairing window closed without success");
      this->end_pairing_(false);
    }
  });
  // mac-optional (no config MAC): always (re)discover on Pair so a REPLACEMENT device can be bonded
  // even when we're still targeting a now-gone device's cached MAC. Drop the current target/link
  // first so the scan learns the closest public-mode device of this type, then full-verify proceeds.
  if (this->mac_optional_) {
    if (this->node_state == espbt::ClientState::ESTABLISHED)
      this->parent()->disconnect();
    this->parent()->set_address(0);
    this->start_pair_scan_();
    return;
  }
  // Restart the handshake as full-verify. If already connected, drop so ble_client reconnects
  // (the device must be advertising in pairing mode for the new connection).
  if (this->node_state == espbt::ClientState::ESTABLISHED)
    this->parent()->disconnect();
}

uint32_t FlicClientBase::send_ota_handoff() {
  // session_/write_handle_ lag a drop whose DISCONNECT event is still queued; the host link
  // lookup doesn't. Skip dead links so the boot record only counts devices actually asked.
  esp_gap_conn_params_t p;
  if (this->session_ != FlicSession::ESTABLISHED || this->write_handle_ == 0 || !this->get_link_params_(&p))
    return 0;
  const uint8_t ind[2] = {OP_FORCE_BT_DISCONNECT_IND, 0x01};  // restart_adv = 1
  this->last_write_err_ = ESP_OK;
  this->write_authenticated_(ind, sizeof(ind));
  if (this->last_write_err_ != ESP_OK)
    return 0;  // not queued (e.g. congested): the tracker's clean disconnect applies, as before
  ESP_LOGD(TAG, "OTA handoff: ForceBtDisconnectInd (restart_adv=1) sent to the %s", this->device_kind_());
  // With slave latency the device only listens every (latency + 1) connection events, so the
  // write can take that long to reach it; budget that twice (one missed listen) plus an event for
  // it to act, from the negotiated params. The hold ends early once the link is gone, so this only
  // costs time when the device ignores the message.
  return (2 * ((uint32_t) p.latency + 1) + 1) * p.interval * 5 / 4 + 250;  // interval in 1.25 ms units
}

bool FlicClientBase::link_up() {
  esp_gap_conn_params_t p;
  return this->get_link_params_(&p);
}

bool FlicClientBase::get_link_params_(esp_gap_conn_params_t *p) {
  // Host-side (L2CAP) link lookup — no radio traffic; fails once the link is gone.
  esp_bd_addr_t bda;
  memcpy(bda, this->parent()->get_remote_bda(), sizeof(esp_bd_addr_t));
  return esp_ble_get_current_conn_params(bda, p) == ESP_OK;
}

void FlicClientBase::start_pair_scan_() {
  this->pair_scan_active_ = true;
  this->pair_candidate_ = 0;
  this->pair_best_rssi_ = -127;
  this->pair_adverts_seen_ = 0;
  ESP_LOGI(TAG, "mac-optional: scanning for a %s in public mode (4s)...", this->device_kind_());
  this->set_timeout("pair_scan", 4000, [this]() { this->commit_pair_candidate_(); });
}

void FlicClientBase::on_advert_(const ble_device_base::ESPBTDevice &device) {
  if (!this->pair_scan_active_)
    return;
  this->pair_adverts_seen_++;
  auto want = espbt::ESPBTUUID::from_raw(this->service_uuid_()).as_128bit();
  auto flic2co = espbt::ESPBTUUID::from_uint16(0x030f);  // Flic company id (manufacturer data)
  bool has_flic_uuid = false;
  for (auto &u : device.get_service_uuids()) {
    if (u.as_128bit() == want) {
      has_flic_uuid = true;
      break;
    }
  }
  bool has_flic_mfr = false;
  int conn_flags = -1;  // Flic 2 manufacturer-data flags byte (bit1 = connected elsewhere)
  for (auto &md : device.get_manufacturer_datas()) {
    if (md.uuid == flic2co) {
      has_flic_mfr = true;
      if (md.data.size() >= 5)
        conn_flags = md.data[4];
    }
  }
  // Per-advert trace (DEBUG): whether the service UUID is visible and whether the device reports
  // connected-elsewhere. Raise the logger to DEBUG for the flic tag to diagnose a discovery miss.
  if (has_flic_uuid || has_flic_mfr) {
    ESP_LOGD(TAG, "advert %012llx rssi=%d uuids=%u mfr=%u flic_uuid=%d flic_mfr=%d flags=0x%02x",
             (unsigned long long) device.address_uint64(), device.get_rssi(),
             (unsigned) device.get_service_uuids().size(), (unsigned) device.get_manufacturer_datas().size(),
             has_flic_uuid, has_flic_mfr, conn_flags & 0xFF);
  }
  if (!has_flic_uuid)
    return;
  // Flic 2/Duo: skip a device already connected to another central (flags bit1) so we don't hijack it.
  if (this->is_flic2_family_() && conn_flags >= 0 && (conn_flags & 0x02) != 0)
    return;
  const int rssi = device.get_rssi();
  if (rssi > this->pair_best_rssi_) {
    this->pair_best_rssi_ = rssi;
    this->pair_candidate_ = device.address_uint64();
  }
}

void FlicClientBase::commit_pair_candidate_() {
  this->pair_scan_active_ = false;
  if (!this->pairing_mode_)  // window already closed (e.g. end_pairing_ fired) — don't target anything
    return;
  ESP_LOGI(TAG, "mac-optional: scan window saw %u adverts total (best flic rssi %d)",
           this->pair_adverts_seen_, this->pair_best_rssi_);
  if (this->pair_candidate_ == 0) {
    if (this->pairing_mode_) {
      ESP_LOGW(TAG, "mac-optional: no %s in public mode found — keep it in pairing mode; rescanning",
               this->device_kind_());
      this->start_pair_scan_();  // retry within the ~60s pairing window
    }
    return;
  }
  ESP_LOGI(TAG, "mac-optional: found %s %012llx (rssi %d) — targeting + connecting to pair",
           this->device_kind_(), (unsigned long long) this->pair_candidate_, this->pair_best_rssi_);
  this->parent()->set_address(this->pair_candidate_);
  this->parent()->set_auto_connect(true);
  // The tracker promotes+connects on the next advert from this address (it is actively
  // advertising), then the armed pairing_mode_ drives full-verify.
}

void FlicClientBase::end_pairing_(bool success) {
  this->pairing_mode_ = false;
  this->cancel_timeout("pairing");
  this->cancel_timeout("pair_scan");  // stop a pending discovery scan from firing after the window
  this->pair_scan_active_ = false;
  this->session_ = FlicSession::IDLE;
  // Reconnect: on success we now have valid creds and the reconnect quick-verifies; on failure
  // we simply drop and wait for the next attempt.
  this->parent()->disconnect();
  (void) success;
}

void FlicClientBase::write_authenticated_(const uint8_t *opcode_payload, size_t len) {
  // Only called after verify derives chaskey_keys_. MAC covers opcode+payload (for both families;
  // the Flic 2 frame header, added in emit_frame_, is excluded — per the spec and pyflic-ble).
  uint8_t buf[128];
  if (len + MAC_LEN > sizeof(buf)) {
    ESP_LOGW(TAG, "packet too large (%u)", (unsigned) len);
    return;
  }
  memcpy(buf, opcode_payload, len);
  chaskey_mac_dir_counter(this->chaskey_keys_, /*direction=*/1, this->counter_to_button_, buf, len,
                          buf + len);
  this->counter_to_button_++;
  this->emit_frame_(buf, len + MAC_LEN);
}

void FlicClientBase::write_raw_(const uint8_t *data, size_t len) { this->emit_frame_(data, len); }

void FlicClientBase::gattc_write_(const uint8_t *data, size_t len) {
  if (this->write_handle_ == 0) {
    ESP_LOGW(TAG, "write before write handle known");
    return;
  }
  esp_err_t st = esp_ble_gattc_write_char((esp_gatt_if_t) this->parent()->get_gattc_if(),
                                          this->parent()->get_conn_id(), this->write_handle_,
                                          (uint16_t) len, const_cast<uint8_t *>(data),
                                          ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (st != ESP_OK) {
    this->last_write_err_ = st;
    ESP_LOGW(TAG, "gattc_write failed: %d", st);
  }
}

}  // namespace flic
}  // namespace esphome

#endif  // USE_ESP32

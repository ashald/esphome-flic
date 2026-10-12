# Board families

A hub holds one Bluetooth LE connection per Flic, and the ESP32 families differ in how many they
allow out of the box. Two limits decide it: the Bluetooth host's, which is the same on every chip,
and the radio controller's, which depends on the family. The figures are for ESPHome 2026.9 with
ESP-IDF 5.5. "Untested" marks what comes from the ESP-IDF sources rather than a running hub.

| Family | Flics per hub | Settings that matter | Tested on |
| --- | --- | --- | --- |
| ESP32 | 6 | `esp32_ble: max_connections` | Adafruit Feather ESP32 V2 |
| ESP32-S3 | 6, but 5 until the controller limit is raised | `max_connections`, `CONFIG_BT_CTRL_BLE_MAX_ACT` | Seeed Studio XIAO ESP32S3 |
| ESP32-C3 | untested; same controller as the S3 | as the S3 | |
| ESP32-C5, ESP32-C6 | untested; 3 until the controller limit is raised | `max_connections`, `CONFIG_BT_LE_MAX_CONNECTIONS` | |
| ESP32-C2 | 2 at most | | |
| ESP32-S2 | none: no Bluetooth | | |

## Six per hub on every family

ESPHome uses the Bluedroid Bluetooth host, which has room for eight GATT applications and keeps
two of them for itself (`GATT_MAX_APPS` in ESP-IDF's `bt_target.h`, a compile-time constant).
Every Flic slot is a BLE client that registers one of the other six when the hub boots. A seventh
slot's registration fails with status 133, ESPHome marks that client failed, and its device never
pairs or connects, however strong its signal. It is the slot that registers last, normally the
last one in the config. ESPHome accepts up to nine BLE clients, so nothing warns about this at
compile time. Other BLE clients on the same board, such as `bluetooth_proxy` connection slots,
take from the same six. Spread larger installations over several hubs.

Set `esp32_ble: max_connections` to the number of slots; ESPHome only warns when it is lower.
It sets the host's connection limit and, on the classic ESP32, the controller's as well, so a
classic ESP32 needs nothing more.

## ESP32-S3 and ESP32-C3: the controller's activity limit

The S3 and C3 share a radio controller that limits its concurrent activities, connections and
scanning counted together, to 6 by default (`CONFIG_BT_CTRL_BLE_MAX_ACT`). The hub scans the whole
time it runs, so out of the box only five Flics can be connected, and `max_connections` doesn't
change that. The sixth device is seen advertising, but every connection attempt fails:

```
[I][esp32_ble_client:148]: [5] [80:E4:DA:xx:xx:xx] 0x00 Connecting
[D][esp32_ble_client:424]: [5] [80:E4:DA:xx:xx:xx] ESP_GATTC_DISCONNECT_EVT, reason 0x100
[E][esp32_ble_client:369]: [5] [80:E4:DA:xx:xx:xx] ESP_GATTC_OPEN_EVT in DISCONNECTING state (status=133)
[W][esp32_ble_client:268]: [5] [80:E4:DA:xx:xx:xx] Connection open error, status=133
```

Raise the limit to the controller's maximum, 10. Each activity reserves about 830 bytes of RAM,
so it is worth setting on any S3 or C3 hub before the day a sixth device is added:

```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_BT_CTRL_BLE_MAX_ACT: "10"
```

## ESP32-C5 and ESP32-C6 (untested)

These use a newer controller with its own connection limit, `CONFIG_BT_LE_MAX_CONNECTIONS`, which
defaults to 3 with ESPHome's Bluetooth host. `max_connections` doesn't raise it either; set it to
the number of slots the same way. The ESP32-C2's controller allows two connections at most.

## Memory

Without PSRAM, the Wi-Fi and Bluetooth host buffers come out of internal RAM, which the controller
needs too. On a board with PSRAM, enable it with the `psram` component (with esp-idf the board
definition alone doesn't) and route those buffers into it:

```yaml
psram:
  mode: quad     # 8 MB ESP32-S3 boards: octal
  speed: 40MHz   # octal: 80MHz

esp32:
  framework:
    sdkconfig_options:
      CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP: "y"     # Wi-Fi and LWIP buffers
      CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST: "y"   # Bluetooth host buffers
```

With this, a Feather ESP32 V2 (classic ESP32, 2 MB of PSRAM) holding five Twists kept about 130 KB
of internal heap free, and each added connection cost almost no internal RAM. The six-slot limit
above is reached long before memory runs out.

## Board notes

**Adafruit Feather ESP32 V2:** classic ESP32 (ESP32-PICO-V3-02) with 2 MB of quad PSRAM.

**Seeed Studio XIAO ESP32S3:** 8 MB of octal PSRAM. The user LED on GPIO21 is active-low, and
ESPHome leaves the pin alone unless told otherwise. To keep the LED dark, declare it as an output;
a GPIO output writes its off level at boot:

```yaml
output:
  - platform: gpio
    id: user_led
    pin:
      number: GPIO21
      inverted: true
```

The charge LED beside the USB-C port is driven by the charger chip and can't be turned off from
firmware.

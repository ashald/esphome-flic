# Board families

How many Flics one hub holds depends on the ESP32 family. This page covers the families we have
run hubs on, with ESPHome 2026.9.0. Others haven't been tested.

| Family | Tested board | Flics per hub |
| --- | --- | --- |
| ESP32 | Adafruit Feather ESP32 V2 | 6 |
| ESP32-S3 | Seeed Studio XIAO ESP32S3 | 6 with `CONFIG_BT_CTRL_BLE_MAX_ACT: "10"`, otherwise 5 |

On both, set `esp32_ble: max_connections` to at least the number of Flics.

## ESP32

Six Flics per hub work. A seventh fails at boot: its BLE client can't register with the
Bluetooth host (status 133), ESPHome marks it failed, and that Flic never pairs or connects,
however good its signal. ESPHome builds such a config without error. Spread more Flics over
several hubs.

## ESP32-S3

With ESPHome's defaults, an S3 hub connects only five Flics. The sixth is seen advertising, but
every connection attempt fails:

```
[I][esp32_ble_client:148]: [5] [80:E4:DA:xx:xx:xx] 0x00 Connecting
[D][esp32_ble_client:424]: [5] [80:E4:DA:xx:xx:xx] ESP_GATTC_DISCONNECT_EVT, reason 0x100
[E][esp32_ble_client:369]: [5] [80:E4:DA:xx:xx:xx] ESP_GATTC_OPEN_EVT in DISCONNECTING state (status=133)
[W][esp32_ble_client:268]: [5] [80:E4:DA:xx:xx:xx] Connection open error, status=133
```

ESPHome leaves the S3's Bluetooth controller at 6 concurrent activities
(`CONFIG_BT_CTRL_BLE_MAX_ACT`), and the scan the hub keeps running counts as one of them. Raising
`max_connections` doesn't change that limit. Raising the limit itself does: at 10, all six Flics
connect.

```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_BT_CTRL_BLE_MAX_ACT: "10"
```

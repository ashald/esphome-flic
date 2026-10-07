# Changelog

## 0.1.0 - 2026-10-07

First release of `flic`, an ESPHome component that lets an ESP32 hold the Bluetooth link to Flic
devices directly, with no Flic Hub, phone or Home Assistant Bluetooth proxy in between.

### Devices

- **Flic Twist:** click, double click and hold; rotation events in the selector, default or
  continuous `push_twist_mode`; a writable 0-100 % position per mode that drives the LED ring.
  Positions are kept in flash and restored whenever the Twist reconnects.
- **Flic 2:** click, double click and hold, including a click released after 0.5-1 s.
- **Flic Duo:** per button, click, double click, hold, swipes in four directions and push-twist
  rotation, plus a 0-100 % dial per button. A press is treated as a twist only after the button
  has been held half a second and the Duo has turned 10°, so holds and swipes aren't mistaken for
  twists.

### Everything else

- Pairing on the ESP with a Pair button; a slot without a MAC learns the address of the device it
  pairs. Pairing a Flic 2 into a Duo slot, or the reverse, is refused with a clear log line.
- Diagnostics per device: connection, status, battery, RSSI, firmware, name, MAC and type. Hub
  sensors count the configured, paired and connected devices.
- OTA updates ask every connected device to keep advertising, so all of them reconnect on their
  own after the update.
- Flic 2 and Duo answer the device's keep-alive pings and keep advertising for 24 hours after a
  lost link, so the hub can reconnect without a press.
- Ready-made packages per device type, a Home Assistant example that makes a Twist the dimmer of a
  light, and host tests of the protocol logic.

### Known limitations

- Only tested with ESPHome 2026.9.0.
- The Duo decides swipe directions itself and reads many swipes as "down".
- The 24-hour reconnect advertising for Flic 2 and Duo is new and not yet proven over a long
  outage.
- Not implemented: raw up/down events, rotation speed, renaming a device, firmware updates, the
  Duo's colour and accelerometer data, and checking signatures on packets from the device.

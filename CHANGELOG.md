# Changelog

## 0.2.0 - 2026-10-16

### Fixed

- **Twist battery level.** A Twist never reports more than 2.419 V, however fresh its batteries, so
  the old 3.0 V-full scale showed every healthy Twist at about 42 %. 2.419 V now reads 100 %; the level
  falls as aging batteries report less (0 % at 2.0 V is an estimate).

### Added

- **Twist `press` and `release` events.** The Twist reports its button going down and coming up as
  they happen, before `click`, `double_click` or `hold` is known, so a config can react without
  waiting for the double-click window. The release that ends a double click is `double_click`.
- **Stable entity ids in the packages.** The Twist package gives its Position number and Events entity
  the ids `<tw_id>_position` and `<tw_id>_events`, and the Flic 2 package its Events entity
  `<fb_id>_events`, so your own YAML can read them or add `on_event` automations
  (`- id: !extend <tw_id>_events`).
- **Stuck Twist recovery.** A Twist can get stuck reporting tiny back-and-forth turns several
  times a second while nobody touches it, with its LED ring lit, until its session restarts. The
  hub now spots this (30 s of reports with no turn and no press) and reconnects the Twist, at most
  once every 5 minutes, counting each time in the new `stuck_reconnects` sensor ("Stuck
  reconnects" in the Twist package).
- **Reconnect button** (`type: reconnect`, in all three packages, hidden by default): the device
  drops its link and the hub reconnects it on a fresh session, without restarting the hub.
- **Disconnect diagnostics.** Every device reports `disconnects`, the open links that dropped since
  boot, and `last_disconnect`: why the link ended (supervision timeout, ended by the device, ended
  by the hub, ...) and how long it had been up. The log also shows the connection parameters each
  device accepted.

### Changed

- **OTA updates keep the Flic links up until the reboot.** The hub used to ask every device to
  drop its link when an OTA started; some stopped advertising before the new firmware was up and
  stayed offline until pressed. Now the links stay up through the upload and the reboot drops
  them abruptly, so the devices keep advertising until the hub reconnects them. The boot log
  reports how many links the last OTA kept.

### Documentation

- **Board families** ([docs/boards.md](docs/boards.md)): how many devices a hub holds on the
  families we tested, a classic ESP32 and an ESP32-S3, and the sdkconfig option an ESP32-S3
  needs to connect a sixth device. The quick start now sets it.

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

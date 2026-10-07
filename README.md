# esphome-flic: Flic Twist, Flic 2 and Flic Duo on ESPHome

`flic` is an ESPHome external component that lets an ESP32 own the Bluetooth link to Flic
devices directly: no Flic Hub, no phone, no Home Assistant Bluetooth adapter or proxy. Each device is
paired to the ESP, kept connected around the clock, and shows up in Home Assistant as a
sub-device of the ESP node with events, numbers and diagnostics.

| Device | `device_type` | What you get |
| --- | --- | --- |
| Flic Twist | `twist` (default) | click / double click / hold, rotation events, a writable 0-100 % position that drives the LED ring |
| Flic 2 | `button` | click / double click / hold |
| Flic Duo | `duo` | per button (big, small): click / double click / hold, four swipe gestures, push-twist rotation and a 0-100 % dial |

Why a dedicated ESP: the links stay up when Home Assistant restarts, presses arrive in
~100 ms, and one board serves several devices. Pairing, reconnection and the session
protocol all run on the ESP.

## Hardware

- An ESP32 with the `esp-idf` framework. An ESP32-S3 is the comfortable choice; a classic
  ESP32 handles about 6 devices.
- Every device is one BLE connection: set `esp32_ble: max_connections` to the number of
  slots.
- Keep the board a few metres from Wi-Fi access points and other 2.4 GHz transmitters. A hub
  sitting next to an AP saw roughly 100 times more link drops than identical hubs elsewhere.
- Tested with ESPHome 2026.9.0 on Adafruit Feather ESP32 V2 hubs, classic ESP32 with six devices
  each, and a Seeed XIAO ESP32S3. Device firmware: Flic Twist 2, Flic 2 11, Flic Duo 15.

## Quick start

The quickest route is the ready-made packages, one per device type, each with the device's full
entity set. A complete hub configuration then only has to list its devices:

```yaml
esphome:
  name: flic-hub

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome
logger:

external_components:
  - source: github://ashald/esphome-flic
    components: [flic]

esp32_ble:
  max_connections: 3   # one per Flic below

esp32_ble_tracker:
  scan_parameters:
    active: false
    connection_scan_window: 150ms   # keep listening for devices re-advertising while links are up

packages:
  flic:
    url: https://github.com/ashald/esphome-flic
    files:
      - path: packages/flic-twist.yaml
        vars: {tw_id: hall, tw_name: "Hall"}
      - path: packages/flic-button.yaml
        vars: {fb_id: door, fb_name: "Front Door"}
      - path: packages/flic-duo.yaml
        vars: {fd_id: desk, fd_name: "Desk"}

sensor:
  - platform: flic   # hub level: Flics configured, paired and connected
    slots:
      name: "Flic slots"
    paired:
      name: "Flics paired"
    connected:
      name: "Flics connected"
```

Each `vars` entry adds one device. For a second device of the same type, repeat its path with
another `*_id` and `*_name`.

To work from a local checkout instead, for example this repository added as a git submodule
named `esphome-flic` next to your configs, point both at it:

```yaml
external_components:
  - source:
      type: local
      path: esphome-flic/components
    components: [flic]

packages:
  hall: !include
    file: esphome-flic/packages/flic-twist.yaml
    vars: {tw_id: hall, tw_name: "Hall"}
```

Then flash the hub, and pair each device as described under Pairing.

## Packages

Each package declares the device's BLE client, its `flic` entry and every entity, grouped
into one Home Assistant device named "<name> Flic Twist", "<name> Flic Button" or "<name> Flic
Duo". The optional `tw_mac`, `fb_mac` or `fd_mac` variable pins the device's address; without it
the slot learns the address when pairing. The packages double as the reference for configuring a
device by hand.

<details>
<summary><code>packages/flic-twist.yaml</code></summary>

```yaml
# One Flic Twist on an ESPHome Flic hub. Include it once per Twist:
#
#   packages:
#     hall: !include
#       file: packages/flic-twist.yaml
#       vars: {tw_id: hall, tw_name: "Hall"}    # tw_mac: optional, learned when pairing
#
# The Twist appears in Home Assistant as its own device, "<tw_name> Flic Twist", under the hub,
# with: Position (the LED-ring position of the active mode), Events, Mode, Battery, Battery
# voltage, RSSI, Connected, Type, Status, Firmware, Name, MAC, and the Pair, Dump creds and
# Unpair buttons (the last two hidden by default). Pair it with the Pair button; the credentials
# are kept in the hub's flash. push_twist_mode is selector (see the README for the modes). More
# ring positions can be exposed as extra numbers with `twist_mode: <0-12>`.

substitutions:
  tw_mac: "00:00:00:00:00:00"

esphome:
  devices:
    - id: ${tw_id}_dev
      name: "${tw_name} Flic Twist"

ble_client:
  - id: ${tw_id}_ble
    mac_address: ${tw_mac}

flic:
  - id: ${tw_id}
    ble_client_id: ${tw_id}_ble
    push_twist_mode: selector

number:
  - platform: flic
    flic_id: ${tw_id}
    device_id: ${tw_id}_dev
    name: "Position"

event:
  - platform: flic
    flic_id: ${tw_id}
    device_id: ${tw_id}_dev
    name: "Events"

sensor:
  - platform: flic
    flic_id: ${tw_id}
    mode:
      name: "Mode"
      device_id: ${tw_id}_dev
    battery_level:
      name: "Battery"
      device_id: ${tw_id}_dev
    battery_voltage:
      name: "Battery voltage"
      device_id: ${tw_id}_dev
    rssi:
      name: "RSSI"
      device_id: ${tw_id}_dev

button:
  - platform: flic
    flic_id: ${tw_id}
    type: pair
    device_id: ${tw_id}_dev
    name: "Pair"
  # One-shot creds export to migrate this Twist to a new board WITHOUT re-pairing (logs the bond
  # secret to the ESP log) — hidden by default; enable it in HA only when migrating.
  - platform: flic
    flic_id: ${tw_id}
    type: dump_creds
    device_id: ${tw_id}_dev
    name: "Dump creds"
    disabled_by_default: true
  # Wipe stored creds to free this slot for re-pairing (e.g. move to another hub) — destructive,
  # so hidden by default; enable it in HA only when intentionally unpairing.
  - platform: flic
    flic_id: ${tw_id}
    type: unpair
    device_id: ${tw_id}_dev
    name: "Unpair"
    disabled_by_default: true

binary_sensor:
  - platform: flic
    flic_id: ${tw_id}
    connected:
      name: "Connected"
      device_id: ${tw_id}_dev

text_sensor:
  - platform: flic
    flic_id: ${tw_id}
    type:
      name: "Type"
      device_id: ${tw_id}_dev
    status:
      name: "Status"  # not paired / disconnected / connected
      device_id: ${tw_id}_dev
    firmware_version:
      name: "Firmware"
      device_id: ${tw_id}_dev
    device_name:
      name: "Name"
      device_id: ${tw_id}_dev
    mac:
      name: "MAC"
      device_id: ${tw_id}_dev
```

</details>

<details>
<summary><code>packages/flic-button.yaml</code></summary>

```yaml
# One Flic 2 on an ESPHome Flic hub. Include it once per button:
#
#   packages:
#     door: !include
#       file: packages/flic-button.yaml
#       vars: {fb_id: door, fb_name: "Front Door"}    # fb_mac: optional, learned when pairing
#
# The button appears in Home Assistant as its own device, "<fb_name> Flic Button", under the hub,
# with: Events (click, double_click, hold), Battery, Battery voltage, RSSI, Connected, Type,
# Status, Firmware, Name, MAC, and the Pair, Dump creds and Unpair buttons (the last two hidden by
# default). Pair it with the Pair button; the credentials are kept in the hub's flash.

substitutions:
  fb_mac: "00:00:00:00:00:00"

esphome:
  devices:
    - id: ${fb_id}_dev
      name: "${fb_name} Flic Button"

ble_client:
  - id: ${fb_id}_ble
    mac_address: ${fb_mac}

flic:
  - id: ${fb_id}
    device_type: button
    ble_client_id: ${fb_id}_ble

event:
  - platform: flic
    flic_id: ${fb_id}
    device_id: ${fb_id}_dev
    name: "Events"

sensor:
  - platform: flic
    flic_id: ${fb_id}
    battery_level:
      name: "Battery"
      device_id: ${fb_id}_dev
    battery_voltage:
      name: "Battery voltage"
      device_id: ${fb_id}_dev
    rssi:
      name: "RSSI"
      device_id: ${fb_id}_dev

button:
  - platform: flic
    flic_id: ${fb_id}
    type: pair
    device_id: ${fb_id}_dev
    name: "Pair"
  - platform: flic
    flic_id: ${fb_id}
    type: dump_creds
    device_id: ${fb_id}_dev
    name: "Dump creds"
    disabled_by_default: true
  - platform: flic
    flic_id: ${fb_id}
    type: unpair
    device_id: ${fb_id}_dev
    name: "Unpair"
    disabled_by_default: true

binary_sensor:
  - platform: flic
    flic_id: ${fb_id}
    connected:
      name: "Connected"
      device_id: ${fb_id}_dev

text_sensor:
  - platform: flic
    flic_id: ${fb_id}
    type:
      name: "Type"
      device_id: ${fb_id}_dev
    status:
      name: "Status"  # not paired / disconnected / connected
      device_id: ${fb_id}_dev
    firmware_version:
      name: "Firmware"
      device_id: ${fb_id}_dev
    device_name:
      name: "Name"
      device_id: ${fb_id}_dev
    mac:
      name: "MAC"
      device_id: ${fb_id}_dev
```

</details>

<details>
<summary><code>packages/flic-duo.yaml</code></summary>

```yaml
# One Flic Duo on an ESPHome Flic hub. Include it once per Duo:
#
#   packages:
#     desk: !include
#       file: packages/flic-duo.yaml
#       vars: {fd_id: desk, fd_name: "Desk"}    # fd_mac: optional, learned when pairing
#
# The Duo appears in Home Assistant as its own device, "<fd_name> Flic Duo", under the hub, with:
# Big button and Small button events (click, double_click, hold, four swipes, push-twist
# rotation), Big dial and Small dial numbers (0-100 %, turned by holding the button and twisting
# the Duo), Battery, Battery voltage, RSSI, Connected, Type, Status, Firmware, Name, MAC, and the
# Pair, Dump creds and Unpair buttons (the last two hidden by default). Pair it with the Pair
# button, then hold the Duo's big button for about 7 s until it flashes.

substitutions:
  fd_mac: "00:00:00:00:00:00"

esphome:
  devices:
    - id: ${fd_id}_dev
      name: "${fd_name} Flic Duo"

ble_client:
  - id: ${fd_id}_ble
    mac_address: ${fd_mac}

flic:
  - id: ${fd_id}
    device_type: duo
    ble_client_id: ${fd_id}_ble

event:
  - platform: flic
    flic_id: ${fd_id}
    duo_button: big
    device_id: ${fd_id}_dev
    name: "Big button"
  - platform: flic
    flic_id: ${fd_id}
    duo_button: small
    device_id: ${fd_id}_dev
    name: "Small button"

number:
  - platform: flic
    flic_id: ${fd_id}
    duo_button: big
    device_id: ${fd_id}_dev
    name: "Big dial"
  - platform: flic
    flic_id: ${fd_id}
    duo_button: small
    device_id: ${fd_id}_dev
    name: "Small dial"

sensor:
  - platform: flic
    flic_id: ${fd_id}
    battery_level:
      name: "Battery"
      device_id: ${fd_id}_dev
    battery_voltage:
      name: "Battery voltage"
      device_id: ${fd_id}_dev
    rssi:
      name: "RSSI"
      device_id: ${fd_id}_dev

button:
  - platform: flic
    flic_id: ${fd_id}
    type: pair
    device_id: ${fd_id}_dev
    name: "Pair"
  - platform: flic
    flic_id: ${fd_id}
    type: dump_creds
    device_id: ${fd_id}_dev
    name: "Dump creds"
    disabled_by_default: true
  - platform: flic
    flic_id: ${fd_id}
    type: unpair
    device_id: ${fd_id}_dev
    name: "Unpair"
    disabled_by_default: true

binary_sensor:
  - platform: flic
    flic_id: ${fd_id}
    connected:
      name: "Connected"
      device_id: ${fd_id}_dev

text_sensor:
  - platform: flic
    flic_id: ${fd_id}
    type:
      name: "Type"
      device_id: ${fd_id}_dev
    status:
      name: "Status"  # not paired / disconnected / connected
      device_id: ${fd_id}_dev
    firmware_version:
      name: "Firmware"
      device_id: ${fd_id}_dev
    device_name:
      name: "Name"
      device_id: ${fd_id}_dev
    mac:
      name: "MAC"
      device_id: ${fd_id}_dev
```

</details>

## Pairing

1. Press the slot's **Pair** button in Home Assistant. A 60 s pairing window opens.
2. Put the device in pairing mode: hold its button (the Duo's big button) for about 7 s until
   it flashes.
3. The ESP bonds it, stores the credentials in flash, and from then on reconnects by itself.

With `mac_address: 00:00:00:00:00:00` the slot scans for the nearest device of its type in
pairing mode and remembers its address. A Flic 2 and a Duo advertise identically; if the wrong
one answers, the pairing is refused with a log line naming the right `device_type`.

Credentials are stored per slot `id`, so renaming the `id` loses the bond. `pairing_id` and
`pairing_key` can seed credentials obtained elsewhere. The diagnostic **Dump creds** button
logs them for moving a device to another board without re-pairing. **Unpair** forgets them
locally. Both are meant to stay disabled in Home Assistant until needed.

A Flic stays bonded to whatever it was paired with before (a phone, a Flic Hub). Only one
client can hold its connection at a time, so remove it from the other system first.

## Configuration

Per device (`flic:` list entries):

| Option | Default | Applies to | Meaning |
| --- | --- | --- | --- |
| `id` | required | all | Slot id. Also the key of its stored credentials. |
| `ble_client_id` | required | all | The `ble_client` entry for this device. |
| `device_type` | `twist` | all | `twist`, `button` (Flic 2) or `duo`. |
| `pairing_id`, `pairing_key` | none | all | Optional pre-provisioned credentials, both or neither. |
| `connection_min_interval` | `100ms` | all | Link parameters requested after connecting. The defaults keep an idle device quiet and save its battery. |
| `connection_max_interval` | `113ms` | all | |
| `connection_latency` | `17` | all | |
| `supervision_timeout` | `8000ms` | all | |
| `push_twist_mode` | `selector` | Twist | How the Twist's rotation is reported, see below. Compile-time. |
| `reconnect_advertising_interval` | `1s` | Flic 2, Duo | After the link is lost the device advertises every 100 ms for 5 s, then at this interval, so the hub reconnects without a press. Range 20 ms to 10.24 s. |
| `reconnect_advertising_timeout` | `24h` | Flic 2, Duo | How long it keeps advertising before going quiet until pressed. `0s` restores the device default: a short burst, after which a missed reconnect needs a press. Needs firmware 7 or newer. |
| `dial_range` | `90°` | Duo | Rotation that spans a dial's 0-100 %. Range 10° to 360°. |

### Twist modes

A Twist has 13 modes, each with its own LED-ring position: 12 selector slots, chosen by
pushing and twisting, and mode 12 for plain rotation. `push_twist_mode` picks the behaviour:

- `selector`: twisting moves the active slot's ring and fires `rotate_clockwise` /
  `rotate_counter_clockwise`. Push-and-twist moves between slots and fires
  `selector_changed`. The Mode sensor shows the active slot (0-11) or 12.
- `default`: twisting fires `twist_increment` / `twist_decrement` per 1 % step, push-twist
  fires `push_twist_increment` / `push_twist_decrement`. The ring is bounded 0-100 %.
- `continuous`: like `default`, but the position wraps around instead of stopping at the ends.

## Entities

| Platform | Key | Twist | Flic 2 | Duo | Notes |
| --- | --- | --- | --- | --- | --- |
| `event` | (entity) | ✓ | ✓ | ✓ ×2 | Duo: one per button, `duo_button: big` / `small` required. |
| `number` | (entity) | ✓ | | ✓ ×2 | Twist: ring position, optional `twist_mode: 0-12` to bind a fixed mode. Duo: dial, `duo_button` required. |
| `sensor` | `battery_level` | ✓ | ✓ | ✓ | Percent from the cell voltage. Twist: 2×AAA. Flic 2 and Duo: CR2032. Read hourly. |
| `sensor` | `battery_voltage` | ✓ | ✓ | ✓ | |
| `sensor` | `rssi` | ✓ | ✓ | ✓ | Read on the open link every 60 s. |
| `sensor` | `mode` | ✓ | | | Active Twist mode, learned from the first event after boot. |
| `binary_sensor` | `connected` | ✓ | ✓ | ✓ | On while the authenticated session is up. |
| `text_sensor` | `type` | ✓ | ✓ | ✓ | `twist`, `flic2` or `duo`. |
| `text_sensor` | `status` | ✓ | ✓ | ✓ | `not paired`, `disconnected` or `connected`. |
| `text_sensor` | `firmware_version`, `device_name`, `mac` | ✓ | ✓ | ✓ | |
| `button` | `type: pair` / `dump_creds` / `unpair` | ✓ | ✓ | ✓ | See Pairing. |

Hub level, without `flic_id`: `sensor` keys `slots`, `paired` and `connected` count the
slots on the node, those with credentials, and those with a session up. They are handy for a
"some Flic is offline" alert.

## Events

| Event type | Twist | Flic 2 | Duo button |
| --- | --- | --- | --- |
| `click`, `double_click`, `hold` | ✓ | ✓ | ✓ |
| `rotate_clockwise`, `rotate_counter_clockwise` | selector mode | | push-twist |
| `selector_changed` | selector mode | | |
| `twist_increment`, `twist_decrement`, `push_twist_increment`, `push_twist_decrement` | default and continuous modes | | |
| `swipe_left`, `swipe_right`, `swipe_up`, `swipe_down` | | | ✓ |

- `click` fires once it is certain no second click follows, so a double click produces only
  `double_click`. `hold` fires about 1 s into a press.
- Events a device queued while it was disconnected are not replayed as live events, so a
  reconnect never triggers old presses. Flic 2 and Duo still acknowledge them, which clears
  the device's queue.
- Duo swipes: press a button, then jerk the whole Duo in a direction while sliding your thumb
  that way, and let go. It takes some practice. A press released with a recognised gesture fires
  `swipe_<direction>` instead of `click`. A gesture the Duo noticed but couldn't classify fires
  nothing, so a failed swipe doesn't count as a click.
- Duo push-twist: hold a button and turn the Duo. Turning counts once the button has been held
  half a second and the Duo has turned 10°; the dial then follows the rotation beyond that, and a
  rotate event fires for each notification in which the rotation crossed a 1 % step. This keeps
  the wobble of a hold, a slow click or a swipe from counting as a twist. A press that turned the
  Duo fires no `click`, `double_click` or `hold`, for every button held, except that a swipe
  released within a second still fires. Start turning within about 1 s of pressing, since `hold`
  fires at 1 s. Turning while holding both buttons moves the big button's dial.
- A double click whose presses were not both plain clicks fires what they were. A click followed
  quickly by a swipe fires `click` and the swipe, not `double_click`.
- A Duo's rotate events keep firing past either end of its dial, so they also work for relative
  control such as volume up and down. A Twist's events stop at the ends of its ring unless
  `push_twist_mode` is `continuous`.

## Positions and the set_position action

The Twist's position number and the Duo's dial numbers are two-way: rotation moves them, and
writing them sets them. A Twist shows the written value on its LED ring. A Duo has no indicator,
so writing its dial re-bases it, for example to a light's current brightness so the next twist
continues from there. Values are stored in flash and survive reboots. A Twist write made while
it is disconnected is applied when it reconnects.

```yaml
- flic.set_position:
    id: hall
    position: !lambda return x;   # 0-100
    twist_mode: 3                  # Twist: optional, default = the mode it is in
- flic.set_position:
    id: desk
    position: 40
    duo_button: big                # Duo: required
```

## Home Assistant: a Twist as a dimmer

These two automations make a Twist the dimmer of one light. Turning it sets the brightness, and
the LED ring follows the light whenever it changes anywhere else: an app, a schedule or Adaptive
Lighting. The entities are the ones `packages/flic-twist.yaml` creates for `tw_name: "Hall"`;
replace `light.hall` with your light. Add them to `automations.yaml` or paste each one into the
automation editor's YAML mode.

```yaml
# A Flic Twist as a dimmer: turning it sets the light's brightness, and the LED ring follows the
# light whenever it changes anywhere else (an app, a schedule, Adaptive Lighting). The entities come
# from packages/flic-twist.yaml with tw_name "Hall"; replace light.hall with your light.
- id: hall_twist_sets_light
  alias: "Hall Twist: dial sets the light"
  mode: restart
  triggers:
    # The rotate events, not the number: the number also changes when the ring is set from the
    # light below, and reacting to that would loop.
    - trigger: state
      entity_id: event.hall_flic_twist_events
  conditions:
    - "{{ trigger.from_state is not none and trigger.from_state.state not in ['unknown', 'unavailable'] }}"
    - "{{ trigger.to_state.attributes.event_type in ['rotate_clockwise', 'rotate_counter_clockwise', 'twist_increment', 'twist_decrement'] }}"
  actions:
    # Fold a burst of rotation events into one light command.
    - delay:
        milliseconds: 50
    - variables:
        pct: "{{ states('number.hall_flic_twist_position') | int(0) }}"
    - if: "{{ pct == 0 }}"
      then:
        - action: light.turn_off
          target:
            entity_id: light.hall
      else:
        - action: light.turn_on
          target:
            entity_id: light.hall
          data:
            brightness_pct: "{{ pct }}"

- id: hall_light_sets_ring
  alias: "Hall Twist: ring follows the light"
  mode: restart
  triggers:
    - trigger: state
      entity_id: light.hall
    - trigger: state
      entity_id: binary_sensor.hall_flic_twist_connected
      to: "on"
  actions:
    # Let changes settle first, e.g. while the dial is being turned: the ring then already shows
    # the new brightness and nothing is written.
    - delay:
        seconds: 1
    - variables:
        pct: >-
          {{ (state_attr('light.hall', 'brightness') | int(0) / 2.55) | round
             if is_state('light.hall', 'on') else 0 }}
    - condition: template
      value_template: >-
        {{ is_state('binary_sensor.hall_flic_twist_connected', 'on')
           and (states('number.hall_flic_twist_position') | float(-100) - pct) | abs > 2 }}
    - action: number.set_value
      target:
        entity_id: number.hall_flic_twist_position
      data:
        value: "{{ pct }}"
```

- **Rotation is read from the event entity**, not from Position. Position also changes when the
  second automation sets the ring, and reacting to that would loop.
- **The ring update waits one second** for changes to settle. While the dial turns, the light
  follows it, so afterwards the ring already matches and nothing is written.
- **A 2 % deadband** stops rounding between the dial and the light's 0-255 brightness from
  causing writes.
- **A reconnect resyncs the ring**, although the hub already restores it from flash.
- **In selector mode**, Position and the rotate events follow whichever slot is active, so the
  light follows the dial on every slot. To give slots different jobs, add a number per slot with
  `twist_mode:` and check the Mode sensor in the first automation.
- **For a Duo**, use a button's event entity and dial, for example
  `event.desk_flic_duo_big_button` and `number.desk_flic_duo_big_dial`. There is no ring, but
  keeping the dial in step lets the next twist start from the light's current brightness.

## Reliability notes

- **OTA updates.** Before an OTA reboot the hub asks every connected device to drop the link
  and keep advertising, so all of them reconnect on their own within seconds of the new
  firmware starting. Without that, a device treats the hub's clean disconnect as "the host
  left" and stays silent until pressed.
- **Reconnect advertising** (Flic 2, Duo): see `reconnect_advertising_interval` above. The
  Twist protocol has no equivalent setting.
- **Session duties.** The component answers the device's ping requests, re-verifies when a
  device ends the session, and disables the device's idle auto-disconnect.
- **Scanning.** ESPHome shrinks its scan window to 30 ms while connections are up. 150 ms
  (`connection_scan_window`) catches re-advertising devices much more reliably. Passive
  scanning is better for hubs that only serve Flics.

## Not implemented

- Raw button up / down events, and rotation speed or acceleration. Both are in the protocol
  and could be added as extra event types.
- Setting the device name, and firmware updates.
- The Duo's colour query and accelerometer readings (the latter appear in debug logs).
- Checking the signature on packets from the device. The specification asks for it; like
  pyflic-ble, the component only signs what it sends.

## Tests

`tests/flic/run.sh` runs host tests of the protocol logic. It needs
Python 3 and a C++17 compiler.

- **Flic 2 event codes** (`flic2_events.h`): all 16 codes against the specification's table.
- **Duo event decoder** (`duo_codec.h`): random packets encoded straight from the Duo
  specification, checked against a reference decoder written from the same text. If
  [pyflic-ble](https://github.com/50ButtonsEach/pyflic-ble) is importable it is cross-checked
  as well. That library drops a packet's last event when the event starts mid-byte and is
  shorter than 47 bits.
- **Duo behaviour** (`duo_input.h`): scripted presses, swipes and twists, covering the rules
  in Events above.

## Protocol and credits

- Flic's protocol specifications for the Flic 2 and the Duo:
  <https://github.com/50ButtonsEach/flic2-documentation/wiki>.
- [pyflic-ble](https://github.com/50ButtonsEach/pyflic-ble) by Shortcut Labs, Apache-2.0: the
  Twist protocol, the verify and crypto flows, and the rotation trackers were ported from it.
- [TweetNaCl](https://tweetnacl.cr.yp.to/), public domain, for X25519 and Ed25519 during
  pairing. SHA-256 and HMAC come from mbedTLS in ESP-IDF.

## License

Apache License 2.0, see `LICENSE`. `NOTICE` lists what this builds on: code ported from
pyflic-ble, also Apache-2.0, and TweetNaCl, public domain and included unmodified.

Flic, Flic 2, Flic Twist and Flic Duo are trademarks of Shortcut Labs AB. This project is not
affiliated with, endorsed by or sponsored by Shortcut Labs.

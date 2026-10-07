# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Borys Pierov
"""flic: ESPHome external component driving Flic devices over BLE.

A dedicated ESP node owns the GATT link to each Flic device (reconnecting on its own),
independent of Home Assistant. Paired on-device via the Pair button (or provisioned with the
pairing_id + pairing_key from an earlier pairing). Three device types share one code base
(FlicClientBase); pick with `device_type:`:

- `twist` (default): a Flic Twist. Decodes button + rotation events, mirrors the knob into a
  writable position `number`, writes the LED ring back.
- `button`: a Flic 2. Surfaces click / double_click / hold events.
- `duo`: a Flic Duo. Two buttons (big / small), each with click / double_click / hold, swipe
  gestures and push-twist rotation, plus a writable 0-100 % dial `number` per button.

Each entry is one device; the `number:` / `event:` / `sensor:` / `binary_sensor:` /
`text_sensor:` / `button:` platforms attach to it. See README.md.
"""

import hashlib
import inspect

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import ble_client, esp32_ble_tracker, ota
from esphome.const import CONF_ID, CONF_POSITION
from esphome.core import CORE, EsphomeError
import esphome.final_validate as fv

CODEOWNERS = ["@ashald"]
DEPENDENCIES = ["ble_client"]
# Pull in the entity component libs the hub references, plus the BLE stack via ble_client.
AUTO_LOAD = [
    "ble_client",
    "number",
    "event",
    "sensor",
    "button",
    "binary_sensor",
    "text_sensor",
]
MULTI_CONF = True

flic_ns = cg.esphome_ns.namespace("flic")
FlicClientBase = flic_ns.class_(
    "FlicClientBase", ble_client.BLEClientNode, cg.Component
)
FlicTwist = flic_ns.class_("FlicTwist", FlicClientBase)
FlicButton = flic_ns.class_("FlicButton", FlicClientBase)
FlicDuo = flic_ns.class_("FlicDuo", FlicButton)
SetPositionAction = flic_ns.class_("SetPositionAction", automation.Action)

PushTwistMode = flic_ns.enum("PushTwistMode")
PUSH_TWIST_MODES = {
    "default": PushTwistMode.PTM_DEFAULT,
    "selector": PushTwistMode.PTM_SELECTOR,
    "continuous": PushTwistMode.PTM_CONTINUOUS,
}

DOMAIN = "flic"
CONF_FLIC_ID = "flic_id"
CONF_DEVICE_TYPE = "device_type"
CONF_TWIST_MODE = "twist_mode"  # a Twist selector mode: 0-11 slots, 12 push-twist
CONF_DUO_BUTTON = "duo_button"  # which Duo button an entity / action is about
CONF_DIAL_RANGE = "dial_range"
CONF_RECONNECT_ADVERTISING_INTERVAL = "reconnect_advertising_interval"
CONF_RECONNECT_ADVERTISING_TIMEOUT = "reconnect_advertising_timeout"
CONF_PAIRING_ID = "pairing_id"
CONF_PAIRING_KEY = "pairing_key"
CONF_PUSH_TWIST_MODE = "push_twist_mode"
CONF_CONNECTION_MIN_INTERVAL = "connection_min_interval"
CONF_CONNECTION_MAX_INTERVAL = "connection_max_interval"
CONF_CONNECTION_LATENCY = "connection_latency"
CONF_SUPERVISION_TIMEOUT = "supervision_timeout"

DEVICE_TWIST = "twist"
DEVICE_BUTTON = "button"
DEVICE_DUO = "duo"
DUO_BUTTONS = {"big": 0, "small": 1}


def device_config(hub_id):
    """The flic entry that declares `hub_id`, during final validation or codegen."""
    try:
        fconf = fv.full_config.get()
    except LookupError:
        fconf = None
    if fconf is not None:
        try:
            return fconf.get_config_for_path(fconf.get_path_for_id(hub_id)[:-1])
        except KeyError:
            return None
    for conf in CORE.config.get(DOMAIN, []):
        if conf[CONF_ID] == hub_id:
            return conf
    return None


def device_type_of(hub_id):
    conf = device_config(hub_id)
    return None if conf is None else conf.get(CONF_DEVICE_TYPE, DEVICE_TWIST)


def selector_for(config, device_type, what):
    """Validate twist_mode / duo_button against the device type and return the C++ selector.

    Twist: the selector mode (twist_mode, optional, -1 = active mode). Duo: the button (duo_button,
    required, 0 big / 1 small). Flic 2: no position. Raises cv.Invalid with a readable message.
    """
    prefix = f"{what}: " if what else ""
    if device_type == DEVICE_DUO:
        if CONF_TWIST_MODE in config:
            raise cv.Invalid(f"{prefix}twist_mode is for a Twist; a Duo takes duo_button: big|small")
        if CONF_DUO_BUTTON not in config:
            raise cv.Invalid(f"{prefix}a Flic Duo needs duo_button: big|small")
        return DUO_BUTTONS[config[CONF_DUO_BUTTON]]
    if CONF_DUO_BUTTON in config:
        raise cv.Invalid(f"{prefix}duo_button only applies to device_type: duo")
    if device_type == DEVICE_TWIST:
        return config.get(CONF_TWIST_MODE, -1)
    raise cv.Invalid(f"{prefix}a Flic 2 button has no position")


def _pairing_key(value):
    value = cv.string_strict(value)
    hexstr = value.replace(":", "").replace(" ", "")
    if len(hexstr) != 32 or any(c not in "0123456789abcdefABCDEF" for c in hexstr):
        raise cv.Invalid("pairing_key must be 16 bytes as 32 hex characters")
    return hexstr.lower()


def _creds_together(config):
    if (CONF_PAIRING_ID in config) != (CONF_PAIRING_KEY in config):
        raise cv.Invalid(
            "pairing_id and pairing_key must be set together (omit both to pair on-device)"
        )
    return config


# Options common to every Flic device family.
_COMMON = {
    # The BLE tracker this device listens on (for mac-optional pair discovery). Registering as a
    # tracker listener at codegen time is what compiles in the tracker's advert-dispatch loop.
    cv.GenerateID(esp32_ble_tracker.CONF_ESP32_BLE_ID): cv.use_id(
        esp32_ble_tracker.ESP32BLETracker
    ),
    # Optional: a device paired on the ESP itself (Pair button) stores its own creds in NVS and
    # needs none here. Provide both to seed creds obtained elsewhere (HA).
    cv.Optional(CONF_PAIRING_ID): cv.int_range(min=0, max=0xFFFFFFFF),
    cv.Optional(CONF_PAIRING_KEY): _pairing_key,
    # Flic's own preferred link params: relaxed interval + high slave latency + long supervision
    # timeout keep an idle device off the air (good for packing several per board) while staying
    # responsive to presses.
    cv.Optional(
        CONF_CONNECTION_MIN_INTERVAL, default="100ms"
    ): cv.positive_time_period_milliseconds,
    cv.Optional(
        CONF_CONNECTION_MAX_INTERVAL, default="113ms"
    ): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_CONNECTION_LATENCY, default=17): cv.int_range(min=0, max=499),
    cv.Optional(
        CONF_SUPERVISION_TIMEOUT, default="8000ms"
    ): cv.positive_time_period_milliseconds,
}


# Flic 2 family (Flic 2, Duo): how the device advertises after it loses the link, so the hub can
# reconnect without a press (SetAdvParameters, firmware >= 7). It always uses 100 ms for the first
# 5 s; then `interval` for `timeout`. 0s timeout = restore the device's default (a short burst).
_FLIC2_FAMILY = {
    cv.Optional(CONF_RECONNECT_ADVERTISING_INTERVAL, default="1s"): cv.All(
        cv.positive_time_period_milliseconds,
        cv.Range(min=cv.TimePeriod(milliseconds=20), max=cv.TimePeriod(milliseconds=10240)),
    ),
    cv.Optional(CONF_RECONNECT_ADVERTISING_TIMEOUT, default="24h"): cv.All(
        cv.positive_time_period_seconds,
        cv.Range(max=cv.TimePeriod(seconds=0xFFFFFFFE)),
    ),
}


def _device_schema(cls, extra=None):
    schema = {cv.GenerateID(): cv.declare_id(cls)}
    schema.update(_COMMON)
    if extra:
        schema.update(extra)
    return (
        cv.Schema(schema).extend(cv.COMPONENT_SCHEMA).extend(ble_client.BLE_CLIENT_SCHEMA)
    )


CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            DEVICE_TWIST: _device_schema(
                FlicTwist,
                {
                    cv.Optional(CONF_PUSH_TWIST_MODE, default="selector"): cv.enum(
                        PUSH_TWIST_MODES, lower=True
                    ),
                },
            ),
            DEVICE_BUTTON: _device_schema(FlicButton, _FLIC2_FAMILY),
            DEVICE_DUO: _device_schema(
                FlicDuo,
                {
                    **_FLIC2_FAMILY,
                    # Rotation that spans a dial's 0-100 % (push-twist, per button).
                    cv.Optional(CONF_DIAL_RANGE, default=90): cv.All(
                        cv.angle, cv.Range(min=10, max=360)
                    ),
                },
            ),
        },
        key=CONF_DEVICE_TYPE,
        default_type=DEVICE_TWIST,
    ),
    _creds_together,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    # Register as a tracker listener at codegen time so the tracker's advert-dispatch loop is
    # compiled in (its parse_device only acts during a mac-optional pair scan).
    await esp32_ble_tracker.register_ble_device(var, config)
    # OTA handoff (FlicOtaHandoff): tell each Flic to keep advertising before an OTA drops its link.
    ota.request_ota_state_listeners()

    # Stable per-instance key for NVS credential storage (survives edits to the config creds).
    storage_hash = int(hashlib.md5(str(config[CONF_ID]).encode()).hexdigest()[:8], 16)
    cg.add(var.set_storage_hash(storage_hash))

    if CONF_PAIRING_ID in config:
        cg.add(var.set_pairing_id(config[CONF_PAIRING_ID]))
    if CONF_PAIRING_KEY in config:
        cg.add(var.set_pairing_key_hex(config[CONF_PAIRING_KEY]))
    if config[CONF_DEVICE_TYPE] == DEVICE_TWIST:
        cg.add(var.set_push_twist_mode(config[CONF_PUSH_TWIST_MODE]))
    if config[CONF_DEVICE_TYPE] in (DEVICE_BUTTON, DEVICE_DUO):
        interval = config[CONF_RECONNECT_ADVERTISING_INTERVAL].total_milliseconds
        cg.add(
            var.set_reconnect_advertising(
                int(round(interval / 0.625)),
                int(config[CONF_RECONNECT_ADVERTISING_TIMEOUT].total_seconds),
            )
        )
    if config[CONF_DEVICE_TYPE] == DEVICE_DUO:
        cg.add(var.set_dial_range_units(int(round(config[CONF_DIAL_RANGE] / 360.0 * 65536))))

    # Convert to BLE units: intervals in 1.25 ms steps, supervision timeout in 10 ms steps.
    min_iv = int(config[CONF_CONNECTION_MIN_INTERVAL].total_milliseconds / 1.25)
    max_iv = int(config[CONF_CONNECTION_MAX_INTERVAL].total_milliseconds / 1.25)
    timeout = int(config[CONF_SUPERVISION_TIMEOUT].total_milliseconds / 10)
    cg.add(var.set_conn_params(min_iv, max_iv, config[CONF_CONNECTION_LATENCY], timeout))


# flic.set_position: set a position (0-100 %):
#   - Twist: the LED ring of a selector mode (twist_mode 0-11, 12 = push-twist; omitted = the mode
#     the Twist is in now). Remembered (NVS) and applied on the next session if disconnected.
#   - Duo: the dial of one button (duo_button: big|small, required) — re-bases it, e.g. to a
#     light's brightness so the next push-twist continues from there.
#
#   - flic.set_position:
#       id: living
#       position: !lambda return x;   # 0-100
#       twist_mode: 3                  # Twist, optional
#       # duo_button: small            # Duo
SET_POSITION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(FlicClientBase),
        cv.Required(CONF_POSITION): cv.templatable(cv.float_range(min=0, max=100)),
        cv.Optional(CONF_TWIST_MODE): cv.templatable(cv.int_range(min=0, max=12)),
        cv.Optional(CONF_DUO_BUTTON): cv.one_of(*DUO_BUTTONS, lower=True),
    }
)


# play() finishes synchronously; the keyword only exists on newer ESPHome releases.
_ACTION_KW = (
    {"synchronous": True}
    if "synchronous" in inspect.signature(automation.register_action).parameters
    else {}
)


@automation.register_action(
    "flic.set_position", SetPositionAction, SET_POSITION_SCHEMA, **_ACTION_KW
)
async def set_position_to_code(config, action_id, template_arg, args):
    device_type = device_type_of(config[CONF_ID])
    selector = None  # Twist without duo_button: twist_mode may be templated, handled below
    if device_type != DEVICE_TWIST or CONF_DUO_BUTTON in config:
        try:
            selector = selector_for(config, device_type, "")
        except cv.Invalid as err:
            raise EsphomeError(f"flic.set_position on '{config[CONF_ID]}': {err}") from err
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_position(await cg.templatable(config[CONF_POSITION], args, float)))
    if selector is not None:
        cg.add(var.set_selector(await cg.templatable(selector, args, cg.int_)))
    elif CONF_TWIST_MODE in config:
        cg.add(var.set_selector(await cg.templatable(config[CONF_TWIST_MODE], args, cg.int_)))
    return var

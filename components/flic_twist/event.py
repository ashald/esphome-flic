# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Borys Pierov
"""Event entities for a Flic device. The registered event types depend on the device type:

- Twist: click / double_click / hold, plus twist_increment / twist_decrement /
  push_twist_increment / push_twist_decrement (push_twist_mode default/continuous) or
  rotate_clockwise / rotate_counter_clockwise / selector_changed (selector). For the absolute knob
  position read the Twist's position `number` instead.
- Flic 2 (`button`): click / double_click / hold.
- Duo: one entity PER BUTTON (`duo_button: big|small`, required): click / double_click / hold,
  swipe_left / swipe_right / swipe_up / swipe_down, and rotate_clockwise /
  rotate_counter_clockwise while the button is held and the Duo turned (push-twist). The dial
  position is the Duo's `number`.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import event

from . import (
    CONF_DUO_BUTTON,
    CONF_FLIC_TWIST_ID,
    DEVICE_BUTTON,
    DEVICE_DUO,
    DEVICE_TWIST,
    DUO_BUTTONS,
    FlicClientBase,
    device_type_of,
)

_PRESSES = ["click", "double_click", "hold"]
EVENT_TYPES = {
    DEVICE_TWIST: _PRESSES
    + [
        "twist_increment",
        "twist_decrement",
        "push_twist_increment",
        "push_twist_decrement",
        "rotate_clockwise",
        "rotate_counter_clockwise",
        "selector_changed",
    ],
    DEVICE_BUTTON: _PRESSES,
    DEVICE_DUO: _PRESSES
    + [
        "swipe_left",
        "swipe_right",
        "swipe_up",
        "swipe_down",
        "rotate_clockwise",
        "rotate_counter_clockwise",
    ],
}

CONFIG_SCHEMA = event.event_schema(event.Event).extend(
    {
        cv.GenerateID(CONF_FLIC_TWIST_ID): cv.use_id(FlicClientBase),
        # Duo only: which of its two buttons this entity reports.
        cv.Optional(CONF_DUO_BUTTON): cv.one_of(*DUO_BUTTONS, lower=True),
    }
)


def _final_validate(config):
    device_type = device_type_of(config[CONF_FLIC_TWIST_ID])
    if device_type == DEVICE_DUO and CONF_DUO_BUTTON not in config:
        raise cv.Invalid("A Flic Duo has one event entity per button: set duo_button: big|small")
    if device_type != DEVICE_DUO and CONF_DUO_BUTTON in config:
        raise cv.Invalid("duo_button only applies to device_type: duo")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    hub = await cg.get_variable(config[CONF_FLIC_TWIST_ID])
    device_type = device_type_of(config[CONF_FLIC_TWIST_ID]) or DEVICE_TWIST
    ev = await event.new_event(config, event_types=EVENT_TYPES[device_type])
    if device_type == DEVICE_DUO:
        cg.add(hub.set_duo_button_event(DUO_BUTTONS[config[CONF_DUO_BUTTON]], ev))
    else:
        cg.add(hub.set_button_event(ev))

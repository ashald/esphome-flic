# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Borys Pierov
"""Writable 0-100 % position number.

Twist: the LED-ring position. By default the number follows the ACTIVE selector mode: physical
rotation (in whatever mode the Twist is in) mirrors into it, and writing it moves that mode's
ring. With `twist_mode: <0-12>` the number is bound to one fixed mode instead, so every selector
slot (0-11) and the push-twist mode (12) can be exposed as its own number. Written values are
remembered (NVS) and applied on the next session if the Twist is disconnected right now, so the
entity keeps showing what the ring really shows.

Duo: the push-twist dial of one button (`duo_button: big|small`, required). Turning the Duo while
holding that button moves it (`dial_range` of rotation = 0-100 %, clamped); writing it re-bases
the dial (the Duo has no indicator), e.g. to a light's brightness so the next twist continues from
there. Remembered in NVS.

Flic 2 buttons have no position. (`mode:` keeps ESPHome's meaning for numbers: the UI style.) For
programmatic use see the flic_twist.set_position action.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import UNIT_PERCENT

from . import (
    CONF_DUO_BUTTON,
    CONF_FLIC_TWIST_ID,
    CONF_TWIST_MODE,
    DEVICE_TWIST,
    DUO_BUTTONS,
    FlicClientBase,
    device_type_of,
    flic_twist_ns,
    selector_for,
)

FlicPositionNumber = flic_twist_ns.class_(
    "FlicPositionNumber", number.Number, cg.Parented.template(FlicClientBase)
)

CONFIG_SCHEMA = number.number_schema(
    FlicPositionNumber, unit_of_measurement=UNIT_PERCENT, icon="mdi:rotate-360"
).extend(
    {
        cv.GenerateID(CONF_FLIC_TWIST_ID): cv.use_id(FlicClientBase),
        # Twist: selector mode this number is bound to (0-11 slots, 12 push-twist); omit = active mode.
        cv.Optional(CONF_TWIST_MODE): cv.int_range(min=0, max=12),
        # Duo: which button's dial.
        cv.Optional(CONF_DUO_BUTTON): cv.one_of(*DUO_BUTTONS, lower=True),
    }
)


def _final_validate(config):
    selector_for(config, device_type_of(config[CONF_FLIC_TWIST_ID]), "number")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    n = await number.new_number(config, min_value=0, max_value=100, step=1)
    await cg.register_parented(n, config[CONF_FLIC_TWIST_ID])
    device_type = device_type_of(config[CONF_FLIC_TWIST_ID]) or DEVICE_TWIST
    cg.add(n.set_selector(selector_for(config, device_type, "number")))
    hub = await cg.get_variable(config[CONF_FLIC_TWIST_ID])
    cg.add(hub.add_position_number(n))

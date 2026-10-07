# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Borys Pierov
"""Diagnostic 'connected' binary sensor for a Flic device (Twist, Flic 2 or Duo).

ON while the authenticated BLE session to the device is established, OFF otherwise. HA's
recorder logs every transition, giving a durable multi-hour record of link stability
(drop count + gap durations) without any external log babysitting.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FLIC_ID, FlicClientBase

CONF_CONNECTED = "connected"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FLIC_ID): cv.use_id(FlicClientBase),
        cv.Optional(CONF_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_FLIC_ID])
    if CONF_CONNECTED in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_CONNECTED])
        cg.add(hub.set_connected_binary_sensor(b))

# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Borys Pierov
"""Text sensors for a Flic device (Twist, Flic 2 or Duo), all needing `flic_twist_id`:

  type              the device type: "twist", "flic2" or "duo",
  status            "not paired" (no creds) / "disconnected" (paired, no session) / "connected",
  firmware_version  the device's firmware version number, requested once per session,
  device_name       the user-settable name stored on the device (GAP Device Name),
  mac               the Bluetooth address the slot targets.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FLIC_TWIST_ID, FlicClientBase

CONF_TYPE = "type"
CONF_STATUS = "status"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_DEVICE_NAME = "device_name"
CONF_MAC = "mac"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_FLIC_TWIST_ID): cv.use_id(FlicClientBase),
        cv.Optional(CONF_TYPE): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:shape",
        ),
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_FIRMWARE_VERSION): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:chip",
        ),
        cv.Optional(CONF_DEVICE_NAME): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_MAC): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_FLIC_TWIST_ID])
    for key, setter in (
        (CONF_TYPE, hub.set_type_text_sensor),
        (CONF_STATUS, hub.set_status_text_sensor),
        (CONF_FIRMWARE_VERSION, hub.set_firmware_text_sensor),
        (CONF_DEVICE_NAME, hub.set_name_text_sensor),
        (CONF_MAC, hub.set_mac_text_sensor),
    ):
        if key in config:
            cg.add(setter(await text_sensor.new_text_sensor(config[key])))

# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Borys Pierov
"""Sensors for the flic_twist component.

Per device (need `flic_twist_id`):
  battery_voltage  measured cell voltage (Twist: 2xAAA millivolts; Flic 2 / Duo: CR2032, 10-bit ADC,
                   3.6 V ref),
                   polled hourly over the authenticated session,
  battery_level    remaining capacity estimated from the voltage with the family's chemistry curve,
  rssi             connection RSSI read on the open link (no scanning),
  mode             (Twist only) the selector mode the knob is in, 0-11 = selector slots, 12 =
                   push-twist; learned from the first event after boot.

Node level (no `flic_twist_id`; one set per hub, from the FlicFleet summary):
  slots      Flic slots configured on this node,
  paired     slots holding pairing credentials,
  connected  slots with an authenticated session up.
Published on every link-status change, so HA can sum a fleet total and alert.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_PERCENT,
    UNIT_VOLT,
)

from . import CONF_FLIC_TWIST_ID, DEVICE_TWIST, FlicClientBase, device_type_of

CONF_BATTERY_VOLTAGE = "battery_voltage"
CONF_BATTERY_LEVEL = "battery_level"
CONF_RSSI = "rssi"
CONF_MODE = "mode"
CONF_SLOTS = "slots"
CONF_PAIRED = "paired"
CONF_CONNECTED = "connected"

_PER_DEVICE = (CONF_BATTERY_VOLTAGE, CONF_BATTERY_LEVEL, CONF_RSSI, CONF_MODE)


def _per_device_needs_id(config):
    if any(k in config for k in _PER_DEVICE) and CONF_FLIC_TWIST_ID not in config:
        raise cv.Invalid(
            "battery_voltage / battery_level / rssi / mode are per-device sensors and need flic_twist_id"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_FLIC_TWIST_ID): cv.use_id(FlicClientBase),
            cv.Optional(CONF_BATTERY_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=3,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_RSSI): sensor.sensor_schema(
                unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
                device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Twist only (compile error if attached to a Flic 2 button).
            cv.Optional(CONF_MODE): sensor.sensor_schema(
                accuracy_decimals=0,
                icon="mdi:dial",
            ),
            # Node level (FlicFleet).
            cv.Optional(CONF_SLOTS): sensor.sensor_schema(
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:bluetooth",
            ),
            cv.Optional(CONF_PAIRED): sensor.sensor_schema(
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:link-variant",
            ),
            cv.Optional(CONF_CONNECTED): sensor.sensor_schema(
                state_class=STATE_CLASS_MEASUREMENT,
                accuracy_decimals=0,
                icon="mdi:bluetooth-connect",
            ),
        }
    ),
    _per_device_needs_id,
)


def _final_validate(config):
    if CONF_MODE in config and device_type_of(config[CONF_FLIC_TWIST_ID]) != DEVICE_TWIST:
        raise cv.Invalid("mode is a Twist sensor (the selector mode the knob is in)")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    if CONF_FLIC_TWIST_ID in config:
        hub = await cg.get_variable(config[CONF_FLIC_TWIST_ID])
        if CONF_BATTERY_VOLTAGE in config:
            cg.add(hub.set_battery_voltage_sensor(await sensor.new_sensor(config[CONF_BATTERY_VOLTAGE])))
        if CONF_BATTERY_LEVEL in config:
            cg.add(hub.set_battery_level_sensor(await sensor.new_sensor(config[CONF_BATTERY_LEVEL])))
        if CONF_RSSI in config:
            cg.add(hub.set_rssi_sensor(await sensor.new_sensor(config[CONF_RSSI])))
        if CONF_MODE in config:
            cg.add(hub.set_mode_sensor(await sensor.new_sensor(config[CONF_MODE])))
    for key, setter in (
        (CONF_SLOTS, "set_slots_sensor"),
        (CONF_PAIRED, "set_paired_sensor"),
        (CONF_CONNECTED, "set_connected_sensor"),
    ):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(cg.RawExpression(f"flic_twist::FlicFleet::get()->{setter}({s})"))

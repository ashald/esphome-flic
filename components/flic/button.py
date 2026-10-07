# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Borys Pierov
"""Buttons for a Flic device (Twist, Flic 2 or Duo). `type:` selects which:

- pair (default): arms a single on-device full-verify pairing attempt (~60 s window), bonding
  this device to this ESP (fresh creds in flash). Press it, then put the device in pairing mode:
  hold its button (the Duo's big button) for ~7 s until it flashes.
- dump_creds: one-shot — logs this device's pairing_id + pairing_key to the ESP log so they can
  be copied into a replacement board's config (secrets.yaml) to migrate WITHOUT re-pairing.
  Diagnostic; keep it disabled_by_default — it prints a bond secret to the log on demand.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_FLIC_ID, FlicClientBase, flic_ns

FlicPairButton = flic_ns.class_(
    "FlicPairButton", button.Button, cg.Parented.template(FlicClientBase)
)
FlicDumpCredsButton = flic_ns.class_(
    "FlicDumpCredsButton", button.Button, cg.Parented.template(FlicClientBase)
)
FlicUnpairButton = flic_ns.class_(
    "FlicUnpairButton", button.Button, cg.Parented.template(FlicClientBase)
)

CONF_TYPE = "type"

_FLIC = {cv.GenerateID(CONF_FLIC_ID): cv.use_id(FlicClientBase)}

CONFIG_SCHEMA = cv.typed_schema(
    {
        "pair": button.button_schema(FlicPairButton).extend(_FLIC),
        "dump_creds": button.button_schema(
            FlicDumpCredsButton, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(_FLIC),
        "unpair": button.button_schema(
            FlicUnpairButton, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(_FLIC),
    },
    key=CONF_TYPE,
    default_type="pair",
)


async def to_code(config):
    b = await button.new_button(config)
    await cg.register_parented(b, config[CONF_FLIC_ID])

import esphome.codegen as cg
from esphome.components import button, cover, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_CLOSE_DURATION,
    CONF_ID,
    CONF_OPEN_DURATION,
    CONF_TYPE,
    PLATFORM_ESP32,
)

somfy_ns = cg.esphome_ns.namespace("somfy")

CODEOWNERS = ["@LeonardPitzu"]
DEPENDENCIES = ["esp32"]

SomfyCover = somfy_ns.class_("SomfyCover", cover.Cover, cg.Component)
SomfyIohcCover = somfy_ns.class_("SomfyIohcCover", cover.Cover, cg.Component)
SomfyRtsHub = somfy_ns.class_("SomfyRtsHub", cg.Component)
SomfyIohcHub = somfy_ns.class_("SomfyIohcHub", cg.Component)
IohcMode = somfy_ns.enum("IohcMode")

# Shared config keys
CONF_SOMFY_ID = "somfy_id"
CONF_REMOTE_CODE = "remote_code"
CONF_SOMFY_STORAGE_KEY = "storage_key"
CONF_SOMFY_STORAGE_NAMESPACE = "storage_namespace"
CONF_REPEAT_COMMAND_COUNT = "repeat_command_count"
CONF_PROG_BUTTON = "prog_button"

# RTS-specific
CONF_ALLOWED_REMOTES = "allowed_remotes"
CONF_DETECTED_REMOTE = "detected_remote"
CONF_REMOTE_RECEIVER = "remote_receiver"

# iohc-specific
CONF_ENCRYPTION_KEY = "encryption_key"
CONF_IOHC_MODE = "mode"
CONF_TARGET_NODE = "target_node"

TYPE_RTS = "rts"
TYPE_IOHC = "iohc"

# iohc mode constants
IOHC_MODE_1W = "1w"
IOHC_MODE_2W = "2w"


def validate_iohc_config(config):
    """Validate iohc-specific config: target_node required for 2W mode."""
    mode = config.get(CONF_IOHC_MODE, IOHC_MODE_1W)
    if mode == IOHC_MODE_2W:
        if CONF_TARGET_NODE not in config:
            raise cv.Invalid(
                f"'{CONF_TARGET_NODE}' is required when mode is '{IOHC_MODE_2W}'"
            )
        if CONF_ENCRYPTION_KEY not in config:
            raise cv.Invalid(
                f"'{CONF_ENCRYPTION_KEY}' (system key) is required for 2W mode"
            )
    return config


def uses_rx(config):
    """Return True if any RX-related field is configured on a cover.

    Note: remote_receiver is set on the hub, not the cover.
    This helper checks the cover-level fields (allowed_remotes, detected_remote).
    """
    if config.get(CONF_DETECTED_REMOTE):
        return True
    if config.get(CONF_ALLOWED_REMOTES):
        return True
    return False


def validate_rts_config(config):
    """Validate RTS cover config: allowed_remotes/detected_remote require remote_receiver.

    Note: This cannot be wired into the cover schema because remote_receiver
    lives on the hub config, not the cover config. The actual enforcement is
    done at compile time via the USE_SOMFY_COVER_RX define.
    """
    has_receiver = CONF_REMOTE_RECEIVER in config
    has_allowed = bool(config.get(CONF_ALLOWED_REMOTES))
    has_detected = CONF_DETECTED_REMOTE in config

    if (has_allowed or has_detected) and not has_receiver:
        raise cv.Invalid(
            f"'{CONF_REMOTE_RECEIVER}' is required when "
            f"'{CONF_ALLOWED_REMOTES}' or '{CONF_DETECTED_REMOTE}' is set"
        )
    return config


# Common fields shared by both RTS and iohc covers
COMMON_COVER_FIELDS = {
    cv.Required(CONF_PROG_BUTTON): cv.use_id(button.Button),
    cv.Required(CONF_OPEN_DURATION): cv.positive_time_period_milliseconds,
    cv.Required(CONF_CLOSE_DURATION): cv.positive_time_period_milliseconds,
    cv.Required(CONF_REMOTE_CODE): cv.hex_uint32_t,
    cv.Required(CONF_SOMFY_STORAGE_KEY): cv.All(cv.string, cv.Length(max=15)),
    cv.Optional(CONF_SOMFY_STORAGE_NAMESPACE, default="somfy"): cv.All(
        cv.string, cv.Length(max=15)
    ),
    cv.Optional(CONF_REPEAT_COMMAND_COUNT, default=4): cv.int_range(min=1, max=100),
}

RTS_COVER_SCHEMA = (
    cover.cover_schema(SomfyCover)
    .extend(
        {
            cv.Required(CONF_SOMFY_ID): cv.use_id(SomfyRtsHub),
            cv.Optional(CONF_ALLOWED_REMOTES, default=[]): cv.ensure_list(
                cv.hex_uint32_t
            ),
            cv.Optional(CONF_DETECTED_REMOTE): cv.use_id(text_sensor.TextSensor),
        }
    )
    .extend(COMMON_COVER_FIELDS)
    .extend(cv.COMPONENT_SCHEMA)
)

IOHC_COVER_SCHEMA = cv.All(
    cover.cover_schema(SomfyIohcCover)
    .extend(
        {
            cv.Required(CONF_SOMFY_ID): cv.use_id(SomfyIohcHub),
            cv.Optional(CONF_ENCRYPTION_KEY): cv.All(
                cv.string, cv.Length(min=32, max=32)
            ),
            cv.Optional(CONF_IOHC_MODE, default=IOHC_MODE_1W): cv.one_of(
                IOHC_MODE_1W, IOHC_MODE_2W, lower=True
            ),
            cv.Optional(CONF_TARGET_NODE): cv.hex_uint32_t,
        }
    )
    .extend(COMMON_COVER_FIELDS)
    .extend(cv.COMPONENT_SCHEMA),
    validate_iohc_config,
)

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            TYPE_RTS: RTS_COVER_SCHEMA,
            TYPE_IOHC: IOHC_COVER_SCHEMA,
        },
    ),
    cv.only_on([PLATFORM_ESP32]),
)


async def to_code(config):
    # SomfyCover extends TimeBasedCover in C++.  Since ESPHome 2026.4 the C++
    # sources live under the time_based/cover/ sub-platform and are only copied
    # to the build tree when an explicit "cover: platform: time_based" YAML entry
    # exists.  AUTO_LOAD cannot be used here because it would trigger schema
    # validation (which fails — the platform has required keys we don't supply).
    # Instead we inject a minimal placeholder so copy_src_tree() picks up the
    # C++ sources.
    from esphome.const import CONF_PLATFORM
    from esphome.core import CORE

    covers = CORE.config.get("cover", [])
    if not any(c.get(CONF_PLATFORM) == "time_based" for c in covers):
        covers.append({CONF_PLATFORM: "time_based"})

    typ = config[CONF_TYPE]

    if typ == TYPE_RTS:
        await _to_code_rts(config)
    elif typ == TYPE_IOHC:
        await _to_code_iohc(config)


async def _to_code_rts(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cover.register_cover(var, config)

    hub = await cg.get_variable(config[CONF_SOMFY_ID])
    cg.add(var.set_hub(hub))

    btn = await cg.get_variable(config[CONF_PROG_BUTTON])
    cg.add(var.set_prog_button(btn))

    cg.add(var.set_open_duration(config[CONF_OPEN_DURATION]))
    cg.add(var.set_close_duration(config[CONF_CLOSE_DURATION]))
    cg.add(var.set_remote_code(config[CONF_REMOTE_CODE]))
    cg.add(var.set_storage_key(config[CONF_SOMFY_STORAGE_KEY]))
    cg.add(var.set_storage_namespace(config[CONF_SOMFY_STORAGE_NAMESPACE]))
    cg.add(var.set_repeat_count(config[CONF_REPEAT_COMMAND_COUNT]))

    if CONF_ALLOWED_REMOTES in config:
        for code in config[CONF_ALLOWED_REMOTES]:
            cg.add(var.add_receive_remote_code(code))

    if CONF_DETECTED_REMOTE in config:
        log_sensor = await cg.get_variable(config[CONF_DETECTED_REMOTE])
        cg.add(var.set_log_text_sensor(log_sensor))


async def _to_code_iohc(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cover.register_cover(var, config)

    hub = await cg.get_variable(config[CONF_SOMFY_ID])
    cg.add(var.set_hub(hub))

    btn = await cg.get_variable(config[CONF_PROG_BUTTON])
    cg.add(var.set_prog_button(btn))

    cg.add(var.set_open_duration(config[CONF_OPEN_DURATION]))
    cg.add(var.set_close_duration(config[CONF_CLOSE_DURATION]))
    cg.add(var.set_remote_code(config[CONF_REMOTE_CODE]))
    cg.add(var.set_storage_key(config[CONF_SOMFY_STORAGE_KEY]))
    cg.add(var.set_storage_namespace(config[CONF_SOMFY_STORAGE_NAMESPACE]))
    cg.add(var.set_repeat_count(config[CONF_REPEAT_COMMAND_COUNT]))

    if CONF_ENCRYPTION_KEY in config:
        cg.add(var.set_encryption_key(config[CONF_ENCRYPTION_KEY]))

    mode = config[CONF_IOHC_MODE]
    if mode == IOHC_MODE_2W:
        cg.add(var.set_mode(IohcMode.MODE_2W))
        if CONF_TARGET_NODE in config:
            cg.add(var.set_target_node(config[CONF_TARGET_NODE]))
    else:
        cg.add(var.set_mode(IohcMode.MODE_1W))

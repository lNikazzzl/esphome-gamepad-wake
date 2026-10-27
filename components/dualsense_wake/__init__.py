import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import binary_sensor, event, number, switch, text, text_sensor
from esphome.components.esp32 import (
    add_idf_sdkconfig_option,
    request_bluetooth,
    request_software_coexistence,
)
from esphome.const import CONF_ID, CONF_TRIGGER_ID

DEPENDENCIES = ["esp32", "wifi"]

CONF_HOST_MAC_TEXT = "host_mac_text"
CONF_PC_IP_TEXT = "pc_ip_text"
CONF_ENABLED_SWITCH = "enabled_switch"
CONF_ANY_DEVICE_SWITCH = "any_device_switch"
CONF_PAUSE_NUMBER = "pause_number"
CONF_GUARD_NUMBER = "guard_number"
CONF_PRESS_EVENT = "press_event"
CONF_LISTENING_SENSOR = "listening_sensor"
CONF_PC_ONLINE_SENSOR = "pc_online_sensor"
CONF_LAST_CALLER_SENSOR = "last_caller_sensor"
CONF_CHIP_ADDRESS_SENSOR = "chip_address_sensor"
CONF_ON_PRESS = "on_press"

dualsense_wake_ns = cg.esphome_ns.namespace("dualsense_wake")
DualsenseWake = dualsense_wake_ns.class_("DualsenseWake", cg.Component)
PressTrigger = dualsense_wake_ns.class_(
    "PressTrigger", automation.Trigger.template(cg.std_string)
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(DualsenseWake),
            cv.Required(CONF_HOST_MAC_TEXT): cv.use_id(text.Text),
            cv.Required(CONF_PC_IP_TEXT): cv.use_id(text.Text),
            cv.Required(CONF_ENABLED_SWITCH): cv.use_id(switch.Switch),
            cv.Required(CONF_ANY_DEVICE_SWITCH): cv.use_id(switch.Switch),
            cv.Required(CONF_PAUSE_NUMBER): cv.use_id(number.Number),
            cv.Required(CONF_GUARD_NUMBER): cv.use_id(number.Number),
            cv.Required(CONF_PRESS_EVENT): cv.use_id(event.Event),
            cv.Required(CONF_LISTENING_SENSOR): cv.use_id(binary_sensor.BinarySensor),
            cv.Required(CONF_PC_ONLINE_SENSOR): cv.use_id(binary_sensor.BinarySensor),
            cv.Required(CONF_LAST_CALLER_SENSOR): cv.use_id(text_sensor.TextSensor),
            cv.Required(CONF_CHIP_ADDRESS_SENSOR): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_ON_PRESS): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PressTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_with_framework("esp-idf"),
)

_ENTITY_SETTERS = {
    CONF_HOST_MAC_TEXT: "set_host_mac_text",
    CONF_PC_IP_TEXT: "set_pc_ip_text",
    CONF_ENABLED_SWITCH: "set_enabled_switch",
    CONF_ANY_DEVICE_SWITCH: "set_any_device_switch",
    CONF_PAUSE_NUMBER: "set_pause_number",
    CONF_GUARD_NUMBER: "set_guard_number",
    CONF_PRESS_EVENT: "set_press_event",
    CONF_LISTENING_SENSOR: "set_listening_sensor",
    CONF_PC_ONLINE_SENSOR: "set_pc_online_sensor",
    CONF_LAST_CALLER_SENSOR: "set_last_caller_sensor",
    CONF_CHIP_ADDRESS_SENSOR: "set_chip_address_sensor",
}


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for key, setter in _ENTITY_SETTERS.items():
        entity = await cg.get_variable(config[key])
        cg.add(getattr(var, setter)(entity))

    for conf in config.get(CONF_ON_PRESS, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "address")], conf)

    # Bare Classic BT controller driven over VHCI, no host stack
    request_bluetooth()
    request_software_coexistence()
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_CONTROLLER_ONLY", True)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", True)

# X570D4I-2T: publish two constant fan_tach virtual sensors.
#
# phosphor-pid-control's "fan" PID class refuses to leave failsafe (PWM=100%)
# unless every tach Input publishes a valid reading, and it ignores
# MissingIsAcceptable / InputUnavailableAsFailed for that class. This board's
# fan headers don't wire tach back to the AST2500, so we feed swampd two
# constant 1500 RPM readings via phosphor-virtual-sensor (constant exprtk
# expression, no D-Bus inputs). These publish at
# /xyz/openbmc_project/sensors/fan_tach/{FanTach_CPU,FanTach_Chassis}, which is
# exactly where the Pid "Inputs" resolve. This replaces the fake-tach loop that
# used to live in nct6779-bridge.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " file://config-virtual-sensor.json"

do_install:append() {
    install -d ${D}${datadir}/${PN}
    install -m 0644 ${UNPACKDIR}/config-virtual-sensor.json \
        ${D}${datadir}/${PN}/virtual_sensor_config.json
}

FILES:${PN}:append = " ${datadir}/${PN}/virtual_sensor_config.json"

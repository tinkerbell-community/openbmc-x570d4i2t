#!/bin/sh
# NCT6779 -> ExternalSensor bridge for X570D4I-2T
#
# The X570D4I-2T's SuperIO/HW-monitor is a Nuvoton NCT6779-family chip
# accessible from the BMC at i2c-1 0x2d. The upstream nct6775-i2c driver
# binds it read-only into /sys/class/hwmon, but entity-manager silently
# rejects "Type": "NCT6779" stanzas on this stack, so the standard
# hwmontempsensor flow can't surface the readings into D-Bus.
#
# Instead, this daemon:
#   1. Ensures the nct6775-i2c device exists + driver is bound on i2c-1 0x2d
#      (re-binds after host power transitions, since the chip is powered
#      from a host rail and disappears in S5).
#   2. Polls the resulting /sys/class/hwmon/hwmonN sensors.
#   3. Publishes them into the corresponding ExternalSensor objects that
#      entity-manager creates from x570d4i2t.json.

set -u

I2C_BUS=1
I2C_ADDR=0x2d
DEV_DIR="/sys/bus/i2c/devices/1-002d"
DRV_DIR="/sys/bus/i2c/drivers/nct6775-i2c"
INTERVAL=5

# Sensor map: hwmon input file → ExternalSensor object path
#
# Indices follow nct6775 driver labels for NCT6779D-family chips:
#   temp1 = SYSTIN     | temp9  = PCH_CPU_TEMP
#   temp2 = CPUTIN     | temp10 = PCH_MCH_TEMP
#   temp3 = AUXTIN0    | fan1, fan2 = the chip's two routed tach inputs (unwired)
#
# X570 Temp itself comes from the W83773G remote-1 diode (reads ~70 C), not
# from NCT6779 temp8 PCH_CHIP_TEMP (that channel reads 0 without LPC init).
TEMP_PATHS="\
temp1_input:SYSTIN \
temp2_input:CPUTIN \
temp3_input:AUXTIN \
temp9_input:PCH_CPU_Temp \
temp10_input:PCH_MCH_Temp"

FAN_PATHS="\
fan1_input:SuperIO_Fan_1 \
fan2_input:SuperIO_Fan_2"

find_hwmon() {
    for h in /sys/class/hwmon/hwmon*; do
        if [ -r "$h/name" ] && [ "$(cat "$h/name")" = "nct6779" ]; then
            echo "$h"
            return 0
        fi
    done
    return 1
}

ensure_device() {
    # If the i2c client exists, try to bind the driver to it (handles the
    # waiting_for_supplier case after a host power cycle).
    if [ -e "$DEV_DIR" ]; then
        if [ ! -L "$DEV_DIR/driver" ]; then
            echo "1-002d" > "$DRV_DIR/bind" 2>/dev/null || true
        fi
        return 0
    fi
    # Device hasn't been instantiated yet (e.g. host just powered up).
    # Ask the i2c subsystem to create it.
    echo "nct6779 $I2C_ADDR" > "/sys/bus/i2c/devices/i2c-$I2C_BUS/new_device" 2>/dev/null || true
    sleep 1
}

set_value() {
    obj="$1"
    val="$2"
    busctl set-property xyz.openbmc_project.ExternalSensor \
        "$obj" xyz.openbmc_project.Sensor.Value Value d "$val" 2>/dev/null || true
}

publish() {
    HW="$1"
    for entry in $TEMP_PATHS; do
        sysfs="${entry%%:*}"
        name="${entry##*:}"
        raw=$(cat "$HW/$sysfs" 2>/dev/null) || continue
        [ -z "$raw" ] && continue
        # NCT6779 temp3 (and possibly other unwired channels) saturates to
        # 127000 / -127000 when the diode is open. Skip those so the
        # ExternalSensor doesn't latch a bogus reading.
        if [ "$raw" -ge 120000 ] || [ "$raw" -le -120000 ]; then
            continue
        fi
        val=$(awk -v r="$raw" 'BEGIN{printf "%.3f", r/1000}')
        set_value "/xyz/openbmc_project/sensors/temperature/$name" "$val"
    done
    for entry in $FAN_PATHS; do
        sysfs="${entry%%:*}"
        name="${entry##*:}"
        raw=$(cat "$HW/$sysfs" 2>/dev/null) || continue
        [ -z "$raw" ] && continue
        # 0 RPM is meaningful (fan stopped); negative / huge values are not.
        if [ "$raw" -lt 0 ] || [ "$raw" -gt 100000 ]; then
            continue
        fi
        set_value "/xyz/openbmc_project/sensors/fan_tach/$name" "$raw.0"
    done
}

publish_fake_tachs() {
    # phosphor-pid-control's "fan" PID class hard-requires every Input tach
    # to publish a reading; missing/NaN forces the zone into failsafe (max
    # PWM) regardless of MissingIsAcceptable / InputUnavailableAsFailed.
    # The X570D4I-2T's fan headers don't wire tach back to AST2500 tach
    # pins, so we feed swampd constant fake RPMs via ExternalSensor objects
    # to keep both zones out of failsafe while the temperature curves run.
    set_value /xyz/openbmc_project/sensors/fan_tach/FanTach_CPU "1500.0"
    set_value /xyz/openbmc_project/sensors/fan_tach/FanTach_Chassis "1500.0"
}

while true; do
    ensure_device
    HW=$(find_hwmon || true)
    if [ -n "$HW" ]; then
        publish "$HW"
    fi
    publish_fake_tachs
    sleep "$INTERVAL"
done

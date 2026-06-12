#!/bin/sh
# Create (or remove) the Redfish Host Interface USB CDC-ECM gadget on the
# X570D4I-2T, directly via configfs.
#
# We deliberately do NOT use phosphor-misc's usb-ctrl: its serial-number
# generation runs `cut -b 0-12`, and busybox cut rejects a 0-based range
# ("cut: invalid range 0-12"), which under `set -e` aborts the whole script
# before the gadget is bound. This script does the same configfs dance with a
# busybox-safe `cut -c1-12`.
#
# The kernel u_ether driver names the resulting netdev usb0; 80-host-redfish.network
# then addresses it 169.254.0.17 so bmcweb answers where the host BIOS dials.

set -e

GADGET=/sys/kernel/config/usb_gadget/hi-ecm
FUNC=ecm.usb0
ACTION="${1:-on}"

log() { logger -t x570-usb-gadget "$*" 2>/dev/null || true; }

if [ "$ACTION" = "off" ]; then
    [ -d "$GADGET" ] || exit 0
    echo "" > "$GADGET/UDC" 2>/dev/null || true
    rm -f "$GADGET/configs/c.1/$FUNC"
    rmdir "$GADGET/functions/$FUNC" 2>/dev/null || true
    rmdir "$GADGET/configs/c.1/strings/0x409" 2>/dev/null || true
    rmdir "$GADGET/configs/c.1" 2>/dev/null || true
    rmdir "$GADGET/strings/0x409" 2>/dev/null || true
    rmdir "$GADGET" 2>/dev/null || true
    log "ECM gadget removed"
    exit 0
fi

# Idempotent: already created.
if [ -d "$GADGET" ]; then
    log "ECM gadget already present"
    exit 0
fi

mkdir -p "$GADGET"
cd "$GADGET"
echo 0x1d6b > idVendor    # Linux Foundation
echo 0x0104 > idProduct   # Multifunction Composite Gadget

mkdir -p strings/0x409
serial=$(cat /etc/machine-id 2>/dev/null | sha256sum | cut -c1-12)
echo "${serial:-000000000000}" > strings/0x409/serialnumber
echo "OpenBMC" > strings/0x409/manufacturer
echo "OpenBMC Redfish Host Interface" > strings/0x409/product

mkdir -p "functions/$FUNC"

mkdir -p configs/c.1/strings/0x409
echo "ECM" > configs/c.1/strings/0x409/configuration
echo 120 > configs/c.1/MaxPower
ln -s "functions/$FUNC" configs/c.1/

# Bind to the first aspeed vhub UDC not already claimed by another gadget.
udc=
for d in /sys/class/udc/*; do
    [ -e "$d" ] || continue
    name=$(basename "$d")
    busy=0
    for u in /sys/kernel/config/usb_gadget/*/UDC; do
        [ -e "$u" ] || continue
        if [ "$(cat "$u" 2>/dev/null)" = "$name" ]; then
            busy=1
            break
        fi
    done
    if [ "$busy" = "0" ]; then
        udc="$name"
        break
    fi
done

if [ -z "$udc" ]; then
    log "ERROR: no free UDC to bind the ECM gadget"
    exit 1
fi

echo "$udc" > UDC
log "ECM gadget bound to UDC $udc (netdev usb0)"

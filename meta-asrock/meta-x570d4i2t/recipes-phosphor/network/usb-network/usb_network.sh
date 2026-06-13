#!/bin/bash

set -e

GADGET_DIR="/sys/kernel/config/usb_gadget/obmc_redfish"

BMC_MAC="02:00:16:92:54:17"
HOST_MAC="02:00:16:92:54:18"

# 1. Create the Gadget
mkdir -p "$GADGET_DIR"
cd "$GADGET_DIR"

echo 0x1d6b > idVendor  # Linux Foundation
echo 0x0104 > idProduct # Multifunction Composite Gadget
echo 0x0200 > bcdUSB    # USB 2.0
echo 0x0100 > bcdDevice # v1.0.0

# 3. Set standard string descriptors
mkdir -p strings/0x409
echo "ASRock Rack" > strings/0x409/manufacturer
echo "X570D4I-2T BMC" > strings/0x409/product
echo "0000000000" > strings/0x409/serialnumber

# 4. Create the Configuration
mkdir -p configs/c.1/strings/0x409
echo "Redfish Host Interface" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower # 500mA max draw

# 5. Create the actual NCM Function (This is your "NCM Node")
mkdir -p functions/ncm.usb0

# (Optional) Set MAC addresses. If omitted, random ones are generated.
# Host MAC is what the OS sees; dev_addr is what the BMC sees.
echo "$HOST_MAC" > functions/ncm.usb0/host_addr
echo "$BMC_MAC" > functions/ncm.usb0/dev_addr

# 6. Link the NCM function to the configuration
ln -s functions/ncm.usb0 configs/c.1/

# 7. Bind the Gadget to the ASPEED Virtual Hub Hardware
# The AST2500 vhub exposes its virtual ports as UDCs (USB Device Controllers).
# "1e6a0000.usb-vhub:p1" is Port 1 of the AST2500 vhub.
echo "1e6a0000.usb-vhub:p1" > UDC

# 8. Assign the BMC-side IP directly with iproute2.
# Binding the UDC creates the usb0 netdev asynchronously, so wait for it to
# appear before configuring. `ip addr replace` is idempotent (safe under set -e
# on a service restart, unlike `ip addr add` which fails if the addr exists).
for _ in $(seq 1 50); do
    [ -e /sys/class/net/usb0 ] && break
    sleep 0.1
done

ip link set usb0 up
ip addr replace 169.254.0.17/16 dev usb0

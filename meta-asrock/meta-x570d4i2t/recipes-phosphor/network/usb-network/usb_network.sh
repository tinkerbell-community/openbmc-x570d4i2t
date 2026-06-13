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

# Microsoft "RNDIS over USB" device-class triple (0xEF Miscellaneous / 0x04 RNDIS
# / 0x01). The AMI BIOS Redfish Host Interface driver — and Windows — bind the
# in-box RNDIS driver off this class + the MS OS descriptors set below. RNDIS is
# the windows-compatible function; NCM/ECM are not bound by the BIOS.
echo 0xEF > bDeviceClass
echo 0x04 > bDeviceSubClass
echo 0x01 > bDeviceProtocol

# 3. Set standard string descriptors
mkdir -p strings/0x409
echo "ASRock Rack" > strings/0x409/manufacturer
echo "X570D4I-2T BMC" > strings/0x409/product
echo "0000000000" > strings/0x409/serialnumber

# 4. Create the Configuration
mkdir -p configs/c.1/strings/0x409
echo "Redfish Host Interface" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower # 500mA max draw

# 5. Create the RNDIS Function (Windows-compatible host interface).
mkdir -p functions/rndis.usb0

# (Optional) Set MAC addresses. If omitted, random ones are generated.
# Host MAC is what the OS sees; dev_addr is what the BMC sees.
echo "$HOST_MAC" > functions/rndis.usb0/host_addr
echo "$BMC_MAC" > functions/rndis.usb0/dev_addr

# 5a. Microsoft OS descriptors so a Windows / AMI-BIOS host auto-loads the RNDIS
# driver with no .inf. sub_compatible_id 5162001 selects the RNDIS 6.0 driver.
echo 1       > os_desc/use
echo 0xcd    > os_desc/b_vendor_code
echo MSFT100 > os_desc/qw_sign
echo RNDIS   > functions/rndis.usb0/os_desc/interface.rndis/compatible_id
echo 5162001 > functions/rndis.usb0/os_desc/interface.rndis/sub_compatible_id

# 6. Link the RNDIS function to the configuration, and link the config into
# os_desc (REQUIRED — without it the host never requests the MS OS descriptor).
ln -s functions/rndis.usb0 configs/c.1/
ln -s configs/c.1 os_desc

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

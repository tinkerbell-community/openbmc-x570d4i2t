#!/bin/bash
#
# Bring up usb0 as a Microsoft Remote-NDIS (RNDIS) LAN-over-USB gadget on the
# aspeed vhub, matching the stock AMI firmware: it presents the Redfish Host
# Interface as RNDIS (CONFIG_SPX_rndisinf + the usbe "Ethernet over USB"
# function) so a Windows host binds it with the in-box RNDIS driver, and Linux
# binds it via rndis_host. Requires CONFIG_USB_CONFIGFS_RNDIS in the kernel
# (see recipes-kernel/linux/linux-aspeed/x570d4i2t.cfg).

GADGET=/sys/kernel/config/usb_gadget/usbnet
FUNC="$GADGET/functions/rndis.usbnet"

# Get MAC Address from uboot, or fallback to a fixed locally-administered MAC
ENV_MAC_ADDR=
ENV_MAC_ADDR=${ENV_MAC_ADDR:-"9C:6B:00:4E:1C:2A"}
ENV_MAC_ADDR=${ENV_MAC_ADDR:-"9C:6B:00:70:57:A4"}
MAC_ADDR=${ENV_MAC_ADDR:-"9C:6B:00:70:57:A4"}

# Generate MAC Address using locally administered MAC
# https://en.wikipedia.org/wiki/MAC_address#Universal_vs._local_(U/L_bit
SUBMAC=$(echo "$MAC_ADDR" | cut -d ":" -f 2-5)

# (Re)create the gadget with the RNDIS function. usb-ctrl supports rndis natively
# (functions/rndis.<name>); the off is tolerated when no gadget exists yet.
/usr/bin/usb-ctrl rndis usbnet off 2>/dev/null || true
/usr/bin/usb-ctrl rndis usbnet on "9C:6B:00:4E:1C:2A" "9C:6B:00:70:57:A4"

# usb-ctrl leaves the device descriptor generic (class 0x00, idProduct 0x0104 =
# "Multifunction Composite Gadget") and binds the UDC. Re-stamp it as a dedicated
# RNDIS device so hosts pick the RNDIS driver: detach the UDC, set the Microsoft
# "RNDIS over USB" device-class triple (0xEF Miscellaneous / 0x04 RNDIS / 0x01)
# plus MS OS descriptors (compatible ID "RNDIS", sub "5162001" for RNDIS 6.0 on
# Vista+), then re-attach. The MS OS descriptors let Windows auto-load the driver
# with no .inf; the class triple covers older hosts. Each write is guarded so a
# kernel without a given attribute does not abort the bring-up.
if [ -d "$GADGET" ]; then
	UDC=$(cat "$GADGET/UDC" 2>/dev/null)
	echo '' > "$GADGET/UDC" 2>/dev/null || true

	echo 0xEF > "$GADGET/bDeviceClass" 2>/dev/null || true
	echo 0x04 > "$GADGET/bDeviceSubClass" 2>/dev/null || true
	echo 0x01 > "$GADGET/bDeviceProtocol" 2>/dev/null || true
	echo "OpenBMC RNDIS Host Interface" > "$GADGET/strings/0x409/product" 2>/dev/null || true

	# Microsoft OS descriptors (Windows auto-binds RNDIS without an .inf)
	echo 1 > "$GADGET/os_desc/use" 2>/dev/null || true
	echo 0xcd > "$GADGET/os_desc/b_vendor_code" 2>/dev/null || true
	echo MSFT100 > "$GADGET/os_desc/qw_sign" 2>/dev/null || true
	if [ -d "$FUNC/os_desc/interface.rndis" ]; then
		echo RNDIS > "$FUNC/os_desc/interface.rndis/compatible_id" 2>/dev/null || true
		echo 5162001 > "$FUNC/os_desc/interface.rndis/sub_compatible_id" 2>/dev/null || true
	fi
	# The os_desc directory needs a symlink to the active config so the host can
	# fetch the descriptor; idempotent across restarts.
	[ -e "$GADGET/os_desc/c.1" ] || ln -s "$GADGET/configs/c.1" "$GADGET/os_desc" 2>/dev/null || true

	echo "$UDC" > "$GADGET/UDC" 2>/dev/null || true
fi

# Force the BMC's usb0 address regardless of carrier. systemd-networkd withholds
# the static address on the aspeed vhub gadget while it has no carrier
# (ConfigureWithoutCarrier is unreliable on this UDC), so assign it directly here
# so the Redfish Host Interface endpoint is always present. Must match the
# Address in 00-bmc-usb0.network so networkd does not fight it.
USB_ADDR="169.254.0.17/16"
for _ in $(seq 1 20); do
	[ -e /sys/class/net/usb0 ] && break
	sleep 0.25
done
ip link set usb0 up 2>/dev/null
ip address replace "$USB_ADDR" dev usb0 2>/dev/null

# NOTE: the upstream Ampere script does `exit 1` here when the MAC came from the
# fallback (no uboot ethaddr) to retry until u-boot env is readable. On this
# board fw_printenv has no readable env, so that would fail-loop forever and
# tear the gadget (and usb0 address) down on every restart. The fallback MAC is
# stable and fine, so succeed instead and leave the gadget + forced address up.
exit 0

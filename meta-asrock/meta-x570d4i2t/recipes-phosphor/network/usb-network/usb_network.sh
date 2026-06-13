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

# Fixed locally-administered MACs for the usb0 gadget. This board has no readable
# u-boot ethaddr, so use stable literals rather than deriving from fw_printenv.
# BMC side (dev_addr) and host side (host_addr); both have the locally-administered
# bit set (0x02 in the first octet: 0x9C & 0x02).
BMC_MAC="9C:6B:00:4E:1C:2A"
HOST_MAC="9C:6B:00:70:57:A4"

# (Re)create the gadget with the RNDIS function. usb-ctrl supports rndis natively
# (functions/rndis.<name>); the off is tolerated when no gadget exists yet.
/usr/bin/usb-ctrl rndis usbnet off 2>/dev/null || true
/usr/bin/usb-ctrl rndis usbnet on "9C:6B:00:70:57:A4"

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

# usb0's address is now owned by systemd-networkd (00-bmc-usb0.network: static
# 169.254.0.17/16, Scope=global, DHCP=no, ConfigureWithoutCarrier=yes — verified
# to apply + persist on the current kernel/RNDIS). Wait for the gadget netdev,
# bring the link up, and nudge networkd to apply the config now.
#
# Do NOT set the address here with `ip address replace`: it defaults a 169.254/16
# address to "scope link", which overrides networkd's Scope=global and makes IPMI
# Get LAN Config (channel 8, the host-interface query) drop the address and return
# 0.0.0.0 — phosphor-ipmi-host only reports global-scope, origin-matching IPs.
for _ in $(seq 1 20); do
	[ -e /sys/class/net/usb0 ] && break
	sleep 0.25
done
ip link set usb0 up 2>/dev/null
networkctl reconfigure usb0 2>/dev/null || true

# Belt-and-suspenders: force phosphor-network to treat usb0 as static (no DHCP).
# IPMI Get LAN Config returns an address only when its Origin matches the channel's
# DHCP state; with DHCP enabled it looks for a DHCP-origin address and ignores our
# Static one. The .network DHCP=no should already yield this on boot; re-assert it
# in case phosphor-network came up with its "both" default before usb0 existed.
busctl set-property xyz.openbmc_project.Network \
	/xyz/openbmc_project/network/usb0 \
	xyz.openbmc_project.Network.EthernetInterface DHCPEnabled s \
	xyz.openbmc_project.Network.EthernetInterface.DHCPConf.none 2>/dev/null || true

# NOTE: the upstream Ampere script does `exit 1` here when the MAC came from the
# fallback (no uboot ethaddr) to retry until u-boot env is readable. On this
# board fw_printenv has no readable env, so that would fail-loop forever and
# tear the gadget (and usb0 address) down on every restart. The fallback MAC is
# stable and fine, so succeed instead and leave the gadget + forced address up.
exit 0

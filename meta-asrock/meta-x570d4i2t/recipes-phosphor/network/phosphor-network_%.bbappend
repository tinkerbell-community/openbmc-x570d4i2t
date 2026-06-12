# X570D4I-2T host-interface networking.
#
# usb0 is the in-band Redfish Host Interface — the host BIOS dials
# https://169.254.0.17/redfish/... over it.  Two adjustments are needed so that
# address lives only on usb0:
#
#  1. Tell phosphor-network to IGNORE usb0 (IGNORED_INTERFACES env), otherwise
#     it auto-generates /etc/systemd/network/00-bmc-usb0.network (DHCP +
#     link-local) which shadows the static 80-host-redfish.network shipped by
#     x570d4i2t-host-interface.
#
#  2. Disable default IPv4 link-local autoconf.  With it on, every managed NIC
#     (eth0/eth1) also grabs a 169.254.x.x/16 address; eth1 in particular ends
#     up at 169.254.x.x/16, colliding with usb0's 169.254.0.17/16 so the BMC
#     can route host-interface replies out the wrong interface.  eth0/eth1 use
#     DHCP, so they lose nothing.
PACKAGECONFIG:remove = "default-link-local-autoconf"

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://10-ignore-usb0.conf"

NETWORK_DROPIN_DIR = "${systemd_system_unitdir}/xyz.openbmc_project.Network.service.d"

do_install:append() {
    install -d ${D}${NETWORK_DROPIN_DIR}
    install -m 0644 ${UNPACKDIR}/10-ignore-usb0.conf \
        ${D}${NETWORK_DROPIN_DIR}/10-ignore-usb0.conf
}

FILES:${PN} += "${NETWORK_DROPIN_DIR}/10-ignore-usb0.conf"

SUMMARY = "In-band USB ethernet (usb0) for the Redfish Host Interface"
DESCRIPTION = "Brings up a CDC-ECM USB gadget (usb0) on the AST2500 vhub UDC so \
the host BIOS/OS can reach the BMC's Redfish/IPMI services in-band. Modeled on \
meta-bytedance/recipes-bytedance/usb-network: a oneshot service drives \
phosphor-misc's usb-ctrl to create the gadget, and a systemd-networkd .network \
assigns the BMC a static link-local address on usb0."
PR = "r1"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

DEPENDS += "systemd"
# usb-ctrl (gadget creation) comes from phosphor-misc; libsystemd for the unit.
RDEPENDS:${PN} += "libsystemd phosphor-misc-usb-ctrl"

inherit allarch systemd

SRC_URI += "file://usb_network.service \
            file://00-bmc-usb0.network"

do_install() {
    install -d ${D}${systemd_unitdir}/system/
    install -m 0644 ${UNPACKDIR}/usb_network.service ${D}${systemd_unitdir}/system

    install -d ${D}${sysconfdir}/systemd/network/
    install -m 0644 ${UNPACKDIR}/00-bmc-usb0.network ${D}${sysconfdir}/systemd/network
}

NATIVE_SYSTEMD_SUPPORT = "1"
SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "usb_network.service"

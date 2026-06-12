SUMMARY = "Redfish Host Interface USB ethernet gadget + address (X570D4I-2T)"
DESCRIPTION = "The AMI host BIOS opens an HTTPS connection to \
https://169.254.0.17/redfish/... over an in-band USB host<->BMC tunnel \
(DTS &vhub) to push SMBIOS and BIOS-configuration data. This recipe (a) \
instantiates the CDC-ECM USB ethernet gadget at boot with usb-ctrl so the \
usb0 interface exists, and (b) pins the static link-local address 169.254.0.17 \
on it so bmcweb answers where the BIOS dials."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

inherit allarch systemd

SRC_URI = "file://x570d4i2t-usb-network.service \
           file://x570d4i2t-usb-gadget.sh \
           file://80-host-redfish.network"

S = "${UNPACKDIR}"

do_install() {
    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/x570d4i2t-usb-gadget.sh \
        ${D}${libexecdir}/x570d4i2t-usb-gadget.sh

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/x570d4i2t-usb-network.service \
        ${D}${systemd_system_unitdir}/x570d4i2t-usb-network.service

    install -d ${D}${systemd_unitdir}/network
    install -m 0644 ${UNPACKDIR}/80-host-redfish.network \
        ${D}${systemd_unitdir}/network/80-host-redfish.network
}

SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "x570d4i2t-usb-network.service"

FILES:${PN} += "${libexecdir}/x570d4i2t-usb-gadget.sh \
                ${systemd_system_unitdir}/x570d4i2t-usb-network.service \
                ${systemd_unitdir}/network/80-host-redfish.network"

# The gadget is created directly via configfs (busybox sh), so no usb-ctrl /
# phosphor-misc dependency is needed.

COMPATIBLE_MACHINE = "x570d4i2t"

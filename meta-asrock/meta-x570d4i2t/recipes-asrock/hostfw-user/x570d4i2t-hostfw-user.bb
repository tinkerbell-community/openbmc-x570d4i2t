SUMMARY = "Provision the HostAutoFW Redfish Host Interface user"
DESCRIPTION = "The AMI host BIOS authenticates to bmcweb over the in-band \
Redfish Host Interface as the HostAutoFW user (firmware DEFAULT_HOST_FW). \
This oneshot creates that account (priv-admin, redfish group) at boot so the \
BIOS's SMBIOS / BIOS-config push is accepted."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://x570d4i2t-hostfw-user.sh \
           file://x570d4i2t-hostfw-user.service"

S = "${UNPACKDIR}"

inherit systemd

SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "x570d4i2t-hostfw-user.service"

do_install() {
    install -d ${D}${libexecdir}
    install -m 0755 ${UNPACKDIR}/x570d4i2t-hostfw-user.sh \
        ${D}${libexecdir}/x570d4i2t-hostfw-user.sh

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/x570d4i2t-hostfw-user.service \
        ${D}${systemd_system_unitdir}/x570d4i2t-hostfw-user.service
}

FILES:${PN} += "${libexecdir}/x570d4i2t-hostfw-user.sh \
                ${systemd_system_unitdir}/x570d4i2t-hostfw-user.service"

# busctl (systemd) + chpasswd/logger (busybox) are always in the image.
RDEPENDS:${PN} += "phosphor-user-manager"

COMPATIBLE_MACHINE = "x570d4i2t"

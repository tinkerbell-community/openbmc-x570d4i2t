SUMMARY = "OpenBMC for ASRock - Applications"
PR = "r1"

inherit packagegroup

PROVIDES = "${PACKAGES}"
PACKAGES = " \
        ${PN}-fans \
        ${PN}-flash \
        ${PN}-system \
        "

PROVIDES += "virtual/obmc-fan-mgmt"
PROVIDES += "virtual/obmc-flash-mgmt"
PROVIDES += "virtual/obmc-system-mgmt"

RPROVIDES:${PN}-fans += "virtual-obmc-fan-mgmt"
RPROVIDES:${PN}-flash += "virtual-obmc-flash-mgmt"
RPROVIDES:${PN}-system += "virtual-obmc-system-mgmt"

SUMMARY:${PN}-fans = "ASRock Fans"
RDEPENDS:${PN}-fans = " \
        phosphor-pid-control \
        "

SUMMARY:${PN}-flash = "ASRock Flash"
RDEPENDS:${PN}-flash = " \
        phosphor-ipmi-blobs \
        phosphor-ipmi-flash \
        "

SUMMARY:${PN}-system = "ASRock System"
RDEPENDS:${PN}-system = " \
        phosphor-host-postd \
        phosphor-post-code-manager \
        phosphor-power-regulators \
        phosphor-software-manager \
        phosphor-virtual-sensor \
        x570d4i2t-vga-enable \
        smbios-mdr \
        "

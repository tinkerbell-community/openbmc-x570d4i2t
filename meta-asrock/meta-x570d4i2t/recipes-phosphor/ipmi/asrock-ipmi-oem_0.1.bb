SUMMARY = "ASRock OEM IPMI commands for the X570D4I-2T"
DESCRIPTION = "\
Implements the ASRock / AMI OEM IPMI command set for the \
X570D4I-2T BMC running OpenBMC, using the same IPMI provider \
plugin architecture as phosphor-host-ipmid.  \
\
Provides: \
  - BIOS OOB configuration protocol (SetBIOSCap / GetBIOSCap, \
    SetPayload / GetPayload) mirroring the interface implemented by \
    intel-ipmi-oem so that host UEFI firmware built with the standard \
    phosphor OOB BIOS-config protocol works unmodified.  Received \
    BIOS setup data is stored in /var/oob/ and published on the \
    xyz.openbmc_project.BIOSConfig.Manager D-Bus interface. \
  - GetBoardInfo OEM command (board product name via inventory D-Bus). \
"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://meson.build \
    file://meson.options \
    file://include/biosconfig.hpp \
    file://include/oemcommands.hpp \
    file://src/biosconfig.cpp \
    file://src/oemcommands.cpp \
    file://src/smbiosmdrv2handler.cpp \
    "

S = "${UNPACKDIR}"

DEPENDS = " \
    boost \
    nlohmann-json \
    phosphor-dbus-interfaces \
    phosphor-ipmi-host \
    phosphor-logging \
    sdbusplus \
    "

inherit meson pkgconfig obmc-phosphor-ipmiprovider-symlink

# Library name must match the library() target in meson.build
LIBRARY_NAMES = "libzasrockoemcmds.so"

# Register the provider for the host-side IPMI daemon (phosphor-ipmi-host)
HOSTIPMI_PROVIDER_LIBRARY += "${LIBRARY_NAMES}"
NETIPMI_PROVIDER_LIBRARY  += "${LIBRARY_NAMES}"

FILES:${PN}:append = " \
    ${libdir}/ipmid-providers/lib*${SOLIBS} \
    ${libdir}/host-ipmid/lib*${SOLIBS} \
    ${libdir}/net-ipmid/lib*${SOLIBS} \
    "
FILES:${PN}-dev:append = " ${libdir}/ipmid-providers/lib*${SOLIBSDEV}"

# Create /var/oob/ at first boot via tmpfiles
do_install:append() {
    install -d ${D}${sysconfdir}/tmpfiles.d
    echo "d /var/oob 0755 root root -" > \
        ${D}${sysconfdir}/tmpfiles.d/asrock-ipmi-oem.conf
}

FILES:${PN}:append = " ${sysconfdir}/tmpfiles.d/asrock-ipmi-oem.conf"

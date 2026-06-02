SUMMARY = "AMI OEM IPMI provider for ASRock X570D4I-2T SMBIOS MDR push"
DESCRIPTION = "Intercepts AMI proprietary MDR block-transfer IPMI commands \
(NetFn 0x3A / 0x32) from the host BIOS over KCS, reassembles the SMBIOS \
table in-memory, writes it to /var/lib/smbios/smbios2 with the MDRSMBIOSHeader, \
then triggers xyz.openbmc_project.Smbios.MDR_V2 AgentSynchronizeData."

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI = " \
    file://CMakeLists.txt \
    file://ami-ipmi-oem.cpp \
"

S = "${WORKDIR}"

DEPENDS = " \
    boost \
    sdbusplus \
    phosphor-dbus-interfaces \
    phosphor-logging \
    phosphor-ipmi-host \
"

inherit cmake pkgconfig obmc-phosphor-ipmiprovider-symlink

# Library name produced by cmake; the obmc-phosphor-ipmiprovider-symlink class
# creates /usr/lib/ipmid-providers/libami-ipmi-oem.so → ../libami-ipmi-oem.so
LIBRARY_NAMES = "libami-ipmi-oem.so"
HOSTIPMI_PROVIDER_LIBRARY += "${LIBRARY_NAMES}"

FILES:${PN} += "${libdir}/ipmid-providers/*.so"

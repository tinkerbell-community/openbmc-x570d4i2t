SUMMARY = "AMI OEM IPMI provider for ASRock X570D4I-2T SMBIOS MDR"
DESCRIPTION = "Handles AMI proprietary MDR commands (NetFn 0x3A) from the host \
BIOS over KCS and ships a pre-baked SMBIOS dump (extracted from the original \
MegaRAC firmware) as the baseline /var/lib/smbios/smbios2 so smbios-mdrv2 has \
a valid Type 0/1/2/3/4/17/127 table from BMC boot. BIOS push commands are \
accepted so the BIOS doesn't error, but their partial-field data is not used \
(full reverse-engineering of the AMI MDR field-overlay protocol is not done)."

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI = " \
    file://CMakeLists.txt;subdir=${BP} \
    file://ami-ipmi-oem.cpp;subdir=${BP} \
    file://smbios.dmp \
"

S = "${UNPACKDIR}/${BP}"

DEPENDS = " \
    phosphor-logging \
    phosphor-ipmi-host \
    systemd \
"

inherit cmake pkgconfig obmc-phosphor-ipmiprovider-symlink

do_install:append() {
    install -d ${D}${datadir}/x570d4i2t
    install -m 0644 ${UNPACKDIR}/smbios.dmp ${D}${datadir}/x570d4i2t/smbios.dmp
}

# Library name produced by cmake; the obmc-phosphor-ipmiprovider-symlink class
# creates /usr/lib/ipmid-providers/libami-ipmi-oem.so → ../libami-ipmi-oem.so
FILES:${PN} += " \
    ${libdir}/ipmid-providers/lib*${SOLIBS} \
    ${libdir}/host-ipmid/lib*${SOLIBS} \
    ${datadir}/x570d4i2t/smbios.dmp \
"
FILES:${PN}-dev += " \
    ${libdir}/ipmid-providers/lib*${SOLIBSDEV} \
    ${libdir}/ipmid-providers/*.la \
"

LIBRARY_NAMES = "libami-ipmi-oem.so"
HOSTIPMI_PROVIDER_LIBRARY += "${LIBRARY_NAMES}"

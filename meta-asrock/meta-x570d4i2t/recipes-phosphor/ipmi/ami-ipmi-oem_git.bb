SUMMARY = "ASRock X570D4I-2T AMI SMBIOS handlers (blob + legacy MDR)"
DESCRIPTION = "Provides two coordinated surfaces backed by a shared converter \
between AMI Aptio V SMBIOS format and OpenBMC MDR V2 on-disk layout: \
(1) a phosphor-ipmi-blobs handler at blob ID /ami/smbios that serves the AMI \
view on read and persists incoming AMI buffers to /var/lib/smbios/smbios2 as \
MDR V2 on commit, and (2) a NetFn 0x3A IPMI provider implementing both the \
AMI proprietary push (0xF3/0xB2/0xA0/0xA1/0xB5) and the MDR V2 read/write \
endpoints (0x3D/0x51/0x52/0x53/0x71/0x72/0x30/0x31) — reads convert the \
on-disk MDR V2 cache to the AMI Aptio V view; writes accept AMI Aptio V \
buffers and convert back. On library load, if no cache exists yet, the baked \
smbios.dmp at /usr/share/x570d4i2t/smbios.dmp is used to seed a baseline."

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI = " \
    file://meson.build;subdir=${BP} \
    file://converter.hpp;subdir=${BP} \
    file://converter.cpp;subdir=${BP} \
    file://handler.hpp;subdir=${BP} \
    file://handler.cpp;subdir=${BP} \
    file://legacy.cpp;subdir=${BP} \
    file://main.cpp;subdir=${BP} \
    file://smbios.dmp \
"

S = "${UNPACKDIR}/${BP}"

DEPENDS = " \
    phosphor-ipmi-blobs \
    phosphor-ipmi-host \
    phosphor-logging \
    sdbusplus \
    systemd \
"

RDEPENDS:${PN} += "phosphor-ipmi-blobs phosphor-ipmi-host"

inherit meson pkgconfig

do_install:append() {
    install -d ${D}${datadir}/x570d4i2t
    install -m 0644 ${UNPACKDIR}/smbios.dmp ${D}${datadir}/x570d4i2t/smbios.dmp
}

FILES:${PN} += " \
    ${libdir}/blob-ipmid/lib*.so \
    ${libdir}/ipmid-providers/lib*.so \
    ${datadir}/x570d4i2t/smbios.dmp \
"

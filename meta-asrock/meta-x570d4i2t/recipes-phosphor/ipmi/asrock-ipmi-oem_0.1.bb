SUMMARY = "ASRock OEM IPMI commands for the X570D4I-2T"
DESCRIPTION = "\
Implements the ASRock / AMI OEM IPMI command set for the \
X570D4I-2T BMC running OpenBMC, using the same IPMI provider \
plugin architecture as phosphor-host-ipmid.  \
\
Provides: \
  - App NetFn overrides: GetDeviceId (real FW version from D-Bus), GetSystemGuid \
  - Chassis NetFn overrides: GetChassisStatus (with SIO intrusion state + \
    identify LED), ChassisIdentify (LED group), GetSystemRestartCause \
  - Sensor NetFn override: PlatformEvent (routes to phosphor-logging / Redfish) \
  - Storage NetFn overrides: GetSELInfo, AddSELEntry (Redfish bridge), GetSELTime \
  - BIOS OOB configuration protocol (SetBIOSCap / GetBIOSCap, \
    SetPayload / GetPayload) — received BIOS XML stored in /var/oob/ and \
    published on xyz.openbmc_project.BIOSConfig.Manager \
  - AMI/ASRock OEM commands (NetFn 0x30): GetBoardInfo, GetSensorInfo, \
    GetFwVersion, MuxSwitching (GPIOJ1), PeciReadWrite stub, PsuInfo, \
    ManageBmcConfig, GetSelPolicy, YAFU stubs (phosphor-ipmi-blobs path) \
  - MDR2 SMBIOS transfer protocol (NetFn 0x3E, 12 handlers) \
"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://meson.build \
    file://meson.options \
    file://include/biosconfig.hpp \
    file://include/oemcommands.hpp \
    file://src/appcommands.cpp \
    file://src/biosconfig.cpp \
    file://src/chassiscommands.cpp \
    file://src/oemcommands.cpp \
    file://src/sensorcommands.cpp \
    file://src/smbiosmdrv2handler.cpp \
    file://src/storagecommands.cpp \
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

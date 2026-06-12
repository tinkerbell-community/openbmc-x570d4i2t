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
  - AMI/ASRock OEM commands (NetFn 0x30): GetBoardInfo, GetSensorInfo, \
    GetFwVersion, MuxSwitching (GPIOJ1), PeciReadWrite stub, PsuInfo, \
    ManageBmcConfig, GetSelPolicy, YAFU stubs (phosphor-ipmi-blobs path) \
\
SMBIOS is handled over the Redfish Host Interface (bmcweb 0001 patch \
-> smbios-mdrv2), not IPMI; BIOS configuration likewise (bmcweb 0002 patch). \
"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://meson.build \
    file://meson.options \
    file://include/oemcommands.hpp \
    file://src/appcommands.cpp \
    file://src/chassiscommands.cpp \
    file://src/oemcommands.cpp \
    file://src/sensorcommands.cpp \
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
    systemd \
    libgpiod \
    libtinyxml2 \
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

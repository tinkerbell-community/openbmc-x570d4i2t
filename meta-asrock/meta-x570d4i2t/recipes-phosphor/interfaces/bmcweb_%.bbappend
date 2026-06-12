# Redfish Host Interface OEM endpoints for the X570D4I-2T.
#
# The AMI Megarac SPX host BIOS pushes SMBIOS and BIOS-configuration data to
# the BMC over the in-band Redfish Host Interface (USB NIC), authenticating as
# the HostAutoFW user. These two patches add the bmcweb routes the BIOS POSTs
# to and wire them into the OpenBMC services:
#   0001 — POST/DELETE /redfish/v1/Systems/<id>/Smbios -> smbios-mdrv2
#   0002 — POST /redfish/v1/Registries (Attribute Registry) and
#          POST /redfish/v1/Systems/<id>/Bios (current values) ->
#          xyz.openbmc_project.BIOSConfigManager BaseBIOSTable/PendingAttributes
# Both mirror the decompiled AMI host-interface Lua handlers (firmware
# v01.91.00: smbios-hi.lua, registry-collection-hi.lua, bios-hi.lua).
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI:append = " \
    file://0001-asrock-smbios-host-interface-push.patch \
    file://0002-asrock-bios-host-interface.patch \
    "

# Disable bmcweb's zstd HTTP compression.
#
# Upstream bmcweb 8a32dac3 (and surrounding commits) attempts zstd compression
# when the client offers it in Accept-Encoding, but the fallback path when
# ZSTD_createCCtx() fails returns an empty body instead of falling back to
# gzip/br. Result: any modern browser (Chrome 123+, Firefox 126+) sees a
# blank page because the HTML/JS responses are 0 bytes.
#
# Disabling http-zstd here forces bmcweb to use gzip/br only — which is
# what every browser already supports, and which works reliably.
#
# Once upstream bmcweb is fixed (correct fallback when zstd init fails, or
# the zstd init issue itself is resolved on ARM AST2500), this bbappend can
# be reverted.

PACKAGECONFIG:remove = "http-zstd"

# Default 30 MB upstream HTTP body limit is too small for our 64 MB BMC image
# tarball uploaded via Redfish UpdateService HttpPushUri / MultipartHttpPushUri.
# Bump to the upstream meson-options max (512 MB) so the firmware push
# endpoint accepts the full image without a 30 MB silent truncation.
EXTRA_OEMESON:append = " -Dhttp-body-limit=512"

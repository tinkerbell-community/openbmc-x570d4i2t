# Disable Intel-specific CPU inventory providers (no PECI on AMD Ryzen).
# mdrv2 (smbiosmdrv2app) is enabled by default in the upstream recipe; it
# provides the xyz.openbmc_project.Smbios.MDR_V2 D-Bus backend that the bmcweb
# Redfish Host Interface push (0001-asrock-smbios-host-interface-push) drives
# via AgentSynchronizeData after writing /var/lib/smbios/smbios2.
PACKAGECONFIG:remove = "cpuinfo cpuinfo-peci"

# NOTE: no IPMI SMBIOS ingestion. The AMI host BIOS pushes SMBIOS over the
# Redfish Host Interface, not IPMI (verified by boot-time KCS capture), so the
# smbios-ipmi-blob handler is intentionally NOT enabled.

# Install the DIMM socket/channel location table for this board.
# Keys are SMBIOS Type 17 Device Locator strings as reported by the ASRock
# BIOS (CPU1_DIMM_A1 / CPU1_DIMM_B1); values map each slot to its
# Socket / MemoryController / Channel / Slot index for Redfish/IPMI DIMM info.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append = " file://memoryLocationTable.json"

do_install:append() {
    install -d ${D}${datadir}/smbios-mdr
    install -m 0644 ${UNPACKDIR}/memoryLocationTable.json \
        ${D}${datadir}/smbios-mdr/memoryLocationTable.json
}

FILES:${PN}:append = " ${datadir}/smbios-mdr/memoryLocationTable.json"

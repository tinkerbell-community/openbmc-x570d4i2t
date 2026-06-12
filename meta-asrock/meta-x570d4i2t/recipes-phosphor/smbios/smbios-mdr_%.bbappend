# Disable Intel-specific CPU inventory providers (no PECI on AMD Ryzen).
# mdrv2 (smbiosmdrv2app) is enabled by default in the upstream recipe; it
# provides the xyz.openbmc_project.Smbios.MDR_V2 D-Bus backend. On this board
# asrock-ipmi-oem receives the AMI BIOS's IPMI SMBIOS fields (NetFn 0x3A 0xB5
# SetSmbiosChunk + NetFn 0x32 0x5D LegacyCtrl) and synthesizes the table from
# FRU/SPD, writes /var/lib/smbios/smbios2, then calls AgentSynchronizeData.
PACKAGECONFIG:remove = "cpuinfo cpuinfo-peci"

# smbios-ipmi-blob (phosphor-ipmi-blobs SMBIOS push) is NOT enabled — the AMI
# BIOS does not use the blob transfer; it uses the AMI-MDR IPMI commands handled
# in asrock-ipmi-oem. (Verified by live busctl capture + decompiled
# libipmimsghndlr: only a few OEM string/fields are pushed, no bulk table.)

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

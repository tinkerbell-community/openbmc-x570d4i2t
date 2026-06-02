# Disable Intel-specific CPU inventory providers (no PECI on AMD Ryzen).
# mdrv2 (smbiosmdrv2app) is enabled by default in the upstream recipe.
PACKAGECONFIG:remove = "cpuinfo cpuinfo-peci"

# Enable the IPMI blob handler so smbios tables can be pushed via blob interface.
PACKAGECONFIG:append = " smbios-ipmi-blob"

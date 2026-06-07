# Enable biosconfig-manager for the X570D4I-2T.
#
# This service provides the xyz.openbmc_project.BIOSConfig.Manager D-Bus
# interface that the asrock-ipmi-oem BIOS OOB plugin publishes received
# BIOS setup data into.  bmcweb then serves that data at:
#   /redfish/v1/Systems/system/Bios
#   /redfish/v1/Systems/system/Bios/Settings
#
# No board-specific overrides are needed; inheriting the recipe as-is
# is sufficient.

# Ensure the state directory exists at build time
do_install:append() {
    install -d ${D}${localstatedir}/lib/bios-settings-manager
}

FILES:${PN}:append = " ${localstatedir}/lib/bios-settings-manager"

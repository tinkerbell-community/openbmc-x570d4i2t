SUMMARY = "Host-side SMBIOS push tool for ASRock X570D4I-2T BMC"
DESCRIPTION = "\
On the ASRock Rack X570D4I-2T (AMD Ryzen/AM4 platform), the AMI BIOS does \
NOT push SMBIOS tables via IPMI KCS.  It relies on the Megarac proprietary \
Redfish Host Interface, which is absent from our OpenBMC build.  \
\
This recipe installs a Python helper script and systemd unit that the HOST \
ADMINISTRATOR can download from the BMC and install on the host Linux system. \
Once installed on the host, the script reads /sys/firmware/dmi/tables/ and \
pushes the live SMBIOS tables to the BMC via IPMI KCS (using NetFn 0x32 AMI \
MDR commands handled by ami-ipmi-oem), enabling smbios-mdrv2 to populate the \
D-Bus / Redfish inventory with real CPU, DIMM, and BIOS version data. \
\
Host installation (run as root on the host Linux system): \
  scp root@<BMC_IP>:/usr/share/host-smbios-push/push-smbios-to-bmc.py \\ \
      /usr/local/bin/ \
  scp root@<BMC_IP>:/usr/share/host-smbios-push/push-smbios-to-bmc.service \\ \
      /etc/systemd/system/ \
  chmod +x /usr/local/bin/push-smbios-to-bmc.py \
  systemctl enable --now push-smbios-to-bmc"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://push-smbios-to-bmc.py \
    file://push-smbios-to-bmc.service \
"

S = "${WORKDIR}"

# This package installs files only — no compilation needed.
do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${datadir}/host-smbios-push
    install -m 0755 ${WORKDIR}/push-smbios-to-bmc.py \
        ${D}${datadir}/host-smbios-push/push-smbios-to-bmc.py
    install -m 0644 ${WORKDIR}/push-smbios-to-bmc.service \
        ${D}${datadir}/host-smbios-push/push-smbios-to-bmc.service
}

FILES:${PN} = "${datadir}/host-smbios-push"

# No runtime dependencies on the BMC side; this is purely static data.
RDEPENDS:${PN} = ""

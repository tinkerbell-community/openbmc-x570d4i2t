SUMMARY = "Patched host BIOS image (ASRock X570D4I-2T 2.59C + SmbiosBmcPushDxe)"
DESCRIPTION = "\
Fetches the stock ASRock X570D4I-2T 2.59C AMI Aptio BIOS image and injects our \
own SmbiosBmcPushDxe DXE driver into the *dispatched* FVMAIN_COMPACT by replacing \
the AMI SendInfoBmcIpmiDxe module (FILE_GUID 9DF02DFD-…) — a module the DXE core \
already dispatches for BMC communication, so dispatch + per-module integrity are \
not disturbed.  Our driver pushes the host SMBIOS table to the BMC over KCS via \
phosphor-ipmi-blobs (blob \"/smbios\") at EndOfDxe/ReadyToBoot, completely \
replacing the closed AMI Redfish-Host-Interface (RHI/RNDIS) stack we could not \
drive from the BMC side.\
\
The injection is SIZE-NEUTRAL: the inner FV is recompressed and the output ROM is \
asserted to be EXACTLY 32 MiB (33554432 bytes) — it is never grown past the SPI \
part size.  The resulting image is what gets written to the host BIOS SPI flash by \
the BMC's existing host-BIOS update path (GPIOJ1 mux + mtd / spi-aspeed-smc), or \
by any in-BIOS / EEPROM-programmer flow.\
"

# The deliverable is the proprietary ASRock/AMI Aptio BIOS image (redistribution
# governed by ASRock) with our small BSD-2-Clause-Patent SmbiosBmcPushDxe injected;
# treat the resulting firmware blob as CLOSED. (The DXE source itself carries its
# BSD header in recipes-phosphor/.../SmbiosBmcPushDxe.c.)
LICENSE = "CLOSED"

COMPATIBLE_MACHINE = "x570d4i2t"

# Stock BIOS, fetched straight from ASRock's public download server (same pattern
# the flasher-iso recipe uses for socflash/FreeDOS).  The 32 MiB ROM "X574I2T2.59C"
# lives inside the zip; Yocto auto-unpacks it into ${UNPACKDIR}.
#   to refresh the sha256 after an ASRock BIOS bump:
#     curl -fsSL 'https://download.asrock.com/BIOS/Server/X570D4I-2T(2.59C)ROM.zip' | sha256sum
SRC_URI = " \
    https://download.asrock.com/BIOS/Server/X570D4I-2T(2.59C)ROM.zip;name=bios;downloadfilename=x570d4i2t-bios-2.59C.zip \
    file://patch_rom.py \
    file://SmbiosBmcPushDxe.efi \
    file://SmbiosBmcPushDxe.c \
    file://SmbiosBmcPushDxe.inf \
"
SRC_URI[bios.sha256sum] = "712fa89eda6334f9e1563217be1fe83d2d66a72b7958bb6984dce70221ac1175"

# Build-host tooling: the patcher is pure-Python (struct + lzma).  The BMC has no
# python3/lzma, so injection MUST happen at build time — the BMC only ever writes
# the finished image.
DEPENDS = "python3-native"

# Stock ROM filename inside the zip, and the patched output name we deploy.
ROM_NAME    ?= "X574I2T2.59C"
PATCHED_ROM ?= "host-bios-${MACHINE}-2.59C-smbiospush.rom"

# 32 MiB SPI part — the output must match this EXACTLY (no boundary growth).
HOST_BIOS_SIZE ?= "33554432"

inherit deploy

# Produces no rootfs packages — this is a deploy-only firmware artifact.
do_package[noexec]           = "1"
do_packagedata[noexec]       = "1"
do_package_write_ipk[noexec] = "1"
do_populate_sysroot[noexec]  = "1"

do_compile() {
    STOCK="${UNPACKDIR}/${ROM_NAME}"
    EFI="${UNPACKDIR}/SmbiosBmcPushDxe.efi"
    PATCHER="${UNPACKDIR}/patch_rom.py"
    OUT="${WORKDIR}/${PATCHED_ROM}"

    [ -f "${STOCK}" ]   || bbfatal "stock ROM not found after unpack: ${STOCK}"
    [ -f "${EFI}" ]     || bbfatal "injected DXE not found: ${EFI}"
    [ -f "${PATCHER}" ] || bbfatal "patcher not found: ${PATCHER}"

    # The patcher needs python3 + the lzma module (inner FV is LZMA-compressed).
    if ! python3 -c "import lzma" 2>/dev/null; then
        bbfatal "python3-native lacks the 'lzma' module (need liblzma at build); add xz-native to DEPENDS"
    fi

    # Sanity: stock ROM must already be 32 MiB.
    SSZ="$(stat -c%s "${STOCK}")"
    [ "${SSZ}" = "${HOST_BIOS_SIZE}" ] || \
        bbfatal "stock ROM is ${SSZ} bytes, expected ${HOST_BIOS_SIZE} (32 MiB)"

    # Inject (replace SendInfoBmcIpmiDxe in the dispatched FVMAIN_COMPACT, recompress).
    python3 "${PATCHER}" "${EFI}" --rom "${STOCK}" --out "${OUT}" || \
        bbfatal "patch_rom.py failed to inject SmbiosBmcPushDxe"

    # HARD 32 MiB assertion — the bundle must never exceed / differ from the part size.
    OSZ="$(stat -c%s "${OUT}")"
    [ "${OSZ}" = "${HOST_BIOS_SIZE}" ] || \
        bbfatal "patched ROM is ${OSZ} bytes, must be EXACTLY ${HOST_BIOS_SIZE} (32 MiB)"

    # Verify our DXE actually landed: the "/smbios" blob id lives in our PE32, and
    # patch_rom exposes the FV/LZMA helpers so we can confirm it is inside the
    # (recompressed) inner FV rather than silently dropped.
    PYTHONPATH="${UNPACKDIR}" python3 - "${OUT}" <<'PYEOF' || bbfatal "injected DXE not present in patched ROM"
import sys, struct, patch_rom as P
rom = open(sys.argv[1], 'rb').read()
fv = rom[P.FV_OFFSET:P.FV_OFFSET + P.FV_SIZE]
oo, osz = P.find_ffs_in_fv(fv, P.OUTER_FFS_GUID)
ffs = fv[oo:oo + osz]; o = 24; comp = None
while o + 4 <= osz:
    s = P.u24(ffs, o); t = ffs[o + 3]
    if t == P.SEC_GUID_DEFINED:
        gd = struct.unpack_from('<H', ffs, o + 20)[0]; comp = ffs[o + gd:o + s]; break
    o += (s + 3) & ~3
inner = P.lzma_decompress(comp)
sys.exit(0 if b'/smbios' in inner else 1)
PYEOF

    bbnote "Injected SmbiosBmcPushDxe into ${ROM_NAME}; output ${PATCHED_ROM} = ${OSZ} bytes (32 MiB), /smbios verified present."
}

do_deploy() {
    install -d "${DEPLOYDIR}"
    install -m 0644 "${WORKDIR}/${PATCHED_ROM}" "${DEPLOYDIR}/${PATCHED_ROM}"
    # Stable convenience symlink for the flash tooling / Redfish upload.
    ln -sf "${PATCHED_ROM}" "${DEPLOYDIR}/host-bios-${MACHINE}.rom"

    bbnote ""
    bbnote "[OK] Patched host BIOS: ${DEPLOYDIR}/${PATCHED_ROM}  (32 MiB)"
    bbnote "  Flash to the host BIOS SPI via the BMC host-BIOS update path"
    bbnote "  (Redfish UpdateService image -> bios-update.sh -> GPIOJ1 mux + mtd),"
    bbnote "  or via an external EEPROM/SPI programmer.  Contains SmbiosBmcPushDxe,"
    bbnote "  which pushes SMBIOS to the BMC over KCS phosphor-ipmi-blobs (/smbios)."
    bbnote ""
}
addtask do_deploy after do_compile before do_build

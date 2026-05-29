#!/bin/bash
# Rebuild OpenBMC BMC Flasher ISO (FreeDOS + ASPEED SocFlash DOS edition).
#
# Produces a BIOS-bootable ISO that flashes the X570D4I-2T's 32 MB BMC SPI NOR
# from the host CPU via the AST2500 P2A (PCIe-to-AHB) bridge using ASPEED's
# SocFlash DOS utility (the only variant ASRock publishes for this board).
#
# *** BIOS-only: enable CSM in BIOS Setup before booting this ISO. ***
# The DOS PMODE/W extender cannot run under UEFI native; the ISO uses isolinux
# + memdisk to boot a FAT16 hard-disk image containing FreeDOS, SocFlash, and
# the OpenBMC firmware.
#
# Inputs:
#   FIRMWARE      build/x570d4i2t/.../obmc-phosphor-image-x570d4i2t.static.mtd
#   SOCFLASH_EXE  vendor/socflash.EXE  (extracted from socflash v1.20.00.zip)
#
# Usage:
#   ./openbmc-flasher-rebuild.sh [--mode WIPE|REFRESH] [--chip 0|1] [/path/to/socflash.EXE]
#
# Defaults: --mode WIPE --chip 0   (X570D4I-2T uses CS0 — confirmed empirically
#                                    by socflash "Boot CS is 0"; CS1 reports
#                                    "Can't Find Flash Chip #1")

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="/tmp/openbmc-flasher"
ISO_OUT="${REPO_ROOT}/.github/x570d4i2t-port/openbmc-flasher.iso"
IMA_OUT="${REPO_ROOT}/.github/x570d4i2t-port/openbmc-x570d4i2t.ima"
USB_OUT="${REPO_ROOT}/.github/x570d4i2t-port/openbmc-bmc-fat32.img"
BMC_ISO_OUT="${REPO_ROOT}/.github/x570d4i2t-port/openbmc-bmc-fat32.iso"
FIRMWARE="${REPO_ROOT}/build/x570d4i2t/tmp/deploy/images/x570d4i2t/obmc-phosphor-image-x570d4i2t.static.mtd"
SOCFLASH_EXE="${REPO_ROOT}/.github/x570d4i2t-port/vendor/socflash.EXE"
FREEDOS_URL="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.3/official/FD13-FloppyEdition.zip"

MODE="WIPE"
CHIP="0"
SOCFLASH_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)     MODE="$2"; shift 2 ;;
        --chip)     CHIP="$2"; shift 2 ;;
        -h|--help)  sed -n '2,22p' "$0"; exit 0 ;;
        *)          SOCFLASH_OVERRIDE="$1"; shift ;;
    esac
done

case "$MODE" in WIPE|REFRESH) ;; *) echo "ERROR: --mode must be WIPE or REFRESH"; exit 1 ;; esac
case "$CHIP" in 0|1) ;; *) echo "ERROR: --chip must be 0 or 1"; exit 1 ;; esac
[ -n "$SOCFLASH_OVERRIDE" ] && SOCFLASH_EXE="$SOCFLASH_OVERRIDE"

# ---------------------------------------------------------------------------
# PHASE 0: input + tool validation
# ---------------------------------------------------------------------------

if [ ! -f "$FIRMWARE" ]; then
    echo "ERROR: firmware image not found:"
    echo "  $FIRMWARE"
    echo "Build first: cd $REPO_ROOT && source ./setup x570d4i2t && bitbake obmc-phosphor-image"
    exit 1
fi
if [ ! -f "$SOCFLASH_EXE" ]; then
    echo "ERROR: socflash.EXE not found at $SOCFLASH_EXE"
    echo "Pass it explicitly: $0 /path/to/socflash.EXE"
    exit 1
fi
if ! file "$SOCFLASH_EXE" | grep -q "MS-DOS"; then
    echo "ERROR: $SOCFLASH_EXE is not a DOS executable"
    echo "  $(file "$SOCFLASH_EXE")"
    exit 1
fi

MISSING=()
for tool in xorriso curl unzip mformat mcopy mdir sfdisk dd file nasm; do
    command -v "$tool" &>/dev/null || MISSING+=("$tool")
done
if [ ${#MISSING[@]} -gt 0 ]; then
    echo "ERROR: missing tools: ${MISSING[*]}"
    echo "Install: sudo pacman -S xorriso curl unzip mtools util-linux coreutils file syslinux nasm"
    exit 1
fi

SYSLINUX_DIR=""
for d in /usr/lib/syslinux/bios /usr/lib/ISOLINUX /usr/share/syslinux; do
    [ -f "$d/memdisk" ] && [ -f "$d/isolinux.bin" ] && [ -f "$d/mbr.bin" ] && SYSLINUX_DIR="$d" && break
done
if [ -z "$SYSLINUX_DIR" ]; then
    echo "ERROR: syslinux modules (memdisk, isolinux.bin, mbr.bin) not found"
    echo "Install: sudo pacman -S syslinux"
    exit 1
fi

# ---------------------------------------------------------------------------
# PHASE 1: build FreeDOS hard-disk image (regenerated every run)
# ---------------------------------------------------------------------------

echo "[*] Preparing work tree..."
mkdir -p "$WORK"/{cache,iso/{isolinux,boot}}
rm -f "$WORK/iso/boot/freedos.img"

# Cache FreeDOS 1.3 Floppy Edition (~22 MB) across runs
if [ ! -f "$WORK/cache/FD13-FloppyEdition.zip" ]; then
    echo "[*] Downloading FreeDOS 1.3 Floppy Edition..."
    curl -fSL --progress-bar -o "$WORK/cache/FD13-FloppyEdition.zip.tmp" "$FREEDOS_URL"
    mv "$WORK/cache/FD13-FloppyEdition.zip.tmp" "$WORK/cache/FD13-FloppyEdition.zip"
fi

if [ ! -f "$WORK/cache/x86BOOT.img" ]; then
    echo "[*] Extracting FreeDOS boot floppy..."
    unzip -q -o -j -d "$WORK/cache" "$WORK/cache/FD13-FloppyEdition.zip" "144m/x86BOOT.img"
fi
FD_BOOT="$WORK/cache/x86BOOT.img"

# Pull KERNEL.SYS + COMMAND.COM out of the FreeDOS boot floppy
echo "[*] Extracting FreeDOS system files..."
mcopy -n -o -i "$FD_BOOT" ::/KERNEL.SYS                   "$WORK/KERNEL.SYS"
mcopy -n -o -i "$FD_BOOT" ::/freedos/bin/command.com      "$WORK/COMMAND.COM"

# Build the FAT16-variant FreeDOS boot sector. FreeDOS' boot.asm compiles to
# DIFFERENT 512-byte binaries for FAT12 vs FAT16 (per `%ifdef ISFAT12/ISFAT16`
# in the source) — the FAT-walk routines diverge. The floppy boot sector we
# previously extracted is FAT12-compiled and walks FAT entries as 12-bit
# values; against our FAT16 partition it produces bogus cluster numbers and
# crashes mid-load with "Error!". We must compile the FAT16 variant.
if [ ! -f "$WORK/cache/fat16-boot.bin" ]; then
    echo "[*] Compiling FreeDOS FAT16 boot sector from upstream source..."
    curl -fsSL -o "$WORK/cache/boot.asm"  "https://raw.githubusercontent.com/FDOS/kernel/master/boot/boot.asm"
    curl -fsSL -o "$WORK/cache/magic.mac" "https://raw.githubusercontent.com/FDOS/kernel/master/boot/magic.mac"
    nasm -DISFAT16=1 -I "$WORK/cache/" -f bin "$WORK/cache/boot.asm" -o "$WORK/cache/fat16-boot.bin"
fi
# Extract bytes 0x3E-0x1FF (FreeDOS code + signature, 450 bytes) — these
# overwrite mformat's generic FAT16 boot code while preserving mformat's
# FAT16 BPB at 0x00-0x3D.
dd if="$WORK/cache/fat16-boot.bin" of="$WORK/fd-bootcode.bin" bs=1 skip=62 count=450 status=none

# Generate FDCONFIG.SYS (FreeDOS' equivalent of CONFIG.SYS).
# Notes:
#   - Skipping DOS=HIGH,UMB: requires HIMEM.SYS/EMM386 which we don't ship;
#     when present without those drivers the kernel may behave unpredictably.
#   - SHELL line uses absolute C:\ paths and explicitly names the autoexec
#     via /P=PATH (FreeCOM defaults to AUTOEXEC.BAT otherwise).
cat > "$WORK/FDCONFIG.SYS" << 'EOF'
FILES=20
BUFFERS=10
LASTDRIVE=Z
SHELL=C:\COMMAND.COM C:\ /E:1024 /P=C:\AUTOEXEC.BAT
EOF

# Generate FDAUTO.BAT (FreeDOS' equivalent of AUTOEXEC.BAT)
SCMD="SOCFLASH if=FW.IMG cs=$CHIP"
if [ "$MODE" = "REFRESH" ]; then
    # Skip the first 64 KiB (preserve u-boot env / vendor config block)
    SCMD="$SCMD skip=0x10000 offset=0x10000"
fi
ALT_CHIP=$([ "$CHIP" = "0" ] && echo 1 || echo 0)

cat > "$WORK/FDAUTO.BAT" << AEOF
@ECHO OFF
CLS
ECHO ===============================================
ECHO   OpenBMC BMC Flasher -- ASRock X570D4I-2T
ECHO ===============================================
ECHO.
ECHO   Mode  : $MODE
ECHO   Chip  : $CHIP   (socflash cs=$CHIP)
ECHO   Image : C:\\FW.IMG  (64 MB OpenBMC)
ECHO.
ECHO   Press Ctrl-C now to abort, or any key to begin.
ECHO   Flash takes ~2-3 minutes. DO NOT power off.
ECHO.
PAUSE
ECHO.
C:
CD \\
$SCMD
IF ERRORLEVEL 1 GOTO FAIL
ECHO.
ECHO ===============================================
ECHO   [OK] Flash succeeded.
ECHO   Eject the virtual CD, then power-cycle host.
ECHO ===============================================
GOTO END
:FAIL
ECHO.
ECHO ===============================================
ECHO   [FAIL] socflash returned an error.
ECHO   Retry on alternate chip-select:
ECHO     SOCFLASH if=FW.IMG cs=$ALT_CHIP
ECHO ===============================================
:END
AEOF

# Sanity-check: SocFlash must be ≤ 8.3 (DOS 8.3 filename limit)
# We name it SOCFLASH.EXE (8.3-clean) in the FAT16.
cp "$SOCFLASH_EXE" "$WORK/SOCFLASH.EXE"

# Build the 80 MB FAT16 hard-disk image
HDD="$WORK/iso/boot/freedos.img"
echo "[*] Building 80 MB FAT16 hard-disk image..."
truncate -s 80M "$HDD"

# Partition table: one active FAT16 partition starting at sector 2048
echo "label: dos
unit: sectors
start=2048, type=06, bootable" | sfdisk --quiet "$HDD"

# Install syslinux MBR boot code (loads active partition's VBR)
dd if="$SYSLINUX_DIR/mbr.bin" of="$HDD" bs=440 count=1 conv=notrunc status=none

# Format the partition as FAT16.
#   - No -F: that flag means "force FAT32" (would shift code to 0x5A).
#   - -H 2048: BPB.hidden_sectors must record the partition's LBA on the
#     disk; FreeDOS' boot loader uses it to translate FAT-relative sectors
#     to absolute LBA. mformat's @@offset does NOT set this automatically.
PART_START_LBA=2048
PART_OFFSET=$((PART_START_LBA * 512))
mformat -H "$PART_START_LBA" -i "$HDD@@$PART_OFFSET" -v "OBMCFLASH" ::

# Patch the FreeDOS boot-loader code over our FAT16 VBR's code area
# (offset 62 within the VBR = end of extended BPB; 450 bytes through 0x1FF).
dd if="$WORK/fd-bootcode.bin" of="$HDD" bs=1 \
   seek=$((PART_OFFSET + 62)) count=450 conv=notrunc status=none

# Populate the FAT16 partition. KERNEL.SYS must come first (FreeDOS boot
# loader expects to find it via the root directory; placing it first
# minimises fragmentation, though FreeDOS handles fragmented kernels too).
echo "[*] Populating FreeDOS partition..."
mcopy -o    -i "$HDD@@$PART_OFFSET" "$WORK/KERNEL.SYS"   ::/
mcopy -o    -i "$HDD@@$PART_OFFSET" "$WORK/COMMAND.COM"  ::/
# Ship the same config under both names so the kernel picks one up either way.
mcopy -o -t -i "$HDD@@$PART_OFFSET" "$WORK/FDCONFIG.SYS" ::/FDCONFIG.SYS
mcopy -o -t -i "$HDD@@$PART_OFFSET" "$WORK/FDCONFIG.SYS" ::/CONFIG.SYS
# Ship the autoexec as AUTOEXEC.BAT (FreeCOM's default lookup name).
mcopy -o -t -i "$HDD@@$PART_OFFSET" "$WORK/FDAUTO.BAT"   ::/AUTOEXEC.BAT
mcopy -o    -i "$HDD@@$PART_OFFSET" "$WORK/SOCFLASH.EXE" ::/

echo "[*] Copying 64 MB firmware image into the FAT16 partition..."
mcopy -o -i "$HDD@@$PART_OFFSET" "$FIRMWARE" ::/FW.IMG

echo "[*] HDD image contents:"
mdir -i "$HDD@@$PART_OFFSET" :: | sed 's/^/    /'

# ---------------------------------------------------------------------------
# PHASE 2: ISO9660 layout (isolinux + memdisk)
# ---------------------------------------------------------------------------

echo "[*] Installing isolinux + memdisk..."
cp "$SYSLINUX_DIR/isolinux.bin" "$WORK/iso/isolinux/"
cp "$SYSLINUX_DIR/ldlinux.c32"  "$WORK/iso/isolinux/" 2>/dev/null || true
cp "$SYSLINUX_DIR/memdisk"      "$WORK/iso/isolinux/"

cat > "$WORK/iso/isolinux/isolinux.cfg" << 'EOF'
DEFAULT flash
PROMPT 1
TIMEOUT 50

LABEL flash
  MENU LABEL OpenBMC BMC Flasher (FreeDOS + SocFlash)
  KERNEL /isolinux/memdisk
  INITRD /boot/freedos.img
  APPEND harddisk

LABEL shell
  MENU LABEL Drop to DOS shell (skip auto-flash)
  KERNEL /isolinux/memdisk
  INITRD /boot/freedos.img
  APPEND harddisk safeint
EOF

# ---------------------------------------------------------------------------
# PHASE 2.5: stage .ima copies for in-BIOS BMC-update path testing
# ---------------------------------------------------------------------------
# The .ima rename lets the user try ASRock/AMI in-BIOS BMC update facilities
# (BIOS Setup -> Server Mgmt -> BMC firmware update, or vendor utilities that
# scan FAT volumes for *.ima). Same raw 64 MB OpenBMC payload, just renamed:
#   - standalone artifact next to the ISO (for USB-stick / web-UI uploads)
#   - BMC.IMA on the ISO9660 root (8.3-clean for in-BIOS scanners)
# Note: in-BIOS paths that route through MegaRAC's update receiver are still
# signature-checked per 05-flash-and-verify.md; this is a "try it and see"
# bypass attempt, not a guaranteed working path.
echo "[*] Staging .ima artifacts..."
cp -f "$FIRMWARE" "$IMA_OUT"
cp -f "$FIRMWARE" "$WORK/iso/BMC.IMA"

# Build a partitioned FAT32 disk image with BMC.IMA at root for direct
# dd-to-USB testing of the in-BIOS BMC update path.
echo "[*] Building FAT32 USB disk image..."
truncate -s 96M "$USB_OUT"
echo "label: dos
unit: sectors
start=2048, type=0c, bootable" | sfdisk --quiet "$USB_OUT"
dd if="$SYSLINUX_DIR/mbr.bin" of="$USB_OUT" bs=440 count=1 conv=notrunc status=none
USB_PART_OFFSET=$((2048 * 512))
mformat -F -H 2048 -i "$USB_OUT@@$USB_PART_OFFSET" -v "BMC-FLASH" ::
mcopy -o -i "$USB_OUT@@$USB_PART_OFFSET" "$FIRMWARE" ::/BMC.IMA

# Same payload as the FAT32 USB image, but wrapped as a plain ISO9660 data
# CD for BMC virtual-media mounting (no boot record needed — the in-BIOS
# flasher just walks the filesystem looking for BMC.IMA).
echo "[*] Building BMC-only data ISO..."
mkdir -p "$WORK/bmc-iso"
cp -f "$FIRMWARE" "$WORK/bmc-iso/BMC.IMA"
xorriso -as mkisofs \
    -V "BMC-FLASH" \
    -J -R -iso-level 3 \
    "$WORK/bmc-iso" \
    -o "$BMC_ISO_OUT" 2>&1 | tail -2

# ---------------------------------------------------------------------------
# PHASE 3: emit the ISO
# ---------------------------------------------------------------------------

echo "[*] Building ISO..."
mkdir -p "$(dirname "$ISO_OUT")"
xorriso -as mkisofs \
    -R -J -V "OBMC-FLASHER" \
    -iso-level 3 \
    -b isolinux/isolinux.bin \
    -c isolinux/boot.cat \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    "$WORK/iso" \
    -o "$ISO_OUT" 2>&1 | tail -3

echo ""
echo "[OK] ISO         : $ISO_OUT  ($(du -sh "$ISO_OUT" | cut -f1))"
echo "     IMA (raw)   : $IMA_OUT  ($(du -sh "$IMA_OUT" | cut -f1))"
echo "     USB (FAT32) : $USB_OUT  ($(du -sh "$USB_OUT" | cut -f1))   dd to USB stick"
echo "     BMC-only ISO: $BMC_ISO_OUT  ($(du -sh "$BMC_ISO_OUT" | cut -f1))   mount via vCD"
echo "     ISO9660 has : /BMC.IMA  (in-BIOS flash-utility candidate)"
echo "     FreeDOS img : $(du -sh "$HDD" | cut -f1)"
echo "     socflash    : $(sha256sum "$SOCFLASH_EXE" | cut -c1-12)... ($(du -sh "$SOCFLASH_EXE" | cut -f1))"
echo ""
echo "*** BIOS-only image — enable CSM in BIOS Setup before booting. ***"
echo ""
echo "Next steps:"
echo "  1. BIOS Setup -> Boot -> CSM / Compatibility Support Module = Enabled"
echo "     (Also set 'Storage' OpRom to 'Legacy' if booting via virtual CD)"
echo "  2. Upload $ISO_OUT to BMC virtual media (MegaRAC web UI -> Remote Control)"
echo "  3. Force one-shot boot to the virtual CD:"
echo "       ipmitool -I lanplus -H <bmc> -U <u> -P <p> chassis bootdev cdrom"
echo "       ipmitool -I lanplus -H <bmc> -U <u> -P <p> chassis power reset"
echo "  4. Confirm the prompt at FDAUTO.BAT; socflash will rewrite the BMC SPI"
echo "  5. After [OK], eject media, re-enter Setup, set CSM back to Disabled"
echo ""

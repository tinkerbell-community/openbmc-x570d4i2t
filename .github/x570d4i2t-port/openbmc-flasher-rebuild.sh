#!/bin/bash
# Rebuild OpenBMC BMC Flasher ISO (hybrid BIOS+UEFI, minimal bare-metal amd64).
#
# Boots on the X570D4I-2T host via BMC virtual media, then flashes the BMC SPI
# NOR over the AST2500 P2A (PCIe-to-AHB) bridge using ASPEED's socflash binary,
# wrapped by the `update_bmc` script from:
#   https://github.com/EyerNi/use-socflash-in-shell
#
# Image contents:
# - Linux kernel (host's, auto-detected)
# - Busybox-only initramfs (static)
# - 64 MB OpenBMC image (obmc-phosphor-image-x570d4i2t.static.mtd) at /firmware.img
# - EyerNi update_bmc + config.ini (X570D4I-2T offsets) at /
# - socflash_x64 binary at /fake_socflash_x86_64 (upstream-expected name)
#
# Usage:
#   ./openbmc-flasher-rebuild.sh [--build-iso] [--mode WIPE|REFRESH] [--chip 0|1] [/path/to/socflash_x64]
#
# Defaults: --mode WIPE --chip 1   (X570D4I-2T uses CS1, full-image overwrite)
#
# Examples:
#   ./openbmc-flasher-rebuild.sh --build-iso  # Build the ISO from scratch
#   ./openbmc-flasher-rebuild.sh /usr/local/bin/socflash_x64
#   ./openbmc-flasher-rebuild.sh --mode REFRESH ~/Downloads/socflash_x64

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="/tmp/openbmc-flasher"
ISO_OUT="${REPO_ROOT}/.github/x570d4i2t-port/openbmc-flasher.iso"
FIRMWARE="${REPO_ROOT}/build/x570d4i2t/tmp/deploy/images/x570d4i2t/obmc-phosphor-image-x570d4i2t.static.mtd"
EYERNI_REPO="https://github.com/EyerNi/use-socflash-in-shell.git"

BUILD_ISO=false
SOCFLASH=""
MODE="WIPE"
CHIP="1"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-iso)   BUILD_ISO=true; shift ;;
        --mode)        MODE="$2"; shift 2 ;;
        --chip)        CHIP="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,28p' "$0"; exit 0 ;;
        *)             SOCFLASH="$1"; shift ;;
    esac
done

case "$MODE" in WIPE|REFRESH) ;; *) echo "ERROR: --mode must be WIPE or REFRESH"; exit 1 ;; esac
case "$CHIP" in 0|1) ;; *) echo "ERROR: --chip must be 0 or 1"; exit 1 ;; esac

# =============================================================================
# PHASE 1: Build the ISO from scratch (if requested or if work dir missing)
# =============================================================================

if [ "$BUILD_ISO" = true ] || [ ! -d "$WORK/initramfs" ]; then
    echo "[*] Building ISO from scratch..."
    echo ""

    if [ ! -f "$FIRMWARE" ]; then
        echo "ERROR: Firmware image not found at:"
        echo "  $FIRMWARE"
        echo ""
        echo "Build the OpenBMC image first:"
        echo "  cd $REPO_ROOT"
        echo "  source ./setup x570d4i2t"
        echo "  bitbake obmc-phosphor-image"
        exit 1
    fi

    # Verify required tools (host build deps)
    MISSING=()
    for tool in xorriso cpio gzip git grub-mkstandalone mkfs.vfat mcopy mmd; do
        command -v "$tool" &>/dev/null || MISSING+=("$tool")
    done
    if [ ${#MISSING[@]} -gt 0 ]; then
        echo "ERROR: Missing tools: ${MISSING[*]}"
        echo "Install with: sudo pacman -S xorriso cpio gzip git grub mtools syslinux busybox"
        exit 1
    fi

    if ! command -v busybox &>/dev/null; then
        echo "ERROR: busybox not installed (sudo pacman -S busybox)"
        exit 1
    fi
    if ! file "$(command -v busybox)" | grep -q "statically linked"; then
        echo "ERROR: busybox must be statically linked"
        exit 1
    fi

    # Locate host kernel
    KERNEL=""
    for candidate in "/boot/vmlinuz-$(uname -r)" /boot/vmlinuz-linux /boot/vmlinuz; do
        [ -f "$candidate" ] && KERNEL="$candidate" && break
    done
    if [ -z "$KERNEL" ]; then
        KERNEL="$(ls -1 /boot/vmlinuz-* 2>/dev/null | head -1)"
    fi
    if [ -z "$KERNEL" ] || [ ! -f "$KERNEL" ]; then
        echo "ERROR: No kernel found under /boot (vmlinuz-*)"
        exit 1
    fi
    echo "[*] Using host kernel: $KERNEL"

    # Locate isolinux bootloader files
    ISOLINUX_DIR=""
    for d in /usr/lib/syslinux/bios /usr/lib/ISOLINUX /usr/share/syslinux; do
        [ -f "$d/isolinux.bin" ] && ISOLINUX_DIR="$d" && break
    done
    if [ -z "$ISOLINUX_DIR" ]; then
        echo "ERROR: isolinux.bin not found (install: pacman -S syslinux)"
        exit 1
    fi

    # Clean and rebuild work tree
    rm -rf "$WORK"
    mkdir -p "$WORK"/{initramfs/{bin,sbin,dev,proc,sys,tmp,etc},iso/{boot/grub,isolinux,EFI/BOOT}}

    # Busybox + applet symlinks
    echo "[*] Installing busybox..."
    cp "$(command -v busybox)" "$WORK/initramfs/bin/busybox"
    for cmd in sh ash echo cat mount umount ls mkdir sleep clear grep awk cut sed printf chmod file head tail uname dd cp mv rm ln; do
        ln -sf busybox "$WORK/initramfs/bin/$cmd"
    done
    ln -sf ../bin/busybox "$WORK/initramfs/sbin/reboot"
    ln -sf ../bin/busybox "$WORK/initramfs/sbin/poweroff"
    ln -sf ../bin/busybox "$WORK/initramfs/sbin/mdev"

    # Firmware image
    echo "[*] Copying 64 MB firmware image..."
    cp "$FIRMWARE" "$WORK/initramfs/firmware.img"

    # EyerNi update_bmc wrapper (https://github.com/EyerNi/use-socflash-in-shell)
    echo "[*] Cloning EyerNi/use-socflash-in-shell..."
    git clone --depth=1 "$EYERNI_REPO" "$WORK/eyerni" >/dev/null 2>&1
    cp "$WORK/eyerni/update_bmc"  "$WORK/initramfs/update_bmc"
    chmod +x "$WORK/initramfs/update_bmc"

    # X570D4I-2T config: flash the whole 64 MB image from offset 0.
    # platform_ball=a8 = upstream default; override here if a future board needs
    # a different AST2500 GPIO ball for the BMC reset assertion during flash.
    cat > "$WORK/initramfs/config.ini" << 'CFG_EOF'
#! WATCH OUT !

#the hardware nor spi chip offset
chip_offset:0x0000000

#the image binary offset
image_offset:0x0000000

#platform hardware design for reset bmc ctrl
platform_ball:a8
CFG_EOF

    # init script — runs update_bmc with configured chip + mode
    echo "[*] Writing init..."
    cat > "$WORK/initramfs/init" << INIT_EOF
#!/bin/sh
export PATH=/bin:/sbin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || mdev -s

clear
echo "==========================================="
echo "  OpenBMC BMC Flasher -- ASRock X570D4I-2T"
echo "==========================================="
echo ""
echo "  Mode: ${MODE}    Chip: ${CHIP}    Image: /firmware.img (64 MB)"
echo ""

if [ ! -x /fake_socflash_x86_64 ]; then
    echo "[!] /fake_socflash_x86_64 (socflash binary) not present in this ISO."
    echo ""
    echo "    Rebuild with the AMI socflash_x64 binary:"
    echo "      openbmc-flasher-rebuild.sh /path/to/socflash_x64"
    echo ""
    echo "    Dropping to shell."
    exec /bin/sh
fi

echo "[*] Flashing BMC SPI via P2A (socflash via update_bmc)..."
echo "[!] Do NOT power off. Takes ~2-3 minutes."
echo ""

cd /
if ./update_bmc ${CHIP} firmware.img ${MODE}; then
    echo ""
    echo "[OK] Flash succeeded. Eject virtual media, then reboot in 15s."
    sleep 15
    echo b > /proc/sysrq-trigger
else
    rc=\$?
    echo ""
    echo "[FAIL] update_bmc exited \$rc. Dropping to shell."
    echo "       Try alt chip-select:  ./update_bmc \$([ ${CHIP} = 0 ] && echo 1 || echo 0) firmware.img ${MODE}"
    echo "       Or raw socflash:      ./fake_socflash_x86_64 cs=${CHIP} w /firmware.img"
    exec /bin/sh
fi
INIT_EOF
    chmod +x "$WORK/initramfs/init"

    # GRUB config (used by both UEFI grub-mkstandalone and BIOS fallback)
    KCMDLINE="console=tty0 console=ttyS0,115200n8 iomem=relaxed amd_iommu=off intel_iommu=off sysrq_always_enabled=1 quiet"
    cat > "$WORK/iso/boot/grub/grub.cfg" << GRUB_EOF
set timeout=5
set default=0

menuentry "OpenBMC BMC Flasher -- Auto-flash and reboot" {
    linux  /boot/vmlinuz $KCMDLINE
    initrd /boot/initramfs.gz
}

menuentry "OpenBMC BMC Flasher -- Shell only (no auto-flash)" {
    linux  /boot/vmlinuz $KCMDLINE init=/bin/sh
    initrd /boot/initramfs.gz
}
GRUB_EOF

    # isolinux (legacy BIOS) config
    cat > "$WORK/iso/isolinux/isolinux.cfg" << ISOLINUX_EOF
DEFAULT flash
PROMPT 1
TIMEOUT 50

LABEL flash
  MENU LABEL OpenBMC BMC Flasher -- Auto-flash and reboot
  KERNEL /boot/vmlinuz
  APPEND initrd=/boot/initramfs.gz $KCMDLINE

LABEL shell
  MENU LABEL Shell only (no auto-flash)
  KERNEL /boot/vmlinuz
  APPEND initrd=/boot/initramfs.gz $KCMDLINE init=/bin/sh
ISOLINUX_EOF

    # Kernel
    cp "$KERNEL" "$WORK/iso/boot/vmlinuz"

    # isolinux files
    cp "$ISOLINUX_DIR/isolinux.bin" "$WORK/iso/isolinux/"
    cp "$ISOLINUX_DIR/ldlinux.c32"  "$WORK/iso/isolinux/"

    # GRUB EFI standalone binary (drives UEFI boot path)
    echo "[*] Building GRUB EFI standalone binary..."
    grub-mkstandalone \
        --format=x86_64-efi \
        --output="$WORK/iso/EFI/BOOT/BOOTX64.EFI" \
        --modules="part_gpt part_msdos fat iso9660 normal boot linux configfile loopback chain efifwsetup efi_gop efi_uga ls search search_label search_fs_uuid search_fs_file gfxterm gfxterm_background gfxterm_menu test all_video loadenv ext2" \
        "boot/grub/grub.cfg=$WORK/iso/boot/grub/grub.cfg" \
        >/dev/null

    # EFI System Partition image (FAT) — referenced by El Torito alt-boot
    echo "[*] Building EFI System Partition image..."
    EFI_IMG="$WORK/iso/boot/efiboot.img"
    truncate -s 8M "$EFI_IMG"
    mkfs.vfat -n EFIBOOT "$EFI_IMG" >/dev/null
    mmd    -i "$EFI_IMG" ::/EFI ::/EFI/BOOT
    mcopy  -i "$EFI_IMG" "$WORK/iso/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
fi

# =============================================================================
# PHASE 2: Add socflash binary if provided (renamed for the EyerNi wrapper)
# =============================================================================

if [ -n "$SOCFLASH" ]; then
    if [ -f "$SOCFLASH" ] && [ -x "$SOCFLASH" ]; then
        SOCFLASH_BIN="$SOCFLASH"
    elif [ -f "$SOCFLASH/socflash_x64" ] && [ -x "$SOCFLASH/socflash_x64" ]; then
        SOCFLASH_BIN="$SOCFLASH/socflash_x64"
    elif [ -f "/usr/local/bin/socflash_x64" ] && [ -x "/usr/local/bin/socflash_x64" ]; then
        SOCFLASH_BIN="/usr/local/bin/socflash_x64"
    else
        echo "ERROR: socflash_x64 not found at: $SOCFLASH"
        exit 1
    fi

    if ! file "$SOCFLASH_BIN" | grep -q "x86-64"; then
        echo "ERROR: $SOCFLASH_BIN is not an x86-64 ELF binary"
        exit 1
    fi

    echo "[*] Embedding socflash: $SOCFLASH_BIN"
    # EyerNi update_bmc invokes ./fake_socflash_x86_64 — drop the real binary
    # under that name so the wrapper works unmodified.
    cp "$SOCFLASH_BIN" "$WORK/initramfs/fake_socflash_x86_64"
    chmod +x "$WORK/initramfs/fake_socflash_x86_64"
fi

# =============================================================================
# PHASE 3: Pack initramfs and emit hybrid BIOS+UEFI ISO
# =============================================================================

echo "[*] Packing initramfs..."
( cd "$WORK/initramfs" && find . | cpio -H newc -o 2>/dev/null | gzip -9 ) \
    > "$WORK/iso/boot/initramfs.gz"
echo "    initramfs.gz: $(du -sh "$WORK/iso/boot/initramfs.gz" | cut -f1)"

# Refresh ESP image so the embedded grub.cfg/EFI binary stay in sync
mcopy -o -i "$WORK/iso/boot/efiboot.img" "$WORK/iso/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI

echo "[*] Building hybrid BIOS+UEFI ISO..."
mkdir -p "$(dirname "$ISO_OUT")"
xorriso -as mkisofs \
    -R -J -V "OpenBMC-BMC-Flasher" \
    -iso-level 3 \
    -b isolinux/isolinux.bin \
    -c isolinux/boot.cat \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -eltorito-alt-boot \
    -e boot/efiboot.img -no-emul-boot \
    -isohybrid-gpt-basdat \
    "$WORK/iso" \
    -o "$ISO_OUT" 2>&1 | tail -5

echo ""
echo "[OK] ISO created: $ISO_OUT  ($(du -sh "$ISO_OUT" | cut -f1))"
echo ""

if [ -x "$WORK/initramfs/fake_socflash_x86_64" ]; then
    echo "[OK] socflash embedded -- auto-flash enabled (chip=${CHIP}, mode=${MODE})."
else
    echo "[!] socflash NOT embedded -- ISO will drop to shell on boot."
    echo "    Rebuild with: $0 /path/to/socflash_x64"
fi

echo ""
echo "Next steps:"
echo "  1. Upload $ISO_OUT to MegaRAC virtual media"
echo "  2. Boot the X570D4I-2T host from the virtual CD (UEFI or BIOS)"
echo "  3. update_bmc auto-flashes, then reboots after 15s (eject media first)"
echo ""

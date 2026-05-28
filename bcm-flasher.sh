#!/bin/bash
# Rebuild OpenBMC BMC Flasher ISO with socflash_x64
#
# This script creates a bootable Linux live ISO that can flash the X570D4I-2T's
# BMC SPI NOR via the AST2500's P2A (PCIe-to-AHB) bridge using socflash.
#
# The ISO includes:
# - Arch Linux kernel
# - Busybox-based minimal init environment
# - 64 MB OpenBMC image (obmc-phosphor-image-x570d4i2t.static.mtd)
# - socflash_x64 binary (if provided)
#
# Usage:
#   ./openbmc-flasher-rebuild.sh [--build-iso] [/path/to/socflash_x64]
#
# Examples:
#   ./openbmc-flasher-rebuild.sh --build-iso  # Build the ISO from scratch
#   ./openbmc-flasher-rebuild.sh /usr/local/bin/socflash_x64  # Update existing ISO with socflash
#   ./openbmc-flasher-rebuild.sh ~/Downloads/socflash_x64     # Use socflash from Downloads

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="/tmp/openbmc-flasher"
ISO_OUT="${REPO_ROOT}/.github/x570d4i2t-port/openbmc-flasher.iso"
FIRMWARE="${REPO_ROOT}/build/x570d4i2t/tmp/deploy/images/x570d4i2t/obmc-phosphor-image-x570d4i2t.static.mtd"

BUILD_ISO=false
SOCFLASH=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-iso)
            BUILD_ISO=true
            shift
            ;;
        *)
            SOCFLASH="$1"
            shift
            ;;
    esac
done

# =============================================================================
# PHASE 1: Build the ISO from scratch (if requested or if work dir missing)
# =============================================================================

if [ "$BUILD_ISO" = true ] || [ ! -d "$WORK/initramfs" ]; then
    echo "[*] Building ISO from scratch..."
    echo ""

    # Verify firmware image exists
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

    # Verify required tools
    for tool in xorriso cpio gzip; do
        if ! command -v "$tool" &>/dev/null; then
            echo "ERROR: Required tool not found: $tool"
            echo "Install with: sudo pacman -S xorriso"
            exit 1
        fi
    done

    # Verify busybox
    if ! command -v busybox &>/dev/null; then
        echo "ERROR: busybox not installed"
        echo "Install with: sudo pacman -S busybox"
        exit 1
    fi

    if ! file $(which busybox) | grep -q "statically linked"; then
        echo "ERROR: busybox must be statically linked"
        exit 1
    fi

    # Clean work directory
    rm -rf "$WORK"
    mkdir -p "$WORK"/{initramfs/{bin,sbin,dev,proc,sys,tmp,etc},iso/{boot,isolinux}}

    # Copy busybox and create applet symlinks
    echo "[*] Setting up busybox..."
    cp $(which busybox) "$WORK/initramfs/bin/busybox"
    for cmd in sh ash echo cat mount ls mkdir sleep; do
        ln -sf busybox "$WORK/initramfs/bin/$cmd"
    done
    ln -sf ../bin/busybox "$WORK/initramfs/sbin/reboot"
    ln -sf ../bin/busybox "$WORK/initramfs/sbin/poweroff"

    # Copy firmware
    echo "[*] Copying 64 MB firmware image..."
    cp "$FIRMWARE" "$WORK/initramfs/firmware.img"

    # Create init script
    echo "[*] Creating init script..."
    cat > "$WORK/initramfs/init" << 'INIT_EOF'
#!/bin/sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev 2>/dev/null || /bin/busybox mdev -s

/bin/busybox clear
echo "==========================================="
echo "  OpenBMC BMC Flasher — ASRock X570D4I-2T"
echo "==========================================="
echo ""

if [ -x /socflash_x64 ]; then
    echo "[*] socflash_x64 found. Flashing BMC SPI..."
    echo "[!] Do NOT power off. This takes ~2-3 minutes."
    echo ""
    if /socflash_x64 cs=1 w /firmware.img; then
        echo ""
        echo "[OK] Flash succeeded. Rebooting in 15s — eject virtual media first."
        sleep 15
        echo b > /proc/sysrq-trigger
    else
        echo ""
        echo "[FAIL] socflash returned error. Dropping to shell."
        echo "       Try: /socflash_x64 cf=1 w /firmware.img  (alt chip-select)"
        exec /bin/sh
    fi
else
    echo "[!] /socflash_x64 not present in this ISO."
    echo ""
    echo "    To rebuild with socflash, place socflash_x64 at:"
    echo "      /tmp/openbmc-flasher/initramfs/socflash_x64"
    echo "    then run the rebuild script."
    echo ""
    echo "    Dropping to shell. Firmware image is at /firmware.img (64 MB)."
    exec /bin/sh
fi
INIT_EOF
    chmod +x "$WORK/initramfs/init"

    # Create GRUB config (for reference, not used in isolinux boot)
    cat > "$WORK/iso/boot/grub/grub.cfg" << 'GRUB_EOF'
set timeout=10
set default=0

menuentry "OpenBMC BMC Flasher — Auto-flash and reboot" {
    linux  /boot/vmlinuz \
           console=tty0 console=ttyS0,115200n8 \
           iomem=relaxed amd_iommu=off intel_iommu=off \
           sysrq_always_enabled=1 quiet
    initrd /boot/initramfs.gz
}

menuentry "OpenBMC BMC Flasher — Shell only (no auto-flash)" {
    linux  /boot/vmlinuz \
           console=tty0 console=ttyS0,115200n8 \
           iomem=relaxed amd_iommu=off intel_iommu=off \
           sysrq_always_enabled=1 init=/bin/sh quiet
    initrd /boot/initramfs.gz
}
GRUB_EOF

    # Create isolinux config
    cat > "$WORK/iso/isolinux/isolinux.cfg" << 'ISOLINUX_EOF'
DEFAULT flash
PROMPT 1
TIMEOUT 100

LABEL flash
  MENU LABEL OpenBMC BMC Flasher -- Auto-flash and reboot
  KERNEL /boot/vmlinuz
  APPEND initrd=/boot/initramfs.gz \
         console=tty0 console=ttyS0,115200n8 \
         iomem=relaxed amd_iommu=off intel_iommu=off \
         sysrq_always_enabled=1 quiet

LABEL shell
  MENU LABEL Shell only (no auto-flash)
  KERNEL /boot/vmlinuz
  APPEND initrd=/boot/initramfs.gz \
         console=tty0 console=ttyS0,115200n8 \
         iomem=relaxed amd_iommu=off intel_iommu=off \
         sysrq_always_enabled=1 init=/bin/sh quiet
ISOLINUX_EOF

    # Pack initramfs
    echo "[*] Packing initramfs (~65 MB, this takes 1-2 minutes)..."
    cd "$WORK/initramfs"
    find . | cpio -H newc -o 2>/dev/null | gzip -9 > "$WORK/iso/boot/initramfs.gz"
    echo "    initramfs.gz: $(du -sh $WORK/iso/boot/initramfs.gz | cut -f1)"
    cd - > /dev/null

    # Copy kernel
    echo "[*] Copying kernel..."
    cp /boot/vmlinuz-linux "$WORK/iso/boot/vmlinuz"
    echo "    kernel: $(du -sh $WORK/iso/boot/vmlinuz | cut -f1)"

    # Set up isolinux boot
    echo "[*] Setting up isolinux..."
    cp /usr/lib/syslinux/bios/isolinux.bin "$WORK/iso/isolinux/"
    cp /usr/lib/syslinux/bios/ldlinux.c32  "$WORK/iso/isolinux/"
fi

# =============================================================================
# PHASE 2: Add socflash_x64 if provided
# =============================================================================

if [ -n "$SOCFLASH" ]; then
    # Resolve socflash path
    if [ -f "$SOCFLASH" ] && [ -x "$SOCFLASH" ]; then
        SOCFLASH_BIN="$SOCFLASH"
    elif [ -f "$SOCFLASH/socflash_x64" ] && [ -x "$SOCFLASH/socflash_x64" ]; then
        SOCFLASH_BIN="$SOCFLASH/socflash_x64"
    elif [ -f "/usr/local/bin/socflash_x64" ] && [ -x "/usr/local/bin/socflash_x64" ]; then
        SOCFLASH_BIN="/usr/local/bin/socflash_x64"
    else
        echo "ERROR: socflash_x64 not found at: $SOCFLASH"
        echo ""
        echo "Valid paths:"
        echo "  - /path/to/socflash_x64 (direct path to binary)"
        echo "  - /path/to/dir/ (directory containing socflash_x64)"
        echo "  - /usr/local/bin/socflash_x64 (system location)"
        exit 1
    fi

    # Verify it's x86_64 ELF
    if ! file "$SOCFLASH_BIN" | grep -q "x86-64"; then
        echo "ERROR: $SOCFLASH_BIN is not an x86-64 binary"
        exit 1
    fi

    echo "[*] Found socflash_x64: $SOCFLASH_BIN"
    echo "    $(file $SOCFLASH_BIN | cut -d: -f2-)"
    echo ""
    echo "[*] Copying socflash_x64 to initramfs..."
    cp "$SOCFLASH_BIN" "$WORK/initramfs/socflash_x64"
    chmod +x "$WORK/initramfs/socflash_x64"

    # Repack initramfs
    echo "[*] Repacking initramfs with socflash (1-2 minutes)..."
    cd "$WORK/initramfs"
    find . | cpio -H newc -o 2>/dev/null | gzip -9 > "$WORK/iso/boot/initramfs.gz"
    echo "    initramfs.gz: $(du -sh $WORK/iso/boot/initramfs.gz | cut -f1)"
    cd - > /dev/null
fi

# =============================================================================
# PHASE 3: Build ISO
# =============================================================================

echo "[*] Building ISO with xorriso..."
mkdir -p "$(dirname "$ISO_OUT")"
xorriso -as mkisofs \
    -R -J -V "OpenBMC-BMC-Flasher" \
    -b isolinux/isolinux.bin \
    -c isolinux/boot.cat \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    "$WORK/iso" \
    -o "$ISO_OUT" 2>&1 | tail -3

echo ""
echo "[OK] ISO created successfully!"
echo ""
echo "     Path: $ISO_OUT"
echo "     Size: $(du -sh $ISO_OUT | cut -f1)"
echo ""

if [ -x "$WORK/initramfs/socflash_x64" ]; then
    echo "[OK] socflash_x64 is included — auto-flash enabled."
else
    echo "[!] socflash_x64 NOT included — ISO will boot to shell."
    echo "    To rebuild with socflash:"
    echo "      $0 /path/to/socflash_x64"
fi

echo ""
echo "Next steps:"
echo "  1. Upload $ISO_OUT to MegaRAC virtual media"
echo "  2. Boot the X570D4I-2T host from the virtual CD"
echo "  3. The BMC will auto-flash (or drop to shell if socflash is missing)"
echo ""

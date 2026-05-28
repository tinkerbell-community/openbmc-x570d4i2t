# OpenBMC BMC Flasher for ASRock X570D4I-2T

Bootable Linux live ISO for flashing the OpenBMC image to the BMC's SPI NOR flash via the AST2500's P2A (PCIe-to-AHB) bridge.

## Overview

The X570D4I-2T's BMC firmware (stock AMI MegaRAC) uses signature verification. Direct web UI flashing is not possible. This ISO allows flashing over the host PCIe/LPC bus using ASPEED's `socflash` utility, bypassing the firmware's checks entirely.

**Requirements:**
- `socflash_x64` binary (ASPEED proprietary tool)
- MegaRAC virtual media support on the host
- Host running Linux from the ISO

## Quick Start

### 1. Build the ISO (first time only)

```bash
cd .github/x570d4i2t-port
./openbmc-flasher-rebuild.sh --build-iso
```

This creates `openbmc-flasher.iso` (~47 MB) containing:
- Arch Linux kernel
- Busybox init environment
- 64 MB OpenBMC BMC image
- Auto-flash init script (runs socflash on boot, if available)

### 2. Add socflash_x64

Once you obtain the `socflash_x64` binary from ASPEED/ASRock:

```bash
./openbmc-flasher-rebuild.sh /path/to/socflash_x64
```

This repacks the ISO with socflash included. When you boot, the ISO will automatically flash the BMC and reboot.

### 3. Flash via MegaRAC virtual media

1. **Upload ISO to MegaRAC:**
   - Open MegaRAC web UI (https://10.0.80.1)
   - Settings → Virtual Media → Select CD → Upload `openbmc-flasher.iso`

2. **Boot host from virtual media:**
   - Power cycle the host (system, not just BMC)
   - Press F11 during boot to select boot device
   - Choose the virtual CD (MegaRAC virtual media)

3. **Watch the flash:**
   - The ISO boots to a minimal Linux shell
   - If socflash is present: auto-flashes and reboots (~2-3 minutes)
   - If socflash is missing: drops to a shell for manual intervention

## What's in the ISO

```
initramfs (30 MB compressed, ~65 MB uncompressed)
  ├── init (shell script — mounts, runs socflash, handles errors)
  ├── bin/busybox (static, all shells/tools)
  ├── firmware.img (64 MB — your OpenBMC build)
  └── socflash_x64 (optional — added via rebuild)

kernel: vmlinuz-linux (Arch Linux, 17 MB)

boot: isolinux + GRUB (BIOS-only bootable)
```

## Boot Parameters

The ISO boots with:

```
console=tty0 console=ttyS0,115200n8    # dual console (video + serial)
iomem=relaxed                           # Allow /dev/mem access (needed by socflash)
amd_iommu=off intel_iommu=off          # Disable IOMMU (P2A bridge needs direct PCIe)
sysrq_always_enabled=1                  # Allow SysRq for reboot (echo b > /proc/sysrq-trigger)
quiet                                   # Reduce boot noise
```

These are critical for socflash to access the BMC's SPI controller.

## Obtaining socflash_x64

Sources:
- **ASPEED SDK** — aspeedtech.com (free registration, partner access)
- **ASRock Rack FTP** — some BMC update packages include it
- **Gemini AI reference** — (user-provided source)

The binary must be:
- x86-64 ELF executable
- Named `socflash_x64` (the script looks for this name)

## Troubleshooting

### ISO boots but socflash not found

```
[!] /socflash_x64 not present in this ISO.
    Dropping to shell. Firmware image is at /firmware.img (64 MB).
```

**Solution:** Rebuild the ISO with socflash:
```bash
./openbmc-flasher-rebuild.sh /path/to/socflash_x64
```

### socflash fails with "flash access error"

This typically means the host OS or BIOS is blocking PCIe access to the BMC's address space. Possible causes:

1. **IOMMU enabled in BIOS** — disable in BIOS settings (UEFI Setup → I/O → IOMMU)
2. **SMM protection** — some Ryzen BIOS versions lock down PCIe → Memory access; check BIOS security settings
3. **Stock firmware restrictions** — try booting from an external Linux distribution first to test if the issue is board-specific

### Flash succeeded but host doesn't boot

The OpenBMC image replaces the entire BMC firmware. If something went wrong:

1. **Fallback to stock BMC** — use external SPI programmer (SOIC8 clip + flashrom) to restore from backup
2. **Check MAC addresses** — if eth0 won't DHCP, the MAC may have been overwritten (use `ip link show` to check)

## Dual-Image and Rollback

The AST2500 supports dual-image flashing (two firmware slots). After this initial flash:

- **Future updates** — use OpenBMC's `phosphor-bmc-code-mgmt` service (in-band, over network)
- **Rollback** — if you have dual slots, the old MegaRAC persists; the BMC can fall back automatically

For now, **back up your stock MegaRAC image** in case you need to restore:

```bash
# If flashing fails mid-way:
flashrom -p ch341a_spi -w stock-x570d4i2t-bmc.bin
```

This is why we recommend having an external SPI programmer as insurance.

## Advanced: Manual Flash from Shell

If the ISO boots but socflash doesn't run automatically, you can flash manually:

```bash
# Once at the shell:
/socflash_x64 cs=1 w /firmware.img

# For REFRESH mode (preserve config):
/socflash_x64 cs=1 w /firmware.img skip=0x0010000 offset=0x0010000

# Check if it worked:
echo $?  # Should return 0 on success
```

Replace `cs=1` with `cs=0` if socflash reports "chip not found" (alternate chip select).

## External SPI Programmer (Fallback)

If P2A access is blocked and socflash fails, use an external programmer:

```bash
# Backup first
flashrom -p ch341a_spi -r stock-backup.bin

# Flash OpenBMC
flashrom -p ch341a_spi -w openbmc-flasher.iso:0:0x04000000

# Verify
flashrom -p ch341a_spi -v openbmc-flasher.iso:0:0x04000000
```

See `05-flash-and-verify.md` for full details.

## References

- `00-overview.md` — Project overview
- `03-device-tree.md` — DTS and hardware mapping
- `05-flash-and-verify.md` — Verification and rollback procedures
- `openbmc-flasher-rebuild.sh` — This script

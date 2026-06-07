#!/usr/bin/env bash
#
# x570-recover.sh — reproducible remote BIOS-recovery toolchain for the
# ASRock Rack X570D4I-2T (AST2500 BMC running OpenBMC).
#
# WHY THIS EXISTS
#   The host BIOS lives on a separate SPI chip the BMC cannot reach (no SPI
#   mux; AMD PSP-locked). The only way to flash it or change BIOS settings
#   remotely is to boot a tiny Linux ON THE HOST via the BMC's USB-mass-storage
#   gadget, and run AMI's userspace tools (afulnx / SCELNX) which talk to the
#   BIOS SMM handler through the amifldrv kernel module — OR edit the AMI
#   "Setup" UEFI variables directly via efivarfs (the host boots UEFI).
#
#   This script rebuilds that whole chain from scratch so the method is never
#   lost again:
#     deps    : fetch afulnx_64 + SCELNX_64 (GitHub), build amifldrv.ko for the
#               running kernel, extract the official 2.50 ROM, cache libs.
#     image A : assemble initramfs (busybox + AFU tools + module + chosen
#               "action" init) -> UKI (systemd-stub) -> GPT/ESP disk.
#     run   A : image A, then stage to the BMC USB gadget, boot the host once
#               from it (UEFI), wait for self-poweroff, pull the result log.
#
# ACTIONS (the job the booted Linux performs, then powers off):
#   pci-route   dump host PCI VGA routing (why the AST2500 VGA is on/off)
#   dump        SCELNX /o — export ALL BIOS setup questions to setup.txt
#   efivar-dump dump every UEFI variable's name+size+first bytes
#   efivar-set  set NAME byte OFFSET=VALUE in a UEFI var (args: VAR OFF VAL ...)
#   scelnx-set  SCELNX /i an edited setup script placed at cache/setup_edit.txt
#   flash       afulnx /P /B /N  (reflash official 2.50; add CLRCFG=1 to clear cfg)
#   shell       leave host up with a serial shell (SOL) for manual work
#
# Requires (build host, Arch): bash, curl, gcc, make, kernel headers for the
#   RUNNING kernel, busybox, mtools, dosfstools, parted, zstd, binutils(objcopy),
#   systemd (linuxx64.efi.stub). BMC reachable at $BMC_IP with $BMC_PASS.
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Config (override via env)
# ---------------------------------------------------------------------------
BMC_IP="${BMC_IP:-10.0.80.1}"
BMC_USER="${BMC_USER:-root}"
BMC_PASS="${BMC_PASS:-0penBmc}"
KVER="${KVER:-$(uname -r)}"
KERNEL_IMG="${KERNEL_IMG:-/boot/vmlinuz-linux}"
EFISTUB="${EFISTUB:-/usr/lib/systemd/boot/efi/linuxx64.efi.stub}"
ROM_ZIP="${ROM_ZIP:-/home/appkins/Downloads/X570D4I-2T(2.50)ROM.zip}"
ROM_NAME="${ROM_NAME:-X574I2T2.50}"
UDC="${UDC:-1e6a0000.usb-vhub:p1}"          # BMC USB gadget port to the host
GADGET="${GADGET:-mass-storage}"
# Kernel console pinned to the legacy 8250 at I/O 0x3f8 (the BMC vuart),
# iomem=relaxed so flashrom/AFU can map chipset MMIO.
CMDLINE="${CMDLINE:-console=tty0 console=uart8250,io,0x3f8,115200n8 earlyprintk=uart8250,io,0x3f8,115200 iomem=relaxed rdinit=/init rw panic=30}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="$HERE/cache"
WORK="$(mktemp -d /tmp/x570-recover.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
SSH=(sshpass -p "$BMC_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=12 "$BMC_USER@$BMC_IP")
SCP=(sshpass -p "$BMC_PASS" scp -o StrictHostKeyChecking=no)

AFULNX_URL="https://raw.githubusercontent.com/doublechiang/tools/master/AfuLnx64/afulnx_64"
SCELNX_URL="https://raw.githubusercontent.com/doublechiang/tools/master/SCELNX64/SCELNX_64"
AMIDRV_BASE="https://raw.githubusercontent.com/jim3ma/afulnx64/master/afulnx64/driver"

log(){ printf '\033[1;36m[x570]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[x570] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# deps: download tools, build amifldrv for $KVER, extract ROM
# ---------------------------------------------------------------------------
cmd_deps(){
  mkdir -p "$CACHE"
  [ -f "$CACHE/afulnx_64" ] || { log "fetch afulnx_64"; curl -fsSL "$AFULNX_URL" -o "$CACHE/afulnx_64"; }
  [ -f "$CACHE/SCELNX_64" ] || { log "fetch SCELNX_64"; curl -fsSL "$SCELNX_URL" -o "$CACHE/SCELNX_64"; }
  file "$CACHE/afulnx_64" | grep -q ELF || die "afulnx_64 not an ELF (download failed)"
  file "$CACHE/SCELNX_64" | grep -q ELF || die "SCELNX_64 not an ELF (download failed)"
  chmod +x "$CACHE/afulnx_64" "$CACHE/SCELNX_64"

  if [ ! -f "$CACHE/amifldrv_mod.ko" ] || [ "$(cat "$CACHE/amifldrv_kver" 2>/dev/null)" != "$KVER" ]; then
    log "build amifldrv.ko for $KVER"
    local d="$WORK/amidrv"; mkdir -p "$d"
    local f; for f in Makefile amifldrv.c amifldrv.h amiwrap.c amiwrap.h; do
      curl -fsSL "$AMIDRV_BASE/$f" -o "$d/$f"; done
    # --- modern-kernel patches (validated on 7.0.10-arch1) ---
    sed -i 's/vma->vm_flags |= VM_LOCKED;/vm_flags_set(vma, VM_LOCKED);/' "$d/amifldrv.c"
    sed -i 's@#define mem_map_reserve(p) set_bit(PG_reserved, &((p)->flags))@#define mem_map_reserve(p) SetPageReserved(p)@'   "$d/amiwrap.c"
    sed -i 's@#define mem_map_unreserve(p) clear_bit(PG_reserved, &((p)->flags))@#define mem_map_unreserve(p) ClearPageReserved(p)@' "$d/amiwrap.c"
    sed -i '1i #define HAVE_UNLOCKED_IOCTL 1' "$d/amiwrap.c"
    sed -i 's/^  owner:   THIS_MODULE,/  .owner =   THIS_MODULE,/' "$d/amiwrap.c"
    sed -i 's/^  open:    wrap_open,/  .open =    wrap_open,/' "$d/amiwrap.c"
    sed -i 's/^  release: wrap_release,/  .release = wrap_release,/' "$d/amiwrap.c"
    sed -i 's/^  unlocked_ioctl:   wrap_unlocked_ioctl,/  .unlocked_ioctl =   wrap_unlocked_ioctl,/' "$d/amiwrap.c"
    sed -i 's/^  mmap:    wrap_mmap,/  .mmap =    wrap_mmap,/' "$d/amiwrap.c"
    sed -i 's/MODULE_LICENSE("Proprietary")/MODULE_LICENSE("GPL")/' "$d/amiwrap.c" "$d/amifldrv.c"
    sed -i 's/SUBDIRS=$(PWD)/M=$(PWD)/' "$d/Makefile"
    sed -i 's/^EXTRA_CFLAGS :=.*/ccflags-y := -Wall -O2 -fno-strict-aliasing -DHAVE_UNLOCKED_IOCTL -Wno-error=designated-init/' "$d/Makefile"
    make -C "/lib/modules/$KVER/build" M="$d" modules >/dev/null
    # the upstream Makefile renames .ko->.o; grab whichever exists
    cp "$d/amifldrv_mod.ko" "$CACHE/amifldrv_mod.ko" 2>/dev/null || cp "$d/amifldrv_mod.o" "$CACHE/amifldrv_mod.ko"
    echo "$KVER" > "$CACHE/amifldrv_kver"
  fi

  if [ ! -f "$CACHE/$ROM_NAME" ]; then
    log "extract ROM $ROM_NAME"; unzip -o "$ROM_ZIP" -d "$CACHE" >/dev/null
  fi
  [ -f "$CACHE/$ROM_NAME" ] || die "ROM $ROM_NAME not found after unzip"
  log "deps ready in $CACHE"
}

# ---------------------------------------------------------------------------
# init action templates -> echo a /init shell script to stdout
# Common header mounts fs, loads storage modules, mounts the boot disk rw at
# /mnt (writable log target), and ALWAYS powers off at the end (watchdog).
# ---------------------------------------------------------------------------
emit_init_header(){ cat <<'H'
#!/bin/busybox sh
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox mount -t proc proc /proc 2>/dev/null
/bin/busybox mount -t sysfs sys /sys 2>/dev/null
exec >/dev/console 2>&1 </dev/console
/bin/busybox --install -s /bin
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export LD_LIBRARY_PATH=/usr/lib:/lib64:/lib
insmod /lib/modules/fat.ko 2>/dev/null; insmod /lib/modules/vfat.ko 2>/dev/null; insmod /lib/modules/usb-storage.ko 2>/dev/null
mkdir -p /mnt; M=0
for t in 1 2 3 4 5 6 7 8; do for p in /dev/sda1 /dev/sda /dev/sdb1 /dev/sdb; do mount -t vfat -o rw "$p" /mnt 2>/dev/null && { M=1; break; }; done; [ "$M" = 1 ] && break; sleep 2; done
load_amidrv(){ insmod /afu/amifldrv_mod.ko 2>/dev/null || insmod /afu/amifldrv_mod.o 2>/dev/null; MAJ=$(awk "/amifldrv/{print \$1}" /proc/devices); [ -n "$MAJ" ] && mknod /dev/amifldrv c "$MAJ" 0; }
H
}
emit_init_footer(){ cat <<'F'
sync; sleep 2; umount /mnt 2>/dev/null; sync; sleep 2
poweroff -f
sleep 25; echo o > /proc/sysrq-trigger 2>/dev/null
F
}

emit_action(){
  local act="$1"; shift || true
  emit_init_header
  case "$act" in
    pci-route) cat <<'A'
LOG=/mnt/RESULT.log
rb(){ dd if="$1/config" bs=1 skip="$2" count="$3" 2>/dev/null | od -An -tx1 | tr -d ' \n'; }
{
echo "=== PCI VGA routing (mounted=$M) ==="
for d in /sys/bus/pci/devices/*; do
  v=$(cat $d/vendor); dev=$(cat $d/device); cls=$(cat $d/class); bv=$(cat $d/boot_vga 2>/dev/null)
  cmd=$(rb $d 4 2); hdr=$(rb $d 14 1)
  line="$(basename $d) ${v}:${dev} class=${cls} cmd=${cmd} hdr=${hdr} boot_vga=${bv:-_}"
  case "$v$dev" in 0x1a03*|0x10de*) lo=$((0x${cmd:0:2})); echo "$line [IO=$((lo&1)) MEM=$(((lo>>1)&1)) BM=$(((lo>>2)&1)) VGAsnoop=$(((lo>>5)&1))]";; *) echo "$line";; esac
  [ "$hdr" = "01" ] && { bc=$(rb $d 62 2); echo "    BRIDGE $(basename $d) bridgectl=${bc} VGAen=$(( ($((0x${bc:0:2}))>>3)&1 ))"; }
done
echo "=== VGA-class (0300) devices ==="; for d in /sys/bus/pci/devices/*; do case "$(cat $d/class)" in 0x0300*) echo "$(basename $d) $(cat $d/vendor):$(cat $d/device)";; esac; done
echo "=== vgaarb ==="; cat /sys/kernel/debug/vgaarb 2>/dev/null
} > $LOG 2>&1
A
;;
    dump) cat <<'A'
load_amidrv
cd /afu
echo "=== SCELNX /o dump ===" > /mnt/RESULT.log
./SCELNX_64 /o /s /mnt/setup.txt /q >> /mnt/RESULT.log 2>&1 </dev/null
echo "rc=$? size=$(wc -c < /mnt/setup.txt 2>/dev/null)" >> /mnt/RESULT.log
A
;;
    efivar-dump) cat <<'A'
mount -t efivarfs none /sys/firmware/efi/efivars 2>/dev/null
{ echo "=== efivars ($(ls /sys/firmware/efi/efivars 2>/dev/null|wc -l)) name size first16 ==="
  for v in /sys/firmware/efi/efivars/*; do [ -f "$v" ] || continue; printf "%-55s %5s  " "$(basename $v)" "$(wc -c < "$v")"; dd if="$v" bs=1 skip=4 count=16 2>/dev/null | od -An -tx1 | tr -d '\n'; echo; done
} > /mnt/RESULT.log 2>&1
A
;;
    efivar-set) # args: VAR OFF VAL [OFF VAL ...]
      local var="$1"; shift
      printf 'mount -t efivarfs none /sys/firmware/efi/efivars 2>/dev/null\n'
      printf 'SV=/sys/firmware/efi/efivars/%s\n' "$var"
      printf '{ echo "=== efivar-set %s ==="; cp "$SV" /tmp/v.bin 2>&1; echo "size=$(wc -c < /tmp/v.bin)"\n' "$var"
      while [ $# -ge 2 ]; do
        local off="$1" val="$2"; shift 2
        printf '  echo "before off=%s : $(dd if=/tmp/v.bin bs=1 skip=$((4+%s)) count=1 2>/dev/null|od -An -tx1)"\n' "$off" "$off"
        printf '  printf "\\\\x%s" | dd of=/tmp/v.bin bs=1 seek=$((4+%s)) count=1 conv=notrunc 2>/dev/null\n' "$val" "$off"
      done
      printf '  chattr -i "$SV" 2>/dev/null; dd if=/tmp/v.bin of="$SV" bs=8192 2>&1 && echo WROTE\n'
      printf '  echo "readback: $(od -A d -t x1 -N 64 "$SV" 2>/dev/null | head -4)"\n'
      printf '} > /mnt/RESULT.log 2>&1\n'
;;
    scelnx-set) cat <<'A'
load_amidrv
cd /afu
echo "=== SCELNX /i /mnt/setup_edit.txt ===" > /mnt/RESULT.log
./SCELNX_64 /i /s /mnt/setup_edit.txt /q >> /mnt/RESULT.log 2>&1 </dev/null
echo "rc=$?" >> /mnt/RESULT.log
A
;;
    flash) cat <<A
load_amidrv
cd /afu
echo "=== backup current BIOS to USB (bios-backup.rom) ===" > /mnt/RESULT.log
./afulnx_64 /mnt/bios-backup.rom /O >> /mnt/RESULT.log 2>&1 </dev/null
echo "backup rc=\$? size=\$(wc -c < /mnt/bios-backup.rom 2>/dev/null)" >> /mnt/RESULT.log
sync
echo "=== afulnx flash $ROM_NAME ${CLRCFG:+/CLRCFG} ===" >> /mnt/RESULT.log
./afulnx_64 /$ROM_NAME /P /B /N ${CLRCFG:+/CLRCFG} >> /mnt/RESULT.log 2>&1 </dev/null
echo "rc=\$?" >> /mnt/RESULT.log
A
;;
    flashrom) cat <<A
echo "=== flashrom -p internal -w /$ROM_NAME (direct SPI; bypasses Secure Flash) ==="
/afu/flashrom -p internal -w /$ROM_NAME 2>&1 | tee /mnt/RESULT.log
echo "FLASHROM rc=\$?" | tee -a /mnt/RESULT.log
sync
A
;;
    shell) cat <<'A'
load_amidrv
echo "=== interactive shell on ttyS0 (SOL). amifldrv loaded. tools in /afu ==="
exec /bin/busybox cttyhack /bin/sh
A
      return 0 ;;   # no footer/poweroff for shell
    *) die "unknown action: $act" ;;
  esac
  emit_init_footer
}

# ---------------------------------------------------------------------------
# image: initramfs -> UKI -> GPT/ESP disk  ($WORK/diskboot.img)
# ---------------------------------------------------------------------------
cmd_image(){
  local act="${1:-pci-route}"; shift || true
  cmd_deps
  local ir="$WORK/ir"; mkdir -p "$ir"/{bin,sbin,usr/bin,usr/lib,lib,lib64,afu,lib/modules,proc,sys,dev,tmp,mnt}
  cp /usr/bin/busybox "$ir/bin/busybox"
  local a; for a in sh mount umount cat ls echo sleep mkdir mknod dd od printf grep sed awk insmod head tail sync poweroff chattr cttyhack setsid tr wc cp tee; do ln -sf busybox "$ir/bin/$a"; done
  # AFU tools + module
  cp "$CACHE/afulnx_64" "$ir/afu/afulnx_64"; chmod +x "$ir/afu/afulnx_64"
  cp "$CACHE/SCELNX_64" "$ir/afu/SCELNX_64"; chmod +x "$ir/afu/SCELNX_64"
  cp "$CACHE/amifldrv_mod.ko" "$ir/afu/amifldrv_mod.ko"
  cp /usr/bin/flashrom "$ir/afu/flashrom"; chmod +x "$ir/afu/flashrom"
  # shared-lib closure for the AFU tools
  copydeps(){
    local libs l dst
    libs=$(ldd "$1" 2>/dev/null | awk '/=>/{print $3} /ld-linux/{print $1}' | grep -E '^/' | sort -u || true)
    for l in $libs; do
      [ -f "$l" ] || continue
      dst="$ir$l"; [ -f "$dst" ] && continue
      mkdir -p "$(dirname "$dst")"; cp "$l" "$dst"
      case "$l" in *ld-linux*) ;; *) copydeps "$l" ;; esac   # don't recurse the loader
    done
  }
  copydeps "$CACHE/afulnx_64"; copydeps "$CACHE/SCELNX_64"; copydeps /usr/bin/flashrom
  # storage modules (decompress .zst)
  local M="/lib/modules/$KVER"
  unzstd -qfc "$M/kernel/fs/fat/fat.ko.zst"               > "$ir/lib/modules/fat.ko"
  unzstd -qfc "$M/kernel/fs/fat/vfat.ko.zst"              > "$ir/lib/modules/vfat.ko"
  unzstd -qfc "$M/kernel/drivers/usb/storage/usb-storage.ko.zst" > "$ir/lib/modules/usb-storage.ko"
  # ROM only needed for flash action (keeps image small otherwise)
  case "$act" in flash|flashrom) cp "$CACHE/$ROM_NAME" "$ir/$ROM_NAME";; esac
  # init for the action
  emit_action "$act" "$@" > "$ir/init"; chmod +x "$ir/init"
  # pack
  ( cd "$ir" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9 ) > "$WORK/initramfs.cpio.gz"
  log "initramfs $(du -h "$WORK/initramfs.cpio.gz"|cut -f1) (action=$act)"
  # UKI via systemd-stub (VMAs above the stub's image base 0x14df90000)
  objcopy \
    --add-section .osrel=<(printf 'NAME=x570rec\nID=x570rec\n') --change-section-vma .osrel=0x14dfb0000 \
    --add-section .cmdline=<(printf '%s' "$CMDLINE")            --change-section-vma .cmdline=0x14dfc0000 \
    --add-section .linux="$KERNEL_IMG"                          --change-section-vma .linux=0x14ff90000 \
    --add-section .initrd="$WORK/initramfs.cpio.gz"             --change-section-vma .initrd=0x153f90000 \
    "$EFISTUB" "$WORK/BOOTX64.EFI"
  objdump -h "$WORK/BOOTX64.EFI" | grep -q .initrd || die "UKI build failed"
  # GPT/ESP disk
  local fat="$WORK/fat.img" disk="$WORK/diskboot.img"
  truncate -s 90M "$fat"; mkfs.fat -F32 -n X570REC "$fat" >/dev/null
  mmd -i "$fat" ::/EFI ::/EFI/BOOT
  mcopy -i "$fat" "$WORK/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
  # extra files some actions read from the disk
  [ -f "$CACHE/setup_edit.txt" ] && [ "$act" = "scelnx-set" ] && mcopy -i "$fat" "$CACHE/setup_edit.txt" ::/setup_edit.txt
  truncate -s 92M "$disk"
  parted -s "$disk" mklabel gpt
  parted -s "$disk" mkpart ESP fat32 1MiB 91MiB
  parted -s "$disk" set 1 esp on; parted -s "$disk" set 1 boot on
  dd if="$fat" of="$disk" bs=1M seek=1 conv=notrunc status=none
  cp "$disk" "$CACHE/diskboot.img"
  log "image ready: $CACHE/diskboot.img  md5=$(md5sum "$disk"|cut -d' ' -f1)"
}

# ---------------------------------------------------------------------------
# run: image -> stage to BMC gadget -> boot host once (UEFI) -> wait off -> pull
# ---------------------------------------------------------------------------
rf(){ curl -sk --max-time 12 -u "$BMC_USER:$BMC_PASS" "$@"; }
cmd_run(){
  local act="${1:-pci-route}"; shift || true
  cmd_image "$act" "$@"
  log "power off host + stage image"
  rf -X POST -H 'Content-Type: application/json' -d '{"ResetType":"ForceOff"}' \
     "https://$BMC_IP/redfish/v1/Systems/system/Actions/ComputerSystem.Reset" >/dev/null || true
  sleep 4
  "${SSH[@]}" "echo '' > /sys/kernel/config/usb_gadget/$GADGET/UDC 2>/dev/null; rm -f /tmp/diskboot.img" || true
  "${SCP[@]}" "$CACHE/diskboot.img" "$BMC_USER@$BMC_IP:/tmp/diskboot.img"
  log "bind gadget + settle"
  "${SSH[@]}" "bash -s" <<EOF
G=/sys/kernel/config/usb_gadget/$GADGET
if [ ! -d "\$G" ]; then cd /sys/kernel/config/usb_gadget/; mkdir $GADGET; cd $GADGET
  echo 0x0525>idVendor; echo 0xa4a5>idProduct; echo 0x0100>bcdDevice; echo 0x0200>bcdUSB
  mkdir strings/0x409; echo OpenBMC>strings/0x409/manufacturer; echo X570REC>strings/0x409/product; echo REC1>strings/0x409/serialnumber
  mkdir functions/mass_storage.usb0; echo 1>functions/mass_storage.usb0/lun.0/removable
  mkdir configs/c.1; mkdir configs/c.1/strings/0x409; echo R>configs/c.1/strings/0x409/configuration; echo 250>configs/c.1/MaxPower
  ln -s functions/mass_storage.usb0 configs/c.1/mass_storage.usb0
fi
echo '' > \$G/UDC 2>/dev/null; sleep 2
echo /tmp/diskboot.img > \$G/functions/mass_storage.usb0/lun.0/file
echo 0 > \$G/functions/mass_storage.usb0/lun.0/cdrom 2>/dev/null
echo $UDC > \$G/UDC; sleep 6
echo "gadget=\$(cat /sys/class/udc/$UDC/state)"
EOF
  log "set one-time USB/UEFI boot + power on"
  rf -X PATCH -H 'Content-Type: application/json' \
     -d '{"Boot":{"BootSourceOverrideEnabled":"Once","BootSourceOverrideTarget":"Usb","BootSourceOverrideMode":"UEFI"}}' \
     "https://$BMC_IP/redfish/v1/Systems/system" >/dev/null
  rf -X POST -H 'Content-Type: application/json' -d '{"ResetType":"On"}' \
     "https://$BMC_IP/redfish/v1/Systems/system/Actions/ComputerSystem.Reset" >/dev/null
  if [ "$act" = "shell" ]; then log "host booting to serial shell; connect: obmc-console-client on the BMC"; return 0; fi
  log "waiting for self-poweroff (job done)…"
  local seen=0 i ps
  for i in $(seq 1 30); do
    sleep 9
    ps=$("${SSH[@]}" "busctl get-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 xyz.openbmc_project.State.Host CurrentHostState 2>/dev/null | grep -o 'Running\|Off'" 2>/dev/null || true)
    [ "$ps" = Running ] && seen=1
    printf '  t+%ds host=%s\n' $((i*9)) "${ps:-?}"
    [ "$ps" = Off ] && [ "$seen" = 1 ] && break
  done
  log "pull result"
  "${SCP[@]}" "$BMC_USER@$BMC_IP:/tmp/diskboot.img" "$WORK/result.img" >/dev/null
  mcopy -n -i "$WORK/result.img@@1M" ::/RESULT.log "$CACHE/RESULT.log" 2>/dev/null \
    && { echo "================ RESULT.log ================"; tr -d '\000' < "$CACHE/RESULT.log"; } \
    || echo "(no RESULT.log — boot/mount may have failed; re-run)"
  # pull setup.txt for the dump action
  [ "$act" = dump ] && mcopy -n -i "$WORK/result.img@@1M" ::/setup.txt "$CACHE/setup.txt" 2>/dev/null && log "setup.txt -> $CACHE/setup.txt ($(wc -l < "$CACHE/setup.txt") lines)"
}

cmd_video(){ # poll BMC aspeed-video signal
  "${SSH[@]}" "grep -E 'Signal|Width|Height|FPS' /sys/kernel/debug/aspeed-video 2>/dev/null"
}

usage(){ sed -n '2,40p' "$0"; echo; echo "Usage: $0 {deps|image|run} <action> [args]   |   $0 video"; }
case "${1:-}" in
  deps)  cmd_deps ;;
  image) shift; cmd_image "$@" ;;
  run)   shift; cmd_run "$@" ;;
  video) cmd_video ;;
  ""|-h|--help) usage ;;
  *) die "unknown command: $1" ;;
esac

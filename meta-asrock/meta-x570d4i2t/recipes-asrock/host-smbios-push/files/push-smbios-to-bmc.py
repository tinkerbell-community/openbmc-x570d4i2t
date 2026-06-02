#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
push-smbios-to-bmc.py — Push host SMBIOS tables to BMC via IPMI MDR commands.

On the ASRock Rack X570D4I-2T, the AMI BIOS does NOT push SMBIOS tables via
IPMI KCS.  It instead uses the Megarac proprietary Redfish Host Interface,
which is not present in our OpenBMC build.

This script fills the gap by reading the live SMBIOS tables from the host OS
(via /sys/firmware/dmi/tables/) and pushing them to the BMC using our
ami-ipmi-oem plugin's AMI MDR V1 IPMI handlers:

  NetFn 0x32, Cmd 0x51 — MdrWriteBegin  (opens transfer session)
  NetFn 0x32, Cmd 0x52 — MdrWriteChunk  (streams data in 48-byte chunks)
  NetFn 0x32, Cmd 0x53 — MdrWriteEnd    (commits; BMC writes smbios2 + notifies)

Usage (from the host system via local KCS):
  python3 push-smbios-to-bmc.py --interface kcs

Usage (from any machine on the network via IPMI LAN):
  python3 push-smbios-to-bmc.py --interface lanplus --bmc 10.0.80.1 \\
      --user root --password 0penBmc

Usage (from any machine, providing a pre-captured SMBIOS file):
  python3 push-smbios-to-bmc.py --smbios-file /path/to/smbios.bin \\
      --interface lanplus --bmc 10.0.80.1 --user root --password 0penBmc

Dependencies:
  ipmitool   (must be in PATH)
  Python 3.6+

On the host system (to run automatically on each boot):
  1. Copy this script to /usr/local/bin/push-smbios-to-bmc.py
  2. Install the companion systemd service (push-smbios-to-bmc.service)
  3. systemctl enable --now push-smbios-to-bmc.service
"""

import argparse
import os
import subprocess
import sys

# ── IPMI constants ────────────────────────────────────────────────────────────
NETFN_AMI = 0x32
CMD_WRITE_BEGIN = 0x51
CMD_WRITE_CHUNK = 0x52
CMD_WRITE_END   = 0x53
REGION_ID       = 0x01  # SMBIOS region

# Max bytes per IPMI KCS message leaving room for command / response headers.
# KCS has a ~64-byte limit; with the 3-byte chunk header we cap at 48 bytes.
MAX_CHUNK_BYTES = 48

# ── SMBIOS source files ───────────────────────────────────────────────────────
DMI_DIR         = "/sys/firmware/dmi/tables"
ENTRY_POINT_F   = os.path.join(DMI_DIR, "smbios_entry_point")
DMI_TABLE_F     = os.path.join(DMI_DIR, "DMI")


def load_smbios_from_host() -> bytes:
    """Combine the SMBIOS entry point and raw DMI table into one buffer."""
    for path in (ENTRY_POINT_F, DMI_TABLE_F):
        if not os.path.exists(path):
            sys.exit(f"ERROR: SMBIOS source not found: {path}\n"
                     "       Run this script on the host system, or supply --smbios-file.")
    ep   = open(ENTRY_POINT_F, "rb").read()
    dmi  = open(DMI_TABLE_F,   "rb").read()
    data = ep + dmi
    anchor = data[:5]
    if b"_SM3_" in anchor or b"_SM_" in anchor:
        print(f"[INFO] Loaded SMBIOS from host: entry_point={len(ep)}B  "
              f"dmi={len(dmi)}B  total={len(data)}B  anchor={anchor[:5]}")
    else:
        sys.exit(f"ERROR: Unexpected anchor bytes in SMBIOS entry point: {anchor!r}")
    return data


def build_ipmitool_prefix(args) -> list:
    """Return the ipmitool invocation prefix for the chosen interface."""
    prefix = ["ipmitool"]
    if args.interface == "kcs":
        prefix += ["-I", "kcs"]
    elif args.interface == "lanplus":
        if not args.bmc:
            sys.exit("ERROR: --bmc required for lanplus interface")
        prefix += ["-I", "lanplus", "-H", args.bmc,
                   "-U", args.user, "-P", args.password]
    else:
        sys.exit(f"ERROR: unknown interface '{args.interface}'")
    return prefix


def ipmi_raw(prefix: list, netfn: int, cmd: int, *payload: int) -> None:
    """Send one IPMI raw command and raise on failure."""
    byte_args = [f"0x{netfn:02X}", f"0x{cmd:02X}"] + [f"0x{b:02X}" for b in payload]
    full_cmd  = prefix + ["raw"] + byte_args
    result    = subprocess.run(full_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        sys.exit(f"ERROR: ipmitool failed (NetFn=0x{netfn:02X} Cmd=0x{cmd:02X}): {detail}")


def push_smbios(data: bytes, ipmi_prefix: list) -> None:
    """Transfer SMBIOS data to the BMC via MdrWriteBegin/Chunk/End."""
    size = len(data)
    print(f"[INFO] Pushing {size} bytes of SMBIOS to BMC ...")

    # Step 1: MdrWriteBegin
    size_lo = size & 0xFF
    size_hi = (size >> 8) & 0xFF
    print(f"[INFO]  -> MdrWriteBegin (0x51): totalSize={size:#06x}")
    ipmi_raw(ipmi_prefix, NETFN_AMI, CMD_WRITE_BEGIN, REGION_ID, size_lo, size_hi)

    # Step 2: MdrWriteChunk (stream in MAX_CHUNK_BYTES-byte pieces)
    offset = 0
    num_chunks = (size + MAX_CHUNK_BYTES - 1) // MAX_CHUNK_BYTES
    print(f"[INFO]  -> MdrWriteChunk (0x52): {num_chunks} chunks × {MAX_CHUNK_BYTES}B")
    while offset < size:
        chunk  = data[offset : offset + MAX_CHUNK_BYTES]
        off_lo = offset & 0xFF
        off_hi = (offset >> 8) & 0xFF
        payload = [REGION_ID, off_lo, off_hi] + list(chunk)
        ipmi_raw(ipmi_prefix, NETFN_AMI, CMD_WRITE_CHUNK, *payload)
        offset += len(chunk)
        # Progress dot every 10 chunks
        if ((offset // MAX_CHUNK_BYTES) % 10) == 0:
            print(".", end="", flush=True)
    print()

    # Step 3: MdrWriteEnd — triggers write to /var/lib/smbios/smbios2
    print("[INFO]  -> MdrWriteEnd (0x53): committing ...")
    ipmi_raw(ipmi_prefix, NETFN_AMI, CMD_WRITE_END, REGION_ID)

    print("[INFO] Done — BMC will parse SMBIOS and populate D-Bus inventory.")


def main():
    parser = argparse.ArgumentParser(
        description="Push host SMBIOS tables to BMC via AMI MDR IPMI commands.")
    parser.add_argument(
        "--interface", default="kcs", choices=["kcs", "lanplus"],
        help="IPMI interface: 'kcs' (local, default) or 'lanplus' (LAN)")
    parser.add_argument("--bmc",      default="",         help="BMC IP/hostname (lanplus)")
    parser.add_argument("--user",     default="root",     help="IPMI username (lanplus)")
    parser.add_argument("--password", default="0penBmc",  help="IPMI password (lanplus)")
    parser.add_argument(
        "--smbios-file", default="",
        help="Optional path to a pre-captured SMBIOS binary file "
             "(entry_point + DMI concatenated).  "
             "If not set, reads from /sys/firmware/dmi/tables/.")
    args = parser.parse_args()

    if args.smbios_file:
        if not os.path.exists(args.smbios_file):
            sys.exit(f"ERROR: SMBIOS file not found: {args.smbios_file}")
        data = open(args.smbios_file, "rb").read()
        print(f"[INFO] Loaded SMBIOS from file: {args.smbios_file} ({len(data)}B)")
    else:
        data = load_smbios_from_host()

    ipmi_prefix = build_ipmitool_prefix(args)
    push_smbios(data, ipmi_prefix)


if __name__ == "__main__":
    main()
